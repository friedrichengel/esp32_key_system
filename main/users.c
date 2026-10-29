#include "users.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "storage.h"
#include "time_sync.h"
#include "event_log.h"

static const char *TAG = "users";

static user_t           *s_users;
static size_t            s_count;
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ------------------------------------------------------------------------
 * Hilfsfunktionen
 * ---------------------------------------------------------------------- */
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

static void sanitize(user_t *u)
{
    u->role = (u->role == USER_ROLE_ADMIN) ? USER_ROLE_ADMIN : USER_ROLE_USER;
    u->code_types &= USER_TYPES_ALL;
    u->doors &= CFG_ALL_DOORS_MASK;
    u->perms &= USER_PERM_ALL;
    if (u->max_codes > CFG_CODES_MAX) u->max_codes = CFG_CODES_MAX;
}

/* ------------------------------------------------------------------------
 * JSON / Persistenz
 * ---------------------------------------------------------------------- */
cJSON *users_entry_to_json(const user_t *u)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "uid", u->uid);
    cJSON_AddStringToObject(o, "name", u->name);
    cJSON_AddStringToObject(o, "role", u->role == USER_ROLE_ADMIN ? "admin" : "user");
    cJSON_AddBoolToObject(o, "enabled", u->enabled);
    cJSON_AddBoolToObject(o, "must_change_pw", u->must_change_pw);
    cJSON_AddNumberToObject(o, "code_types", u->code_types);
    cJSON_AddNumberToObject(o, "doors", u->doors);
    cJSON_AddBoolToObject(o, "edit_shared", (u->perms & USER_PERM_EDIT_SHARED) != 0);
    cJSON_AddBoolToObject(o, "open_doors", (u->perms & USER_PERM_OPEN_DOORS) != 0);
    cJSON_AddNumberToObject(o, "max_codes", u->max_codes);
    cJSON_AddNumberToObject(o, "created_at", (double)u->created_at);
    cJSON_AddStringToObject(o, "created_by", u->created_by);
    cJSON_AddNumberToObject(o, "last_login", (double)u->last_login);
    cJSON_AddBoolToObject(o, "twofa_enabled", u->twofa.enabled);
    return o;
}

static cJSON *entry_to_file_json(const user_t *u)
{
    cJSON *o = users_entry_to_json(u);
    char hex[2 * SETTINGS_RECOVERY_COUNT * 32 + 1];
    hex_encode(u->pw_hash, sizeof(u->pw_hash), hex);
    cJSON_AddStringToObject(o, "pw_hash", hex);
    hex_encode(u->pw_salt, sizeof(u->pw_salt), hex);
    cJSON_AddStringToObject(o, "pw_salt", hex);
    cJSON_AddNumberToObject(o, "pw_iterations", u->pw_iterations);
    cJSON *t = cJSON_AddObjectToObject(o, "twofa");
    cJSON_AddBoolToObject(t, "enabled", u->twofa.enabled);
    if (u->twofa.enabled) {
        hex_encode(u->twofa.secret, sizeof(u->twofa.secret), hex);
        cJSON_AddStringToObject(t, "secret", hex);
        cJSON *rec = cJSON_AddArrayToObject(t, "recovery");
        for (int i = 0; i < SETTINGS_RECOVERY_COUNT; i++) {
            hex_encode(u->twofa.recovery_hash[i], 32, hex);
            cJSON_AddItemToArray(rec, cJSON_CreateString(hex));
        }
        cJSON_AddNumberToObject(t, "used", u->twofa.recovery_used);
        cJSON_AddNumberToObject(t, "last_counter", (double)u->twofa.last_counter);
    }
    return o;
}

