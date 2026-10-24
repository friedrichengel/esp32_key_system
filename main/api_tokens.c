#include "api_tokens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "psa/crypto.h"

#include "storage.h"
#include "time_sync.h"
#include "event_log.h"

static const char *TAG = "tokens";

static api_token_t       s_tokens[CFG_TOKENS_MAX];
static size_t            s_count;
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ------------------------------------------------------------------------
 * Hilfsfunktionen
 * ---------------------------------------------------------------------- */
static bool sha256(const void *data, size_t len, uint8_t out[32])
{
    size_t olen = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &olen) == PSA_SUCCESS && olen == 32;
}

/* Vergleich ohne frühzeitigen Abbruch (kein Timing-Leck über die Position des ersten Unterschieds) */
static int ct_memcmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d;
}

static void b64url_encode(const uint8_t *in, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = T[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = T[v & 63];
    }
    out[o] = '\0';
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static bool hex_decode(const char *s, uint8_t *out, size_t n)
{
    if (!s || strlen(s) != 2 * n) return false;
    for (size_t i = 0; i < 2 * n; i++) {
        char c = (char)tolower((unsigned char)s[i]);
        uint8_t v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return false;
        out[i / 2] = (i & 1) ? (out[i / 2] | v) : (uint8_t)(v << 4);
    }
    return true;
}

static const char *js(const cJSON *o, const char *k, const char *d)
{
    cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return (cJSON_IsString(i) && i->valuestring) ? i->valuestring : d;
}
static double jnum(const cJSON *o, const char *k, double d)
{
    cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : d;
}
static bool jb(const cJSON *o, const char *k, bool d)
{
    cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(i) ? cJSON_IsTrue(i) : d;
}

#define ALL_DOORS ((1u << CFG_MAX_DOORS) - 1)

/* ------------------------------------------------------------------------
 * IP-Regeln: "a.b.c.d", "a.b.c.d/24", "fd00::1", "fd00::/64"
 * ---------------------------------------------------------------------- */
static bool parse_ip(const char *s, bool *v6, uint8_t out[16])
{
    esp_ip4_addr_t a4;
    esp_ip6_addr_t a6;
    if (esp_netif_str_to_ip4(s, &a4) == ESP_OK) {
        *v6 = false;
        memcpy(out, &a4.addr, 4);
        return true;
    }
    if (esp_netif_str_to_ip6(s, &a6) == ESP_OK) {
        *v6 = true;
        memcpy(out, a6.addr, 16);
        return true;
    }
    return false;
}

static bool parse_rule(const char *rule, bool *v6, uint8_t addr[16], int *prefix)
{
    char buf[TOKEN_IP_LEN];
    strlcpy(buf, rule, sizeof(buf));
    char *slash = strchr(buf, '/');
    int p = -1;
    if (slash) {
        *slash = '\0';
        char *end = NULL;
        long v = strtol(slash + 1, &end, 10);
        if (!end || *end || v < 0) return false;
        p = (int)v;
    }
    if (!parse_ip(buf, v6, addr)) return false;
    int max = *v6 ? 128 : 32;
    if (p < 0) p = max;
    if (p > max) return false;
    *prefix = p;
    return true;
}

static bool prefix_match(const uint8_t *a, const uint8_t *b, int bits)
{
    int full = bits / 8, rest = bits % 8;
    if (full && memcmp(a, b, full) != 0) return false;
    if (rest) {
        uint8_t mask = (uint8_t)(0xFF << (8 - rest));
        if ((a[full] & mask) != (b[full] & mask)) return false;
    }
    return true;
}

bool api_tokens_valid_ip_rule(const char *rule)
{
    if (!rule || !rule[0]) return true;
    if (strlen(rule) >= TOKEN_IP_LEN) return false;
    bool v6; uint8_t a[16]; int p;
    return parse_rule(rule, &v6, a, &p);
}

static bool ip_allowed(const char *rule, const char *peer)
{
    if (!rule || !rule[0]) return true;
    bool rv6, pv6;
    uint8_t ra[16], pa[16];
    int prefix;
    if (!parse_rule(rule, &rv6, ra, &prefix)) return false;
    if (!peer || !parse_ip(peer, &pv6, pa)) return false;
    if (rv6 != pv6) return false;
    return prefix_match(ra, pa, prefix);
}

/* ------------------------------------------------------------------------
 * JSON / Persistenz
 * ---------------------------------------------------------------------- */
cJSON *api_tokens_entry_to_json(const api_token_t *t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", t->id);
    cJSON_AddStringToObject(o, "name", t->name);
    cJSON_AddNumberToObject(o, "doors", t->doors);
    cJSON_AddBoolToObject(o, "can_open", t->can_open);
    cJSON_AddBoolToObject(o, "can_status", t->can_status);
    cJSON_AddBoolToObject(o, "enabled", t->enabled);
    cJSON_AddStringToObject(o, "allow_ip", t->allow_ip);
    cJSON_AddNumberToObject(o, "expires_at", (double)t->expires_at);
    cJSON_AddNumberToObject(o, "created_at", (double)t->created_at);
    cJSON_AddNumberToObject(o, "last_used", (double)t->last_used);
    cJSON_AddNumberToObject(o, "use_count", t->use_count);
    return o;
}

static cJSON *entry_to_file_json(const api_token_t *t)
{
    cJSON *o = api_tokens_entry_to_json(t);
    char hex[65];
    hex_encode(t->hash, 32, hex);
    cJSON_AddStringToObject(o, "hash", hex);
    return o;
}

static bool entry_from_file_json(const cJSON *o, api_token_t *t)
{
    memset(t, 0, sizeof(*t));
    strlcpy(t->id, js(o, "id", ""), sizeof(t->id));
    strlcpy(t->name, js(o, "name", ""), sizeof(t->name));
    if (!hex_decode(js(o, "hash", NULL), t->hash, 32)) return false;
    t->doors = (uint32_t)jnum(o, "doors", ALL_DOORS) & ALL_DOORS;
    t->can_open = jb(o, "can_open", true);
    t->can_status = jb(o, "can_status", true);
    t->enabled = jb(o, "enabled", true);
    strlcpy(t->allow_ip, js(o, "allow_ip", ""), sizeof(t->allow_ip));
    t->expires_at = (int64_t)jnum(o, "expires_at", 0);
    t->created_at = (int64_t)jnum(o, "created_at", 0);
    t->last_used = (int64_t)jnum(o, "last_used", 0);
    t->use_count = (uint32_t)jnum(o, "use_count", 0);
    return strlen(t->id) == TOKEN_ID_LEN && t->name[0];
}

static esp_err_t save_locked(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s_count; i++) {
        cJSON_AddItemToArray(arr, entry_to_file_json(&s_tokens[i]));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "tokens", arr);
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return ESP_ERR_NO_MEM;
    esp_err_t err = storage_write_file(CFG_TOKENS_PATH, txt, strlen(txt));
    cJSON_free(txt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tokens.json speichern fehlgeschlagen");
    }
    return err;
}

