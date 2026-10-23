#include "codes_db.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "storage.h"
#include "time_sync.h"
#include "event_log.h"

static const char *TAG = "codes";

static code_entry_t     *s_codes;
static size_t            s_count;
static uint32_t          s_next_id = 1;
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ------------------------------------------------------------------------
 * JSON (de)serialisierung
 * ---------------------------------------------------------------------- */
cJSON *codes_entry_to_json(const code_entry_t *e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", e->id);
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "code", e->code);
    cJSON_AddStringToObject(o, "type", codes_type_name(e->type));
    cJSON_AddBoolToObject(o, "enabled", e->enabled);
    cJSON_AddNumberToObject(o, "valid_from", (double)e->valid_from);
    cJSON_AddNumberToObject(o, "valid_until", (double)e->valid_until);
    cJSON_AddBoolToObject(o, "win_enable", e->win_enable);
    cJSON_AddNumberToObject(o, "weekdays", e->weekdays);
    cJSON_AddNumberToObject(o, "win_from", e->win_from_min);
    cJSON_AddNumberToObject(o, "win_to", e->win_to_min);
    cJSON_AddNumberToObject(o, "max_uses", e->max_uses);
    cJSON_AddNumberToObject(o, "uses", e->uses);
    cJSON_AddNumberToObject(o, "last_used", (double)e->last_used);
    cJSON_AddNumberToObject(o, "created_at", (double)e->created_at);
    cJSON_AddStringToObject(o, "created_by", e->created_by);
    return o;
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

bool codes_entry_from_json(const cJSON *o, code_entry_t *e)
{
    memset(e, 0, sizeof(*e));
    e->id = (uint32_t)jnum(o, "id", 0);
    strlcpy(e->name, js(o, "name", ""), sizeof(e->name));
    strlcpy(e->code, js(o, "code", ""), sizeof(e->code));
    code_type_t t;
    if (!codes_type_from_name(js(o, "type", "permanent"), &t)) return false;
    e->type = t;
    e->enabled = jb(o, "enabled", true);
    e->valid_from = (int64_t)jnum(o, "valid_from", 0);
    e->valid_until = (int64_t)jnum(o, "valid_until", 0);
    e->win_enable = jb(o, "win_enable", false);
    e->weekdays = (uint8_t)jnum(o, "weekdays", 0x7F) & 0x7F;
    e->win_from_min = (uint16_t)jnum(o, "win_from", 0);
    e->win_to_min = (uint16_t)jnum(o, "win_to", 0);
    e->max_uses = (uint32_t)jnum(o, "max_uses", 0);
    e->uses = (uint32_t)jnum(o, "uses", 0);
    e->last_used = (int64_t)jnum(o, "last_used", 0);
    e->created_at = (int64_t)jnum(o, "created_at", 0);
    strlcpy(e->created_by, js(o, "created_by", ""), sizeof(e->created_by));
    if (e->type == CODE_TYPE_ONETIME) e->max_uses = 1;
    if (e->win_from_min > 1439) e->win_from_min = 0;
    if (e->win_to_min > 1440) e->win_to_min = 0;
    return codes_valid_code_string(e->code) && e->name[0];
}

/* ------------------------------------------------------------------------
 * Persistenz
 * ---------------------------------------------------------------------- */
static esp_err_t save_locked(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s_count; i++) {
        cJSON_AddItemToArray(arr, codes_entry_to_json(&s_codes[i]));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next_id", s_next_id);
    cJSON_AddItemToObject(root, "codes", arr);
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return ESP_ERR_NO_MEM;
    esp_err_t err = storage_write_file(CFG_CODES_PATH, txt, strlen(txt));
    cJSON_free(txt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Codes speichern fehlgeschlagen");
    }
    return err;
}

static void load(void)
{
    size_t len = 0;
    char *txt = storage_read_file(CFG_CODES_PATH, &len);
    if (!txt) {
        ESP_LOGI(TAG, "Keine Code-Datei, starte leer");
        return;
    }
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) {
        ESP_LOGE(TAG, "codes.json unlesbar");
        event_log(EV_SYSTEM, "codes.json unlesbar, Codes leer");
        return;
    }
    s_next_id = (uint32_t)jnum(root, "next_id", 1);
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "codes");
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (s_count >= CFG_CODES_MAX) break;
        code_entry_t e;
        if (codes_entry_from_json(it, &e) && e.id) {
            s_codes[s_count++] = e;
            if (e.id >= s_next_id) s_next_id = e.id + 1;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%u Codes geladen", (unsigned)s_count);
}

