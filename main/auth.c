#include "auth.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"
#include "time_sync.h"
#include "users.h"

static const char *TAG = "auth";

#define TOTP_STEP_S     30
#define TOTP_DIGITS     1000000
#define PREAUTH_MAX     4
#define PREAUTH_TRIES   5

static auth_session_t s_sessions[AUTH_MAX_SESSIONS];
static uint32_t       s_fail_count;
static int64_t        s_lock_until_us;
static uint32_t       s_lock_s = 60;

typedef struct {
    char    ticket[AUTH_TICKET_HEX_LEN + 1];
    uint8_t uid;
    int64_t expires_us;
    uint8_t tries;
} preauth_t;
static preauth_t s_preauth[PREAUTH_MAX];

/* 2FA-Einrichtung in Arbeit (noch nicht bestätigt); ein Vorgang gleichzeitig, an eine uid gebunden */
static struct {
    bool    active;
    uint8_t uid;
    int64_t expires_us;
    uint8_t secret[SETTINGS_TOTP_SECRET_LEN];
    uint8_t recovery_hash[SETTINGS_RECOVERY_COUNT][32];
} s_pending;

esp_err_t auth_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    memset(s_preauth, 0, sizeof(s_preauth));
    memset(&s_pending, 0, sizeof(s_pending));
    psa_crypto_init();
    return ESP_OK;
}

/* ========================================================================
 * Krypto-Helfer (PSA)
 * ====================================================================== */
static psa_status_t hmac_compute(psa_algorithm_t hash_alg, const uint8_t *key, size_t key_len,
                                 const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
                                 uint8_t *out, size_t out_size, size_t *out_len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(hash_alg));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_SIGN_HASH);
    psa_key_id_t kid = 0;
    psa_status_t st = psa_import_key(&attr, key, key_len, &kid);
    if (st != PSA_SUCCESS) return st;

    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    st = psa_mac_sign_setup(&op, kid, PSA_ALG_HMAC(hash_alg));
    if (st == PSA_SUCCESS && alen) st = psa_mac_update(&op, a, alen);
    if (st == PSA_SUCCESS && blen) st = psa_mac_update(&op, b, blen);
    if (st == PSA_SUCCESS) st = psa_mac_sign_finish(&op, out, out_size, out_len);
    psa_mac_abort(&op);
    psa_destroy_key(kid);
    return st;
}

static bool sha256(const void *data, size_t len, uint8_t out[32])
{
    size_t olen = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &olen) == PSA_SUCCESS && olen == 32;
}

bool auth_const_time_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* PBKDF2-HMAC-SHA256 (RFC 8018); der Schlüssel wird einmal importiert */
int auth_hash_password(const char *pw, const uint8_t *salt, size_t salt_len,
                       uint32_t iterations, uint8_t *out, size_t out_len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_SIGN_HASH);
    psa_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, (const uint8_t *)pw, strlen(pw), &key);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key: %d", (int)st);
        return -2;
    }

    uint8_t u[32], t[32];
    uint32_t block = 1;
    size_t pos = 0;
    while (pos < out_len && st == PSA_SUCCESS) {
        uint8_t be[4] = { block >> 24, block >> 16, block >> 8, block };
        psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
        size_t olen = 0;
        st = psa_mac_sign_setup(&op, key, PSA_ALG_HMAC(PSA_ALG_SHA_256));
        if (st == PSA_SUCCESS) st = psa_mac_update(&op, salt, salt_len);
        if (st == PSA_SUCCESS) st = psa_mac_update(&op, be, 4);
        if (st == PSA_SUCCESS) st = psa_mac_sign_finish(&op, u, 32, &olen);
        psa_mac_abort(&op);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations && st == PSA_SUCCESS; i++) {
            op = (psa_mac_operation_t)PSA_MAC_OPERATION_INIT;
            st = psa_mac_sign_setup(&op, key, PSA_ALG_HMAC(PSA_ALG_SHA_256));
            if (st == PSA_SUCCESS) st = psa_mac_update(&op, u, 32);
            if (st == PSA_SUCCESS) st = psa_mac_sign_finish(&op, u, 32, &olen);
            psa_mac_abort(&op);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t n = (out_len - pos < 32) ? out_len - pos : 32;
        memcpy(out + pos, t, n);
        pos += n;
        block++;
    }
    psa_destroy_key(key);
    return (st == PSA_SUCCESS) ? 0 : -3;
}