static void load(void)
{
    size_t len = 0;
    char *txt = storage_read_file(CFG_TOKENS_PATH, &len);
    if (!txt) {
        ESP_LOGI(TAG, "Keine Token-Datei");
        return;
    }
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) {
        ESP_LOGE(TAG, "tokens.json unlesbar");
        event_log(EV_SYSTEM, "tokens.json unlesbar, keine API-Token aktiv");
        return;
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "tokens");
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (s_count >= CFG_TOKENS_MAX) break;
        api_token_t t;
        if (entry_from_file_json(it, &t)) {
            s_tokens[s_count++] = t;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%u API-Token geladen", (unsigned)s_count);
}

esp_err_t api_tokens_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    load();
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * CRUD
 * ---------------------------------------------------------------------- */
size_t api_tokens_count(void)
{
    return s_count;
}

size_t api_tokens_list(api_token_t *out, size_t max)
{
    LOCK();
    size_t n = s_count < max ? s_count : max;
    memcpy(out, s_tokens, n * sizeof(api_token_t));
    UNLOCK();
    return n;
}

static int find_locked(const char *id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_tokens[i].id, id) == 0) return (int)i;
    }
    return -1;
}

bool api_tokens_get(const char *id, api_token_t *out)
{
    if (!id) return false;
    LOCK();
    int i = find_locked(id);
    if (i >= 0) *out = s_tokens[i];
    UNLOCK();
    return i >= 0;
}

