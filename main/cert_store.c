#include "cert_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "psa/crypto.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

#include "config.h"
#include "settings.h"
#include "storage.h"
#include "event_log.h"

static const char *TAG = "cert";

static char  *s_cert_pem;      /* nullterminiert, s_cert_len inkl. NUL */
static size_t s_cert_len;
static char  *s_key_pem;
static size_t s_key_len;
static bool   s_generated_this_boot;

/* Zertifikatserzeugung dauert Sekunden, Watchdog zwischendurch füttern. */
static inline void wdt_feed_safe(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

static bool pem_looks_like(const char *pem, size_t len, const char *marker)
{
    return pem && len > 100 && strstr(pem, marker) != NULL;
}

/* ------------------------------------------------------------------------
 * Selbstsigniertes EC-P-256-Zertifikat erzeugen (mbedTLS 4 / PSA)
 * ---------------------------------------------------------------------- */
static int generate_self_signed(const char *cn, const char *org, const char *san_host,
                                char **cert_out, char **key_out)
{
    int ret = -1;
    psa_key_id_t kid = 0;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    unsigned char *cert_pem = NULL, *key_pem = NULL;
    const size_t buf_size = 4096;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d", (int)st);
        goto cleanup;
    }

    cert_pem = calloc(1, buf_size);
    key_pem = calloc(1, buf_size);
    if (!cert_pem || !key_pem) {
        ret = -100;
        goto cleanup;
    }

    /* Schlüsselpaar über PSA erzeugen und in einen transparenten PK-Kontext kopieren */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH |
                            PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    wdt_feed_safe();
    st = psa_generate_key(&attr, &kid);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key: %d", (int)st);
        goto cleanup;
    }
    ret = mbedtls_pk_copy_from_psa(kid, &key);
    if (ret != 0) {
        goto cleanup;
    }
    wdt_feed_safe();

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    char dn[200];
    snprintf(dn, sizeof(dn), "CN=%s,O=%s", cn, org);
    if ((ret = mbedtls_x509write_crt_set_subject_name(&crt, dn)) != 0) goto cleanup;
    if ((ret = mbedtls_x509write_crt_set_issuer_name(&crt, dn)) != 0) goto cleanup;

    unsigned char serial[8];
    esp_fill_random(serial, sizeof(serial));
    serial[0] = (serial[0] & 0x7F) | 0x40;
    if ((ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0) goto cleanup;

    /* Feste Gültigkeit: beim ersten Boot gibt es meist noch keine NTP-Zeit. */
    if ((ret = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20451231235959")) != 0) goto cleanup;
    if ((ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) != 0) goto cleanup;
    mbedtls_x509write_crt_set_subject_key_identifier(&crt);

    /* SAN: Hostname und Hostname.local, damit Browser den Namen akzeptieren */
    char san_local[80];
    snprintf(san_local, sizeof(san_local), "%s.local", san_host);
    mbedtls_x509_san_list san2 = {
        .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                  .san.unstructured_name = { .p = (unsigned char *)san_local, .len = strlen(san_local) } },
        .next = NULL,
    };
    mbedtls_x509_san_list san1 = {
        .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                  .san.unstructured_name = { .p = (unsigned char *)san_host, .len = strlen(san_host) } },
        .next = &san2,
    };
    if ((ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san1)) != 0) goto cleanup;

    wdt_feed_safe();
    if ((ret = mbedtls_x509write_crt_pem(&crt, cert_pem, buf_size)) != 0) goto cleanup;
    if ((ret = mbedtls_pk_write_key_pem(&key, key_pem, buf_size)) != 0) goto cleanup;

    *cert_out = (char *)cert_pem;
    *key_out = (char *)key_pem;
    cert_pem = key_pem = NULL;
    ret = 0;

cleanup:
    free(cert_pem);
    free(key_pem);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    if (kid) {
        psa_destroy_key(kid);
    }
    return ret;
}

/* ------------------------------------------------------------------------
 * Laden / Erzeugen
 * ---------------------------------------------------------------------- */
