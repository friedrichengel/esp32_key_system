#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"
#include "net_eth.h"

static const char *TAG = "time";

/* Vor diesem Zeitpunkt gilt die Uhr als nicht gestellt (1.1.2025 UTC). */
#define TIME_PLAUSIBLE_MIN 1735689600

static bool   s_valid;
static time_t s_last_sync;
static bool   s_sntp_running;
/* SNTP hält nur Zeiger auf die Servernamen, deshalb eigene Kopien. */
static char   s_srv1[SETTINGS_STR_HOST];
static char   s_srv2[SETTINGS_STR_HOST];

/* Wächter: nach jedem Versuch CFG_NTP_TIMEOUT_S warten, dann Ursache ermitteln */
static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t  s_kick;
static volatile uint32_t  s_attempt_seq;        /* zählt Versuche (Start, Restart, neue IP, manuell) */
static volatile uint32_t  s_sync_count;         /* zählt erfolgreiche Antworten                       */
static volatile bool      s_attempt_pending;
static uint32_t           s_attempt_uptime_s;   /* 0 = noch nie                                       */
static uint32_t           s_last_fail_uptime_s;
static bool               s_last_failed;
static bool               s_force_log;          /* manueller Versuch: Ergebnis immer protokollieren  */
static char               s_last_result[160];

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void set_result(const char *msg, bool failed)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_last_result, msg, sizeof(s_last_result));
    s_last_failed = failed;
    portEXIT_CRITICAL(&s_lock);
}

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
    bool was_failed = s_last_failed;
    s_valid = true;
    s_sync_count++;
    s_attempt_pending = false;
    char buf[24];
    time_sync_format_local(s_last_sync, buf, sizeof(buf));
    char msg[64];
    snprintf(msg, sizeof(msg), "Synchronisiert um %s", buf);
    set_result(msg, false);
    if (!was_valid) {
        event_log(EV_SYSTEM, "Zeit per NTP synchronisiert: %s", buf);
    } else if (was_failed) {
        event_log(EV_SYSTEM, "NTP wieder erreichbar, Zeit synchronisiert: %s", buf);
    } else {
        ESP_LOGI(TAG, "NTP-Sync: %s", buf);
    }
}

/* Versucht, einen Servernamen aufzulösen. Literale IP-Adressen gelten als aufgelöst. */
static bool resolve_server(const char *name)
{
    ip_addr_t lit;
    if (ipaddr_aton(name, &lit)) {
        return true;
    }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(name, "123", &hints, &res);
    if (rc == 0 && res) {
        freeaddrinfo(res);
        return true;
    }
    return false;
}

/* Läuft im Wächter-Task, darf blockieren (DNS-Abfragen). */
static void diagnose(void)
{
    bool force = s_force_log;
    s_force_log = false;

    net_status_t ns;
    net_eth_get_status(&ns);

    char reason[128];
    if (!net_eth_has_ip()) {
        snprintf(reason, sizeof(reason), "keine IPv4-Adresse (Link, DHCP?)");
    } else if (ns.ip4_fallback && !ns.wifi_has_ip4) {
        snprintf(reason, sizeof(reason), "DHCP ohne Antwort, Rueckfalladresse %.15s aktiv, kein Internet", ns.ip4);
    } else if (!ns.wifi_has_ip4 && (!ns.gw4[0] || strcmp(ns.gw4, "0.0.0.0") == 0)) {
        snprintf(reason, sizeof(reason), "kein Standard-Gateway, Server nicht erreichbar");
    } else {
        bool srv1_ok = resolve_server(s_srv1);
        bool srv2_ok = s_srv2[0] ? resolve_server(s_srv2) : false;
        const char *dns = (ns.dns1[0] && strcmp(ns.dns1, "0.0.0.0") != 0) ? ns.dns1 : NULL;
        if (!srv1_ok && !srv2_ok) {
            if (!dns) {
                snprintf(reason, sizeof(reason), "kein DNS-Server bekannt, %.40s nicht aufloesbar", s_srv1);
            } else {
                snprintf(reason, sizeof(reason), "DNS %.15s loest %.40s nicht auf (kein Internet?)", dns, s_srv1);
            }
        } else {
            const char *ok_name = srv1_ok ? s_srv1 : s_srv2;
            snprintf(reason, sizeof(reason), "%.40s aufgeloest, aber keine Antwort in %u s (UDP 123 blockiert?)",
                     ok_name, (unsigned)CFG_NTP_TIMEOUT_S);
        }
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "NTP fehlgeschlagen: %s", reason);
    uint32_t now = uptime_s();
    bool repeat = s_last_failed && strcmp(s_last_result, msg) == 0 &&
                  (now - s_last_fail_uptime_s) < CFG_NTP_REPEAT_LOG_S;
    set_result(msg, true);
    s_last_fail_uptime_s = now;
    s_attempt_pending = false;
    if (force || !repeat) {
        event_log(EV_SYSTEM, "%s", msg);
    } else {
        ESP_LOGW(TAG, "%s (wiederholt, nicht protokolliert)", msg);
    }
}