esp_err_t codes_init(void)
{
    s_codes = heap_caps_calloc(CFG_CODES_MAX, sizeof(code_entry_t), MALLOC_CAP_SPIRAM);
    if (!s_codes) s_codes = calloc(CFG_CODES_MAX, sizeof(code_entry_t));
    if (!s_codes) return ESP_ERR_NO_MEM;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    load();
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * CRUD
 * ---------------------------------------------------------------------- */
size_t codes_count(void)
{
    return s_count;
}

size_t codes_list(code_entry_t *out, size_t max)
{
    LOCK();
    size_t n = s_count < max ? s_count : max;
    memcpy(out, s_codes, n * sizeof(code_entry_t));
    UNLOCK();
    return n;
}

static int find_index_locked(uint32_t id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_codes[i].id == id) return (int)i;
    }
    return -1;
}

static bool code_in_use_locked(const char *code, uint32_t except_id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_codes[i].id != except_id && strcmp(s_codes[i].code, code) == 0) return true;
    }
    return false;
}

bool codes_get(uint32_t id, code_entry_t *out)
{
    LOCK();
    int i = find_index_locked(id);
    if (i >= 0) *out = s_codes[i];
    UNLOCK();
    return i >= 0;
}

esp_err_t codes_add(code_entry_t *e)
{
    if (!codes_valid_code_string(e->code) || !e->name[0]) return ESP_ERR_INVALID_ARG;
    LOCK();
    if (s_count >= CFG_CODES_MAX) { UNLOCK(); return ESP_ERR_NO_MEM; }
    if (code_in_use_locked(e->code, 0)) { UNLOCK(); return ESP_ERR_INVALID_STATE; }
    e->id = s_next_id++;
    e->uses = 0;
    e->last_used = 0;
    e->created_at = time_sync_is_valid() ? time(NULL) : 0;
    if (e->type == CODE_TYPE_ONETIME) e->max_uses = 1;
    s_codes[s_count++] = *e;
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

esp_err_t codes_update(const code_entry_t *e)
{
    if (!codes_valid_code_string(e->code) || !e->name[0]) return ESP_ERR_INVALID_ARG;
    LOCK();
    int i = find_index_locked(e->id);
    if (i < 0) { UNLOCK(); return ESP_ERR_NOT_FOUND; }
    if (code_in_use_locked(e->code, e->id)) { UNLOCK(); return ESP_ERR_INVALID_STATE; }
    code_entry_t upd = *e;
    /* Zähler und Herkunft bleiben erhalten */
    upd.uses = s_codes[i].uses;
    upd.last_used = s_codes[i].last_used;
    upd.created_at = s_codes[i].created_at;
    strlcpy(upd.created_by, s_codes[i].created_by, sizeof(upd.created_by));
    if (upd.type == CODE_TYPE_ONETIME) upd.max_uses = 1;
    s_codes[i] = upd;
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

esp_err_t codes_delete(uint32_t id)
{
    LOCK();
    int i = find_index_locked(id);
    if (i < 0) { UNLOCK(); return ESP_ERR_NOT_FOUND; }
    memmove(&s_codes[i], &s_codes[i + 1], (s_count - i - 1) * sizeof(code_entry_t));
    s_count--;
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

esp_err_t codes_clear(void)
{
    LOCK();
    s_count = 0;
    esp_err_t err = save_locked();
    UNLOCK();
    return err;
}

/* ------------------------------------------------------------------------
 * Prüfung
 * ---------------------------------------------------------------------- */
static code_result_t check_entry(const code_entry_t *e, time_t now, bool time_ok)
{
    if (!e->enabled) return CODE_DISABLED;
    if (e->max_uses && e->uses >= e->max_uses) return CODE_USES_EXHAUSTED;

    bool needs_time = (e->valid_from || e->valid_until || e->win_enable || e->type == CODE_TYPE_TIMED);
    if (needs_time && !time_ok) return CODE_TIME_UNKNOWN;

    if (e->valid_from && now < e->valid_from) return CODE_NOT_YET_VALID;
    if (e->valid_until && now > e->valid_until) return CODE_EXPIRED;

    if (e->win_enable) {
        struct tm tm;
        localtime_r(&now, &tm);
        int wd = (tm.tm_wday + 6) % 7;             /* tm_wday: 0 = Sonntag -> Bit0 = Montag */
        int minute = tm.tm_hour * 60 + tm.tm_min;
        bool day_ok = e->weekdays & (1u << wd);
        bool time_in;
        if (e->win_from_min <= e->win_to_min) {
            time_in = minute >= e->win_from_min && minute < e->win_to_min;
        } else {                                   /* Fenster über Mitternacht */
            time_in = minute >= e->win_from_min || minute < e->win_to_min;
            if (minute < e->win_to_min) {          /* Vortag zählt */
                int prev = (wd + 6) % 7;
                day_ok = e->weekdays & (1u << prev);
            }
        }
        if (!day_ok || !time_in) return CODE_OUTSIDE_WINDOW;
    }
    return CODE_OK;
}

code_result_t codes_check_and_use(const char *code, code_entry_t *matched)
{
    if (!codes_valid_code_string(code)) return CODE_UNKNOWN;
    bool time_ok = time_sync_is_valid();
    time_t now = time(NULL);
    code_result_t res = CODE_UNKNOWN;

    LOCK();
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_codes[i].code, code) != 0) continue;
        res = check_entry(&s_codes[i], now, time_ok);
        if (res == CODE_OK) {
            s_codes[i].uses++;
            s_codes[i].last_used = time_ok ? now : 0;
            save_locked();
        }
        if (matched) *matched = s_codes[i];
        break;
    }
    UNLOCK();
    return res;
}

