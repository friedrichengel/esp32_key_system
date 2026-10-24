/*
 * esp32_key_system – Einstiegspunkt
 *
 * Boot-Reihenfolge:
 *   Relais sicher aus -> NVS -> Settings -> Ereignisprotokoll -> LittleFS -> Status-LED
 *   -> Zeit/TZ -> Ethernet (IPv4/IPv6, mDNS, SNTP folgen per Event)
 *   -> [Phase 2] HTTPS-Server  -> [Phase 4] Keypad, Relais, Codes  -> [Phase 5] OTA-Bestätigung
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "bootloader_random.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"
#include "storage.h"
#include "status_led.h"
#include "time_sync.h"
#include "net_eth.h"
#include "cert_store.h"
#include "auth.h"
#include "web_server.h"
#include "codes_db.h"
#include "access.h"
#include "door_relay.h"
#include "api_tokens.h"
#include "ota_update.h"

static const char *TAG = "main";

static void rng_init(void)
{
    /* Der Bootloader schaltet den TRNG vor dem App-Start ab ("Disabling RNG early entropy
     * source"). Ohne Wi-Fi/BT fuettert auf dem S31 nichts den Zufallsgenerator nach, und
     * esp_random() liefert nur ein zaehlerartiges Muster (auf dem Board gemessen 24.10.2026).
     * Davon haengen Session-Cookies, Zertifikatsschluessel, TOTP-Secrets und API-Token ab.
     * Deshalb die Entropiequelle dauerhaft einschalten. Achtung: nicht kompatibel mit
     * gleichzeitiger ADC-Nutzung; die Firmware verwendet keinen ADC. */
    bootloader_random_enable();
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* BOOT-Taster (GPIO61) CFG_FACTORY_RESET_HOLD_MS lang halten -> Werksreset + Neustart */
static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CFG_PIN_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    uint32_t held_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(CFG_PIN_BOOT_BUTTON) == 0) {
            held_ms += 100;
            if (held_ms == 2000) {
                status_led_set(LED_LOCKED);        /* rot blinkend: weiter halten = Werksreset */
            }
            if (held_ms >= CFG_FACTORY_RESET_HOLD_MS) {
                event_log(EV_SYSTEM, "Werksreset ueber BOOT-Taster");
                settings_factory_reset();
                cert_store_delete();
                codes_clear();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            if (held_ms >= 2000) {
                status_led_set(LED_OK);
            }
            held_ms = 0;
        }
    }
}

static void log_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "esp32_key_system %s (%s %s)", app->version, app->date, app->time);
    ESP_LOGI(TAG, "Chip: %d Kerne, Rev %d, Flash %" PRIu32 " MB, PSRAM %s",
             chip.cores, chip.revision, flash_size / (1024 * 1024),
             (chip.features & CHIP_FEATURE_EMB_PSRAM) ? "embedded" : "extern/keins");
    ESP_LOGI(TAG, "Laufende Partition: %s @ 0x%" PRIx32, running->label, running->address);
}

void app_main(void)
{
    rng_init();                 /* vor allem, was Zufall braucht */
    door_relay_safe_init();     /* alle Relais sicher aus, bevor irgendetwas anderes laeuft */
    nvs_init();
    log_banner();

    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(event_log_init());
    if (storage_init() == ESP_OK) {      /* Fehler wird geloggt, Betrieb geht weiter */
        event_log_enable_file(true);
    }
    status_led_init();
    time_sync_init();

    door_apply_settings();      /* Relais-Polaritaet jetzt aus den Settings (NVS gelesen) */

    event_log(EV_SYSTEM, "Start, Firmware %s, Reset-Grund %d",
              esp_app_get_description()->version, (int)esp_reset_reason());

    esp_err_t err = net_eth_start();
    if (err != ESP_OK) {
        event_log(EV_SYSTEM, "Ethernet-Start fehlgeschlagen: %s", esp_err_to_name(err));
    }

    /* Zutritt: Codes laden, Relais + Keypad starten (unabhängig vom Netz) */
    ESP_ERROR_CHECK(codes_init());
    ESP_ERROR_CHECK(api_tokens_init());
    access_init();

    /* Zertifikat laden/erzeugen (braucht LittleFS), dann HTTPS-Server */
    cert_store_init();
    auth_init();
    web_server_start();

    /* Nach einem Update: laufende Firmware bestätigen (Timer oder sobald IP da ist) */
    ota_init();
    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 3, NULL);

    bool confirmed = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(confirmed ? 30000 : 2000));
        net_status_t st;
        net_eth_get_status(&st);
        if (!confirmed && st.has_ip4) {
            ota_confirm_running();
            confirmed = true;
        }
        if (!confirmed) {
            continue;
        }
        char tbuf[24];
        time_sync_format_local(time(NULL), tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "heartbeat: link=%d %uM ip4=%s ip6=%d zeit=%s(%s) heap=%" PRIu32,
                 st.link_up, st.speed_mbps, st.has_ip4 ? st.ip4 : "-", st.ip6_count,
                 tbuf, time_sync_is_valid() ? "ok" : "ungueltig", esp_get_free_heap_size());
    }
}