static void unload(void)
{
    free(s_cert_pem);
    free(s_key_pem);
    s_cert_pem = s_key_pem = NULL;
    s_cert_len = s_key_len = 0;
}

static bool load_from_storage(void)
{
    unload();
    size_t cl = 0, kl = 0;
    char *c = storage_read_file(CFG_CERT_PATH, &cl);
    char *k = storage_read_file(CFG_KEY_PATH, &kl);
    if (pem_looks_like(c, cl, "BEGIN CERTIFICATE") && pem_looks_like(k, kl, "PRIVATE KEY")) {
        s_cert_pem = c;
        s_cert_len = cl + 1;
        s_key_pem = k;
        s_key_len = kl + 1;
        return true;
    }
    free(c);
    free(k);
    return false;
}

esp_err_t cert_store_init(void)
{
    if (load_from_storage()) {
        ESP_LOGI(TAG, "Zertifikat/Key von %s geladen", CFG_FS_BASE_PATH);
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    const char *host = g_settings.net.hostname[0] ? g_settings.net.hostname : CFG_HOSTNAME_DEFAULT;
    char cn[96];
    snprintf(cn, sizeof(cn), "%s-%02x%02x%02x%02x%02x%02x", host,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Erzeuge gerätespezifisches selbstsigniertes Zertifikat (CN=%s)", cn);
    int64_t t0 = esp_timer_get_time();
    char *cert = NULL, *key = NULL;
    int ret = generate_self_signed(cn, CFG_CERT_ORG_NAME, host, &cert, &key);
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;

    if (ret != 0) {
        /* Halbfertige Dateien weg, sonst blockiert ein kaputtes PEM den HTTPS-Start */
        storage_remove(CFG_CERT_PATH);
        storage_remove(CFG_KEY_PATH);
        event_log(EV_SYSTEM, "Zertifikatserzeugung fehlgeschlagen rc=%d, HTTP-only", ret);
        return ESP_FAIL;
    }

    esp_err_t err = storage_write_file(CFG_CERT_PATH, cert, strlen(cert));
    if (err == ESP_OK) {
        err = storage_write_file(CFG_KEY_PATH, key, strlen(key));
    }
    if (err != ESP_OK) {
        /* Nicht persistierbar, aber für diesen Boot im RAM verwendbar */
        event_log(EV_SYSTEM, "Zertifikat konnte nicht gespeichert werden");
    }
    s_cert_pem = cert;
    s_cert_len = strlen(cert) + 1;
    s_key_pem = key;
    s_key_len = strlen(key) + 1;
    s_generated_this_boot = true;
    event_log(EV_SYSTEM, "Geraetezertifikat erzeugt (%lld ms)", (long long)dt_ms);
    return ESP_OK;
}

bool cert_store_available(void)
{
    return s_cert_pem && s_key_pem;
}

const char *cert_store_cert_pem(size_t *len)
{
    if (len) *len = s_cert_len;
    return s_cert_pem;
}

const char *cert_store_key_pem(size_t *len)
{
    if (len) *len = s_key_len;
    return s_key_pem;
}

/* ------------------------------------------------------------------------
 * Info / Validierung
 * ---------------------------------------------------------------------- */
static void fmt_x509_time(const mbedtls_x509_time *t, char *buf, size_t len)
{
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->mon, t->day, t->hour, t->min, t->sec);
}

static void describe_key(const mbedtls_pk_context *pk, char *buf, size_t len)
{
    /* mbedTLS 4: Schlüsseltyp über die PSA-Attribute abfragen */
    const char *name = "?";
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    if (mbedtls_pk_get_psa_attributes(pk, PSA_KEY_USAGE_VERIFY_HASH, &attr) == 0) {
        psa_key_type_t t = psa_get_key_type(&attr);
        if (PSA_KEY_TYPE_IS_RSA(t)) {
            name = "RSA";
        } else if (PSA_KEY_TYPE_IS_ECC(t)) {
            name = "EC";
        }
    }
    psa_reset_key_attributes(&attr);
    snprintf(buf, len, "%s %u", name, (unsigned)mbedtls_pk_get_bitlen(pk));
}

esp_err_t cert_store_get_info(cert_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (!cert_store_available()) {
        return ESP_ERR_NOT_FOUND;
    }
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&pk);

    int ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)s_cert_pem, s_cert_len);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        return ESP_FAIL;
    }
    info->available = true;
    info->cert_len = s_cert_len - 1;
    info->key_len = s_key_len - 1;
    mbedtls_x509_dn_gets(info->subject, sizeof(info->subject), &crt.subject);
    mbedtls_x509_dn_gets(info->issuer, sizeof(info->issuer), &crt.issuer);
    fmt_x509_time(&crt.valid_from, info->valid_from, sizeof(info->valid_from));
    fmt_x509_time(&crt.valid_to, info->valid_to, sizeof(info->valid_to));
    describe_key(&crt.pk, info->key_type, sizeof(info->key_type));
    /* Von uns erzeugt: O = CFG_CERT_ORG_NAME und selbstsigniert */
    info->uploaded = !(strstr(info->subject, "O=" CFG_CERT_ORG_NAME) && strcmp(info->subject, info->issuer) == 0);

    unsigned char hash[32];
    size_t hlen = 0;
    psa_crypto_init();
    if (psa_hash_compute(PSA_ALG_SHA_256, crt.raw.p, crt.raw.len, hash, sizeof(hash), &hlen) == PSA_SUCCESS) {
        char *p = info->fingerprint_sha256;
        for (int i = 0; i < 32; i++) {
            p += snprintf(p, 4, "%02X%s", hash[i], i < 31 ? ":" : "");
        }
    }

    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)s_key_pem, s_key_len, NULL, 0) == 0) {
        info->key_matches = (mbedtls_pk_check_pair(&crt.pk, &pk) == 0);
    }
    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&crt);
    return ESP_OK;
}