static bool entry_from_file_json(const cJSON *o, user_t *u)
{
    memset(u, 0, sizeof(*u));
    u->uid = (uint8_t)jnum(o, "uid", 0);
    strlcpy(u->name, js(o, "name", ""), sizeof(u->name));
    if (!hex_decode(js(o, "pw_hash", NULL), u->pw_hash, sizeof(u->pw_hash))) return false;
    if (!hex_decode(js(o, "pw_salt", NULL), u->pw_salt, sizeof(u->pw_salt))) return false;
    u->pw_iterations = (uint32_t)jnum(o, "pw_iterations", 0);
    u->role = strcmp(js(o, "role", "user"), "admin") == 0 ? USER_ROLE_ADMIN : USER_ROLE_USER;
    u->enabled = jb(o, "enabled", true);
    u->must_change_pw = jb(o, "must_change_pw", false);
    u->code_types = (uint8_t)jnum(o, "code_types", USER_TYPES_ALL);
    u->doors = (uint32_t)jnum(o, "doors", CFG_ALL_DOORS_MASK);
    u->perms = (jb(o, "edit_shared", false) ? USER_PERM_EDIT_SHARED : 0) |
               (jb(o, "open_doors", false) ? USER_PERM_OPEN_DOORS : 0);
    u->max_codes = (uint16_t)jnum(o, "max_codes", 0);
    u->created_at = (int64_t)jnum(o, "created_at", 0);
    strlcpy(u->created_by, js(o, "created_by", ""), sizeof(u->created_by));
    u->last_login = (int64_t)jnum(o, "last_login", 0);
    cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "twofa");
    if (cJSON_IsObject(t) && jb(t, "enabled", false)) {
        bool ok = hex_decode(js(t, "secret", NULL), u->twofa.secret, sizeof(u->twofa.secret));
        cJSON *rec = cJSON_GetObjectItemCaseSensitive(t, "recovery");
        int n = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, rec) {
            if (n >= SETTINGS_RECOVERY_COUNT) break;
            if (!cJSON_IsString(it) || !hex_decode(it->valuestring, u->twofa.recovery_hash[n], 32)) ok = false;
            n++;
        }
        if (ok && n == SETTINGS_RECOVERY_COUNT) {
            u->twofa.enabled = true;
            u->twofa.recovery_used = (uint8_t)jnum(t, "used", 0);
            u->twofa.last_counter = (int64_t)jnum(t, "last_counter", 0);
        } else {
            memset(&u->twofa, 0, sizeof(u->twofa));
        }
    }
    sanitize(u);
    return u->uid >= 1 && u->uid <= CFG_USERS_MAX && auth_valid_username(u->name) && u->pw_iterations > 0;
}

static esp_err_t save_locked(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s_count; i++) {
        cJSON_AddItemToArray(arr, entry_to_file_json(&s_users[i]));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "users", arr);
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return ESP_ERR_NO_MEM;
    esp_err_t err = storage_write_file(CFG_USERS_PATH, txt, strlen(txt));
    cJSON_free(txt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "users.json speichern fehlgeschlagen");
    }
    return err;
}

static int find_locked(uint8_t uid)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_users[i].uid == uid) return (int)i;
    }
    return -1;
}

