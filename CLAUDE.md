# esp32_key_system – CLAUDE.md

## Projektübersicht

Zutrittssystem auf dem **ESP32-S31-Function-CoreBoard-1** (Espressif, ESP32-S31, 16 MB Flash, 16 MB Octal-PSRAM, Gigabit-Ethernet über YT8531-PHY/RGMII). Ein Keypad mit RS232-Ausgang liefert Codes, die Firmware prüft sie gegen Dauer-, Zeit- und Einmalcodes und schaltet ein Relais für den Türöffner. Verwaltung über eine HTTPS-Webseite (IPv4 + IPv6), Firmware-Updates nur signiert. Plan und Entscheidungen: [PLAN.md](PLAN.md).

Mechanismen (Zertifikate, OTA-Signierung, Login/Session, Web-UI-Einbettung) folgen bewusst den Projekten Casambi_Gateway / Casambi_Logic, nur portiert von Arduino auf ESP-IDF.

## Build-System

- **Framework:** ESP-IDF 6.1 (kein Arduino, der S31 wird von arduino-esp32 noch nicht unterstützt), Sprache C.
- **PlatformIO:** Fork-Plattform `naelolaiz/platform-espressif32#esp32s31-support`, Board `esp32-s31-function-coreboard-1`, `framework = espidf`, `src_dir = main`. Das offizielle Plattformpaket kennt den S31 noch nicht (Issue #1777).
- **Alternativ:** natives ESP-IDF ≥ 6.1 mit `idf.py set-target esp32s31 && idf.py build`.
- **Konfiguration:** `sdkconfig.defaults` ist die Quelle; PlatformIO erzeugt daraus `sdkconfig.coreboard1` (gitignored). Nach Änderungen an den Defaults diese Datei löschen.
- **Partitionen:** `partitions.csv` (nvs 24 K, otadata, phy, coredump 64 K, ota_0/ota_1 je 4 MB, LittleFS `storage` 7 MB).

```bash
pio run                     # bauen (bettet Web-UI ein, signiert firmware.bin)
pio run -t upload           # flashen
pio device monitor          # 115200 Baud
```

### Build-Hooks (platformio.ini `extra_scripts`)

| Skript | Zweck |
|---|---|
| `pre:tools/pio_pre_build.py` | ruft `tools/embed_web_assets.py`: `main/web/*` → gzip → `main/web_assets_gen.h` (gitignored) |
| `post:tools/sign_firmware.py` | `--secure-pad-v2` beim Image-Bau, dann `espsecure sign-data` (RSA-3072, Secure-Boot-v2-Format), ersetzt `firmware.bin` in-place |

`main/CMakeLists.txt` ruft `embed_web_assets.py` zusätzlich per `execute_process`, damit auch `idf.py` die Web-UI einbettet.

## OTA-Signierung

- Modus: `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, Schema RSA-3072 (ECDSA wegen S31-Errata nicht). Kein Hardware-Secure-Boot, keine eFuses (Entscheidung 23.10.2026, evtl. später).
- Privater Schlüssel `keys/ota_signing_key.pem` (gitignored) oder Umgebungsvariable `OTA_SIGNING_KEY`. Erzeugen: `tools/gen_signing_key.sh`. **Schlüssel sichern**, Geräte akzeptieren nur damit signierte Images.
- `esp_ota_end()` prüft die Signatur gegen den öffentlichen Schlüssel im Signaturblock der laufenden App. Ein unsigniertes Image bootet per USB-Flash, kann aber keine OTA-Updates annehmen.
- Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; `ota_update.c` bestätigt die neue Firmware bei IP-Bezug oder nach `CFG_OTA_CONFIRM_DELAY_S`.

## Verzeichnisstruktur

```
main/
├── main.c              Boot-Reihenfolge, Heartbeat, BOOT-Taster-Werksreset, OTA-Bestätigung
├── config.h            ALLE Pins und Compile-Zeit-Konstanten (Pins sind Vorschläge, siehe PLAN.md §1)
├── settings.[ch]       Laufzeit-Konfiguration als NVS-Blob (Namespace "keysys")
├── event_log.[ch]      Ringpuffer (PSRAM) + Datei /storage/events.log mit Rotation
├── storage.[ch]        LittleFS unter /storage, atomares Schreiben (tmp + rename)
├── status_led.[ch]     WS2812 an GPIO60, Zustandsautomat mit Blinkmustern
├── time_sync.[ch]      SNTP (esp_netif_sntp), TZ, Flag "Zeit gültig"
├── net_eth.[ch]        EMAC RGMII + generischer PHY + YT8531-Workarounds, IPv4/IPv6, mDNS
├── cert_store.[ch]     /storage/webserver_cert.pem + _key.pem, EC-P-256 selbstsigniert (PSA), Upload-Validierung
├── auth.[ch]           PBKDF2-HMAC-SHA256 (PSA), 4 Sessions, CSRF, Login-Sperre,
│                       2FA: TOTP (HMAC-SHA1 via PSA, ±1 Schritt, Replay-Schutz) + 8 Recovery-Codes (SHA-256-Hashes),
│                       Pre-Auth-Tickets zwischen Passwort und zweitem Faktor; STRENG: ohne gültige Zeit nur Recovery-Code
├── web_server.[ch]     esp_https_server + Redirect-Server :80, REST-API, statische Dateien (gzip)
├── codes_db.[ch]       Codes als /storage/codes.json, Prüfung (Gültigkeit, Fenster, Nutzungen)
├── keypad_rs232.[ch]   UART1-Task, Rahmen-Parser (Terminator/feste Länge/Timeout), Rohdaten für Lernmodus
├── door_relay.[ch]     Relais-Impuls per esp_timer, Türkontakt/REX entprellt
├── access.[ch]         Keypad-Code → codes_check → door_open, Sperre nach Fehlversuchen
├── ota_update.[ch]     esp_ota_* mit Signaturprüfung, Rollback, Info
└── web/                index.html, app.js, style.css (Vanilla, deutsch), qrcode.js (vendored, MIT, Kazuhiko Arase)
tools/                  embed_web_assets.py, pio_pre_build.py, sign_firmware.py, gen_signing_key.sh, sign_firmware.sh
docs/                   hardware.md (Pins, J2-Header), api.md (REST-API)
keys/                   nur .gitkeep; private Schlüssel lokal
```

## Hardware (Vorschlag, gegen Schaltplan prüfen)

| Funktion | GPIO | Hinweis |
|---|---|---|
| Ethernet MDC/MDIO/PHY-Reset | 5 / 6 / 7 | RGMII-Daten GPIO8–19 fest (IDF-Default für S31) |
| Keypad UART1 RX/TX | 38 / 37 | über MAX3232 |
| Relais | 40 | Polarität in Settings (`relay_active_low`) |
| Türkontakt / Austaster | 42 / 39 | gegen GND, interner Pull-up |
| WS2812 / BOOT | 60 / 61 | onboard |

## Konventionen

- Pins und Grenzen nur in `config.h`; Laufzeitwerte nur über `g_settings` + `settings_save()`.
- Jede sicherheitsrelevante Aktion über `event_log(EV_*, ...)`; **Codes nie protokollieren** (nur Länge/Name).
- Schreibende API-Routen verlangen Session-Cookie **und** Header `X-CSRF-Token` (`require_auth_csrf`).
- Deutsch in UI und Protokolltexten, ASCII-Umschreibungen (ue/oe/ae) in Log-Strings, Umlaute in HTML.
- Compiler-Warnungen sind Fehler (`-Werror` im IDF-Default), insbesondere `format-truncation`.
- `SETTINGS_VERSION` bei Strukturänderungen am NVS-Blob erhöhen; ohne Migrationscode fällt das Gerät auf Defaults zurück (Stand v2, noch keine Geräte im Feld).
- Neue Web-Dateien in `main/CMakeLists.txt` (WEB_FILES), `tools/pio_pre_build.py` (FILES) und als Route in `web_server.c` eintragen.

## Stand

Phasen 0–5 aus PLAN.md sind implementiert und bauen fehlerfrei. Erster Hardwaretest 24.10.2026 (Board an COM4): Boot, Ethernet (1000 Mbit/s, DHCP, SLAAC), HTTPS über IPv4/IPv6 und mDNS laufen. Der RGMII-Sendetakt muss aus der APLL kommen (`CONFIG_ETH_EMAC_RGMII_TX_CLK_SRC_APLL`), weil die MPLL vom PSRAM belegt ist. Testnetz hat weder Internet noch NTP, die Uhr bleibt dort ungültig. Ungetestet: Keypad, Relais, OTA, 2FA, Browser-Login. Offene Punkte: PLAN.md §10 „Umsetzungsstand“ und §11.
