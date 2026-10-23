#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "cJSON.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"
#include "auth.h"
#include "cert_store.h"
#include "net_eth.h"
#include "time_sync.h"
#include "storage.h"
#include "codes_db.h"
#include "door_relay.h"
#include "keypad_rs232.h"
#include "access.h"
#include "ota_update.h"

/* Eingebettete Web-UI (gzip), erzeugt von tools/embed_web_assets.py */
#include "web_assets_gen.h"

static const char *TAG = "web";

static httpd_handle_t s_https;
static httpd_handle_t s_redirect;
static bool           s_tls;

#define MAX_JSON_BODY   8192
#define MAX_PEM_BODY    16384
#define MAX_BACKUP_BODY 65536

/* ========================================================================
 * Hilfsfunktionen
 * ====================================================================== */
static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!txt) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free(txt);
    return r;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t send_ok_reboot(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "reboot_required", true);
    return send_json(req, o);
}

/* Body komplett lesen; Rückgabe nullterminiert, free() durch Aufrufer. */
static char *read_body(httpd_req_t *req, size_t max_len, size_t *len_out)
{
    size_t total = req->content_len;
    if (total == 0 || total > max_len) {
        return NULL;
    }
    char *buf = malloc(total + 1);
    if (!buf) {
        return NULL;
    }
    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[total] = '\0';
    if (len_out) *len_out = total;
    return buf;
}

static cJSON *read_json_max(httpd_req_t *req, size_t max_len)
{
    char *body = read_body(req, max_len, NULL);
    if (!body) {
        return NULL;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    return j;
}

static cJSON *read_json(httpd_req_t *req)
{
    return read_json_max(req, MAX_JSON_BODY);
}

static const char *jstr(const cJSON *o, const char *key, const char *dflt)
{
    cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return (cJSON_IsString(i) && i->valuestring) ? i->valuestring : dflt;
}

static int jint(const cJSON *o, const char *key, int dflt)
{
    cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(i) ? i->valueint : dflt;
}

static bool jbool(const cJSON *o, const char *key, bool dflt)
{
    cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(i) ? cJSON_IsTrue(i) : dflt;
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    strlcpy(dst, src ? src : "", dst_len);
}

/* Letztes Pfadsegment als Zahl (für /api/codes/<id>), Query wird ignoriert */
static uint32_t uri_last_id(const httpd_req_t *req)
{
    char path[CONFIG_HTTPD_MAX_URI_LEN + 1];
    strlcpy(path, req->uri, sizeof(path));
    char *q = strchr(path, '?');
    if (q) *q = '\0';
    char *slash = strrchr(path, '/');
    return slash ? (uint32_t)strtoul(slash + 1, NULL, 10) : 0;
}

/* ------------------------------------------------------------------------
 * Auth
 * ---------------------------------------------------------------------- */
static auth_session_t *get_session(httpd_req_t *req)
{
    char token[AUTH_TOKEN_HEX_LEN + 1];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, AUTH_COOKIE_NAME, token, &len) != ESP_OK) {
        return NULL;
    }
    return auth_session_find(token);
}

static auth_session_t *require_auth(httpd_req_t *req)
{
    auth_session_t *s = get_session(req);
    if (!s) {
        send_error(req, "401 Unauthorized", "Nicht angemeldet");
    }
    return s;
}

static auth_session_t *require_auth_csrf(httpd_req_t *req)
{
    auth_session_t *s = require_auth(req);
    if (!s) {
        return NULL;
    }
    char csrf[AUTH_CSRF_HEX_LEN + 1] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", csrf, sizeof(csrf)) != ESP_OK ||
        strcmp(csrf, s->csrf) != 0) {
        send_error(req, "403 Forbidden", "CSRF-Token fehlt oder ungueltig");
        return NULL;
    }
    return s;
}

static void set_session_cookie(httpd_req_t *req, char *hdrbuf, size_t hdrlen, const char *token)
{
    if (token) {
        snprintf(hdrbuf, hdrlen, "%s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d%s",
                 AUTH_COOKIE_NAME, token, CFG_SESSION_TIMEOUT_S, s_tls ? "; Secure" : "");
    } else {
        snprintf(hdrbuf, hdrlen, "%s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0", AUTH_COOKIE_NAME);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", hdrbuf);
}

/* ========================================================================
 * Settings <-> JSON (für Netzwerk-/System-Seite und Backup)
 * ====================================================================== */
static cJSON *net_to_json(const settings_network_t *n)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "hostname", n->hostname);
    cJSON_AddStringToObject(o, "ipv4_mode", n->ipv4_mode == IPV4_MODE_STATIC ? "static" : "dhcp");
    cJSON_AddStringToObject(o, "ipv4_addr", n->ipv4_addr);
    cJSON_AddStringToObject(o, "ipv4_mask", n->ipv4_mask);
    cJSON_AddStringToObject(o, "ipv4_gw", n->ipv4_gw);
    cJSON_AddStringToObject(o, "dns1", n->dns1);
    cJSON_AddStringToObject(o, "dns2", n->dns2);
    cJSON_AddBoolToObject(o, "ipv6_slaac", n->ipv6_slaac);
    cJSON_AddBoolToObject(o, "ipv6_static_enable", n->ipv6_static_enable);
    cJSON_AddStringToObject(o, "ipv6_addr", n->ipv6_addr);
    cJSON_AddNumberToObject(o, "ipv6_prefix_len", n->ipv6_prefix_len);
    cJSON_AddStringToObject(o, "ntp1", n->ntp1);
    cJSON_AddStringToObject(o, "ntp2", n->ntp2);
    cJSON_AddStringToObject(o, "tz", n->tz);
    return o;
}

static bool valid_hostname(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 63 || h[0] == '-' || h[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)h[i]) && h[i] != '-') return false;
    }
    return true;
}

static bool valid_ip4_or_empty(const char *s, bool allow_empty)
{
    if (!s || !*s) return allow_empty;
    esp_ip4_addr_t a;
    return esp_netif_str_to_ip4(s, &a) == ESP_OK;
}