static void watch_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_kick, portMAX_DELAY);
        uint32_t seq = s_attempt_seq, syncs = s_sync_count;
        vTaskDelay(pdMS_TO_TICKS(CFG_NTP_TIMEOUT_S * 1000));
        if (s_attempt_seq != seq) continue;   /* neuer Versuch läuft, Semaphore ist schon wieder gesetzt */
        if (s_sync_count != syncs) continue;  /* Antwort kam                                            */
        diagnose();
    }
}

static void begin_attempt(bool force_log)
{
    s_attempt_uptime_s = uptime_s();
    s_attempt_pending = true;
    if (force_log) s_force_log = true;
    s_attempt_seq++;
    if (!s_kick) {
        s_kick = xSemaphoreCreateBinary();
        xTaskCreate(watch_task, "ntpwatch", 4096, NULL, 3, NULL);
    }
    xSemaphoreGive(s_kick);
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

static esp_err_t sntp_start_locked(bool force_log)
{
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
        char msg[64];
        snprintf(msg, sizeof(msg), "SNTP-Start fehlgeschlagen: %s", esp_err_to_name(err));
        set_result(msg, true);
        event_log(EV_SYSTEM, "%s", msg);
        return err;
    }
    s_sntp_running = true;
    ESP_LOGI(TAG, "SNTP gestartet (%s%s%s)", s_srv1, s_srv2[0] ? ", " : "", s_srv2);
    begin_attempt(force_log);
    return ESP_OK;
}

esp_err_t time_sync_start(void)
{
    if (s_sntp_running) {
        /* Neue Adresse: nicht auf den Wiederholungs-Timer von lwIP warten, sofort anfragen */
        if (esp_sntp_restart()) {
            begin_attempt(false);
        }
        return ESP_OK;
    }
    return sntp_start_locked(false);
}

esp_err_t time_sync_restart(void)
{
    apply_tz();
    if (s_sntp_running) {
        esp_netif_sntp_deinit();
        s_sntp_running = false;
    }
    return sntp_start_locked(false);
}

esp_err_t time_sync_retry(const char *who)
{
    const char *s1 = g_settings.net.ntp1[0] ? g_settings.net.ntp1 : CFG_NTP_SERVER_1_DEFAULT;
    event_log(EV_SYSTEM, "NTP-Neuversuch durch %s (%.40s%s%.40s)", who ? who : "?", s1,
              g_settings.net.ntp2[0] ? ", " : "", g_settings.net.ntp2);
    apply_tz();
    if (s_sntp_running) {
        esp_netif_sntp_deinit();
        s_sntp_running = false;
    }
    return sntp_start_locked(true);
}

bool time_sync_is_valid(void)
{
    return s_valid;
}

time_t time_sync_last_sync(void)
{
    return s_last_sync;
}

void time_sync_get_status(time_sync_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->running = s_sntp_running;
    st->valid = s_valid;
    st->attempt_pending = s_attempt_pending;
    st->last_sync = s_last_sync;
    st->sync_count = s_sync_count;
    st->last_attempt_uptime_s = s_attempt_uptime_s;
    st->timeout_s = CFG_NTP_TIMEOUT_S;
    strlcpy(st->server1, s_sntp_running ? s_srv1 :
            (g_settings.net.ntp1[0] ? g_settings.net.ntp1 : CFG_NTP_SERVER_1_DEFAULT), sizeof(st->server1));
    strlcpy(st->server2, s_sntp_running ? s_srv2 : g_settings.net.ntp2, sizeof(st->server2));
    portENTER_CRITICAL(&s_lock);
    strlcpy(st->last_result, s_last_result, sizeof(st->last_result));
    st->last_failed = s_last_failed;
    portEXIT_CRITICAL(&s_lock);
}

void time_sync_format_local(time_t t, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}