esp_err_t api_tokens_create(api_token_t *t, char *plain_out, size_t plain_len)
{
    if (!t || !t->name[0] || !plain_out || plain_len < TOKEN_PLAIN_LEN + 1) return ESP_ERR_INVALID_ARG;
    if (!api_tokens_valid_ip_rule(t->allow_ip)) return ESP_ERR_INVALID_ARG;

    uint8_t id_raw[6], secret_raw[32];
    char secret[TOKEN_SECRET_LEN + 1];

    LOCK();
    if (s_count >= CFG_TOKENS_MAX) { UNLOCK(); return ESP_ERR_NO_MEM; }
    do {
        esp_fill_random(id_raw, sizeof(id_raw));
        b64url_encode(id_raw, sizeof(id_raw), t->id);
    } while (find_locked(t->id) >= 0);
    esp_fill_random(secret_raw, sizeof(secret_raw));
    b64url_encode(secret_raw, sizeof(secret_raw), secret);
    if (!sha256(secret, TOKEN_SECRET_LEN, t->hash)) { UNLOCK(); return ESP_FAIL; }

    t->doors &= ALL_DOORS;
    t->created_at = time_sync_is_valid() ? time(NULL) : 0;
    t->last_used = 0;
    t->use_count = 0;
    s_tokens[s_count++] = *t;
    esp_err_t err = save_locked();
    UNLOCK();

    snprintf(plain_out, plain_len, "%s.%s.%s", TOKEN_PREFIX, t->id, secret);
    memset(secret, 0, sizeof(secret));
    memset(secret_raw, 0, sizeof(secret_raw));
    return err;
}