const char *codes_result_text(code_result_t r)
{
    switch (r) {
    case CODE_OK:             return "ok";
    case CODE_UNKNOWN:        return "unbekannter Code";
    case CODE_DISABLED:       return "Code deaktiviert";
    case CODE_NOT_YET_VALID:  return "Code noch nicht gueltig";
    case CODE_EXPIRED:        return "Code abgelaufen";
    case CODE_OUTSIDE_WINDOW: return "ausserhalb des Zeitfensters";
    case CODE_USES_EXHAUSTED: return "Nutzungen aufgebraucht";
    case CODE_TIME_UNKNOWN:   return "Uhrzeit nicht synchron, Zeitcode abgelehnt";
    }
    return "?";
}

const char *codes_type_name(code_type_t t)
{
    switch (t) {
    case CODE_TYPE_TIMED:   return "timed";
    case CODE_TYPE_ONETIME: return "onetime";
    default:                return "permanent";
    }
}

bool codes_type_from_name(const char *s, code_type_t *out)
{
    if (!s) return false;
    if (strcmp(s, "permanent") == 0) { *out = CODE_TYPE_PERMANENT; return true; }
    if (strcmp(s, "timed") == 0)     { *out = CODE_TYPE_TIMED; return true; }
    if (strcmp(s, "onetime") == 0)   { *out = CODE_TYPE_ONETIME; return true; }
    return false;
}

bool codes_valid_code_string(const char *code)
{
    if (!code) return false;
    size_t n = strlen(code);
    if (n < CFG_CODE_MIN_LEN || n > CFG_KEYPAD_MAX_CODE_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)code[i])) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Backup / Restore
 * ---------------------------------------------------------------------- */
cJSON *codes_to_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    LOCK();
    for (size_t i = 0; i < s_count; i++) {
        cJSON_AddItemToArray(arr, codes_entry_to_json(&s_codes[i]));
    }
    UNLOCK();
    return arr;
}

esp_err_t codes_from_json(const cJSON *arr, bool replace)
{
    if (!cJSON_IsArray(arr)) return ESP_ERR_INVALID_ARG;
    LOCK();
    if (replace) s_count = 0;
    cJSON *it;
    size_t imported = 0;
    cJSON_ArrayForEach(it, arr) {
        if (s_count >= CFG_CODES_MAX) break;
        code_entry_t e;
        if (!codes_entry_from_json(it, &e)) continue;
        if (code_in_use_locked(e.code, 0)) continue;
        if (!e.id || find_index_locked(e.id) >= 0) e.id = s_next_id++;
        if (e.id >= s_next_id) s_next_id = e.id + 1;
        s_codes[s_count++] = e;
        imported++;
    }
    esp_err_t err = save_locked();
    UNLOCK();
    ESP_LOGI(TAG, "%u Codes importiert", (unsigned)imported);
    return err;
}