static int find_name_locked(const char *name)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcasecmp(s_users[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static void load(void)
{
    size_t len = 0;
    char *txt = storage_read_file(CFG_USERS_PATH, &len);
    if (!txt) {
        ESP_LOGI(TAG, "Keine Benutzerdatei");
        return;
    }
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) {
        ESP_LOGE(TAG, "users.json unlesbar");
        event_log(EV_SYSTEM, "users.json unlesbar, nur Admin-Login moeglich");
        return;
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "users");
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (s_count >= CFG_USERS_MAX) break;
        user_t u;
        if (entry_from_file_json(it, &u) && find_locked(u.uid) < 0 && find_name_locked(u.name) < 0) {
            s_users[s_count++] = u;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%u Benutzer geladen", (unsigned)s_count);
}

esp_err_t users_init(void)
{
    s_users = heap_caps_calloc(CFG_USERS_MAX, sizeof(user_t), MALLOC_CAP_SPIRAM);
    if (!s_users) s_users = calloc(CFG_USERS_MAX, sizeof(user_t));
    if (!s_users) return ESP_ERR_NO_MEM;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    load();
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Lesen
 * ---------------------------------------------------------------------- */
size_t users_count(void)
{
    return s_count;
}

size_t users_list(user_t *out, size_t max)
{
    LOCK();
    size_t n = s_count < max ? s_count : max;
    memcpy(out, s_users, n * sizeof(user_t));
    UNLOCK();
    return n;
}

bool users_get(uint8_t uid, user_t *out)
{
    LOCK();
    int i = find_locked(uid);
    if (i >= 0 && out) *out = s_users[i];
    UNLOCK();
    return i >= 0;
}

bool users_get_by_name(const char *name, user_t *out)
{
    if (!name || !*name) return false;
    LOCK();
    int i = find_name_locked(name);
    if (i >= 0 && out) *out = s_users[i];
    UNLOCK();
    return i >= 0;
}

bool users_name_available(const char *name, int except_uid)
{
    if (!auth_valid_username(name)) return false;
    if (except_uid != AUTH_UID_ADMIN && strcasecmp(name, g_settings.admin.user) == 0) return false;
    LOCK();
    int i = find_name_locked(name);
    bool free_name = (i < 0) || (except_uid > 0 && s_users[i].uid == (uint8_t)except_uid);
    UNLOCK();
    return free_name;
}

/* ------------------------------------------------------------------------
 * Schreiben
 * ---------------------------------------------------------------------- */
static esp_err_t hash_into(user_t *u, const char *password)
{
    if (!password || strlen(password) < AUTH_PW_MIN_LEN) return ESP_ERR_INVALID_ARG;
    esp_fill_random(u->pw_salt, sizeof(u->pw_salt));
    u->pw_iterations = AUTH_PW_ITERATIONS;
    if (auth_hash_password(password, u->pw_salt, sizeof(u->pw_salt), u->pw_iterations,
                           u->pw_hash, sizeof(u->pw_hash)) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t users_create(user_t *u, const char *password)
{
    if (!u || !auth_valid_username(u->name)) return ESP_ERR_INVALID_ARG;
    if (!users_name_available(u->name, -1)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = hash_into(u, password);          /* PBKDF2 außerhalb des Locks */
    if (err != ESP_OK) return err;
    sanitize(u);
    memset(&u->twofa, 0, sizeof(u->twofa));
    u->created_at = time_sync_is_valid() ? time(NULL) : 0;
    u->last_login = 0;

    LOCK();
    if (s_count >= CFG_USERS_MAX) { UNLOCK(); return ESP_ERR_NO_MEM; }
    if (find_name_locked(u->name) >= 0) { UNLOCK(); return ESP_ERR_INVALID_STATE; }
    u->uid = 0;
    for (uint8_t cand = 1; cand <= CFG_USERS_MAX; cand++) {
        if (find_locked(cand) < 0) { u->uid = cand; break; }
    }
    if (!u->uid) { UNLOCK(); return ESP_ERR_NO_MEM; }
    s_users[s_count++] = *u;
    err = save_locked();
    UNLOCK();
    return err;
}

esp_err_t users_update(const user_t *u)
{
    if (!u || !auth_valid_username(u->name)) return ESP_ERR_INVALID_ARG;
    if (!users_name_available(u->name, u->uid)) return ESP_ERR_INVALID_STATE;
    LOCK();
    int i = find_locked(u->uid);
    if (i < 0) { UNLOCK(); return ESP_ERR_NOT_FOUND; }
    user_t *cur = &s_users[i];
    strlcpy(cur->name, u->name, sizeof(cur->name));
    cur->role = u->role;
    cur->enabled = u->enabled;
    cur->must_change_pw = u->must_change_pw;
    cur->code_types = u->code_types;
    cur->doors = u->doors;
    cur->perms = u->perms;
    cur->max_codes = u->max_codes;
    sanitize(cur);
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

esp_err_t users_set_password(uint8_t uid, const char *password, bool must_change)
{
    user_t tmp;
    esp_err_t err = hash_into(&tmp, password);
    if (err != ESP_OK) return err;
    LOCK();
    int i = find_locked(uid);
    if (i < 0) { UNLOCK(); return ESP_ERR_NOT_FOUND; }
    memcpy(s_users[i].pw_hash, tmp.pw_hash, sizeof(tmp.pw_hash));
    memcpy(s_users[i].pw_salt, tmp.pw_salt, sizeof(tmp.pw_salt));
    s_users[i].pw_iterations = tmp.pw_iterations;
    s_users[i].must_change_pw = must_change;
    err = save_locked();
    UNLOCK();
    memset(&tmp, 0, sizeof(tmp));
    return err;
}

esp_err_t users_delete(uint8_t uid)
{
    LOCK();
    int i = find_locked(uid);
    if (i < 0) { UNLOCK(); return ESP_ERR_NOT_FOUND; }
    memmove(&s_users[i], &s_users[i + 1], (s_count - i - 1) * sizeof(user_t));
    s_count--;
    memset(&s_users[s_count], 0, sizeof(user_t));
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

esp_err_t users_clear(void)
{
    LOCK();
    s_count = 0;
    memset(s_users, 0, CFG_USERS_MAX * sizeof(user_t));
    esp_err_t err = storage_remove(CFG_USERS_PATH);
    UNLOCK();
    return err == ESP_OK || err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

/* ------------------------------------------------------------------------
 * Login-Hilfen
 * ---------------------------------------------------------------------- */
bool users_verify_password(const user_t *u, const char *password)
{
    if (!u || !password || u->pw_iterations == 0) return false;
    uint8_t hash[SETTINGS_PW_HASH_LEN];
    if (auth_hash_password(password, u->pw_salt, sizeof(u->pw_salt), u->pw_iterations, hash, sizeof(hash)) != 0) {
        return false;
    }
    return auth_const_time_eq(hash, u->pw_hash, sizeof(hash));
}

void users_touch_login(uint8_t uid)
{
    LOCK();
    int i = find_locked(uid);
    if (i >= 0) {
        s_users[i].last_login = time_sync_is_valid() ? time(NULL) : 0;
        save_locked();
    }
    UNLOCK();
}

bool users_twofa_get(uint8_t uid, settings_twofa_t *t)
{
    LOCK();
    int i = find_locked(uid);
    if (i >= 0) *t = s_users[i].twofa;
    UNLOCK();
    return i >= 0;
}

esp_err_t users_twofa_set(uint8_t uid, const settings_twofa_t *t)
{
    LOCK();
    int i = find_locked(uid);
    if (i < 0) { UNLOCK(); return ESP_ERR_NOT_FOUND; }
    s_users[i].twofa = *t;
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

void users_fill_principal(const user_t *u, auth_principal_t *p)
{
    memset(p, 0, sizeof(*p));
    p->uid = u->uid;
    p->is_admin = (u->role == USER_ROLE_ADMIN);
    strlcpy(p->name, u->name, sizeof(p->name));
    p->must_change_pw = u->must_change_pw;
    if (p->is_admin) {
        p->code_types = USER_TYPES_ALL;
        p->doors = CFG_ALL_DOORS_MASK;
        p->perms = USER_PERM_ALL;
        p->max_codes = 0;
    } else {
        p->code_types = u->code_types;
        p->doors = u->doors;
        p->perms = u->perms;
        p->max_codes = u->max_codes;
    }
}

/* ------------------------------------------------------------------------
 * Backup / Restore
 * ---------------------------------------------------------------------- */
cJSON *users_to_json(bool with_secrets)
{
    cJSON *arr = cJSON_CreateArray();
    LOCK();
    for (size_t i = 0; i < s_count; i++) {
        cJSON_AddItemToArray(arr, with_secrets ? entry_to_file_json(&s_users[i]) : users_entry_to_json(&s_users[i]));
    }
    UNLOCK();
    return arr;
}

esp_err_t users_from_json(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) return ESP_ERR_INVALID_ARG;
    LOCK();
    s_count = 0;
    memset(s_users, 0, CFG_USERS_MAX * sizeof(user_t));
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (s_count >= CFG_USERS_MAX) break;
        user_t u;
        if (!entry_from_file_json(it, &u)) continue;
        if (find_locked(u.uid) >= 0 || find_name_locked(u.name) >= 0) continue;
        if (strcasecmp(u.name, g_settings.admin.user) == 0) continue;
        s_users[s_count++] = u;
    }
    esp_err_t err = save_locked();
    UNLOCK();
    ESP_LOGI(TAG, "%u Benutzer importiert", (unsigned)s_count);
    return err;
}