/* Liest Netzwerkfelder aus JSON in *n (Ausgangswert = aktuelle Settings). Fehlertext oder NULL. */
static const char *net_from_json(const cJSON *j, settings_network_t *n)
{
    copy_str(n->hostname, sizeof(n->hostname), jstr(j, "hostname", n->hostname));
    n->ipv4_mode = strcmp(jstr(j, "ipv4_mode", n->ipv4_mode == IPV4_MODE_STATIC ? "static" : "dhcp"), "static") == 0
                   ? IPV4_MODE_STATIC : IPV4_MODE_DHCP;
    copy_str(n->ipv4_addr, sizeof(n->ipv4_addr), jstr(j, "ipv4_addr", n->ipv4_addr));
    copy_str(n->ipv4_mask, sizeof(n->ipv4_mask), jstr(j, "ipv4_mask", n->ipv4_mask));
    copy_str(n->ipv4_gw, sizeof(n->ipv4_gw), jstr(j, "ipv4_gw", n->ipv4_gw));
    copy_str(n->dns1, sizeof(n->dns1), jstr(j, "dns1", n->dns1));
    copy_str(n->dns2, sizeof(n->dns2), jstr(j, "dns2", n->dns2));
    n->ipv6_slaac = jbool(j, "ipv6_slaac", n->ipv6_slaac);
    n->ipv6_static_enable = jbool(j, "ipv6_static_enable", n->ipv6_static_enable);
    copy_str(n->ipv6_addr, sizeof(n->ipv6_addr), jstr(j, "ipv6_addr", n->ipv6_addr));
    n->ipv6_prefix_len = (uint8_t)jint(j, "ipv6_prefix_len", n->ipv6_prefix_len);
    copy_str(n->ntp1, sizeof(n->ntp1), jstr(j, "ntp1", n->ntp1));
    copy_str(n->ntp2, sizeof(n->ntp2), jstr(j, "ntp2", n->ntp2));
    copy_str(n->tz, sizeof(n->tz), jstr(j, "tz", n->tz));

    if (!valid_hostname(n->hostname)) return "Hostname ungueltig (a-z, 0-9, '-', max. 63)";
    if (n->ipv4_mode == IPV4_MODE_STATIC &&
        (!valid_ip4_or_empty(n->ipv4_addr, false) || !valid_ip4_or_empty(n->ipv4_mask, false) ||
         !valid_ip4_or_empty(n->ipv4_gw, true))) {
        return "Statische IPv4-Adresse, Maske oder Gateway ungueltig";
    }
    if (!valid_ip4_or_empty(n->dns1, true) || !valid_ip4_or_empty(n->dns2, true)) return "DNS-Server ungueltig";
    if (n->ipv6_static_enable) {
        esp_ip6_addr_t a;
        if (esp_netif_str_to_ip6(n->ipv6_addr, &a) != ESP_OK || n->ipv6_prefix_len < 1 || n->ipv6_prefix_len > 128) {
            return "Statische IPv6-Adresse oder Praefix ungueltig";
        }
    }
    if (!n->ntp1[0]) copy_str(n->ntp1, sizeof(n->ntp1), CFG_NTP_SERVER_1_DEFAULT);
    if (!n->tz[0]) copy_str(n->tz, sizeof(n->tz), CFG_TZ_DEFAULT);
    return NULL;
}

static cJSON *door_to_json(const settings_door_t *d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "relay_pulse_ms", d->relay_pulse_ms);
    cJSON_AddBoolToObject(o, "relay_active_low", d->relay_active_low);
    cJSON_AddBoolToObject(o, "door_contact_enable", d->door_contact_enable);
    cJSON_AddBoolToObject(o, "exit_button_enable", d->exit_button_enable);
    return o;
}

static void door_from_json(const cJSON *d, settings_door_t *s)
{
    int pulse = jint(d, "relay_pulse_ms", s->relay_pulse_ms);
    if (pulse < CFG_RELAY_PULSE_MS_MIN) pulse = CFG_RELAY_PULSE_MS_MIN;
    if (pulse > CFG_RELAY_PULSE_MS_MAX) pulse = CFG_RELAY_PULSE_MS_MAX;
    s->relay_pulse_ms = (uint16_t)pulse;
    s->relay_active_low = jbool(d, "relay_active_low", s->relay_active_low);
    s->door_contact_enable = jbool(d, "door_contact_enable", s->door_contact_enable);
    s->exit_button_enable = jbool(d, "exit_button_enable", s->exit_button_enable);
}

static const char *ctrl_char_name(char c, char *buf)
{
    if (c == '\r') return "CR";
    if (c == '\n') return "LF";
    if (c == 0) return "none";
    buf[0] = c; buf[1] = 0;
    return buf;
}

static char parse_ctrl_char(const char *s, char dflt)
{
    if (!s) return dflt;
    if (strcmp(s, "CR") == 0 || strcmp(s, "\\r") == 0) return '\r';
    if (strcmp(s, "LF") == 0 || strcmp(s, "\\n") == 0) return '\n';
    if (strcmp(s, "none") == 0 || !*s) return 0;
    return s[0];
}

static cJSON *keypad_to_json(const settings_keypad_t *k)
{
    char t[2], c[2];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "baud", k->baud);
    cJSON_AddNumberToObject(o, "parity", k->parity);
    cJSON_AddNumberToObject(o, "frame_mode", k->frame_mode);
    cJSON_AddStringToObject(o, "terminator", ctrl_char_name(k->terminator, t));
    cJSON_AddStringToObject(o, "clear_key", ctrl_char_name(k->clear_key, c));
    cJSON_AddNumberToObject(o, "fixed_len", k->fixed_len);
    cJSON_AddNumberToObject(o, "timeout_ms", k->timeout_ms);
    cJSON_AddBoolToObject(o, "learn_mode", k->learn_mode);
    return o;
}

static void keypad_from_json(const cJSON *k, settings_keypad_t *s)
{
    int baud = jint(k, "baud", s->baud);
    if (baud >= 1200 && baud <= 115200) s->baud = baud;
    int par = jint(k, "parity", s->parity);
    if (par >= 0 && par <= 2) s->parity = par;
    int fm = jint(k, "frame_mode", s->frame_mode);
    if (fm >= 0 && fm <= 2) s->frame_mode = fm;
    s->terminator = parse_ctrl_char(jstr(k, "terminator", NULL), s->terminator);
    s->clear_key = parse_ctrl_char(jstr(k, "clear_key", NULL), s->clear_key);
    int fl = jint(k, "fixed_len", s->fixed_len);
    if (fl >= CFG_CODE_MIN_LEN && fl <= CFG_KEYPAD_MAX_CODE_LEN) s->fixed_len = fl;
    int to = jint(k, "timeout_ms", s->timeout_ms);
    if (to >= 200 && to <= 10000) s->timeout_ms = to;
    s->learn_mode = jbool(k, "learn_mode", s->learn_mode);
}

/* ========================================================================
 * Statische Dateien
 * ====================================================================== */
static esp_err_t send_static(httpd_req_t *req, const char *type, const unsigned char *data, size_t len)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, (const char *)data, len);
}