bool auth_valid_username(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n == 0 || n >= AUTH_NAME_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= 0x20 || c == 0x7F || c == '"' || c == '\\' || c == '<' || c == '>') return false;
    }
    return true;
}

/* ========================================================================
 * Passwort / Login
 * ====================================================================== */
esp_err_t auth_set_password(const char *user, const char *password)
{
    if (!auth_valid_username(user) || !password || strlen(password) < AUTH_PW_MIN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!users_name_available(user, AUTH_UID_ADMIN)) {
        return ESP_ERR_INVALID_STATE;             /* Name gehört bereits einem Benutzer */
    }
    settings_admin_t *a = &g_settings.admin;
    strlcpy(a->user, user, sizeof(a->user));
    esp_fill_random(a->pw_salt, sizeof(a->pw_salt));
    a->pw_iterations = AUTH_PW_ITERATIONS;
    int ret = auth_hash_password(password, a->pw_salt, sizeof(a->pw_salt), a->pw_iterations,
                                 a->pw_hash, sizeof(a->pw_hash));
    if (ret != 0) {
        ESP_LOGE(TAG, "PBKDF2 fehlgeschlagen: %d", ret);
        return ESP_FAIL;
    }
    a->setup_done = true;
    auth_sessions_destroy_uid(AUTH_UID_ADMIN);
    return settings_save();
}

static bool admin_password_ok(const char *password)
{
    const settings_admin_t *a = &g_settings.admin;
    if (!a->setup_done || a->pw_iterations == 0 || !password) return false;
    uint8_t hash[SETTINGS_PW_HASH_LEN];
    if (auth_hash_password(password, a->pw_salt, sizeof(a->pw_salt), a->pw_iterations, hash, sizeof(hash)) != 0) {
        return false;
    }
    return auth_const_time_eq(hash, a->pw_hash, sizeof(hash));
}

bool auth_principal_load(uint8_t uid, auth_principal_t *p)
{
    memset(p, 0, sizeof(*p));
    if (uid == AUTH_UID_ADMIN) {
        if (!g_settings.admin.setup_done) return false;
        p->uid = AUTH_UID_ADMIN;
        p->is_admin = true;
        strlcpy(p->name, g_settings.admin.user, sizeof(p->name));
        p->code_types = USER_TYPES_ALL;
        p->doors = CFG_ALL_DOORS_MASK;
        p->perms = USER_PERM_ALL;
        return true;
    }
    user_t u;
    if (!users_get(uid, &u) || !u.enabled) return false;
    users_fill_principal(&u, p);
    return true;
}

auth_login_result_t auth_login_check(const char *user, const char *password, auth_principal_t *p)
{
    if (!user || !*user || !password) return LOGIN_BAD_CREDENTIALS;
    const settings_admin_t *a = &g_settings.admin;
    if (a->setup_done && strcmp(user, a->user) == 0) {
        if (!admin_password_ok(password)) return LOGIN_BAD_CREDENTIALS;
        auth_principal_load(AUTH_UID_ADMIN, p);
        return LOGIN_OK;
    }
    user_t u;
    if (users_get_by_name(user, &u)) {
        if (!users_verify_password(&u, password)) return LOGIN_BAD_CREDENTIALS;
        if (!u.enabled) return LOGIN_DISABLED;
        users_fill_principal(&u, p);
        return LOGIN_OK;
    }
    /* Unbekannter Name: trotzdem einen Hash rechnen, damit die Antwortzeit den Namen nicht verraet */
    static const uint8_t dummy_salt[SETTINGS_PW_SALT_LEN] = { 0 };
    uint8_t dummy[SETTINGS_PW_HASH_LEN];
    auth_hash_password(password, dummy_salt, sizeof(dummy_salt), AUTH_PW_ITERATIONS, dummy, sizeof(dummy));
    return LOGIN_BAD_CREDENTIALS;
}

