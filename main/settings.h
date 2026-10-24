/*
 * settings.h – Laufzeit-Konfiguration in NVS (Namespace "keysys", Blob "cfg")
 *
 * Alles, was der Admin über die Webseite ändert, steht hier. Compile-Zeit-Werte
 * (Pins, Grenzen) stehen in config.h. Der Blob trägt eine Versionsnummer; bei
 * Versionssprung werden fehlende Felder mit Defaults belegt (settings_migrate).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

#define SETTINGS_VERSION        3               /* v3: doors[], keypad.door (24.10.2026) */
#define SETTINGS_TOTP_SECRET_LEN 20
#define SETTINGS_RECOVERY_COUNT 8
#define SETTINGS_STR_SHORT      32
#define SETTINGS_STR_HOST       64
#define SETTINGS_STR_IP4        16
#define SETTINGS_STR_IP6        48
#define SETTINGS_STR_TZ         64
#define SETTINGS_PW_HASH_LEN    32
#define SETTINGS_PW_SALT_LEN    16

typedef enum {
    IPV4_MODE_DHCP = 0,
    IPV4_MODE_STATIC = 1,
} ipv4_mode_t;

typedef struct {
    char     hostname[SETTINGS_STR_HOST];

    /* IPv4 */
    uint8_t  ipv4_mode;                     /* ipv4_mode_t */
    char     ipv4_addr[SETTINGS_STR_IP4];
    char     ipv4_mask[SETTINGS_STR_IP4];
    char     ipv4_gw[SETTINGS_STR_IP4];
    char     dns1[SETTINGS_STR_IP4];        /* leer = vom DHCP */
    char     dns2[SETTINGS_STR_IP4];

    /* IPv6 – Link-Local gibt es immer */
    bool     ipv6_slaac;                    /* Adressen aus Router Advertisements */
    bool     ipv6_static_enable;
    char     ipv6_addr[SETTINGS_STR_IP6];   /* statische globale/ULA-Adresse       */
    uint8_t  ipv6_prefix_len;               /* nur informativ (lwIP nutzt RA-Präfixe) */

    /* Zeit */
    char     ntp1[SETTINGS_STR_HOST];
    char     ntp2[SETTINGS_STR_HOST];
    char     tz[SETTINGS_STR_TZ];           /* POSIX-TZ-String */
} settings_network_t;

/* Eine Tür = ein Relais, optional Türkontakt und Austaster (Pins in config.h) */
typedef struct {
    bool     enabled;
    char     name[SETTINGS_STR_SHORT];
    uint16_t relay_pulse_ms;
    bool     relay_active_low;
    bool     door_contact_enable;
    bool     exit_button_enable;
} settings_door_t;

typedef enum {
    KEYPAD_FRAME_TERMINATOR = 0,            /* Code endet mit Abschlusszeichen      */
    KEYPAD_FRAME_FIXED_LEN  = 1,            /* feste Anzahl Ziffern                 */
    KEYPAD_FRAME_TIMEOUT    = 2,            /* Pause nach letztem Zeichen           */
} keypad_frame_mode_t;

typedef struct {
    uint32_t baud;
    uint8_t  parity;                        /* 0 = none, 1 = even, 2 = odd          */
    uint8_t  frame_mode;                    /* keypad_frame_mode_t                  */
    char     terminator;                    /* z. B. '#', '\r', '\n'                 */
    char     clear_key;                     /* z. B. '*' löscht die Eingabe, 0 = aus */
    uint8_t  fixed_len;
    uint16_t timeout_ms;
    bool     learn_mode;                    /* Rohdaten auf der System-Seite zeigen  */
    uint8_t  door;                          /* Index 0..CFG_MAX_DOORS-1 der Tür, die das Keypad öffnet */
} settings_keypad_t;

typedef struct {
    char     user[SETTINGS_STR_SHORT];
    uint8_t  pw_hash[SETTINGS_PW_HASH_LEN]; /* PBKDF2-HMAC-SHA256                   */
    uint8_t  pw_salt[SETTINGS_PW_SALT_LEN];
    uint32_t pw_iterations;
    bool     setup_done;                    /* false = Setup-Assistent erzwingen    */
} settings_admin_t;

typedef struct {
    bool     enabled;
    uint8_t  secret[SETTINGS_TOTP_SECRET_LEN];        /* TOTP-Geheimnis (RFC 6238, SHA-1, 30 s, 6 Stellen) */
    uint8_t  recovery_hash[SETTINGS_RECOVERY_COUNT][32]; /* SHA-256 der Recovery-Codes */
    uint8_t  recovery_used;                           /* Bitmaske verbrauchter Codes */
    int64_t  last_counter;                            /* Replay-Schutz: letzter akzeptierter Zeitschritt */
} settings_twofa_t;

typedef struct {
    uint32_t           version;
    settings_network_t net;
    settings_door_t    doors[CFG_MAX_DOORS];
    settings_keypad_t  keypad;
    settings_admin_t   admin;
    settings_twofa_t   twofa;
} settings_t;

/* Globale Instanz; nach settings_init() gültig. Schreibzugriff nur über settings_save(). */
extern settings_t g_settings;

esp_err_t settings_init(void);            /* lädt aus NVS oder legt Defaults an    */
esp_err_t settings_save(void);            /* schreibt g_settings nach NVS          */
void      settings_set_defaults(settings_t *s);
void      settings_door_defaults(settings_door_t *d, int idx);
esp_err_t settings_factory_reset(void);   /* löscht den NVS-Namespace              */