static esp_err_t h_index(httpd_req_t *req)
{
    return send_static(req, "text/html; charset=utf-8", web_index_html_gz, web_index_html_gz_len);
}
static esp_err_t h_app_js(httpd_req_t *req)
{
    return send_static(req, "application/javascript; charset=utf-8", web_app_js_gz, web_app_js_gz_len);
}
static esp_err_t h_style_css(httpd_req_t *req)
{
    return send_static(req, "text/css; charset=utf-8", web_style_css_gz, web_style_css_gz_len);
}
static esp_err_t h_qrcode_js(httpd_req_t *req)
{
    return send_static(req, "application/javascript; charset=utf-8", web_qrcode_js_gz, web_qrcode_js_gz_len);
}

/* ========================================================================
 * API: Session / Login / Setup
 * ====================================================================== */
static esp_err_t h_session_get(httpd_req_t *req)
{
    auth_session_t *s = get_session(req);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "authenticated", s != NULL);
    cJSON_AddBoolToObject(o, "setup_required", auth_setup_required());
    cJSON_AddBoolToObject(o, "tls", s_tls);
    cJSON_AddStringToObject(o, "hostname", g_settings.net.hostname);
    cJSON_AddBoolToObject(o, "twofa_enabled", auth_twofa_enabled());
    if (s) {
        cJSON_AddStringToObject(o, "user", s->user);
        cJSON_AddStringToObject(o, "csrf", s->csrf);
    }
    return send_json(req, o);
}

static esp_err_t finish_login(httpd_req_t *req, const char *user)
{
    auth_session_t *s = auth_session_create(user);
    char cookie[160];
    set_session_cookie(req, cookie, sizeof(cookie), s->token);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "user", s->user);
    cJSON_AddStringToObject(o, "csrf", s->csrf);
    return send_json(req, o);
}

static esp_err_t h_setup_post(httpd_req_t *req)
{
    if (!auth_setup_required()) {
        return send_error(req, "409 Conflict", "Setup bereits abgeschlossen");
    }
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    const char *user = jstr(j, "user", "admin");
    const char *pw = jstr(j, "password", "");
    esp_err_t err = auth_set_password(user, pw);
    cJSON_Delete(j);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "Passwort muss mindestens 8 Zeichen haben");
    }
    event_log(EV_LOGIN, "Ersteinrichtung abgeschlossen, Admin '%s'", g_settings.admin.user);
    return finish_login(req, g_settings.admin.user);
}

static esp_err_t h_login_post(httpd_req_t *req)
{
    uint32_t retry = 0;
    if (!auth_login_allowed(&retry)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Gesperrt, erneut in %" PRIu32 " s", retry);
        return send_error(req, "429 Too Many Requests", msg);
    }
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    const char *user = jstr(j, "user", "");
    const char *pw = jstr(j, "password", "");
    bool ok = auth_verify_password(user, pw);
    char user_copy[32];
    copy_str(user_copy, sizeof(user_copy), user);
    cJSON_Delete(j);

    if (!ok) {
        auth_login_failed();
        event_log(EV_LOGIN, "Login fehlgeschlagen fuer '%s'", user_copy);
        vTaskDelay(pdMS_TO_TICKS(500));
        return send_error(req, "401 Unauthorized", "Benutzer oder Passwort falsch");
    }
    if (auth_twofa_enabled()) {
        /* Passwort stimmt, zweiter Faktor fehlt noch: Ticket statt Session */
        const char *ticket = auth_preauth_create(user_copy);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "twofa_required", true);
        cJSON_AddStringToObject(o, "ticket", ticket);
        cJSON_AddBoolToObject(o, "time_valid", time_sync_is_valid());
        cJSON_AddNumberToObject(o, "recovery_remaining", auth_twofa_recovery_remaining());
        return send_json(req, o);
    }
    auth_login_succeeded();
    event_log(EV_LOGIN, "Login '%s'", user_copy);
    return finish_login(req, user_copy);
}

static const char *twofa_error_text(twofa_result_t r)
{
    switch (r) {
    case TWOFA_TIME_INVALID: return "Uhrzeit nicht synchron: Einmalcodes der App koennen nicht geprueft werden, bitte Recovery-Code verwenden";
    case TWOFA_REPLAY:       return "Dieser Code wurde bereits verwendet";
    case TWOFA_NOT_ENABLED:  return "2FA ist nicht aktiv";
    default:                 return "Code falsch";
    }
}

/* Zweiter Schritt: {ticket, code} – code ist TOTP (6 Ziffern) oder Recovery-Code */
static esp_err_t h_login_2fa_post(httpd_req_t *req)
{
    uint32_t retry = 0;
    if (!auth_login_allowed(&retry)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Gesperrt, erneut in %" PRIu32 " s", retry);
        return send_error(req, "429 Too Many Requests", msg);
    }
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    char ticket[AUTH_TICKET_HEX_LEN + 1], code[32], user[32];
    copy_str(ticket, sizeof(ticket), jstr(j, "ticket", ""));
    copy_str(code, sizeof(code), jstr(j, "code", ""));
    cJSON_Delete(j);

    if (!auth_preauth_check(ticket, user, sizeof(user))) {
        return send_error(req, "401 Unauthorized", "Anmeldung abgelaufen, bitte Passwort erneut eingeben");
    }
    bool used_recovery = false;
    twofa_result_t r = auth_twofa_verify(code, &used_recovery);
    if (r != TWOFA_OK) {
        auth_login_failed();
        event_log(EV_LOGIN, "2FA fehlgeschlagen fuer '%s': %s", user, twofa_error_text(r));
        vTaskDelay(pdMS_TO_TICKS(500));
        return send_error(req, "401 Unauthorized", twofa_error_text(r));
    }
    auth_preauth_consume(ticket);
    auth_login_succeeded();
    event_log(EV_LOGIN, "Login '%s' mit 2FA (%s)", user, used_recovery ? "Recovery-Code" : "TOTP");
    return finish_login(req, user);
}

/* ========================================================================
 * API: 2FA-Verwaltung (angemeldet, Passwort erneut erforderlich)
 * ====================================================================== */
static esp_err_t h_twofa_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", auth_twofa_enabled());
    cJSON_AddBoolToObject(o, "time_valid", time_sync_is_valid());
    cJSON_AddNumberToObject(o, "recovery_remaining", auth_twofa_recovery_remaining());
    cJSON_AddNumberToObject(o, "recovery_total", SETTINGS_RECOVERY_COUNT);
    cJSON_AddStringToObject(o, "issuer", CFG_TWOFA_ISSUER);
    return send_json(req, o);
}

