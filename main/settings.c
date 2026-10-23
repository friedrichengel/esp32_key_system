#include "settings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config.h"

static const char *TAG = "settings";
static const char *NVS_NS  = "keysys";
static const char *NVS_KEY = "cfg";

settings_t g_settings;

void settings_set_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->version = SETTINGS_VERSION;

    strlcpy(s->net.hostname, CFG_HOSTNAME_DEFAULT, sizeof(s->net.hostname));
    s->net.ipv4_mode = IPV4_MODE_DHCP;
    /* Vorbelegung für "statisch" = dieselbe Adresse wie der DHCP-Rückfall */
    strlcpy(s->net.ipv4_addr, CFG_IPV4_FALLBACK_ADDR, sizeof(s->net.ipv4_addr));
    strlcpy(s->net.ipv4_mask, CFG_IPV4_FALLBACK_MASK, sizeof(s->net.ipv4_mask));
    strlcpy(s->net.ipv4_gw, CFG_IPV4_FALLBACK_GW, sizeof(s->net.ipv4_gw));
    s->net.ipv6_slaac = true;
    s->net.ipv6_static_enable = false;
    s->net.ipv6_prefix_len = 64;
    strlcpy(s->net.ntp1, CFG_NTP_SERVER_1_DEFAULT, sizeof(s->net.ntp1));
    strlcpy(s->net.ntp2, CFG_NTP_SERVER_2_DEFAULT, sizeof(s->net.ntp2));
    strlcpy(s->net.tz, CFG_TZ_DEFAULT, sizeof(s->net.tz));

    s->door.relay_pulse_ms = CFG_RELAY_PULSE_MS_DEFAULT;
    s->door.relay_active_low = CFG_RELAY_ACTIVE_LOW;
    s->door.door_contact_enable = false;
    s->door.exit_button_enable = false;

    s->keypad.baud = CFG_KEYPAD_BAUD_DEFAULT;
    s->keypad.parity = 0;
    s->keypad.frame_mode = KEYPAD_FRAME_TERMINATOR;
    s->keypad.terminator = '#';
    s->keypad.clear_key = '*';
    s->keypad.fixed_len = 6;
    s->keypad.timeout_ms = CFG_KEYPAD_TIMEOUT_MS;
    s->keypad.learn_mode = false;

    strlcpy(s->admin.user, "admin", sizeof(s->admin.user));
    s->admin.pw_iterations = 0;
    s->admin.setup_done = false;
}

/* Platz für spätere Versionssprünge: alte Blobs feldweise übernehmen. */
static void settings_migrate(settings_t *s, uint32_t from_version)
{
    (void)s;
    ESP_LOGW(TAG, "Konfiguration v%" PRIu32 " -> v%d migriert", from_version, SETTINGS_VERSION);
}

esp_err_t settings_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Keine Konfiguration in NVS, Defaults werden angelegt");
        settings_set_defaults(&g_settings);
        return settings_save();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        settings_set_defaults(&g_settings);
        return err;
    }

    settings_t tmp;
    size_t len = sizeof(tmp);
    err = nvs_get_blob(h, NVS_KEY, &tmp, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(tmp) && tmp.version == SETTINGS_VERSION) {
        g_settings = tmp;
        ESP_LOGI(TAG, "Konfiguration geladen (host=%s, ipv4=%s)", g_settings.net.hostname,
                 g_settings.net.ipv4_mode == IPV4_MODE_DHCP ? "dhcp" : "static");
        return ESP_OK;
    }

    settings_set_defaults(&g_settings);
    if (err == ESP_OK) {
        /* Blob vorhanden, aber andere Version/Größe */
        settings_migrate(&g_settings, tmp.version);
    } else {
        ESP_LOGW(TAG, "Konfiguration unlesbar (%s), Defaults", esp_err_to_name(err));
    }
    return settings_save();
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return err;
    }
    g_settings.version = SETTINGS_VERSION;
    err = nvs_set_blob(h, NVS_KEY, &g_settings, sizeof(g_settings));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Speichern fehlgeschlagen: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Konfiguration gespeichert");
    }
    return err;
}

esp_err_t settings_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "Werksreset der Konfiguration: %s", esp_err_to_name(err));
    return err;
}