bool auth_verify_principal_password(const auth_principal_t *p, const char *password)
{
    if (!p) return false;
    if (p->uid == AUTH_UID_ADMIN) return admin_password_ok(password);
    user_t u;
    return users_get(p->uid, &u) && users_verify_password(&u, password);
}

bool auth_setup_required(void)
{
    return !g_settings.admin.setup_done;
}

/* ========================================================================
 * Rate-Limit
 * ====================================================================== */
bool auth_login_allowed(uint32_t *retry_after_s)
{
    int64_t now = esp_timer_get_time();
    if (s_lock_until_us > now) {
        if (retry_after_s) *retry_after_s = (uint32_t)((s_lock_until_us - now) / 1000000) + 1;
        return false;
    }
    return true;
}

void auth_login_failed(void)
{
    s_fail_count++;
    if (s_fail_count >= CFG_LOGIN_FAIL_LIMIT) {
        s_lock_until_us = esp_timer_get_time() + (int64_t)s_lock_s * 1000000;
        event_log(EV_LOGIN, "Login gesperrt fuer %" PRIu32 " s nach %" PRIu32 " Fehlversuchen", s_lock_s, s_fail_count);
        s_fail_count = 0;
        if (s_lock_s < 600) s_lock_s *= 2;
    }
}

void auth_login_succeeded(void)
{
    s_fail_count = 0;
    s_lock_s = 60;
}

/* ========================================================================
 * Sessions
 * ====================================================================== */
void auth_random_hex(char *out, size_t nbytes)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t buf[32];
    if (nbytes > sizeof(buf)) nbytes = sizeof(buf);
    esp_fill_random(buf, nbytes);
    for (size_t i = 0; i < nbytes; i++) {
        out[2 * i] = hex[buf[i] >> 4];
        out[2 * i + 1] = hex[buf[i] & 0xF];
    }
    out[2 * nbytes] = '\0';
}

auth_session_t *auth_session_create(const auth_principal_t *p)
{
    int64_t now = esp_timer_get_time();
    auth_session_t *slot = NULL;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == 0 || s_sessions[i].expires_us < now) {
            slot = &s_sessions[i];
            break;
        }
    }
    if (!slot) {
        slot = &s_sessions[0];
        for (int i = 1; i < AUTH_MAX_SESSIONS; i++) {
            if (s_sessions[i].expires_us < slot->expires_us) slot = &s_sessions[i];
        }
    }
    memset(slot, 0, sizeof(*slot));
    auth_random_hex(slot->token, AUTH_TOKEN_HEX_LEN / 2);
    auth_random_hex(slot->csrf, AUTH_CSRF_HEX_LEN / 2);
    slot->expires_us = now + (int64_t)CFG_SESSION_TIMEOUT_S * 1000000;
    slot->uid = p->uid;
    strlcpy(slot->user, p->name, sizeof(slot->user));
    return slot;
}

auth_session_t *auth_session_find(const char *token)
{
    if (!token || strlen(token) != AUTH_TOKEN_HEX_LEN) {
        return NULL;
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        auth_session_t *s = &s_sessions[i];
        if (s->token[0] && s->expires_us >= now &&
            auth_const_time_eq((const uint8_t *)s->token, (const uint8_t *)token, AUTH_TOKEN_HEX_LEN)) {
            s->expires_us = now + (int64_t)CFG_SESSION_TIMEOUT_S * 1000000;
            return s;
        }
    }
    return NULL;
}