static void add_recovery_array(cJSON *o, char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1])
{
    cJSON *arr = cJSON_AddArrayToObject(o, "recovery_codes");
    for (int i = 0; i < SETTINGS_RECOVERY_COUNT; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(recovery[i]));
    }
}

/* Einrichtung starten: {password} -> Geheimnis, otpauth-URI, Recovery-Codes (einmalig sichtbar) */
static esp_err_t h_twofa_setup_post(httpd_req_t *req)
{
    auth_session_t *s = require_auth_csrf(req);
    if (!s) return ESP_OK;
    if (!time_sync_is_valid()) {
        return send_error(req, "409 Conflict", "Uhrzeit nicht synchron: 2FA kann erst eingerichtet werden, wenn NTP die Zeit gesetzt hat");
    }
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    bool pw_ok = auth_verify_password(g_settings.admin.user, jstr(j, "password", ""));
    cJSON_Delete(j);
    if (!pw_ok) return send_error(req, "403 Forbidden", "Passwort falsch");

    char secret[AUTH_B32_SECRET_LEN];
    char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1];
    if (auth_twofa_begin_setup(secret, recovery) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Einrichtung fehlgeschlagen");
    }
    char uri[256];
    snprintf(uri, sizeof(uri), "otpauth://totp/%s:%s@%s?secret=%s&issuer=%s&algorithm=SHA1&digits=6&period=30",
             CFG_TWOFA_ISSUER, s->user, g_settings.net.hostname, secret, CFG_TWOFA_ISSUER);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "secret", secret);
    cJSON_AddStringToObject(o, "otpauth", uri);
    add_recovery_array(o, recovery);
    cJSON_AddNumberToObject(o, "expires_s", CFG_TWOFA_SETUP_TTL_S);
    return send_json(req, o);
}

static esp_err_t h_twofa_confirm_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    char code[16];
    copy_str(code, sizeof(code), jstr(j, "code", ""));
    cJSON_Delete(j);
    esp_err_t err = auth_twofa_confirm_setup(code);
    if (err == ESP_ERR_INVALID_STATE)    return send_error(req, "409 Conflict", "Keine Einrichtung aktiv oder abgelaufen, bitte neu starten");
    if (err == ESP_ERR_INVALID_RESPONSE) return send_error(req, "409 Conflict", "Uhrzeit nicht synchron");
    if (err == ESP_ERR_INVALID_ARG)      return send_error(req, "400 Bad Request", "Code stimmt nicht, bitte den aktuellen Code aus der App eingeben");
    if (err != ESP_OK)                   return send_error(req, "500 Internal Server Error", "Speichern fehlgeschlagen");
    return send_ok(req);
}

static esp_err_t h_twofa_cancel_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    auth_twofa_cancel_setup();
    return send_ok(req);
}

/* Passwort + gültiger zweiter Faktor (TOTP oder Recovery) für Deaktivieren/Neuerzeugen */
static bool twofa_reauth(httpd_req_t *req, cJSON *j, const char **err_status, const char **err_msg)
{
    if (!auth_verify_password(g_settings.admin.user, jstr(j, "password", ""))) {
        *err_status = "403 Forbidden"; *err_msg = "Passwort falsch";
        return false;
    }
    bool used_recovery = false;
    twofa_result_t r = auth_twofa_verify(jstr(j, "code", ""), &used_recovery);
    if (r != TWOFA_OK) {
        *err_status = "403 Forbidden"; *err_msg = twofa_error_text(r);
        return false;
    }
    (void)req;
    return true;
}

static esp_err_t h_twofa_disable_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    if (!auth_twofa_enabled()) return send_error(req, "409 Conflict", "2FA ist nicht aktiv");
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    const char *st, *msg;
    bool ok = twofa_reauth(req, j, &st, &msg);
    cJSON_Delete(j);
    if (!ok) return send_error(req, st, msg);
    auth_twofa_disable();
    return send_ok(req);
}

static esp_err_t h_twofa_recovery_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    if (!auth_twofa_enabled()) return send_error(req, "409 Conflict", "2FA ist nicht aktiv");
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    const char *st, *msg;
    bool ok = twofa_reauth(req, j, &st, &msg);
    cJSON_Delete(j);
    if (!ok) return send_error(req, st, msg);
    char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1];
    if (auth_twofa_regenerate_recovery(recovery) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Speichern fehlgeschlagen");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    add_recovery_array(o, recovery);
    return send_json(req, o);
}

static esp_err_t h_logout_post(httpd_req_t *req)
{
    char token[AUTH_TOKEN_HEX_LEN + 1];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, AUTH_COOKIE_NAME, token, &len) == ESP_OK) {
        auth_session_destroy(token);
    }
    char cookie[160];
    set_session_cookie(req, cookie, sizeof(cookie), NULL);
    return send_ok(req);
}

/* ========================================================================
 * API: Status
 * ====================================================================== */
