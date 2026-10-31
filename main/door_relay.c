#include "door_relay.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
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
    volatile int8_t active_relay;                 /* Poolindex des angezogenen Relais, -1 = keins */
    volatile uint8_t active_dir;                  /* door_dir_t */
    uint8_t  last_dir;
    int64_t  last_open_s;
    bool     contact_closed;
    bool     contact_known;
    bool     inputs_configured;
    int      contact_cnt;
    int      rex_cnt;
    bool     rex_pressed;
} door_state_t;

static door_state_t s_doors[CFG_MAX_DOORS];
/* Besitzer je Relais des Pools (Türindex) oder -1; aus den Settings abgeleitet */
static int8_t s_relay_owner[CFG_MAX_DOORS];

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

bool door_is_gate(int idx)
{
    return valid_idx(idx) && g_settings.door_ext[idx].kind == DOOR_KIND_GATE;
}

/* Relais (Poolindex), das die Tür für die Richtung nutzt; -1 wenn keins */
static int relay_for(int idx, door_dir_t dir)
{
    if (!valid_idx(idx)) return -1;
    const settings_door_ext_t *e = &g_settings.door_ext[idx];
    if (e->kind == DOOR_KIND_GATE) {
        int r = dir == DOOR_DIR_DOWN ? e->relay_down : e->relay_up;
        return valid_idx(r) ? r : -1;
    }
    return dir == DOOR_DIR_UP ? idx : -1;
}

int door_relay_gpio(int idx)      { return valid_idx(idx) ? (int)s_relay_gpio[idx] : -1; }
int door_relay_up_gpio(int idx)   { int r = relay_for(idx, DOOR_DIR_UP);   return r < 0 ? -1 : (int)s_relay_gpio[r]; }
int door_relay_down_gpio(int idx) { int r = relay_for(idx, DOOR_DIR_DOWN); return r < 0 ? -1 : (int)s_relay_gpio[r]; }
int door_contact_gpio(int idx)    { return valid_idx(idx) ? (int)s_contact_gpio[idx] : -1; }
int door_rex_gpio(int idx)        { return valid_idx(idx) ? (int)s_rex_gpio[idx] : -1; }

/* Besitzer der Relais neu bestimmen: niedrigster aktiver Türindex gewinnt */
static void compute_owners(void)
{
    for (int r = 0; r < CFG_MAX_DOORS; r++) s_relay_owner[r] = -1;
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        if (!g_settings.doors[i].enabled) continue;
        int up = relay_for(i, DOOR_DIR_UP), down = relay_for(i, DOOR_DIR_DOWN);
        if (up >= 0 && s_relay_owner[up] < 0) s_relay_owner[up] = (int8_t)i;
        if (down >= 0 && s_relay_owner[down] < 0) s_relay_owner[down] = (int8_t)i;
    }
}

/* Pegel eines Pool-Relais setzen; Polarität vom Besitzer, sonst Compile-Zeit-Default */
static void relay_write(int relay, bool on)
{
    int owner = s_relay_owner[relay];
    bool active_low = owner >= 0 ? g_settings.doors[owner].relay_active_low : CFG_RELAY_ACTIVE_LOW;
    gpio_set_level(s_relay_gpio[relay], active_low ? !on : on);
}

bool door_any_relay_active(void)
{
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        if (s_doors[i].relay_active) return true;
    }
    return false;
}

/* Angezogenes Relais der Tür loslassen (Timer bleibt dem Aufrufer überlassen) */
static void release_door(int idx)
{
    door_state_t *d = &s_doors[idx];
    if (d->active_relay >= 0) relay_write(d->active_relay, false);
    d->active_relay = -1;
    d->active_dir = DOOR_DIR_NONE;
    d->relay_active = false;
}