esp_err_t api_tokens_update(const api_token_t *t)
{
    if (!t || !t->name[0]) return ESP_ERR_INVALID_ARG;
    if (!api_tokens_valid_ip_rule(t->allow_ip)) return ESP_ERR_INVALID_ARG;
    LOCK();
    int i = find_locked(t->id);
    if (i < 0) { UNLOCK(); return ESP_ERR_NOT_FOUND; }
    api_token_t *cur = &s_tokens[i];
    strlcpy(cur->name, t->name, sizeof(cur->name));
    cur->doors = t->doors & ALL_DOORS;
    cur->can_open = t->can_open;
    cur->can_status = t->can_status;
    cur->enabled = t->enabled;
    strlcpy(cur->allow_ip, t->allow_ip, sizeof(cur->allow_ip));
    cur->expires_at = t->expires_at;
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

esp_err_t api_tokens_delete(const char *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    LOCK();
    int i = find_locked(id);
    if (i < 0) { UNLOCK(); return ESP_ERR_NOT_FOUND; }
    memmove(&s_tokens[i], &s_tokens[i + 1], (s_count - i - 1) * sizeof(api_token_t));
    s_count--;
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

esp_err_t api_tokens_clear(void)
{
    LOCK();
    s_count = 0;
    memset(s_tokens, 0, sizeof(s_tokens));
    esp_err_t err = storage_remove(CFG_TOKENS_PATH);
    UNLOCK();
    return err == ESP_OK || err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

/* ------------------------------------------------------------------------
 * Prüfung
 * ---------------------------------------------------------------------- */
token_result_t api_tokens_verify(const char *plain, const char *peer_ip, bool persist_use, api_token_t *out)
{
    size_t n = plain ? strlen(plain) : 0;
    if (n != TOKEN_PLAIN_LEN || strncmp(plain, TOKEN_PREFIX ".", 4) != 0 || plain[4 + TOKEN_ID_LEN] != '.') {
        return TOKEN_MALFORMED;
    }
    char id[TOKEN_ID_LEN + 1];
    memcpy(id, plain + 4, TOKEN_ID_LEN);
    id[TOKEN_ID_LEN] = '\0';
    const char *secret = plain + 4 + TOKEN_ID_LEN + 1;

    uint8_t h[32];
    if (!sha256(secret, TOKEN_SECRET_LEN, h)) return TOKEN_UNKNOWN;

    LOCK();
    int i = find_locked(id);
    /* Unbekannte id und falsches Secret sind von außen nicht unterscheidbar */
    if (i < 0 || ct_memcmp(h, s_tokens[i].hash, 32) != 0) {
        UNLOCK();
        return TOKEN_UNKNOWN;
    }
    api_token_t *t = &s_tokens[i];
    token_result_t res = TOKEN_OK;
    if (!t->enabled) {
        res = TOKEN_DISABLED;
    } else if (t->expires_at && time_sync_is_valid() && time(NULL) > t->expires_at) {
        res = TOKEN_EXPIRED;
    } else if (!ip_allowed(t->allow_ip, peer_ip)) {
        res = TOKEN_IP_DENIED;
    }
    if (res == TOKEN_OK) {
        t->use_count++;
        t->last_used = time_sync_is_valid() ? time(NULL) : 0;
        if (persist_use) save_locked();
    }
    if (out) *out = *t;
    UNLOCK();
    return res;
}

const char *api_tokens_result_text(token_result_t r)
{
    switch (r) {
    case TOKEN_OK:        return "ok";
    case TOKEN_MALFORMED: return "Token-Format ungueltig";
    case TOKEN_UNKNOWN:   return "Token unbekannt";
    case TOKEN_DISABLED:  return "Token gesperrt";
    case TOKEN_EXPIRED:   return "Token abgelaufen";
    case TOKEN_IP_DENIED: return "Quelladresse nicht erlaubt";
    }
    return "?";
}

/* ------------------------------------------------------------------------
 * Rate-Limit je Quell-IP (kleine LRU-Tabelle)
 * ---------------------------------------------------------------------- */
typedef struct {
    char     ip[TOKEN_IP_LEN];
    uint32_t fails;
    uint32_t lock_s;
    int64_t  lock_until_us;
    int64_t  last_us;
} rate_entry_t;

#define RATE_ENTRIES 8
static rate_entry_t s_rate[RATE_ENTRIES];

static rate_entry_t *rate_slot(const char *ip, bool create)
{
    const char *key = (ip && ip[0]) ? ip : "?";
    rate_entry_t *oldest = &s_rate[0];
    for (int i = 0; i < RATE_ENTRIES; i++) {
        if (s_rate[i].ip[0] && strcmp(s_rate[i].ip, key) == 0) return &s_rate[i];
        if (!s_rate[i].ip[0]) { oldest = &s_rate[i]; break; }
        if (s_rate[i].last_us < oldest->last_us) oldest = &s_rate[i];
    }
    if (!create) return NULL;
    memset(oldest, 0, sizeof(*oldest));
    strlcpy(oldest->ip, key, sizeof(oldest->ip));
    oldest->lock_s = CFG_TOKEN_LOCK_BASE_S;
    return oldest;
}

bool api_tokens_rate_allowed(const char *peer_ip, uint32_t *retry_after_s)
{
    LOCK();
    rate_entry_t *e = rate_slot(peer_ip, false);
    bool ok = true;
    if (e) {
        int64_t now = esp_timer_get_time();
        if (e->lock_until_us > now) {
            ok = false;
            if (retry_after_s) *retry_after_s = (uint32_t)((e->lock_until_us - now) / 1000000) + 1;
        }
    }
    UNLOCK();
    return ok;
}

void api_tokens_rate_failed(const char *peer_ip)
{
    LOCK();
    rate_entry_t *e = rate_slot(peer_ip, true);
    int64_t now = esp_timer_get_time();
    e->last_us = now;
    e->fails++;
    if (e->fails >= CFG_TOKEN_FAIL_LIMIT) {
        e->fails = 0;
        e->lock_until_us = now + (int64_t)e->lock_s * 1000000;
        event_log(EV_DENIED, "API: %s nach %d Fehlversuchen fuer %" PRIu32 " s gesperrt", e->ip,
                  CFG_TOKEN_FAIL_LIMIT, e->lock_s);
        if (e->lock_s * 2 <= CFG_TOKEN_LOCK_MAX_S) e->lock_s *= 2;
    }
    UNLOCK();
}

void api_tokens_rate_succeeded(const char *peer_ip)
{
    LOCK();
    rate_entry_t *e = rate_slot(peer_ip, false);
    if (e) {
        e->fails = 0;
        e->lock_s = CFG_TOKEN_LOCK_BASE_S;
        e->last_us = esp_timer_get_time();
    }
    UNLOCK();
}