static esp_err_t h_status_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    net_status_t ns;
    net_eth_get_status(&ns);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();
    char tbuf[24];
    time_sync_format_local(time(NULL), tbuf, sizeof(tbuf));

    cJSON *o = cJSON_CreateObject();
    cJSON *fw = cJSON_AddObjectToObject(o, "firmware");
    cJSON_AddStringToObject(fw, "version", app->version);
    cJSON_AddStringToObject(fw, "idf", app->idf_ver);
    cJSON_AddStringToObject(fw, "built", app->date);
    cJSON_AddStringToObject(fw, "partition", part ? part->label : "?");

    cJSON *sys = cJSON_AddObjectToObject(o, "system");
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(sys, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(sys, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    size_t fs_total = 0, fs_used = 0;
    storage_usage(&fs_total, &fs_used);
    cJSON_AddNumberToObject(sys, "fs_total", fs_total);
    cJSON_AddNumberToObject(sys, "fs_used", fs_used);
    cJSON_AddStringToObject(sys, "time", tbuf);
    cJSON_AddBoolToObject(sys, "time_valid", time_sync_is_valid());
    cJSON_AddNumberToObject(sys, "last_sync", (double)time_sync_last_sync());
    cJSON_AddBoolToObject(sys, "tls", s_tls);

    cJSON *net = cJSON_AddObjectToObject(o, "network");
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             ns.mac[0], ns.mac[1], ns.mac[2], ns.mac[3], ns.mac[4], ns.mac[5]);
    cJSON_AddStringToObject(net, "mac", mac);
    cJSON_AddStringToObject(net, "hostname", ns.hostname);
    cJSON_AddBoolToObject(net, "link", ns.link_up);
    cJSON_AddNumberToObject(net, "speed", ns.speed_mbps);
    cJSON_AddBoolToObject(net, "full_duplex", ns.full_duplex);
    cJSON_AddBoolToObject(net, "has_ip4", ns.has_ip4);
    cJSON_AddBoolToObject(net, "ip4_fallback", ns.ip4_fallback);
    cJSON_AddStringToObject(net, "ip4_mode", g_settings.net.ipv4_mode == IPV4_MODE_STATIC ? "static" : "dhcp");
    cJSON_AddStringToObject(net, "ip4", ns.ip4);
    cJSON_AddStringToObject(net, "mask4", ns.mask4);
    cJSON_AddStringToObject(net, "gw4", ns.gw4);
    cJSON_AddStringToObject(net, "dns1", ns.dns1);
    cJSON_AddStringToObject(net, "dns2", ns.dns2);
    cJSON *ip6 = cJSON_AddArrayToObject(net, "ip6");
    for (int i = 0; i < ns.ip6_count; i++) {
        cJSON_AddItemToArray(ip6, cJSON_CreateString(ns.ip6[i]));
    }

    uint32_t lock_rem = 0;
    bool locked = access_keypad_locked(&lock_rem);
    cJSON *door = cJSON_AddObjectToObject(o, "door");
    cJSON_AddBoolToObject(door, "relay_active", door_relay_active());
    cJSON_AddBoolToObject(door, "contact_enabled", g_settings.door.door_contact_enable);
    cJSON_AddBoolToObject(door, "contact_closed", door_contact_closed());
    cJSON_AddBoolToObject(door, "keypad_locked", locked);
    cJSON_AddNumberToObject(door, "keypad_lock_remaining_s", lock_rem);
    cJSON_AddNumberToObject(door, "keypad_fail_count", access_fail_count());
    cJSON_AddNumberToObject(door, "keypad_rx_count", keypad_rx_count());
    cJSON_AddNumberToObject(door, "keypad_last_rx_uptime_s", (double)keypad_last_rx_uptime_s());
    cJSON_AddNumberToObject(door, "last_open_uptime_s", (double)door_last_open_uptime_s());
    cJSON_AddNumberToObject(door, "codes_count", codes_count());
    cJSON_AddBoolToObject(door, "learn_mode", g_settings.keypad.learn_mode);
    return send_json(req, o);
}

/* ========================================================================
 * API: Netzwerk
 * ====================================================================== */
static esp_err_t h_network_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    return send_json(req, net_to_json(&g_settings.net));
}

static esp_err_t h_network_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    settings_network_t n = g_settings.net;
    const char *err = net_from_json(j, &n);
    cJSON_Delete(j);
    if (err) return send_error(req, "400 Bad Request", err);

    g_settings.net = n;
    if (settings_save() != ESP_OK) return send_error(req, "500 Internal Server Error", "Speichern fehlgeschlagen");
    event_log(EV_CONFIG, "Netzwerkeinstellungen geaendert (ipv4=%s, host=%s)",
              n.ipv4_mode == IPV4_MODE_STATIC ? n.ipv4_addr : "dhcp", n.hostname);
    time_sync_restart();
    return send_ok_reboot(req);
}

/* ========================================================================
 * API: Zertifikat
 * ====================================================================== */
static esp_err_t h_cert_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cert_info_t ci;
    esp_err_t err = cert_store_get_info(&ci);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", err == ESP_OK && ci.available);
    cJSON_AddBoolToObject(o, "tls_active", s_tls);
    cJSON_AddBoolToObject(o, "cert_file", storage_file_exists(CFG_CERT_PATH));
    cJSON_AddBoolToObject(o, "key_file", storage_file_exists(CFG_KEY_PATH));
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(o, "uploaded", ci.uploaded);
        cJSON_AddBoolToObject(o, "key_matches", ci.key_matches);
        cJSON_AddStringToObject(o, "subject", ci.subject);
        cJSON_AddStringToObject(o, "issuer", ci.issuer);
        cJSON_AddStringToObject(o, "valid_from", ci.valid_from);
        cJSON_AddStringToObject(o, "valid_to", ci.valid_to);
        cJSON_AddStringToObject(o, "key_type", ci.key_type);
        cJSON_AddStringToObject(o, "fingerprint_sha256", ci.fingerprint_sha256);
        cJSON_AddNumberToObject(o, "cert_len", ci.cert_len);
    }
    return send_json(req, o);
}

static esp_err_t h_cert_download(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    size_t len = 0;
    const char *pem = cert_store_cert_pem(&len);
    if (!pem) return send_error(req, "404 Not Found", "Kein Zertifikat");
    httpd_resp_set_type(req, "application/x-pem-file");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"keysystem-cert.pem\"");
    return httpd_resp_send(req, pem, len - 1);
}

static esp_err_t h_cert_upload(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    char query[32] = {0}, type[8] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "type", type, sizeof(type));

    size_t len = 0;
    char *pem = read_body(req, MAX_PEM_BODY, &len);
    if (!pem) return send_error(req, "400 Bad Request", "PEM fehlt oder zu gross (max. 16 KB)");

    esp_err_t err;
    if (strcmp(type, "key") == 0) {
        err = cert_store_store_key(pem, len);
    } else if (strcmp(type, "cert") == 0) {
        err = cert_store_store_cert(pem, len);
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    free(pem);
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, "400 Bad Request", "PEM nicht lesbar (unverschluesseltes PEM erwartet)");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Speichern fehlgeschlagen");
    return send_ok_reboot(req);
}

static esp_err_t h_cert_regenerate(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    cert_store_delete();
    return send_ok_reboot(req);
}

/* ========================================================================
 * API: Protokoll
 * ====================================================================== */
static esp_err_t h_log_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char query[64] = {0}, val[16] = {0};
    uint32_t after = 0;
    int max = 100;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "after", val, sizeof(val)) == ESP_OK) after = strtoul(val, NULL, 10);
        if (httpd_query_key_value(query, "max", val, sizeof(val)) == ESP_OK) max = atoi(val);
    }
    if (max < 1) max = 1;
    if (max > 200) max = 200;

    event_entry_t *buf = malloc(sizeof(event_entry_t) * max);
    if (!buf) return send_error(req, "500 Internal Server Error", "Speicher");
    size_t n = event_log_read(buf, max, after);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "last_id", event_log_last_id());
    cJSON *arr = cJSON_AddArrayToObject(o, "entries");
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", buf[i].id);
        cJSON_AddNumberToObject(e, "ts", (double)buf[i].ts);
        cJSON_AddNumberToObject(e, "uptime", buf[i].uptime_s);
        cJSON_AddStringToObject(e, "cat", event_category_name(buf[i].category));
        cJSON_AddStringToObject(e, "msg", buf[i].msg);
        cJSON_AddItemToArray(arr, e);
    }
    free(buf);
    return send_json(req, o);
}

