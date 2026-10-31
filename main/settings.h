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

#define SETTINGS_VERSION        6               /* v6: wifi, v5: door_ext[] (Rolltor), v4: site, v3: doors[], keypad.door */
#define SETTINGS_TOTP_SECRET_LEN 20
#define SETTINGS_RECOVERY_COUNT 8
#define SETTINGS_STR_SHORT      32
#define SETTINGS_STR_HOST       64
#define SETTINGS_STR_IP4        16
#define SETTINGS_STR_IP6        48
#define SETTINGS_STR_TZ         64
#define SETTINGS_PW_HASH_LEN    32
#define SETTINGS_PW_SALT_LEN    16
#define SETTINGS_STR_EMAIL      64
#define SETTINGS_STR_INFO       512
#define SETTINGS_WIFI_SSID_LEN  33              /* 32 Byte + NUL                          */
#define SETTINGS_WIFI_PASS_LEN  65              /* WPA2-PSK: 8..63 Zeichen + NUL           */

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
    uint16_t relay_pulse_ms;                /* Impulsdauer; Rolltor: je Fahrbefehl        */
    bool     relay_active_low;              /* gilt für alle Relais dieser Tür            */
    bool     door_contact_enable;
    bool     exit_button_enable;
} settings_door_t;

typedef enum {
    DOOR_KIND_OPENER = 0,                   /* Türöffner: ein Relais, Impuls = auf        */
    DOOR_KIND_GATE   = 1,                   /* Rolltor: zwei Relais, Auf und Ab           */
} door_kind_t;

/* Erweiterung je Tür (ab v5, eigenes Array, damit der alte Blob ein Präfix bleibt).
 * Die CFG_MAX_DOORS Relais-Pins bilden einen Pool (Relais 1..4). Ein Türöffner nutzt fest
 * das Relais mit seinem Index, ein Rolltor die hier gewählten zwei; jedes Relais darf nur
 * einer aktiven Tür gehören (door_settings_check). */
typedef struct {
    uint8_t  kind;                          /* door_kind_t                                */
    uint8_t  relay_up;                      /* Relaisindex 0..CFG_MAX_DOORS-1 für "Auf"   */
    uint8_t  relay_down;                    /* Relaisindex für "Ab"                       */
    uint8_t  reserved;
} settings_door_ext_t;

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

/* Anzeige auf der Loginseite – ohne Anmeldung für jeden im Netz sichtbar! */
typedef struct {
    char     name[SETTINGS_STR_SHORT];                /* Anlagenbezeichnung, leer = nur Hostname */
    char     support_email[SETTINGS_STR_EMAIL];       /* Systembetreuer                          */
    char     info_text[SETTINGS_STR_INFO];            /* mehrzeiliger Text, z. B. Adresse        */
} settings_site_t;

/* WLAN statt Kabel (ab v6). Ab Werk AUS: Das Gerät nutzt Ethernet, bis der Admin WLAN gezielt
 * einschaltet. Ist WLAN aktiv, wird der Ethernet-Treiber nicht gestartet; IP-, Hostname- und
 * NTP-Einstellungen aus settings_network_t gelten dann für die WLAN-Schnittstelle. */
typedef struct {
    bool     enabled;
    char     ssid[SETTINGS_WIFI_SSID_LEN];
    char     password[SETTINGS_WIFI_PASS_LEN];        /* leer = offenes Netz                     */
} settings_wifi_t;

/* Neue Felder nur am Ende anhängen: settings_init() übernimmt ältere Blobs als Präfix. */
typedef struct {
    uint32_t           version;
    settings_network_t net;
    settings_door_t    doors[CFG_MAX_DOORS];
    settings_keypad_t  keypad;
    settings_admin_t   admin;
    settings_twofa_t   twofa;                         /* 2FA des Admins; Benutzer: users.json   */
    settings_site_t    site;                          /* ab v4                                   */
    settings_door_ext_t door_ext[CFG_MAX_DOORS];      /* ab v5: Türtyp, Rolltor-Relais           */
    settings_wifi_t    wifi;                          /* ab v6: WLAN statt Ethernet, Default aus */
} settings_t;

/* Globale Instanz; nach settings_init() gültig. Schreibzugriff nur über settings_save(). */
extern settings_t g_settings;

esp_err_t settings_init(void);            /* lädt aus NVS oder legt Defaults an    */
esp_err_t settings_save(void);            /* schreibt g_settings nach NVS          */
void      settings_set_defaults(settings_t *s);
void      settings_door_defaults(settings_door_t *d, int idx);
void      settings_door_ext_defaults(settings_door_ext_t *e, int idx);
esp_err_t settings_factory_reset(void);   /* löscht den NVS-Namespace              */
