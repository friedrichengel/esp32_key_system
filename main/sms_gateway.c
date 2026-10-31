#include "sms_gateway.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"

static const char *TAG = "sms";
static SemaphoreHandle_t s_lock;

bool sms_gateway_enabled(void)
{
    const settings_sms_t *s = &g_settings.sms;
    return s->enabled && s->url[0] && s->user[0];
}

bool sms_gateway_valid_number(const char *number)
{
    if (!number) return false;
    int digits = 0;
    bool first = true;
    for (const char *p = number; *p; p++) {
        if (*p == ' ' || *p == '-') continue;
        if (*p == '+' && first) { first = false; continue; }
        first = false;
        if (!isdigit((unsigned char)*p)) return false;
        digits++;
    }
    return digits >= 6 && digits <= 20;
}

static void set_err(char *err, size_t len, const char *msg)
{
    if (err && len) strlcpy(err, msg, len);
}

/* POST mit JSON-Body; Antwort (max. resp_len-1 Byte) als String, HTTP-Status in *status */
static esp_err_t http_post_json(const char *url, const char *bearer, const char *body, char *resp, size_t resp_len,
                                int *status)
{
    const settings_sms_t *s = &g_settings.sms;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CFG_SMS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = s->verify_tls ? esp_crt_bundle_attach : NULL,
        .skip_cert_common_name_check = !s->verify_tls,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "application/json");
    char auth[128];
    if (bearer) {
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    int blen = (int)strlen(body);
    esp_err_t err = esp_http_client_open(c, blen);
    if (err == ESP_OK) {
        if (esp_http_client_write(c, body, blen) != blen) err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(c) < 0) err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        int n = esp_http_client_read_response(c, resp, (int)resp_len - 1);
        resp[n > 0 ? n : 0] = 0;
        *status = esp_http_client_get_status_code(c);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    memset(auth, 0, sizeof(auth));
    return err;
}

/* RutOS-Antwort: {"success":true,"data":{...}} oder {"success":false,"errors":[{"error":"..."}]} */
static cJSON *rutos_parse(const char *resp, int status, char *err, size_t err_len)
{
    cJSON *j = cJSON_Parse(resp);
    if (!j) {
        char m[64];
        snprintf(m, sizeof(m), "Router antwortet nicht mit JSON (HTTP %d)", status);
        set_err(err, err_len, m);
        return NULL;
    }
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(j, "success");
    if (!cJSON_IsTrue(ok)) {
        const char *text = NULL;
        cJSON *errs = cJSON_GetObjectItemCaseSensitive(j, "errors");
        cJSON *first = cJSON_IsArray(errs) ? cJSON_GetArrayItem(errs, 0) : NULL;
        cJSON *e = first ? cJSON_GetObjectItemCaseSensitive(first, "error") : NULL;
        if (cJSON_IsString(e) && e->valuestring) text = e->valuestring;
        char m[120];
        snprintf(m, sizeof(m), "Router meldet Fehler (HTTP %d): %s", status, text ? text : "unbekannt");
        set_err(err, err_len, m);
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}

esp_err_t sms_gateway_send(const char *number, const char *text, char *err, size_t err_len)
{
    const settings_sms_t *s = &g_settings.sms;
    set_err(err, err_len, "");
    if (!sms_gateway_enabled()) { set_err(err, err_len, "SMS-Versand ist nicht eingerichtet (System)"); return ESP_ERR_INVALID_STATE; }
    if (!sms_gateway_valid_number(number)) { set_err(err, err_len, "Telefonnummer ungueltig"); return ESP_ERR_INVALID_ARG; }
    if (!text || !text[0]) { set_err(err, err_len, "Leerer Text"); return ESP_ERR_INVALID_ARG; }
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(CFG_SMS_HTTP_TIMEOUT_MS * 3)) != pdTRUE) {
        set_err(err, err_len, "SMS-Versand ist gerade belegt");
        return ESP_ERR_TIMEOUT;
    }

    char *resp = malloc(CFG_SMS_RESP_MAX);
    char *body = malloc(CFG_SMS_RESP_MAX);
    char url[sizeof(s->url) + 40];
    char token[160] = {0};
    esp_err_t res = ESP_FAIL;
    int status = 0;
    if (!resp || !body) { set_err(err, err_len, "Kein Speicher"); res = ESP_ERR_NO_MEM; goto out; }

    /* 1. Login */
    cJSON *lj = cJSON_CreateObject();
    cJSON_AddStringToObject(lj, "username", s->user);
    cJSON_AddStringToObject(lj, "password", s->password);
    cJSON_PrintPreallocated(lj, body, CFG_SMS_RESP_MAX, false);
    cJSON_Delete(lj);
    snprintf(url, sizeof(url), "%s/api/login", s->url);
    res = http_post_json(url, NULL, body, resp, CFG_SMS_RESP_MAX, &status);
    memset(body, 0, CFG_SMS_RESP_MAX);
    if (res != ESP_OK) {
        char m[120];
        snprintf(m, sizeof(m), "Router nicht erreichbar (%s)", esp_err_to_name(res));
        set_err(err, err_len, m);
        goto out;
    }
    cJSON *lr = rutos_parse(resp, status, err, err_len);
    if (!lr) { res = ESP_FAIL; goto out; }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(lr, "data");
    cJSON *tok = data ? cJSON_GetObjectItemCaseSensitive(data, "token") : NULL;
    if (!cJSON_IsString(tok) || !tok->valuestring || strlen(tok->valuestring) >= sizeof(token)) {
        cJSON_Delete(lr);
        set_err(err, err_len, "Login am Router ohne Token (Benutzer/Passwort pruefen)");
        res = ESP_FAIL;
        goto out;
    }
    strlcpy(token, tok->valuestring, sizeof(token));
    cJSON_Delete(lr);

    /* 2. Senden */
    cJSON *sj = cJSON_CreateObject();
    cJSON *d = cJSON_AddObjectToObject(sj, "data");
    cJSON_AddStringToObject(d, "number", number);
    cJSON_AddStringToObject(d, "message", text);
    if (s->modem[0]) cJSON_AddStringToObject(d, "modem", s->modem);
    cJSON_PrintPreallocated(sj, body, CFG_SMS_RESP_MAX, false);
    cJSON_Delete(sj);
    snprintf(url, sizeof(url), "%s/api/messages/actions/send", s->url);
    res = http_post_json(url, token, body, resp, CFG_SMS_RESP_MAX, &status);
    memset(body, 0, CFG_SMS_RESP_MAX);
    if (res != ESP_OK) {
        char m[120];
        snprintf(m, sizeof(m), "Senden fehlgeschlagen (%s)", esp_err_to_name(res));
        set_err(err, err_len, m);
        goto out;
    }
    cJSON *sr = rutos_parse(resp, status, err, err_len);
    if (!sr) { res = ESP_FAIL; goto out; }
    cJSON_Delete(sr);
    res = ESP_OK;
    ESP_LOGI(TAG, "SMS an %s gesendet", number);

out:
    memset(token, 0, sizeof(token));
    free(resp);
    free(body);
    xSemaphoreGive(s_lock);
    return res;
}