/* ========================================================================
 * API: Codes
 * ====================================================================== */
static esp_err_t h_codes_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "codes", codes_to_json());
    cJSON_AddNumberToObject(o, "max", CFG_CODES_MAX);
    cJSON_AddBoolToObject(o, "time_valid", time_sync_is_valid());
    return send_json(req, o);
}

static esp_err_t codes_err_response(httpd_req_t *req, esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return send_ok(req);
    case ESP_ERR_INVALID_ARG:   return send_error(req, "400 Bad Request", "Name fehlt oder Code ungueltig (4-12 Zeichen, Ziffern/Buchstaben)");
    case ESP_ERR_INVALID_STATE: return send_error(req, "409 Conflict", "Dieser Code ist bereits vergeben");
    case ESP_ERR_NOT_FOUND:     return send_error(req, "404 Not Found", "Code nicht gefunden");
    case ESP_ERR_NO_MEM:        return send_error(req, "507 Insufficient Storage", "Maximale Anzahl Codes erreicht");
    default:                    return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
}

static esp_err_t h_codes_post(httpd_req_t *req)
{
    auth_session_t *s = require_auth_csrf(req);
    if (!s) return ESP_OK;
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    code_entry_t e;
    bool ok = codes_entry_from_json(j, &e);
    cJSON_Delete(j);
    if (!ok) return codes_err_response(req, ESP_ERR_INVALID_ARG);
    copy_str(e.created_by, sizeof(e.created_by), s->user);
    esp_err_t err = codes_add(&e);
    if (err == ESP_OK) {
        event_log(EV_CONFIG, "Code '%s' (#%" PRIu32 ", %s) angelegt von %s", e.name, e.id, codes_type_name(e.type), s->user);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddNumberToObject(o, "id", e.id);
        return send_json(req, o);
    }
    return codes_err_response(req, err);
}

static esp_err_t h_codes_put(httpd_req_t *req)
{
    auth_session_t *s = require_auth_csrf(req);
    if (!s) return ESP_OK;
    uint32_t id = uri_last_id(req);
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    code_entry_t e;
    bool ok = codes_entry_from_json(j, &e);
    cJSON_Delete(j);
    if (!ok) return codes_err_response(req, ESP_ERR_INVALID_ARG);
    e.id = id;
    esp_err_t err = codes_update(&e);
    if (err == ESP_OK) {
        event_log(EV_CONFIG, "Code '%s' (#%" PRIu32 ") geaendert von %s", e.name, e.id, s->user);
    }
    return codes_err_response(req, err);
}

static esp_err_t h_codes_delete(httpd_req_t *req)
{
    auth_session_t *s = require_auth_csrf(req);
    if (!s) return ESP_OK;
    uint32_t id = uri_last_id(req);
    code_entry_t e;
    bool found = codes_get(id, &e);
    esp_err_t err = codes_delete(id);
    if (err == ESP_OK && found) {
        event_log(EV_CONFIG, "Code '%s' (#%" PRIu32 ") geloescht von %s", e.name, id, s->user);
    }
    return codes_err_response(req, err);
}

/* ========================================================================
 * API: Tür / Keypad
 * ====================================================================== */
static esp_err_t h_door_open(httpd_req_t *req)
{
    auth_session_t *s = require_auth_csrf(req);
    if (!s) return ESP_OK;
    char reason[48];
    snprintf(reason, sizeof(reason), "Web-UI %s", s->user);
    esp_err_t err = door_open(reason, 0);
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Relais konnte nicht geschaltet werden");
    return send_ok(req);
}

static esp_err_t h_keypad_raw(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char *hex = malloc(CFG_KEYPAD_RAW_BUF * 3 + 1);
    char *ascii = malloc(CFG_KEYPAD_RAW_BUF + 1);
    if (!hex || !ascii) { free(hex); free(ascii); return send_error(req, "500 Internal Server Error", "Speicher"); }
    size_t n = keypad_get_raw(hex, CFG_KEYPAD_RAW_BUF * 3 + 1, ascii, CFG_KEYPAD_RAW_BUF + 1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "count", n);
    cJSON_AddNumberToObject(o, "rx_total", keypad_rx_count());
    cJSON_AddNumberToObject(o, "last_rx_uptime_s", (double)keypad_last_rx_uptime_s());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "hex", hex);
    cJSON_AddStringToObject(o, "ascii", ascii);
    cJSON_AddBoolToObject(o, "learn_mode", g_settings.keypad.learn_mode);
    free(hex);
    free(ascii);
    return send_json(req, o);
}

/* ========================================================================
 * API: System
 * ====================================================================== */
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void web_server_schedule_reboot(uint32_t delay_ms)
{
    const esp_timer_create_args_t args = { .callback = reboot_timer_cb, .name = "reboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)delay_ms * 1000);
    }
}

static esp_err_t h_system_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "door", door_to_json(&g_settings.door));
    cJSON_AddItemToObject(o, "keypad", keypad_to_json(&g_settings.keypad));
    cJSON_AddStringToObject(o, "admin_user", g_settings.admin.user);
    cJSON *pins = cJSON_AddObjectToObject(o, "pins");
    cJSON_AddNumberToObject(pins, "relay", CFG_RELAY_GPIO);
    cJSON_AddNumberToObject(pins, "door_contact", CFG_DOOR_CONTACT_GPIO);
    cJSON_AddNumberToObject(pins, "exit_button", CFG_EXIT_BUTTON_GPIO);
    cJSON_AddNumberToObject(pins, "keypad_rx", CFG_KEYPAD_RX_GPIO);
    cJSON_AddNumberToObject(pins, "keypad_tx", CFG_KEYPAD_TX_GPIO);
    return send_json(req, o);
}

static esp_err_t h_system_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "door");
    if (cJSON_IsObject(d)) door_from_json(d, &g_settings.door);
    cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "keypad");
    if (cJSON_IsObject(k)) keypad_from_json(k, &g_settings.keypad);
    cJSON_Delete(j);
    if (settings_save() != ESP_OK) return send_error(req, "500 Internal Server Error", "Speichern fehlgeschlagen");
    door_apply_settings();
    keypad_apply_settings();
    event_log(EV_CONFIG, "Systemeinstellungen geaendert (Relais %u ms, Keypad %" PRIu32 " Bd%s)",
              g_settings.door.relay_pulse_ms, g_settings.keypad.baud, g_settings.keypad.learn_mode ? ", Lernmodus" : "");
    return send_ok(req);
}

