#include "door_relay.h"

#include <inttypes.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "settings.h"
#include "event_log.h"
#include "status_led.h"

static const char *TAG = "door";

static const gpio_num_t s_relay_gpio[CFG_MAX_DOORS]   = CFG_DOOR_RELAY_GPIOS;
static const gpio_num_t s_contact_gpio[CFG_MAX_DOORS] = CFG_DOOR_CONTACT_GPIOS;
static const gpio_num_t s_rex_gpio[CFG_MAX_DOORS]     = CFG_DOOR_REX_GPIOS;

typedef struct {
    esp_timer_handle_t timer;
    volatile bool relay_active;
    int64_t  last_open_s;
    bool     contact_closed;
    bool     contact_known;
    bool     inputs_configured;
    int      contact_cnt;
    int      rex_cnt;
    bool     rex_pressed;
} door_state_t;

static door_state_t s_doors[CFG_MAX_DOORS];

static bool valid_idx(int idx)
{
    return idx >= 0 && idx < CFG_MAX_DOORS;
}

const char *door_name(int idx)
{
    return valid_idx(idx) ? g_settings.doors[idx].name : "?";
}

bool door_is_enabled(int idx)
{
    return valid_idx(idx) && g_settings.doors[idx].enabled;
}

int door_relay_gpio(int idx)   { return valid_idx(idx) ? (int)s_relay_gpio[idx] : -1; }
int door_contact_gpio(int idx) { return valid_idx(idx) ? (int)s_contact_gpio[idx] : -1; }
int door_rex_gpio(int idx)     { return valid_idx(idx) ? (int)s_rex_gpio[idx] : -1; }

static void relay_write(int idx, bool on)
{
    bool level = g_settings.doors[idx].relay_active_low ? !on : on;
    gpio_set_level(s_relay_gpio[idx], level);
}

bool door_any_relay_active(void)
{
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        if (s_doors[i].relay_active) return true;
    }
    return false;
}

static void release_cb(void *arg)
{
    int idx = (int)(intptr_t)arg;
    relay_write(idx, false);
    s_doors[idx].relay_active = false;
    if (!door_any_relay_active()) {
        status_led_set(LED_OK);
    }
    ESP_LOGI(TAG, "Relais Tuer %d aus", idx + 1);
}

void door_relay_safe_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        mask |= 1ULL << s_relay_gpio[i];
    }
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        gpio_set_level(s_relay_gpio[i], CFG_RELAY_ACTIVE_LOW ? 1 : 0);
    }
}

esp_err_t door_open(int idx, const char *reason, uint32_t pulse_ms)
{
    if (!valid_idx(idx)) return ESP_ERR_INVALID_ARG;
    const settings_door_t *cfg = &g_settings.doors[idx];
    if (!cfg->enabled) return ESP_ERR_INVALID_STATE;
    door_state_t *d = &s_doors[idx];

    if (pulse_ms == 0) pulse_ms = cfg->relay_pulse_ms;
    if (pulse_ms < CFG_RELAY_PULSE_MS_MIN) pulse_ms = CFG_RELAY_PULSE_MS_MIN;
    if (pulse_ms > CFG_RELAY_PULSE_MS_MAX) pulse_ms = CFG_RELAY_PULSE_MS_MAX;

    esp_timer_stop(d->timer);                    /* laufenden Impuls verlängern */
    relay_write(idx, true);
    d->relay_active = true;
    d->last_open_s = esp_timer_get_time() / 1000000;
    status_led_set(LED_DOOR_OPEN);
    esp_err_t err = esp_timer_start_once(d->timer, (uint64_t)pulse_ms * 1000);
    if (err != ESP_OK) {
        /* Sicherheitsnetz: ohne Timer sofort wieder aus */
        release_cb((void *)(intptr_t)idx);
        return err;
    }
    event_log(EV_ACCESS, "Tuer %d '%s' geoeffnet (%s), %" PRIu32 " ms", idx + 1, cfg->name,
              reason ? reason : "?", pulse_ms);
    return ESP_OK;
}

bool door_relay_active(int idx)
{
    return valid_idx(idx) && s_doors[idx].relay_active;
}

bool door_contact_closed(int idx)
{
    if (!valid_idx(idx) || !g_settings.doors[idx].door_contact_enable) return true;
    return s_doors[idx].contact_closed;
}

