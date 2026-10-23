#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"

static const char *TAG = "time";

/* Vor diesem Zeitpunkt gilt die Uhr als nicht gestellt (1.1.2025 UTC). */
#define TIME_PLAUSIBLE_MIN 1735689600

static bool   s_valid;
static time_t s_last_sync;
static bool   s_sntp_running;
/* SNTP hält nur Zeiger auf die Servernamen, deshalb eigene Kopien. */
static char   s_srv1[SETTINGS_STR_HOST];
static char   s_srv2[SETTINGS_STR_HOST];

static void apply_tz(void)
{
    const char *tz = g_settings.net.tz[0] ? g_settings.net.tz : CFG_TZ_DEFAULT;
    setenv("TZ", tz, 1);
    tzset();
}

static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_last_sync = time(NULL);
    bool was_valid = s_valid;
    s_valid = true;
    char buf[24];
    time_sync_format_local(s_last_sync, buf, sizeof(buf));
    if (!was_valid) {
        event_log(EV_SYSTEM, "Zeit per NTP synchronisiert: %s", buf);
    } else {
        ESP_LOGI(TAG, "NTP-Sync: %s", buf);
    }
}

esp_err_t time_sync_init(void)
{
    apply_tz();
    /* Nach einem Software-Reset läuft die RTC weiter; dann ist die Zeit schon brauchbar. */
    time_t now = time(NULL);
    s_valid = (now > TIME_PLAUSIBLE_MIN);
    ESP_LOGI(TAG, "Uhr beim Start %s", s_valid ? "plausibel (RTC)" : "nicht gestellt");
    return ESP_OK;
}

esp_err_t time_sync_start(void)
{
    if (s_sntp_running) {
        return ESP_OK;
    }
    strlcpy(s_srv1, g_settings.net.ntp1[0] ? g_settings.net.ntp1 : CFG_NTP_SERVER_1_DEFAULT, sizeof(s_srv1));
    strlcpy(s_srv2, g_settings.net.ntp2, sizeof(s_srv2));

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        s_srv2[0] ? 2 : 1, ESP_SNTP_SERVER_LIST(s_srv1, s_srv2));
    cfg.wait_for_sync = false;
    cfg.start = true;
    cfg.sync_cb = on_sync;
    cfg.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init: %s", esp_err_to_name(err));
        return err;
    }
    s_sntp_running = true;
    ESP_LOGI(TAG, "SNTP gestartet (%s%s%s)", s_srv1, s_srv2[0] ? ", " : "", s_srv2);
    return ESP_OK;
}

esp_err_t time_sync_restart(void)
{
    apply_tz();
    if (s_sntp_running) {
        esp_netif_sntp_deinit();
        s_sntp_running = false;
    }
    return time_sync_start();
}

bool time_sync_is_valid(void)
{
    return s_valid;
}

time_t time_sync_last_sync(void)
{
    return s_last_sync;
}

void time_sync_format_local(time_t t, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}