static esp_err_t h_password_post(httpd_req_t *req)
{
    auth_session_t *s = require_auth_csrf(req);
    if (!s) return ESP_OK;
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "JSON erwartet");
    const char *old = jstr(j, "old_password", "");
    const char *nw = jstr(j, "new_password", "");
    const char *user = jstr(j, "user", g_settings.admin.user);
    if (!auth_verify_password(g_settings.admin.user, old)) {
        cJSON_Delete(j);
        return send_error(req, "403 Forbidden", "Altes Passwort falsch");
    }
    esp_err_t err = auth_set_password(user, nw);
    cJSON_Delete(j);
    if (err != ESP_OK) return send_error(req, "400 Bad Request", "Neues Passwort muss mindestens 8 Zeichen haben");
    event_log(EV_CONFIG, "Admin-Passwort geaendert");
    char cookie[160];
    set_session_cookie(req, cookie, sizeof(cookie), NULL);
    return send_ok(req);
}

static esp_err_t h_reboot_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    event_log(EV_SYSTEM, "Neustart ueber Web-UI");
    web_server_schedule_reboot(1500);
    return send_ok(req);
}

static esp_err_t h_factory_reset_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    event_log(EV_SYSTEM, "Werksreset ueber Web-UI");
    settings_factory_reset();
    cert_store_delete();
    codes_clear();
    web_server_schedule_reboot(1500);
    return send_ok(req);
}

/* ========================================================================
 * API: Backup / Restore (ohne Passwort-Hash, ohne Zertifikat/Key)
 * ====================================================================== */
static esp_err_t h_backup_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "format", "esp32_key_system_backup");
    cJSON_AddNumberToObject(o, "version", 1);
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "created", (double)(time_sync_is_valid() ? time(NULL) : 0));
    cJSON *st = cJSON_AddObjectToObject(o, "settings");
    cJSON_AddItemToObject(st, "net", net_to_json(&g_settings.net));
    cJSON_AddItemToObject(st, "door", door_to_json(&g_settings.door));
    cJSON_AddItemToObject(st, "keypad", keypad_to_json(&g_settings.keypad));
    cJSON_AddItemToObject(o, "codes", codes_to_json());
    char fname[40 + SETTINGS_STR_HOST];
    snprintf(fname, sizeof(fname), "attachment; filename=\"%s-backup.json\"", g_settings.net.hostname);
    httpd_resp_set_hdr(req, "Content-Disposition", fname);
    return send_json(req, o);
}

static esp_err_t h_restore_post(httpd_req_t *req)
{
    auth_session_t *s = require_auth_csrf(req);
    if (!s) return ESP_OK;
    cJSON *j = read_json_max(req, MAX_BACKUP_BODY);
    if (!j) return send_error(req, "400 Bad Request", "JSON fehlt, ungueltig oder zu gross (max. 64 KB)");
    if (strcmp(jstr(j, "format", ""), "esp32_key_system_backup") != 0) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "Keine Backup-Datei dieses Geraetetyps");
    }
    bool with_net = jbool(j, "restore_network", true);     /* Client kann das Netz ausklammern */
    cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "settings");
    settings_t ns = g_settings;
    if (cJSON_IsObject(st)) {
        cJSON *net = cJSON_GetObjectItemCaseSensitive(st, "net");
        if (with_net && cJSON_IsObject(net)) {
            const char *err = net_from_json(net, &ns.net);
            if (err) { cJSON_Delete(j); return send_error(req, "400 Bad Request", err); }
        }
        cJSON *d = cJSON_GetObjectItemCaseSensitive(st, "door");
        if (cJSON_IsObject(d)) door_from_json(d, &ns.door);
        cJSON *k = cJSON_GetObjectItemCaseSensitive(st, "keypad");
        if (cJSON_IsObject(k)) keypad_from_json(k, &ns.keypad);
    }
    cJSON *codes = cJSON_GetObjectItemCaseSensitive(j, "codes");
    esp_err_t cerr = cJSON_IsArray(codes) ? codes_from_json(codes, true) : ESP_OK;
    cJSON_Delete(j);

    g_settings = ns;
    esp_err_t serr = settings_save();
    door_apply_settings();
    keypad_apply_settings();
    event_log(EV_CONFIG, "Backup eingespielt von %s (%u Codes)%s", s->user, (unsigned)codes_count(),
              (serr != ESP_OK || cerr != ESP_OK) ? " MIT FEHLERN" : "");
    if (serr != ESP_OK || cerr != ESP_OK) return send_error(req, "500 Internal Server Error", "Teilweise fehlgeschlagen, Protokoll pruefen");
    return send_ok_reboot(req);
}

/* ========================================================================
 * API: Firmware (signiertes OTA)
 * ====================================================================== */
static esp_err_t h_firmware_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    return send_json(req, ota_info_json());
}

/* Body = firmware.bin (application/octet-stream), wird direkt in die OTA-Partition geschrieben */
static esp_err_t h_firmware_post(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    size_t total = req->content_len;
    if (total < 4096) return send_error(req, "400 Bad Request", "Kein Firmware-Image");

    esp_err_t err = ota_begin(total);
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", ota_last_error());

    const size_t chunk = 4096;
    char *buf = malloc(chunk);
    if (!buf) { ota_abort(); return send_error(req, "500 Internal Server Error", "Speicher"); }

    size_t got = 0;
    int timeouts = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf, (total - got) < chunk ? (total - got) : chunk);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 10) break;
            continue;
        }
        if (r <= 0) break;
        timeouts = 0;
        if (ota_write(buf, r) != ESP_OK) break;
        got += r;
    }
    free(buf);

    if (got != total) {
        ota_abort();
        return send_error(req, "400 Bad Request", "Upload unvollstaendig");
    }
    err = ota_end();
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", ota_last_error());
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "reboot_required", true);
    cJSON_AddNumberToObject(o, "bytes", got);
    return send_json(req, o);
}

static esp_err_t h_firmware_rollback(httpd_req_t *req)
{
    if (!require_auth_csrf(req)) return ESP_OK;
    esp_err_t err = ota_rollback();
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "409 Conflict", "In der anderen Partition liegt keine Firmware");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return send_ok_reboot(req);
}

/* ========================================================================
 * HTTP -> HTTPS Redirect (Port 80)
 * ====================================================================== */
