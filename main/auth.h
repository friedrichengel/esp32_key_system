/*
 * auth.h – Admin-Login, Passwort-Hash (PBKDF2-HMAC-SHA256), Sessions, CSRF, Rate-Limit,
 *          Zwei-Faktor-Authentifizierung (TOTP RFC 6238 + 8 Recovery-Codes)
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
#define AUTH_MAX_SESSIONS    4
#define AUTH_COOKIE_NAME     "ks_session"
#define AUTH_TICKET_HEX_LEN  32
#define AUTH_RECOVERY_LEN    11      /* "ABCDE-FGHJK"                    */
#define AUTH_B32_SECRET_LEN  (SETTINGS_TOTP_SECRET_LEN * 8 / 5 + 1)

typedef struct {
    char    token[AUTH_TOKEN_HEX_LEN + 1];
    char    csrf[AUTH_CSRF_HEX_LEN + 1];
    int64_t expires_us;
    char    user[32];
} auth_session_t;

typedef enum {
    TWOFA_OK = 0,
    TWOFA_WRONG_CODE,
    TWOFA_TIME_INVALID,      /* strenge Variante: TOTP ohne synchrone Uhr nicht prüfbar */
    TWOFA_REPLAY,            /* Code wurde bereits verwendet                           */
    TWOFA_NOT_ENABLED,
} twofa_result_t;

esp_err_t auth_init(void);

/* Passwort */
esp_err_t auth_set_password(const char *user, const char *password);
bool      auth_verify_password(const char *user, const char *password);
bool      auth_setup_required(void);

/* Login-Rate-Limit (gilt für Passwort- und 2FA-Versuche) */
bool      auth_login_allowed(uint32_t *retry_after_s);
void      auth_login_failed(void);
void      auth_login_succeeded(void);

/* Sessions */
auth_session_t *auth_session_create(const char *user);
auth_session_t *auth_session_find(const char *token);
void            auth_session_destroy(const char *token);
void            auth_sessions_clear(void);

/* Zwischenschritt Passwort -> 2FA: Ticket (gültig CFG_TWOFA_TICKET_TTL_S, max. 5 Versuche) */
const char *auth_preauth_create(const char *user);
bool        auth_preauth_check(const char *ticket, char *user_out, size_t user_len); /* zählt Versuch */
void        auth_preauth_consume(const char *ticket);

/* 2FA – Einrichtung (pending, bis ein gültiger TOTP-Code bestätigt) */
bool        auth_twofa_enabled(void);
esp_err_t   auth_twofa_begin_setup(char secret_b32[AUTH_B32_SECRET_LEN],
                                   char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1]);
esp_err_t   auth_twofa_confirm_setup(const char *totp_code);      /* aktiviert + speichert */
void        auth_twofa_cancel_setup(void);
esp_err_t   auth_twofa_disable(void);
esp_err_t   auth_twofa_regenerate_recovery(char recovery[SETTINGS_RECOVERY_COUNT][AUTH_RECOVERY_LEN + 1]);
int         auth_twofa_recovery_remaining(void);

/* 2FA – Prüfung: akzeptiert TOTP (nur bei gültiger Zeit) oder Recovery-Code */
twofa_result_t auth_twofa_verify(const char *code, bool *used_recovery);

void      auth_random_hex(char *out, size_t nbytes);     /* out braucht 2*nbytes+1 */
