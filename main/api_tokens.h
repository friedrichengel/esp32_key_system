/*
 * api_tokens.h – Bearer-Token für Drittsysteme (Hausautomation), PLAN.md §8a
 *
 * Token-Klartext: "ks1.<id>.<secret>" – id = 6 Byte Zufall (Base64url, 8 Zeichen, nur zum
 * Nachschlagen und fürs Protokoll), secret = 32 Byte Zufall (Base64url, 43 Zeichen).
 * Gespeichert wird nur SHA-256(secret) in /storage/tokens.json; der Klartext existiert
 * genau einmal bei der Erzeugung. Vergleich in konstanter Zeit.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"
#include "cJSON.h"
#include "config.h"

#define TOKEN_PREFIX      "ks1"
#define TOKEN_ID_LEN      8                              /* 6 Byte -> 8 Zeichen Base64url  */
#define TOKEN_SECRET_LEN  43                             /* 32 Byte -> 43 Zeichen Base64url */
#define TOKEN_PLAIN_LEN   (3 + 1 + TOKEN_ID_LEN + 1 + TOKEN_SECRET_LEN)   /* 56 */
#define TOKEN_NAME_LEN    32
#define TOKEN_IP_LEN      48

typedef struct {
    char     id[TOKEN_ID_LEN + 1];
    char     name[TOKEN_NAME_LEN];
    uint8_t  hash[32];               /* SHA-256 des Secrets                                  */
    uint32_t doors;                  /* Bitmaske, Bit n = Tür n+1                            */
    bool     can_open;               /* darf Türen öffnen                                    */
    bool     can_status;             /* darf Status lesen                                    */
    bool     enabled;
    char     allow_ip[TOKEN_IP_LEN]; /* "" = jede Quelle, sonst Adresse oder Präfix (10.0.0.0/8, fd00::/64) */
    int64_t  expires_at;             /* Unix-Sekunden, 0 = nie; nur geprüft bei gültiger Uhr */
    int64_t  created_at;
    int64_t  last_used;
    uint32_t use_count;
} api_token_t;

typedef enum {
    TOKEN_OK = 0,
    TOKEN_MALFORMED,                 /* kein "ks1.<id>.<secret>" mit passender Länge */
    TOKEN_UNKNOWN,                   /* id unbekannt oder Secret falsch              */
    TOKEN_DISABLED,
    TOKEN_EXPIRED,
    TOKEN_IP_DENIED,
} token_result_t;

esp_err_t      api_tokens_init(void);
size_t         api_tokens_count(void);
size_t         api_tokens_list(api_token_t *out, size_t max);
bool           api_tokens_get(const char *id, api_token_t *out);

/* Legt ein Token an (Felder name, doors, can_open, can_status, enabled, allow_ip, expires_at aus *t); vergibt id,
 * hash, created_at. plain_out (>= TOKEN_PLAIN_LEN + 1) erhält den Klartext – nur jetzt. */
esp_err_t      api_tokens_create(api_token_t *t, char *plain_out, size_t plain_len);
esp_err_t      api_tokens_update(const api_token_t *t);    /* id muss existieren; hash, created_at, Zähler bleiben */
esp_err_t      api_tokens_delete(const char *id);
esp_err_t      api_tokens_clear(void);

/* Prüft einen Klartext-Token gegen Hash, Status, Ablauf und Quell-IP. Bei TOKEN_OK werden
 * use_count/last_used erhöht; persist_use = true schreibt das auch in die Datei. */
token_result_t api_tokens_verify(const char *plain, const char *peer_ip, bool persist_use, api_token_t *out);
const char    *api_tokens_result_text(token_result_t r);
bool           api_tokens_valid_ip_rule(const char *rule);  /* "" erlaubt */

/* Rate-Limit für fehlgeschlagene Bearer-Versuche je Quell-IP (CFG_TOKEN_FAIL_LIMIT, Sperre verdoppelt sich) */
bool           api_tokens_rate_allowed(const char *peer_ip, uint32_t *retry_after_s);
void           api_tokens_rate_failed(const char *peer_ip);
void           api_tokens_rate_succeeded(const char *peer_ip);

cJSON         *api_tokens_entry_to_json(const api_token_t *t);   /* ohne Hash, für die Admin-UI */