static esp_err_t h_redirect(httpd_req_t *req)
{
    char host[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || !host[0]) {
        snprintf(host, sizeof(host), "%s.local", g_settings.net.hostname);
    }
    char *colon = strrchr(host, ':');
    if (colon && (host[0] != '[' || strchr(colon, ']') == NULL)) {
        *colon = '\0';
    }
    char loc[8 + sizeof(host) + CONFIG_HTTPD_MAX_URI_LEN + 1];
    snprintf(loc, sizeof(loc), "https://%s%.*s", host, CONFIG_HTTPD_MAX_URI_LEN, req->uri);
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t start_redirect_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CFG_HTTP_REDIRECT_PORT;
    cfg.ctrl_port = 32770;
    cfg.max_open_sockets = 3;
    cfg.max_uri_handlers = 2;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 4096;
    esp_err_t err = httpd_start(&s_redirect, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Redirect-Server (Port %d): %s", CFG_HTTP_REDIRECT_PORT, esp_err_to_name(err));
        return err;
    }
    httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = h_redirect };
    httpd_register_uri_handler(s_redirect, &any);
    return ESP_OK;
}

/* ========================================================================
 * Start
 * ====================================================================== */
#define ROUTE(u, m, h) { .uri = (u), .method = (m), .handler = (h), .user_ctx = NULL }

static const httpd_uri_t s_routes[] = {
    ROUTE("/",                         HTTP_GET,    h_index),
    ROUTE("/index.html",               HTTP_GET,    h_index),
    ROUTE("/app.js",                   HTTP_GET,    h_app_js),
    ROUTE("/style.css",                HTTP_GET,    h_style_css),
    ROUTE("/qrcode.js",                HTTP_GET,    h_qrcode_js),
    ROUTE("/api/session",              HTTP_GET,    h_session_get),
    ROUTE("/api/setup",                HTTP_POST,   h_setup_post),
    ROUTE("/api/login",                HTTP_POST,   h_login_post),
    ROUTE("/api/login/2fa",            HTTP_POST,   h_login_2fa_post),
    ROUTE("/api/twofa",                HTTP_GET,    h_twofa_get),
    ROUTE("/api/twofa/setup",          HTTP_POST,   h_twofa_setup_post),
    ROUTE("/api/twofa/confirm",        HTTP_POST,   h_twofa_confirm_post),
    ROUTE("/api/twofa/cancel",         HTTP_POST,   h_twofa_cancel_post),
    ROUTE("/api/twofa/disable",        HTTP_POST,   h_twofa_disable_post),
    ROUTE("/api/twofa/recovery",       HTTP_POST,   h_twofa_recovery_post),
    ROUTE("/api/logout",               HTTP_POST,   h_logout_post),
    ROUTE("/api/status",               HTTP_GET,    h_status_get),
    ROUTE("/api/network",              HTTP_GET,    h_network_get),
    ROUTE("/api/network",              HTTP_POST,   h_network_post),
    ROUTE("/api/cert",                 HTTP_GET,    h_cert_get),
    ROUTE("/api/cert/download",        HTTP_GET,    h_cert_download),
    ROUTE("/api/cert/upload",          HTTP_POST,   h_cert_upload),
    ROUTE("/api/cert/regenerate",      HTTP_POST,   h_cert_regenerate),
    ROUTE("/api/log",                  HTTP_GET,    h_log_get),
    ROUTE("/api/codes",                HTTP_GET,    h_codes_get),
    ROUTE("/api/codes",                HTTP_POST,   h_codes_post),
    ROUTE("/api/codes/*",              HTTP_PUT,    h_codes_put),
    ROUTE("/api/codes/*",              HTTP_DELETE, h_codes_delete),
    ROUTE("/api/door/open",            HTTP_POST,   h_door_open),
    ROUTE("/api/keypad/raw",           HTTP_GET,    h_keypad_raw),
    ROUTE("/api/system",               HTTP_GET,    h_system_get),
    ROUTE("/api/system",               HTTP_POST,   h_system_post),
    ROUTE("/api/system/password",      HTTP_POST,   h_password_post),
    ROUTE("/api/system/reboot",        HTTP_POST,   h_reboot_post),
    ROUTE("/api/system/factory-reset", HTTP_POST,   h_factory_reset_post),
    ROUTE("/api/backup",               HTTP_GET,    h_backup_get),
    ROUTE("/api/restore",              HTTP_POST,   h_restore_post),
    ROUTE("/api/firmware",             HTTP_GET,    h_firmware_get),
    ROUTE("/api/firmware",             HTTP_POST,   h_firmware_post),
    ROUTE("/api/firmware/rollback",    HTTP_POST,   h_firmware_rollback),
};

esp_err_t web_server_start(void)
{
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.max_uri_handlers = sizeof(s_routes) / sizeof(s_routes[0]) + 8;
    cfg.httpd.max_open_sockets = CFG_HTTPS_MAX_SOCKETS;
    cfg.httpd.stack_size = 16384;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;
    cfg.httpd.recv_wait_timeout = 30;      /* Firmware-Upload über TLS braucht Luft */
    cfg.httpd.send_wait_timeout = 10;
    cfg.port_secure = CFG_HTTPS_PORT;
    cfg.port_insecure = CFG_HTTP_REDIRECT_PORT;
    cfg.session_tickets = true;

    size_t clen = 0, klen = 0;
    const char *cert = cert_store_cert_pem(&clen);
    const char *key = cert_store_key_pem(&klen);
    if (cert && key) {
        cfg.servercert = (const uint8_t *)cert;
        cfg.servercert_len = clen;
        cfg.prvtkey_pem = (const uint8_t *)key;
        cfg.prvtkey_len = klen;
        cfg.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
        s_tls = true;
    } else {
        /* Casambi: ohne verwendbares Zertifikat HTTP-only als Notfallmodus */
        cfg.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;
        s_tls = false;
        event_log(EV_SYSTEM, "Kein Zertifikat, Webserver laeuft HTTP-only auf Port %d", CFG_HTTP_REDIRECT_PORT);
    }

    esp_err_t err = httpd_ssl_start(&s_https, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start: %s", esp_err_to_name(err));
        event_log(EV_SYSTEM, "Webserver-Start fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        httpd_register_uri_handler(s_https, &s_routes[i]);
    }
    if (s_tls) {
        start_redirect_server();
    }
    ESP_LOGI(TAG, "Webserver gestartet: %s auf Port %d", s_tls ? "HTTPS" : "HTTP", s_tls ? CFG_HTTPS_PORT : CFG_HTTP_REDIRECT_PORT);
    return ESP_OK;
}

bool web_server_is_tls(void)
{
    return s_tls;
}