int64_t door_last_open_uptime_s(int idx)
{
    return valid_idx(idx) ? s_doors[idx].last_open_s : 0;
}

/* Kontakt-/REX-Pins einer Tür einmalig als Eingang mit Pull-up konfigurieren (nur wenn genutzt) */
static void ensure_inputs(int idx)
{
    const settings_door_t *cfg = &g_settings.doors[idx];
    if (s_doors[idx].inputs_configured || !cfg->enabled ||
        (!cfg->door_contact_enable && !cfg->exit_button_enable)) {
        return;
    }
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << s_contact_gpio[idx]) | (1ULL << s_rex_gpio[idx]),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
    s_doors[idx].inputs_configured = true;
    s_doors[idx].contact_closed = true;
    s_doors[idx].contact_known = false;
}

void door_apply_settings(void)
{
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        relay_write(i, s_doors[i].relay_active);
        ensure_inputs(i);
    }
}

/* Eingänge aller aktiven Türen entprellt pollen (20 ms Raster) */
static void input_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        for (int i = 0; i < CFG_MAX_DOORS; i++) {
            const settings_door_t *cfg = &g_settings.doors[i];
            door_state_t *d = &s_doors[i];
            if (!cfg->enabled || !d->inputs_configured) continue;

            if (cfg->door_contact_enable) {
                bool closed = gpio_get_level(s_contact_gpio[i]) == 0;   /* Reed gegen GND */
                if (closed != d->contact_closed) {
                    if (++d->contact_cnt * 20 >= CFG_INPUT_DEBOUNCE_MS) {
                        d->contact_cnt = 0;
                        d->contact_closed = closed;
                        if (d->contact_known) {
                            int64_t since_open = esp_timer_get_time() / 1000000 - d->last_open_s;
                            if (!closed && !d->relay_active && since_open > 15) {
                                event_log(EV_SYSTEM, "ALARM: Tuer %d '%s' ohne Freigabe geoeffnet", i + 1, cfg->name);
                            } else {
                                event_log(EV_SYSTEM, "Tuerkontakt %d '%s': %s", i + 1, cfg->name,
                                          closed ? "geschlossen" : "offen");
                            }
                        }
                        d->contact_known = true;
                    }
                } else {
                    d->contact_cnt = 0;
                    d->contact_known = true;
                }
            }

            if (cfg->exit_button_enable) {
                bool pressed = gpio_get_level(s_rex_gpio[i]) == 0;
                if (pressed != d->rex_pressed) {
                    if (++d->rex_cnt * 20 >= CFG_INPUT_DEBOUNCE_MS) {
                        d->rex_cnt = 0;
                        d->rex_pressed = pressed;
                        if (pressed) {
                            door_open(i, "Austaster", 0);
                        }
                    }
                } else {
                    d->rex_cnt = 0;
                }
            }
        }
    }
}

esp_err_t door_init(void)
{
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        const esp_timer_create_args_t args = {
            .callback = release_cb,
            .arg = (void *)(intptr_t)i,
            .name = "relay",
        };
        esp_err_t err = esp_timer_create(&args, &s_doors[i].timer);
        if (err != ESP_OK) return err;
        /* Relais-Pins sind seit door_relay_safe_init() Ausgänge; jetzt mit NVS-Polarität setzen */
        relay_write(i, false);
        s_doors[i].contact_closed = true;
        ensure_inputs(i);
    }

    xTaskCreate(input_task, "door_in", 3072, NULL, 5, NULL);

    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        const settings_door_t *cfg = &g_settings.doors[i];
        if (!cfg->enabled) {
            ESP_LOGI(TAG, "Tuer %d: deaktiviert (Relais GPIO%d)", i + 1, s_relay_gpio[i]);
            continue;
        }
        ESP_LOGI(TAG, "Tuer %d '%s': Relais GPIO%d (%s), Kontakt GPIO%d %s, REX GPIO%d %s", i + 1, cfg->name,
                 s_relay_gpio[i], cfg->relay_active_low ? "active-low" : "active-high",
                 s_contact_gpio[i], cfg->door_contact_enable ? "aktiv" : "aus",
                 s_rex_gpio[i], cfg->exit_button_enable ? "aktiv" : "aus");
    }
    return ESP_OK;
}
