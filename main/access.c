#include "access.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"
#include "status_led.h"
#include "codes_db.h"
#include "door_relay.h"
#include "keypad_rs232.h"

static const char *TAG = "access";

static uint32_t s_fail_count;
static int64_t  s_lock_until_us;
static uint32_t s_lock_s = CFG_KEYPAD_LOCK_BASE_S;
static int64_t  s_last_locked_log_us;

bool access_keypad_locked(uint32_t *remaining_s)
{
    int64_t now = esp_timer_get_time();
    if (s_lock_until_us > now) {
        if (remaining_s) *remaining_s = (uint32_t)((s_lock_until_us - now) / 1000000) + 1;
        return true;
    }
    return false;
}

uint32_t access_fail_count(void)
{
    return s_fail_count;
}

int access_keypad_door(void)
{
    int d = g_settings.keypad.door;
    return (d >= 0 && d < CFG_MAX_DOORS) ? d : 0;
}

static void on_failure(code_result_t res, size_t code_len)
{
    s_fail_count++;
    /* Den Code selbst nie protokollieren, nur Länge und Grund */
    event_log(EV_DENIED, "Code abgelehnt (%u Zeichen): %s [%" PRIu32 "/%d]", (unsigned)code_len,
              codes_result_text(res), s_fail_count, CFG_KEYPAD_FAIL_LIMIT);
    status_led_flash(LED_DENIED, 800);

    if (s_fail_count >= CFG_KEYPAD_FAIL_LIMIT) {
        s_lock_until_us = esp_timer_get_time() + (int64_t)s_lock_s * 1000000;
        event_log(EV_DENIED, "Keypad gesperrt fuer %" PRIu32 " s", s_lock_s);
        status_led_set(LED_LOCKED);
        s_fail_count = 0;
        if (s_lock_s * 2 <= CFG_KEYPAD_LOCK_MAX_S) s_lock_s *= 2;
    }
}

void access_on_keypad_code(const char *code)
{
    size_t len = strlen(code);
    if (g_settings.keypad.learn_mode) {
        ESP_LOGI(TAG, "Lernmodus: Code mit %u Zeichen erkannt", (unsigned)len);
    }

    uint32_t remaining = 0;
    if (access_keypad_locked(&remaining)) {
        int64_t now = esp_timer_get_time();
        if (now - s_last_locked_log_us > 5 * 1000000) {   /* nicht jede Taste loggen */
            event_log(EV_DENIED, "Eingabe waehrend Sperre ignoriert (%" PRIu32 " s verbleibend)", remaining);
            s_last_locked_log_us = now;
        }
        status_led_flash(LED_LOCKED, 500);
        return;
    }
    if (s_lock_until_us) {
        /* Sperre abgelaufen */
        s_lock_until_us = 0;
        status_led_set(LED_OK);
    }

    int door = access_keypad_door();
    if (!door_is_enabled(door)) {
        event_log(EV_DENIED, "Keypad: zugeordnete Tuer %d ist deaktiviert", door + 1);
        status_led_flash(LED_DENIED, 800);
        return;
    }
    code_entry_t m;
    bool in_grace = false;
    code_result_t res = codes_check_and_use(code, door, &m, &in_grace);
    if (res != CODE_OK) {
        on_failure(res, len);
        return;
    }
    s_fail_count = 0;
    s_lock_s = CFG_KEYPAD_LOCK_BASE_S;
    if (m.type == CODE_TYPE_ONETIME && m.grace_s) {
        /* Einmalcode mit Nutzungsfenster: erste Eingabe startet es, weitere laufen darin */
        event_log(EV_ACCESS, "Zutritt Tuer %d: '%s' (#%" PRIu32 ", onetime) per Keypad, %s noch %" PRIu32 " s",
                  door + 1, m.name, m.id, in_grace ? "erneut im Nutzungsfenster," : "Nutzungsfenster gestartet,",
                  codes_grace_remaining_s(&m));
    } else {
        event_log(EV_ACCESS, "Zutritt Tuer %d: '%s' (#%" PRIu32 ", %s%s) per Keypad", door + 1, m.name, m.id,
                  codes_type_name(m.type), m.max_uses ? (m.uses >= m.max_uses ? ", letzte Nutzung" : "") : "");
    }
    door_open(door, m.name, 0);
}

esp_err_t access_init(void)
{
    esp_err_t err = door_init();
    if (err != ESP_OK) {
        event_log(EV_SYSTEM, "Relais-Init fehlgeschlagen: %s", esp_err_to_name(err));
    }
    err = keypad_start(access_on_keypad_code);
    if (err != ESP_OK) {
        event_log(EV_SYSTEM, "Keypad-Init fehlgeschlagen: %s", esp_err_to_name(err));
    }
    return err;
}