void auth_session_destroy(const char *token)
{
    auth_session_t *s = auth_session_find(token);
    if (s) {
        memset(s, 0, sizeof(*s));
    }
}

void auth_sessions_clear(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
}

void auth_sessions_destroy_uid(uint8_t uid)
{
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] && s_sessions[i].uid == uid) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
        }
    }
    for (int i = 0; i < PREAUTH_MAX; i++) {
        if (s_preauth[i].ticket[0] && s_preauth[i].uid == uid) {
            memset(&s_preauth[i], 0, sizeof(s_preauth[i]));
        }
    }
}

bool auth_session_principal(const auth_session_t *s, auth_principal_t *p)
{
    if (!s) return false;
    if (!auth_principal_load(s->uid, p)) return false;
    /* Admin-Konto umbenannt oder Benutzer gelöscht und uid neu vergeben: Session verfällt */
    return strcmp(p->name, s->user) == 0;
}

/* ========================================================================
 * Pre-Auth-Tickets (zwischen Passwort und zweitem Faktor)
 * ====================================================================== */
const char *auth_preauth_create(const auth_principal_t *p)
{
    int64_t now = esp_timer_get_time();
    preauth_t *slot = &s_preauth[0];
    for (int i = 0; i < PREAUTH_MAX; i++) {
        if (s_preauth[i].ticket[0] == 0 || s_preauth[i].expires_us < now) { slot = &s_preauth[i]; break; }
        if (s_preauth[i].expires_us < slot->expires_us) slot = &s_preauth[i];
    }
    memset(slot, 0, sizeof(*slot));
    auth_random_hex(slot->ticket, AUTH_TICKET_HEX_LEN / 2);
    slot->uid = p->uid;
    slot->expires_us = now + (int64_t)CFG_TWOFA_TICKET_TTL_S * 1000000;
    return slot->ticket;
}

static preauth_t *preauth_find(const char *ticket)
{
    if (!ticket || strlen(ticket) != AUTH_TICKET_HEX_LEN) return NULL;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < PREAUTH_MAX; i++) {
        preauth_t *p = &s_preauth[i];
        if (p->ticket[0] && p->expires_us >= now &&
            auth_const_time_eq((const uint8_t *)p->ticket, (const uint8_t *)ticket, AUTH_TICKET_HEX_LEN)) {
            return p;
        }
    }
    return NULL;
}

bool auth_preauth_check(const char *ticket, auth_principal_t *p)
{
    preauth_t *t = preauth_find(ticket);
    if (!t) return false;
    if (++t->tries > PREAUTH_TRIES) {
        memset(t, 0, sizeof(*t));
        return false;
    }
    return auth_principal_load(t->uid, p);
}

void auth_preauth_consume(const char *ticket)
{
    preauth_t *p = preauth_find(ticket);
    if (p) memset(p, 0, sizeof(*p));
}

/* ========================================================================
 * TOTP / Recovery-Codes
 * ====================================================================== */
