/*
 * cert_store.h – HTTPS-Zertifikat und Key wie bei Casambi_Gateway/Casambi_Logic
 *
 * Dateien /storage/webserver_cert.pem und /storage/webserver_key.pem. Liegt kein
 * gültiges Paar vor, wird beim Boot ein gerätespezifisches, selbstsigniertes
 * EC-P-256-Zertifikat erzeugt (CN = <hostname>-<mac>, 2025–2045). Der Admin kann
 * Zertifikat und Key getrennt hochladen; aktiv wird ein Wechsel nach Neustart.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    bool  available;          /* ein verwendbares Paar ist geladen         */
    bool  uploaded;           /* kein von uns erzeugtes Zertifikat         */
    bool  key_matches;        /* privater Key passt zum Zertifikat         */
    char  subject[128];
    char  issuer[128];
    char  valid_from[20];     /* "YYYY-MM-DD HH:MM:SS"                      */
    char  valid_to[20];
    char  key_type[16];       /* "EC P-256", "RSA 2048" ...                 */
    char  fingerprint_sha256[96]; /* "AB:CD:..."                            */
    size_t cert_len;
    size_t key_len;
} cert_info_t;

esp_err_t   cert_store_init(void);                 /* laden oder erzeugen (Casambi: ensure_persistent_cert) */
bool        cert_store_available(void);
const char *cert_store_cert_pem(size_t *len_incl_nul);
const char *cert_store_key_pem(size_t *len_incl_nul);
esp_err_t   cert_store_get_info(cert_info_t *info);

/* Upload: prüft das PEM (parsebar; Key passt optional zum vorhandenen Zertifikat) und speichert. */
esp_err_t   cert_store_store_cert(const char *pem, size_t len);
esp_err_t   cert_store_store_key(const char *pem, size_t len);
/* Löscht beide Dateien; beim nächsten Boot wird neu erzeugt. */
esp_err_t   cert_store_delete(void);
