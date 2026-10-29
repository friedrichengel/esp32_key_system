/*
 * config.h – zentrale Compile-Zeit-Konfiguration für esp32_key_system
 *
 * Alles, was hardware- oder einsatzabhängig ist und später angepasst wird, steht hier:
 * Pins, UART-Defaults, Relais-Grenzen, Web-Server-Ports, Dateipfade.
 * Laufzeit-Einstellungen (IP-Adressen, Codes, Passwort ...) liegen in NVS (settings.c).
 *
 * Board: ESP32-S31-Function-CoreBoard-1. Externe Pins am 24.10.2026 gegen den Schaltplan
 * geprüft (docs/hardware.md): GPIO35/36/37-49 sind auf J2 frei, Codec liegt auf GPIO50-57,
 * GPIO4 ist der PHY-Interrupt, GPIO0/1 tragen den Footprint des 32-kHz-Quarzes.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

/* ========================================================================
 * Onboard (fest durch das CoreBoard vorgegeben)
 * ====================================================================== */
#define CFG_PIN_LED_WS2812        GPIO_NUM_60   /* J2-6  adressierbare RGB-LED             */
#define CFG_PIN_BOOT_BUTTON       GPIO_NUM_61   /* J2-5  BOOT-Taster -> Werksreset         */
#define CFG_PIN_UART0_TX          GPIO_NUM_58   /* J2-3  Konsole (USB-UART-Bridge)         */
#define CFG_PIN_UART0_RX          GPIO_NUM_59   /* J2-4                                    */

/* Ethernet: YT8531-PHY über RGMII, Datenpins GPIO8-19 (IO-MUX-Set 0, fest) */
#define CFG_ETH_MDC_GPIO          GPIO_NUM_5
#define CFG_ETH_MDIO_GPIO         GPIO_NUM_6
#define CFG_ETH_PHY_RST_GPIO      GPIO_NUM_7
#define CFG_ETH_PHY_ADDR          0

/* ========================================================================
 * Extern angeschlossen (J2-Header)
 * ====================================================================== */
/* Genau ein Keypad je Gerät, RS232-Ausgang über MAX3232-Pegelwandler, fest einer Tür zugeordnet */
#define CFG_KEYPAD_UART_NUM       UART_NUM_1
#define CFG_KEYPAD_RX_GPIO        GPIO_NUM_38   /* J2-22 */
#define CFG_KEYPAD_TX_GPIO        GPIO_NUM_37   /* J2-23, optional (Quittung/Beep), sonst UART_PIN_NO_CHANGE */
#define CFG_KEYPAD_BAUD_DEFAULT   9600
#define CFG_KEYPAD_RX_BUF_SIZE    512
#define CFG_KEYPAD_MAX_CODE_LEN   12            /* Ziffern je Code                          */
#define CFG_KEYPAD_TIMEOUT_MS     1500          /* Eingabe gilt nach dieser Pause als fertig */
#define CFG_KEYPAD_IDLE_RESET_MS  10000         /* Teileingabe nach dieser Pause verwerfen   */
#define CFG_KEYPAD_RAW_BUF        256           /* Rohbytes für den Lernmodus                */

/* Türen: bis zu CFG_MAX_DOORS Relais, je Tür optional Türkontakt (Reed gegen GND) und
 * Austaster (REX, gegen GND), beide mit internem Pull-up. Tür 1 = ursprüngliche Pins.
 * Die Pins der Türen 2-4 liegen auf freien J2-Pins; GPIO2/3 bleiben als Reserve frei. */
#define CFG_MAX_DOORS             4
/*                                  Tür 1        Tür 2        Tür 3        Tür 4   */
#define CFG_DOOR_RELAY_GPIOS      { GPIO_NUM_40, GPIO_NUM_43, GPIO_NUM_46, GPIO_NUM_49 }  /* J2-19, 17, 16, 11 */
#define CFG_DOOR_CONTACT_GPIOS    { GPIO_NUM_42, GPIO_NUM_44, GPIO_NUM_47, GPIO_NUM_36 }  /* J2-20, 18, 13, 24 */
#define CFG_DOOR_REX_GPIOS        { GPIO_NUM_39, GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_35 }  /* J2-21, 15, 14, 25 */
#define CFG_RELAY_ACTIVE_LOW      0             /* Compile-Zeit-Default, bis NVS gelesen ist */
#define CFG_RELAY_PULSE_MS_DEFAULT 3000
#define CFG_RELAY_PULSE_MS_MIN    500
#define CFG_RELAY_PULSE_MS_MAX    30000         /* harte Obergrenze in der Firmware         */
#define CFG_INPUT_DEBOUNCE_MS     50