static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
/* Recovery-Alphabet ohne verwechselbare Zeichen (0/O, 1/I) */
static const char RECOV[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

static void base32_encode(const uint8_t *in, size_t len, char *out)
{
    int bits = 0, value = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        value = (value << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = B32[(value >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) out[o++] = B32[(value << (5 - bits)) & 31];
    out[o] = '\0';
}

static uint32_t totp_at(const uint8_t *secret, uint64_t counter)
{
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) { msg[i] = counter & 0xFF; counter >>= 8; }
    uint8_t mac[32];
    size_t mlen = 0;
    if (hmac_compute(PSA_ALG_SHA_1, secret, SETTINGS_TOTP_SECRET_LEN, msg, 8, NULL, 0, mac, sizeof(mac), &mlen) != PSA_SUCCESS || mlen < 20) {
        return 0xFFFFFFFF;
    }
    int off = mac[19] & 0x0F;
    uint32_t bin = ((uint32_t)(mac[off] & 0x7F) << 24) | ((uint32_t)mac[off + 1] << 16) |
                   ((uint32_t)mac[off + 2] << 8) | mac[off + 3];
    return bin % TOTP_DIGITS;
}

/* Prüft einen 6-stelligen Code gegen secret; gibt den passenden Zeitschritt zurück oder -1 */
static int64_t totp_match(const uint8_t *secret, const char *code)
{
    if (!code || strlen(code) != 6) return -1;
    for (int i = 0; i < 6; i++) if (!isdigit((unsigned char)code[i])) return -1;
    uint32_t want = (uint32_t)atoi(code);
    int64_t step = (int64_t)time(NULL) / TOTP_STEP_S;
    for (int d = -CFG_TWOFA_WINDOW; d <= CFG_TWOFA_WINDOW; d++) {
        if (totp_at(secret, (uint64_t)(step + d)) == want) return step + d;
    }
    return -1;
}

static void make_recovery_codes(char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1],
                                uint8_t hashes[SETTINGS_RECOVERY_COUNT][32])
{
    for (int i = 0; i < SETTINGS_RECOVERY_COUNT; i++) {
        uint8_t rnd[10];
        esp_fill_random(rnd, sizeof(rnd));
        char *c = recovery[i];
        for (int j = 0; j < 10; j++) {
            c[j < 5 ? j : j + 1] = RECOV[rnd[j] % 32];
        }
        c[5] = '-';
        c[AUTH_RECOVERY_LEN] = '\0';
        sha256(c, AUTH_RECOVERY_LEN, hashes[i]);
    }
}

/* "abcde fghjk" / "ABCDEFGHJK" -> "ABCDE-FGHJK"; false wenn das kein Recovery-Code sein kann */
static bool normalize_recovery(const char *in, char out[AUTH_RECOVERY_LEN + 1])
{
    int n = 0;
    for (const char *p = in; *p && n < 10; p++) {
        if (*p == '-' || *p == ' ') continue;
        if (!isalnum((unsigned char)*p)) return false;
        out[n < 5 ? n : n + 1] = (char)toupper((unsigned char)*p);
        n++;
    }
    if (n != 10) return false;
    out[5] = '-';
    out[AUTH_RECOVERY_LEN] = '\0';
    return true;
}

/* 2FA-Datensatz lesen/schreiben: Admin im NVS-Blob, Benutzer in users.json */
static bool twofa_get(uint8_t uid, settings_twofa_t *t)
{
    if (uid == AUTH_UID_ADMIN) {
        *t = g_settings.twofa;
        return true;
    }
    return users_twofa_get(uid, t);
}

static esp_err_t twofa_put(uint8_t uid, const settings_twofa_t *t)
{
    if (uid == AUTH_UID_ADMIN) {
        g_settings.twofa = *t;
        return settings_save();
    }
    return users_twofa_set(uid, t);
}

static int recovery_remaining(const settings_twofa_t *t)
{
    int n = 0;
    for (int i = 0; i < SETTINGS_RECOVERY_COUNT; i++) {
        if (!(t->recovery_used & (1u << i))) n++;
    }
    return n;
}

bool auth_twofa_enabled(uint8_t uid)
{
    settings_twofa_t t;
    return twofa_get(uid, &t) && t.enabled;
}

int auth_twofa_recovery_remaining(uint8_t uid)
{
    settings_twofa_t t;
    if (!twofa_get(uid, &t)) return 0;
    return recovery_remaining(&t);
}

esp_err_t auth_twofa_begin_setup(uint8_t uid, char secret_b32[AUTH_B32_SECRET_LEN],
                                 char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1])
{
    memset(&s_pending, 0, sizeof(s_pending));
    esp_fill_random(s_pending.secret, sizeof(s_pending.secret));
    make_recovery_codes(recovery, s_pending.recovery_hash);
    base32_encode(s_pending.secret, sizeof(s_pending.secret), secret_b32);
    s_pending.uid = uid;
    s_pending.active = true;
    s_pending.expires_us = esp_timer_get_time() + (int64_t)CFG_TWOFA_SETUP_TTL_S * 1000000;
    return ESP_OK;
}

