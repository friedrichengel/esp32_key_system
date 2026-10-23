#include "ota_update.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "config.h"
#include "event_log.h"
#include "status_led.h"
#include "net_eth.h"

static const char *TAG = "ota";

static ota_state_t            s_state = OTA_IDLE;
static esp_ota_handle_t       s_handle;
static const esp_partition_t *s_target;
static size_t                 s_written;
static char                   s_err[96];
static bool                   s_confirmed;
static bool                   s_pending_verify;
static esp_timer_handle_t     s_confirm_timer;

static void set_err(const char *msg, esp_err_t err)
{
    snprintf(s_err, sizeof(s_err), "%s%s%s", msg, err != ESP_OK ? ": " : "", err != ESP_OK ? esp_err_to_name(err) : "");
    ESP_LOGE(TAG, "%s", s_err);
}

/* ------------------------------------------------------------------------
 * Rollback-Bestätigung
 * ---------------------------------------------------------------------- */
void ota_confirm_running(void)
{
    if (s_confirmed || !s_pending_verify) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    s_confirmed = true;
    if (err == ESP_OK) {
        event_log(EV_OTA, "Neue Firmware %s bestaetigt, Rollback aufgehoben", esp_app_get_description()->version);
    } else {
        ESP_LOGW(TAG, "mark_app_valid: %s", esp_err_to_name(err));
    }
}

static void confirm_timer_cb(void *arg)
{
    (void)arg;
    /* Nach der Wartezeit läuft das System offensichtlich stabil (Watchdog hätte sonst zugeschlagen). */
    ota_confirm_running();
}

esp_err_t ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        event_log(EV_OTA, "Erster Start nach Update (%s), Bestaetigung in %d s oder bei IP-Bezug",
                  running->label, CFG_OTA_CONFIRM_DELAY_S);
        const esp_timer_create_args_t args = { .callback = confirm_timer_cb, .name = "ota_confirm" };
        if (esp_timer_create(&args, &s_confirm_timer) == ESP_OK) {
            esp_timer_start_once(s_confirm_timer, (uint64_t)CFG_OTA_CONFIRM_DELAY_S * 1000000);
        }
    } else {
        s_confirmed = true;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Update-Ablauf
 * ---------------------------------------------------------------------- */
esp_err_t ota_begin(size_t image_size)
{
    if (s_state == OTA_RECEIVING || s_state == OTA_VERIFYING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_err[0] = '\0';
    s_written = 0;
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!s_target) {
        set_err("Keine OTA-Partition gefunden", ESP_OK);
        s_state = OTA_FAILED;
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size && image_size > s_target->size) {
        set_err("Image groesser als die OTA-Partition", ESP_OK);
        s_state = OTA_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_begin(s_target, image_size ? image_size : OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        set_err("esp_ota_begin", err);
        s_state = OTA_FAILED;
        return err;
    }
    s_state = OTA_RECEIVING;
    status_led_set(LED_OTA);
    event_log(EV_OTA, "Firmware-Upload gestartet -> %s (%u Byte)", s_target->label, (unsigned)image_size);
    return ESP_OK;
}

esp_err_t ota_write(const void *data, size_t len)
{
    if (s_state != OTA_RECEIVING) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        set_err("esp_ota_write", err);
        esp_ota_abort(s_handle);
        s_state = OTA_FAILED;
        status_led_set(LED_OK);
        return err;
    }
    s_written += len;
    return ESP_OK;
}

esp_err_t ota_end(void)
{
    if (s_state != OTA_RECEIVING) return ESP_ERR_INVALID_STATE;
    s_state = OTA_VERIFYING;
    esp_err_t err = esp_ota_end(s_handle);     /* prüft Image-Header, Hash und Signatur */
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            set_err("Image ungueltig oder Signatur falsch (nur mit dem Projektschluessel signierte Images werden angenommen)", ESP_OK);
        } else {
            set_err("esp_ota_end", err);
        }
        s_state = OTA_FAILED;
        status_led_set(LED_OK);
        event_log(EV_OTA, "Firmware-Update abgelehnt: %s", s_err);
        return err;
    }
    esp_app_desc_t desc;
    char ver[32] = "?";
    if (esp_ota_get_partition_description(s_target, &desc) == ESP_OK) {
        strlcpy(ver, desc.version, sizeof(ver));
    }
    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        set_err("esp_ota_set_boot_partition", err);
        s_state = OTA_FAILED;
        status_led_set(LED_OK);
        return err;
    }
    s_state = OTA_DONE;
    event_log(EV_OTA, "Firmware %s verifiziert (%u Byte) -> Boot von %s nach Neustart", ver, (unsigned)s_written, s_target->label);
    return ESP_OK;
}

void ota_abort(void)
{
    if (s_state == OTA_RECEIVING) {
        esp_ota_abort(s_handle);
        event_log(EV_OTA, "Firmware-Upload abgebrochen nach %u Byte", (unsigned)s_written);
    }
    s_state = OTA_IDLE;
    status_led_set(LED_OK);
}

ota_state_t ota_state(void)
{
    return s_state;
}

const char *ota_last_error(void)
{
    return s_err;
}

size_t ota_bytes_written(void)
{
    return s_written;
}

/* ------------------------------------------------------------------------
 * Info / Rollback
 * ---------------------------------------------------------------------- */
static const char *state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static void add_partition(cJSON *o, const char *key, const esp_partition_t *p)
{
    cJSON *po = cJSON_AddObjectToObject(o, key);
    if (!p) return;
    cJSON_AddStringToObject(po, "label", p->label);
    cJSON_AddNumberToObject(po, "address", p->address);
    cJSON_AddNumberToObject(po, "size", p->size);
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
        cJSON_AddStringToObject(po, "state", state_name(st));
    }
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
        cJSON_AddStringToObject(po, "version", d.version);
        cJSON_AddStringToObject(po, "project", d.project_name);
        cJSON_AddStringToObject(po, "built", d.date);
        cJSON_AddStringToObject(po, "idf", d.idf_ver);
    }
}

cJSON *ota_info_json(void)
{
    cJSON *o = cJSON_CreateObject();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    add_partition(o, "running", running);
    add_partition(o, "next", next);
    cJSON_AddBoolToObject(o, "pending_verify", s_pending_verify && !s_confirmed);
    cJSON_AddBoolToObject(o, "rollback_possible", esp_ota_check_rollback_is_possible());
    cJSON_AddNumberToObject(o, "state", s_state);
    cJSON_AddStringToObject(o, "last_error", s_err);
    cJSON_AddNumberToObject(o, "bytes_written", s_written);
    cJSON_AddBoolToObject(o, "signature_required", true);
    return o;
}

esp_err_t ota_rollback(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) return ESP_ERR_NOT_FOUND;
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(next, &d) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;                 /* dort liegt kein Image */
    }
    esp_err_t err = esp_ota_set_boot_partition(next);
    if (err == ESP_OK) {
        event_log(EV_OTA, "Rollback: naechster Start von %s (Firmware %s)", next->label, d.version);
    }
    return err;
}