static void release_cb(void *arg)
{
    int idx = (int)(intptr_t)arg;
    release_door(idx);
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
        s_relay_owner[i] = -1;
        s_doors[i].active_relay = -1;
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

static esp_err_t door_pulse(int idx, door_dir_t dir, const char *reason, uint32_t pulse_ms)
{
    if (!valid_idx(idx)) return ESP_ERR_INVALID_ARG;
    const settings_door_t *cfg = &g_settings.doors[idx];
    if (!cfg->enabled) return ESP_ERR_INVALID_STATE;
    bool gate = door_is_gate(idx);
    if (dir == DOOR_DIR_DOWN && !gate) return ESP_ERR_NOT_SUPPORTED;
    int relay = relay_for(idx, dir);
    if (relay < 0 || s_relay_owner[relay] != idx) {
        event_log(EV_SYSTEM, "Tuer %d '%s': Relais %d nicht verfuegbar (Konfiguration pruefen)", idx + 1,
                  cfg->name, relay + 1);
        return ESP_ERR_INVALID_STATE;
    }
    door_state_t *d = &s_doors[idx];

    if (pulse_ms == 0) pulse_ms = cfg->relay_pulse_ms;
    if (pulse_ms < CFG_RELAY_PULSE_MS_MIN) pulse_ms = CFG_RELAY_PULSE_MS_MIN;
    if (pulse_ms > CFG_RELAY_PULSE_MS_MAX) pulse_ms = CFG_RELAY_PULSE_MS_MAX;

    esp_timer_stop(d->timer);                    /* laufenden Impuls verlängern bzw. ersetzen */
    if (d->active_relay >= 0 && d->active_relay != relay) {
        release_door(idx);                       /* Verriegelung: nie Auf und Ab zugleich */
    }
    relay_write(relay, true);
    d->active_relay = (int8_t)relay;
    d->active_dir = (uint8_t)dir;
    d->last_dir = (uint8_t)dir;
    d->relay_active = true;
    d->last_open_s = esp_timer_get_time() / 1000000;
    status_led_set(LED_DOOR_OPEN);
    esp_err_t err = esp_timer_start_once(d->timer, (uint64_t)pulse_ms * 1000);
    if (err != ESP_OK) {
        /* Sicherheitsnetz: ohne Timer sofort wieder aus */
        release_cb((void *)(intptr_t)idx);
        return err;
    }
    if (gate) {
        event_log(EV_ACCESS, "Rolltor %d '%s' faehrt %s (%s), Relais %d, %" PRIu32 " ms", idx + 1, cfg->name,
                  dir == DOOR_DIR_DOWN ? "ab" : "auf", reason ? reason : "?", relay + 1, pulse_ms);
    } else {
        event_log(EV_ACCESS, "Tuer %d '%s' geoeffnet (%s), %" PRIu32 " ms", idx + 1, cfg->name,
                  reason ? reason : "?", pulse_ms);
    }
    return ESP_OK;
}

esp_err_t door_open(int idx, const char *reason, uint32_t pulse_ms)
{
    return door_pulse(idx, DOOR_DIR_UP, reason, pulse_ms);
}

esp_err_t door_close(int idx, const char *reason, uint32_t pulse_ms)
{
    return door_pulse(idx, DOOR_DIR_DOWN, reason, pulse_ms);
}

bool door_relay_active(int idx)
{
    return valid_idx(idx) && s_doors[idx].relay_active;
}

door_dir_t door_active_dir(int idx)
{
    return valid_idx(idx) && s_doors[idx].relay_active ? (door_dir_t)s_doors[idx].active_dir : DOOR_DIR_NONE;
}

door_dir_t door_last_dir(int idx)
{
    return valid_idx(idx) ? (door_dir_t)s_doors[idx].last_dir : DOOR_DIR_NONE;
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

const char *door_settings_check(const settings_door_t doors[CFG_MAX_DOORS],
                                const settings_door_ext_t ext[CFG_MAX_DOORS], char *err, size_t err_len)
{
    int owner[CFG_MAX_DOORS];
    for (int r = 0; r < CFG_MAX_DOORS; r++) owner[r] = -1;
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        int used[2] = { i, -1 };
        if (ext[i].kind == DOOR_KIND_GATE) {
            if (ext[i].relay_up >= CFG_MAX_DOORS || ext[i].relay_down >= CFG_MAX_DOORS) {
                snprintf(err, err_len, "Tuer %d: Relais fuer Auf/Ab muss 1..%d sein", i + 1, CFG_MAX_DOORS);
                return err;
            }
            if (ext[i].relay_up == ext[i].relay_down) {
                snprintf(err, err_len, "Tuer %d: Rolltor braucht zwei verschiedene Relais fuer Auf und Ab", i + 1);
                return err;
            }
            used[0] = ext[i].relay_up;
            used[1] = ext[i].relay_down;
        } else if (ext[i].kind != DOOR_KIND_OPENER) {
            snprintf(err, err_len, "Tuer %d: unbekannter Tuertyp", i + 1);
            return err;
        }
        if (!doors[i].enabled) continue;
        for (int k = 0; k < 2; k++) {
            int r = used[k];
            if (r < 0) continue;
            if (owner[r] >= 0) {
                snprintf(err, err_len, "Relais %d wird von Tuer %d und Tuer %d genutzt; eine davon deaktivieren oder "
                         "ein anderes Relais waehlen", r + 1, owner[r] + 1, i + 1);
                return err;
            }
            owner[r] = i;
        }
    }
    return NULL;
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
    /* Laufende Impulse beenden, dann Besitzer und Polarität neu setzen.
     * Wird in main.c auch vor door_init() aufgerufen, dann gibt es noch keine Timer. */
    bool was_active = door_any_relay_active();
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        if (s_doors[i].timer) esp_timer_stop(s_doors[i].timer);
        release_door(i);
    }
    compute_owners();
    for (int r = 0; r < CFG_MAX_DOORS; r++) {
        relay_write(r, false);
    }
    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        ensure_inputs(i);
    }
    if (was_active) status_led_set(LED_OK);
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
            const char *what = door_is_gate(i) ? "Rolltor" : "Tuer";

            if (cfg->door_contact_enable) {
                bool closed = gpio_get_level(s_contact_gpio[i]) == 0;   /* Reed gegen GND */
                if (closed != d->contact_closed) {
                    if (++d->contact_cnt * 20 >= CFG_INPUT_DEBOUNCE_MS) {
                        d->contact_cnt = 0;
                        d->contact_closed = closed;
                        if (d->contact_known) {
                            int64_t since_open = esp_timer_get_time() / 1000000 - d->last_open_s;
                            if (!closed && !d->relay_active && since_open > 15) {
                                event_log(EV_SYSTEM, "ALARM: %s %d '%s' ohne Freigabe geoeffnet", what, i + 1, cfg->name);
                            } else {
                                event_log(EV_SYSTEM, "Kontakt %s %d '%s': %s", what, i + 1, cfg->name,
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
        s_doors[i].active_relay = -1;
        s_doors[i].contact_closed = true;
    }
    /* Relais-Pins sind seit door_relay_safe_init() Ausgänge; jetzt Besitzer und NVS-Polarität setzen */
    char err[128];
    if (door_settings_check(g_settings.doors, g_settings.door_ext, err, sizeof(err))) {
        ESP_LOGW(TAG, "Tuerkonfiguration: %s", err);
        event_log(EV_SYSTEM, "Tuerkonfiguration fehlerhaft: %s", err);
    }
    compute_owners();
    for (int r = 0; r < CFG_MAX_DOORS; r++) relay_write(r, false);
    for (int i = 0; i < CFG_MAX_DOORS; i++) ensure_inputs(i);

    xTaskCreate(input_task, "door_in", 3072, NULL, 5, NULL);

    for (int i = 0; i < CFG_MAX_DOORS; i++) {
        const settings_door_t *cfg = &g_settings.doors[i];
        if (!cfg->enabled) {
            ESP_LOGI(TAG, "Tuer %d: deaktiviert (Relais GPIO%d)", i + 1, s_relay_gpio[i]);
            continue;
        }
        if (door_is_gate(i)) {
            ESP_LOGI(TAG, "Rolltor %d '%s': Auf Relais %d GPIO%d, Ab Relais %d GPIO%d (%s), Kontakt GPIO%d %s, REX GPIO%d %s",
                     i + 1, cfg->name, g_settings.door_ext[i].relay_up + 1, door_relay_up_gpio(i),
                     g_settings.door_ext[i].relay_down + 1, door_relay_down_gpio(i),
                     cfg->relay_active_low ? "active-low" : "active-high",
                     s_contact_gpio[i], cfg->door_contact_enable ? "aktiv" : "aus",
                     s_rex_gpio[i], cfg->exit_button_enable ? "aktiv" : "aus");
        } else {
            ESP_LOGI(TAG, "Tuer %d '%s': Relais GPIO%d (%s), Kontakt GPIO%d %s, REX GPIO%d %s", i + 1, cfg->name,
                     s_relay_gpio[i], cfg->relay_active_low ? "active-low" : "active-high",
                     s_contact_gpio[i], cfg->door_contact_enable ? "aktiv" : "aus",
                     s_rex_gpio[i], cfg->exit_button_enable ? "aktiv" : "aus");
        }
    }
    return ESP_OK;
}