esp_err_t auth_twofa_confirm_setup(uint8_t uid, const char *totp_code)
{
    if (!s_pending.active || s_pending.uid != uid || esp_timer_get_time() > s_pending.expires_us) {
        if (s_pending.uid == uid) s_pending.active = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (!time_sync_is_valid()) {
        return ESP_ERR_INVALID_RESPONSE;          /* ohne Uhr kann der Bestätigungscode nicht geprüft werden */
    }
    int64_t step = totp_match(s_pending.secret, totp_code);
    if (step < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_twofa_t t;
    memset(&t, 0, sizeof(t));
    memcpy(t.secret, s_pending.secret, sizeof(t.secret));
    memcpy(t.recovery_hash, s_pending.recovery_hash, sizeof(t.recovery_hash));
    t.last_counter = step;
    t.enabled = true;
    memset(&s_pending, 0, sizeof(s_pending));
    esp_err_t err = twofa_put(uid, &t);
    memset(&t, 0, sizeof(t));
    return err;
}

void auth_twofa_cancel_setup(uint8_t uid)
{
    if (s_pending.uid == uid) memset(&s_pending, 0, sizeof(s_pending));
}

esp_err_t auth_twofa_disable(uint8_t uid)
{
    settings_twofa_t t;
    memset(&t, 0, sizeof(t));
    return twofa_put(uid, &t);
}

esp_err_t auth_twofa_regenerate_recovery(uint8_t uid, char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1])
{
    settings_twofa_t t;
    if (!twofa_get(uid, &t) || !t.enabled) return ESP_ERR_INVALID_STATE;
    make_recovery_codes(recovery, t.recovery_hash);
    t.recovery_used = 0;
    esp_err_t err = twofa_put(uid, &t);
    memset(&t, 0, sizeof(t));
    return err;
}

twofa_result_t auth_twofa_verify(uint8_t uid, const char *code, bool *used_recovery)
{
    if (used_recovery) *used_recovery = false;
    settings_twofa_t t;
    if (!twofa_get(uid, &t) || !t.enabled || !code) return TWOFA_NOT_ENABLED;

    /* Recovery-Code? (10 Zeichen aus dem Alphabet, optional mit Bindestrich) */
    char norm[AUTH_RECOVERY_LEN + 1];
    size_t len = strlen(code);
    bool looks_recovery = (len >= 10 && len <= 12 && normalize_recovery(code, norm));
    if (looks_recovery) {
        uint8_t h[32];
        if (!sha256(norm, AUTH_RECOVERY_LEN, h)) return TWOFA_WRONG_CODE;
        for (int i = 0; i < SETTINGS_RECOVERY_COUNT; i++) {
            bool match = auth_const_time_eq(h, t.recovery_hash[i], 32);
            if (match && (t.recovery_used & (1u << i))) return TWOFA_REPLAY;
            if (match) {
                t.recovery_used |= (1u << i);
                twofa_put(uid, &t);
                if (used_recovery) *used_recovery = true;
                event_log(EV_LOGIN, "Recovery-Code verwendet (uid %u), %d verbleibend", uid, recovery_remaining(&t));
                return TWOFA_OK;
            }
        }
        return TWOFA_WRONG_CODE;
    }

    /* TOTP – strenge Variante: nur mit synchroner Uhr */
    if (!time_sync_is_valid()) return TWOFA_TIME_INVALID;
    int64_t step = totp_match(t.secret, code);
    if (step < 0) return TWOFA_WRONG_CODE;
    if (step <= t.last_counter) return TWOFA_REPLAY;
    t.last_counter = step;
    twofa_put(uid, &t);
    return TWOFA_OK;
}
