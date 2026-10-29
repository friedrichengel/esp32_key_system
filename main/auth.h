/*
 * auth.h – Login, Passwort-Hash (PBKDF2-HMAC-SHA256), Sessions, CSRF, Rate-Limit,
 *          Zwei-Faktor-Authentifizierung (TOTP RFC 6238 + 8 Recovery-Codes) je Benutzer
 *
 * Identitäten: Der Admin (uid 0) liegt im NVS-Blob (settings.admin / settings.twofa) und ist
 * das Bootstrap-Konto. Weitere Benutzer (uid 1..CFG_USERS_MAX) verwaltet users.c in
 * /storage/users.json. Jede Session trägt die uid; Rechte werden bei jedem Request frisch
 * aus den Benutzerdaten gelesen (auth_session_principal), damit Sperren sofort wirken.
 *
 * Strenge Variante: Ohne synchrone Uhrzeit (time_sync_is_valid() == false) werden keine
 * TOTP-Codes geprüft, der zweite Faktor ist dann nur per Recovery-Code möglich.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "settings.h"

#define AUTH_TOKEN_HEX_LEN   64      /* 32 Byte Zufall als Hex          */
#define AUTH_CSRF_HEX_LEN    32      /* 16 Byte                          */
#define AUTH_MAX_SESSIONS    8
#define AUTH_COOKIE_NAME     "ks_session"
#define AUTH_TICKET_HEX_LEN  32
#define AUTH_RECOVERY_LEN    11      /* "ABCDE-FGHJK"                    */
#define AUTH_B32_SECRET_LEN  (SETTINGS_TOTP_SECRET_LEN * 8 / 5 + 1)
#define AUTH_PW_ITERATIONS   10000
#define AUTH_PW_MIN_LEN      8
#define AUTH_NAME_LEN        32

#define AUTH_UID_ADMIN       0

/* Rechte eines Benutzers (Admin hat immer alle) */
#define USER_PERM_EDIT_SHARED  (1u << 0)   /* freigegebene Codes voll bearbeiten        */
#define USER_PERM_OPEN_DOORS   (1u << 1)   /* Türen per Web-UI öffnen (nur Türmaske)   */
#define USER_PERM_ALL          (USER_PERM_EDIT_SHARED | USER_PERM_OPEN_DOORS)
#define USER_TYPES_ALL         0x07        /* Bit = code_type_t: Dauer, Zeit, Einmal     */

/* Wer gerade handelt – aus Session + aktuellen Benutzerdaten abgeleitet */
typedef struct {
    uint8_t  uid;                    /* 0 = Admin aus NVS                                 */
    bool     is_admin;
    char     name[AUTH_NAME_LEN];
    uint8_t  code_types;             /* Bitmaske erlaubter Code-Typen beim Anlegen       */
    uint32_t doors;                  /* Türen, für die Codes angelegt / geöffnet werden   */
    uint8_t  perms;                  /* USER_PERM_*                                       */
    uint16_t max_codes;              /* eigenes Kontingent, 0 = nur Gesamtlimit           */
    bool     must_change_pw;         /* Startpasswort: erst Passwort ändern               */
} auth_principal_t;

typedef struct {
    char    token[AUTH_TOKEN_HEX_LEN + 1];
    char    csrf[AUTH_CSRF_HEX_LEN + 1];
    int64_t expires_us;
    uint8_t uid;
    char    user[AUTH_NAME_LEN];
} auth_session_t;

typedef enum {
    LOGIN_OK = 0,
    LOGIN_BAD_CREDENTIALS,
    LOGIN_DISABLED,          /* Passwort stimmt, Benutzer ist gesperrt */
} auth_login_result_t;

typedef enum {
    TWOFA_OK = 0,
    TWOFA_WRONG_CODE,
    TWOFA_TIME_INVALID,      /* strenge Variante: TOTP ohne synchrone Uhr nicht prüfbar */
    TWOFA_REPLAY,            /* Code wurde bereits verwendet                           */
    TWOFA_NOT_ENABLED,
} twofa_result_t;

esp_err_t auth_init(void);

/* Hash-Helfer (auch für users.c) */
int       auth_hash_password(const char *pw, const uint8_t *salt, size_t salt_len,
                             uint32_t iterations, uint8_t *out, size_t out_len);
bool      auth_const_time_eq(const uint8_t *a, const uint8_t *b, size_t n);
bool      auth_valid_username(const char *name);     /* 1..31 Zeichen, druckbar, ohne Leerzeichen */

/* Admin-Passwort (NVS); Name darf nicht mit einem Benutzer kollidieren */
esp_err_t auth_set_password(const char *user, const char *password);
bool      auth_setup_required(void);

/* Login: Admin oder Benutzer, konstante Antwortzeit auch bei unbekanntem Namen */
auth_login_result_t auth_login_check(const char *user, const char *password, auth_principal_t *p);
bool      auth_verify_principal_password(const auth_principal_t *p, const char *password);
bool      auth_principal_load(uint8_t uid, auth_principal_t *p);   /* false: unbekannt/gesperrt */

/* Login-Rate-Limit (gilt für Passwort- und 2FA-Versuche) */
bool      auth_login_allowed(uint32_t *retry_after_s);
void      auth_login_failed(void);
void      auth_login_succeeded(void);

/* Sessions */
auth_session_t *auth_session_create(const auth_principal_t *p);
auth_session_t *auth_session_find(const char *token);
void            auth_session_destroy(const char *token);
void            auth_sessions_clear(void);
void            auth_sessions_destroy_uid(uint8_t uid);
bool            auth_session_principal(const auth_session_t *s, auth_principal_t *p);

/* Zwischenschritt Passwort -> 2FA: Ticket (gültig CFG_TWOFA_TICKET_TTL_S, max. 5 Versuche) */
const char *auth_preauth_create(const auth_principal_t *p);
bool        auth_preauth_check(const char *ticket, auth_principal_t *p); /* zählt Versuch */
void        auth_preauth_consume(const char *ticket);

/* 2FA je Benutzer (uid 0 = Admin in NVS, sonst users.json) */
bool        auth_twofa_enabled(uint8_t uid);
int         auth_twofa_recovery_remaining(uint8_t uid);
esp_err_t   auth_twofa_begin_setup(uint8_t uid, char secret_b32[AUTH_B32_SECRET_LEN],
                                   char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1]);
esp_err_t   auth_twofa_confirm_setup(uint8_t uid, const char *totp_code);   /* aktiviert + speichert */
void        auth_twofa_cancel_setup(uint8_t uid);
esp_err_t   auth_twofa_disable(uint8_t uid);
esp_err_t   auth_twofa_regenerate_recovery(uint8_t uid, char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1]);

/* 2FA – Prüfung: akzeptiert TOTP (nur bei gültiger Zeit) oder Recovery-Code */
twofa_result_t auth_twofa_verify(uint8_t uid, const char *code, bool *used_recovery);

void      auth_random_hex(char *out, size_t nbytes);     /* out braucht 2*nbytes+1 */
