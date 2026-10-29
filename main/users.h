/*
 * users.h – Benutzer zusätzlich zum Admin (PLAN.md §8b)
 *
 * Bis zu CFG_USERS_MAX Benutzer (uid 1..CFG_USERS_MAX) in /storage/users.json: Name,
 * PBKDF2-Hash, Rolle, Rechte (erlaubte Code-Typen, Türmaske, Freigaben bearbeiten,
 * Türen öffnen), Kontingent, optional eigene 2FA. Der Admin (uid 0) bleibt im NVS-Blob.
 * Codes tragen owner_uid und eine Freigabemaske (Bit uid), siehe codes_db.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
#include "config.h"
#include "settings.h"
#include "auth.h"

#define USERS_SHARE_MASK   (((1u << (CFG_USERS_MAX + 1)) - 1) & ~1u)   /* Bits 1..CFG_USERS_MAX */
#define USERS_UID_BIT(uid) (1u << (uid))

typedef enum {
    USER_ROLE_USER  = 0,
    USER_ROLE_ADMIN = 1,
} user_role_t;

typedef struct {
    uint8_t  uid;                           /* 1..CFG_USERS_MAX                          */
    char     name[AUTH_NAME_LEN];
    uint8_t  pw_hash[SETTINGS_PW_HASH_LEN];
    uint8_t  pw_salt[SETTINGS_PW_SALT_LEN];
    uint32_t pw_iterations;
    uint8_t  role;                          /* user_role_t                               */
    bool     enabled;
    bool     must_change_pw;
    uint8_t  code_types;                    /* Bitmaske code_type_t                      */
    uint32_t doors;                         /* Bitmaske Tür n = Bit n-1                  */
    uint8_t  perms;                         /* USER_PERM_*                               */
    uint16_t max_codes;                     /* 0 = nur Gesamtlimit                       */
    int64_t  created_at;
    char     created_by[AUTH_NAME_LEN];
    int64_t  last_login;
    settings_twofa_t twofa;
} user_t;

esp_err_t users_init(void);
size_t    users_count(void);
size_t    users_list(user_t *out, size_t max);
bool      users_get(uint8_t uid, user_t *out);
bool      users_get_by_name(const char *name, user_t *out);
/* Name frei? Prüft Benutzerliste (ohne except_uid) und den Admin-Namen (außer except_uid == 0). */
bool      users_name_available(const char *name, int except_uid);

/* Anlegen: vergibt uid, hasht password; Fehler: INVALID_ARG (Name/Passwort), INVALID_STATE (Name belegt), NO_MEM (voll) */
esp_err_t users_create(user_t *u, const char *password);
/* Ändert Name, Rolle, aktiv, Rechte, Kontingent, must_change_pw – nicht Passwort/2FA/Zähler */
esp_err_t users_update(const user_t *u);
esp_err_t users_set_password(uint8_t uid, const char *password, bool must_change);
esp_err_t users_delete(uint8_t uid);
esp_err_t users_clear(void);

bool      users_verify_password(const user_t *u, const char *password);
void      users_touch_login(uint8_t uid);
bool      users_twofa_get(uint8_t uid, settings_twofa_t *t);
esp_err_t users_twofa_set(uint8_t uid, const settings_twofa_t *t);
void      users_fill_principal(const user_t *u, auth_principal_t *p);

/* JSON: API (ohne Geheimnisse), Backup (mit Hash und 2FA), Restore (ersetzt alle Benutzer) */
cJSON    *users_entry_to_json(const user_t *u);
cJSON    *users_to_json(bool with_secrets);
esp_err_t users_from_json(const cJSON *arr);
