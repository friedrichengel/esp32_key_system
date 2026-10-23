#include "door_relay.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"
#include "status_led.h"

static const char *TAG = "door";

static volatile bool     s_relay_active;
static esp_timer_handle_t s_release_timer;
static int64_t           s_last_open_s;
static bool              s_contact_closed = true;
static bool              s_contact_known;

static void relay_write(bool on)
{
    bool level = g_settings.door.relay_active_low ? !on : on;
    gpio_set_level(CFG_RELAY_GPIO, level);
}

static void release_cb(void *arg)
{
    (void)arg;
    relay_write(false);
    s_relay_active = false;
    status_led_set(LED_OK);
    ESP_LOGI(TAG, "Relais aus");
}

esp_err_t door_open(const char *reason, uint32_t pulse_ms)
{
    if (pulse_ms == 0) pulse_ms = g_settings.door.relay_pulse_ms;
    if (pulse_ms < CFG_RELAY_PULSE_MS_MIN) pulse_ms = CFG_RELAY_PULSE_MS_MIN;
    if (pulse_ms > CFG_RELAY_PULSE_MS_MAX) pulse_ms = CFG_RELAY_PULSE_MS_MAX;

    esp_timer_stop(s_release_timer);             /* laufenden Impuls verlängern */
    relay_write(true);
    s_relay_active = true;
    s_last_open_s = esp_timer_get_time() / 1000000;
    status_led_set(LED_DOOR_OPEN);
    esp_err_t err = esp_timer_start_once(s_release_timer, (uint64_t)pulse_ms * 1000);
    if (err != ESP_OK) {
        /* Sicherheitsnetz: ohne Timer sofort wieder aus */
        release_cb(NULL);
        return err;
    }
    event_log(EV_ACCESS, "Tuer geoeffnet (%s), %" PRIu32 " ms", reason ? reason : "?", pulse_ms);
    return ESP_OK;
}

bool door_relay_active(void)
{
    return s_relay_active;
}

bool door_contact_closed(void)
{
    return g_settings.door.door_contact_enable ? s_contact_closed : true;
}

int64_t door_last_open_uptime_s(void)
{
    return s_last_open_s;
}

void door_apply_settings(void)
{
    relay_write(s_relay_active);
}

/* Eingänge entprellt pollen (20 ms Raster) */
static void input_task(void *arg)
{
    (void)arg;
    int contact_cnt = 0, rex_cnt = 0;
    bool rex_pressed = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        if (g_settings.door.door_contact_enable) {
            bool closed = gpio_get_level(CFG_DOOR_CONTACT_GPIO) == 0;   /* Reed gegen GND */
            if (closed != s_contact_closed) {
                if (++contact_cnt * 20 >= CFG_INPUT_DEBOUNCE_MS) {
                    contact_cnt = 0;
                    s_contact_closed = closed;
                    if (s_contact_known) {
                        int64_t since_open = esp_timer_get_time() / 1000000 - s_last_open_s;
                        if (!closed && !s_relay_active && since_open > 15) {
                            event_log(EV_SYSTEM, "ALARM: Tuer ohne Freigabe geoeffnet");
                        } else {
                            event_log(EV_SYSTEM, "Tuerkontakt: %s", closed ? "geschlossen" : "offen");
                        }
                    }
                    s_contact_known = true;
                }
            } else {
                contact_cnt = 0;
                s_contact_known = true;
            }
        }

        if (g_settings.door.exit_button_enable) {
            bool pressed = gpio_get_level(CFG_EXIT_BUTTON_GPIO) == 0;
            if (pressed != rex_pressed) {
                if (++rex_cnt * 20 >= CFG_INPUT_DEBOUNCE_MS) {
                    rex_cnt = 0;
                    rex_pressed = pressed;
                    if (pressed) {
                        door_open("Austaster", 0);
                    }
                }
            } else {
                rex_cnt = 0;
            }
        }
    }
}

esp_err_t door_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << CFG_RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    relay_write(false);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << CFG_DOOR_CONTACT_GPIO) | (1ULL << CFG_EXIT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    const esp_timer_create_args_t args = { .callback = release_cb, .name = "relay" };
    esp_err_t err = esp_timer_create(&args, &s_release_timer);
    if (err != ESP_OK) return err;

    xTaskCreate(input_task, "door_in", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Relais GPIO%d (%s), Kontakt GPIO%d %s, REX GPIO%d %s", CFG_RELAY_GPIO,
             g_settings.door.relay_active_low ? "active-low" : "active-high",
             CFG_DOOR_CONTACT_GPIO, g_settings.door.door_contact_enable ? "aktiv" : "aus",
             CFG_EXIT_BUTTON_GPIO, g_settings.door.exit_button_enable ? "aktiv" : "aus");
    return ESP_OK;
}