/* ========================================================================
 * Zutrittslogik
 * ====================================================================== */
#define CFG_CODES_MAX             200           /* Einträge in der Code-Datenbank            */
#define CFG_CODE_MIN_LEN          4
#define CFG_KEYPAD_FAIL_LIMIT     5             /* Fehlversuche bis zur Sperre              */
#define CFG_KEYPAD_LOCK_BASE_S    30            /* erste Sperre, verdoppelt sich            */
#define CFG_KEYPAD_LOCK_MAX_S     600

/* API-Token für Drittsysteme (Hausautomation), siehe PLAN.md §8a */
#define CFG_TOKENS_MAX            8
#define CFG_TOKEN_FAIL_LIMIT      10            /* Fehlversuche je Quell-IP bis zur Sperre  */
#define CFG_TOKEN_LOCK_BASE_S     60            /* erste Sperre, verdoppelt sich            */
#define CFG_TOKEN_LOCK_MAX_S      900

/* ========================================================================
 * Netzwerk / Web
 * ====================================================================== */
#define CFG_HOSTNAME_DEFAULT      "keysystem"
/* Feste Adresse: Vorgabe für den statischen Modus UND Rückfall, wenn DHCP keine Adresse liefert */
#define CFG_IPV4_FALLBACK_ADDR    "192.168.1.50"
#define CFG_IPV4_FALLBACK_MASK    "255.255.255.0"
#define CFG_IPV4_FALLBACK_GW      "192.168.1.1"
#define CFG_IPV4_FALLBACK_DNS     "192.168.1.1"
#define CFG_DHCP_FALLBACK_TIMEOUT_S 20          /* nach Link-up ohne DHCP-Lease -> Fallback   */
#define CFG_HTTPS_PORT            443
#define CFG_HTTP_REDIRECT_PORT    80
#define CFG_HTTPS_MAX_SOCKETS     4             /* ~40 KB RAM je TLS-Socket                 */
#define CFG_SESSION_TIMEOUT_S     1800
#define CFG_LOGIN_FAIL_LIMIT      5
#define CFG_TWOFA_TICKET_TTL_S    300           /* Zeit zwischen Passwort und 2FA-Code        */
#define CFG_TWOFA_WINDOW          1             /* akzeptierte Zeitschritte +/- (30 s je Schritt) */
#define CFG_TWOFA_SETUP_TTL_S     600           /* unbestätigte Einrichtung verfällt danach   */
#define CFG_TWOFA_ISSUER          "KeySystem"
#define CFG_NTP_SERVER_1_DEFAULT  "pool.ntp.org"
#define CFG_NTP_SERVER_2_DEFAULT  "time.cloudflare.com"
#define CFG_TZ_DEFAULT            "CET-1CEST,M3.5.0,M10.5.0/3"

/* ========================================================================
 * Dateisystem (LittleFS-Partition "storage")
 * ====================================================================== */
#define CFG_FS_BASE_PATH          "/storage"
#define CFG_FS_PARTITION_LABEL    "storage"
#define CFG_CERT_PATH             CFG_FS_BASE_PATH "/webserver_cert.pem"
#define CFG_KEY_PATH              CFG_FS_BASE_PATH "/webserver_key.pem"
#define CFG_LOG_PATH              CFG_FS_BASE_PATH "/events.log"
#define CFG_CODES_PATH            CFG_FS_BASE_PATH "/codes.json"
#define CFG_TOKENS_PATH           CFG_FS_BASE_PATH "/tokens.json"
#define CFG_CERT_ORG_NAME         "esp32_key_system"

/* ========================================================================
 * System
 * ====================================================================== */
#define CFG_FACTORY_RESET_HOLD_MS 10000         /* BOOT-Taster so lange halten              */
#define CFG_OTA_CONFIRM_DELAY_S   60            /* neue Firmware gilt nach Netz-Start als ok */
#define CFG_EVENT_LOG_CAPACITY    500           /* Einträge im RAM-Ringpuffer (PSRAM)       */
#define CFG_EVENT_LOG_FILE_MAX    (256 * 1024)  /* Protokolldatei rotiert ab dieser Größe   */
#define CFG_LED_BRIGHTNESS        40            /* 0-255, WS2812 ist sehr hell              */