esp_err_t cert_store_store_cert(const char *pem, size_t len)
{
    if (!pem_looks_like(pem, len, "BEGIN CERTIFICATE")) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    /* Parser braucht nullterminierten Puffer inkl. NUL in der Länge */
    char *tmp = malloc(len + 1);
    if (!tmp) return ESP_ERR_NO_MEM;
    memcpy(tmp, pem, len);
    tmp[len] = '\0';
    int ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)tmp, len + 1);
    mbedtls_x509_crt_free(&crt);
    if (ret != 0) {
        free(tmp);
        ESP_LOGW(TAG, "Zertifikat-Upload nicht parsebar: -0x%04x", -ret);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = storage_write_file(CFG_CERT_PATH, tmp, len);
    free(tmp);
    if (err == ESP_OK) {
        event_log(EV_CONFIG, "HTTPS-Zertifikat hochgeladen (%u Byte)", (unsigned)len);
    }
    return err;
}

esp_err_t cert_store_store_key(const char *pem, size_t len)
{
    if (!pem_looks_like(pem, len, "PRIVATE KEY")) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    char *tmp = malloc(len + 1);
    if (!tmp) return ESP_ERR_NO_MEM;
    memcpy(tmp, pem, len);
    tmp[len] = '\0';
    int ret = mbedtls_pk_parse_key(&pk, (const unsigned char *)tmp, len + 1, NULL, 0);
    mbedtls_pk_free(&pk);
    if (ret != 0) {
        free(tmp);
        ESP_LOGW(TAG, "Key-Upload nicht parsebar (verschlüsselt?): -0x%04x", -ret);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = storage_write_file(CFG_KEY_PATH, tmp, len);
    free(tmp);
    if (err == ESP_OK) {
        event_log(EV_CONFIG, "HTTPS-Key hochgeladen (%u Byte)", (unsigned)len);
    }
    return err;
}

esp_err_t cert_store_delete(void)
{
    storage_remove(CFG_CERT_PATH);
    storage_remove(CFG_KEY_PATH);
    event_log(EV_CONFIG, "HTTPS-Zertifikat geloescht, Neuerzeugung beim naechsten Start");
    return ESP_OK;
}
