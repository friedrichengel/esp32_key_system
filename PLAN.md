# esp32_key_system – Umsetzungsplan

Zutrittssystem auf Basis **ESP32-S31-Function-CoreBoard-1**: Keypad mit RS232-Ausgabe → Code-Prüfung (Dauer- und Zeitcodes) → Relais öffnet die Tür. Verwaltung über eine HTTPS-Webseite (IPv4 + IPv6), signierte Firmware-Updates.

Stand: 23.10.2026. Dieses Dokument ist der Plan, noch keine fertige Implementierung. Offene Entscheidungen stehen in Abschnitt 11.

---

## 1. Hardware-Befund (Board)

Das „Function-CoreBoard1“ ist das **ESP32-S31-Function-CoreBoard-1** von Espressif (Chip ESP32-S31, nicht „S1“). Der Chip ist seit Mai 2026 auf dem Markt, die Toolchain-Unterstützung ist noch frisch. Fakten aus Espressif-Doku und ESP-IDF-Quellen:

| Punkt | Befund |
|---|---|
| SoC | ESP32-S31, Dual-Core RISC-V 320 MHz, 512 KB HP-SRAM |
| Modul | ESP32-S31-WROOM-3: 16 MB Flash, 16 MB PSRAM |
| Netzwerk | Gigabit-Ethernet (RJ45) über **YT8531-PHY per RGMII**, zusätzlich Wi-Fi 6, BLE 5.4, 802.15.4 |
| Ethernet-Pins | MDC = GPIO5, MDIO = GPIO6, PHY-Reset = GPIO7; RGMII-Datenpins GPIO8–19 (IO-MUX-Set 0 laut `esp_hal_emac/esp32s31/emac_periph.c`) |
| USB | Type-C J1 USB-Serial/JTAG (nativ, GPIO33/34, meldet sich als VID 303A:1001), Type-C J3 USB-UART-Bridge CH9102X (Konsole GPIO58/59, Auto-Program über DTR/RTS), USB-A Host (TPS2051, 500 mA) |
| Sonstiges | WS2812-LED an GPIO60 (über Pegelwandler Q1, 5 V), BOOT an GPIO61, ES8311-Audiocodec an **GPIO50–57** (I2C SCL 50 / SDA 51, I2S MCLK 52, SCLK 53, ASDOUT 54, LRCK 55, DSDIN 56, PA_CTRL 57), PHY-Interrupt ETH_INTN an **GPIO4**, Footprint für 32-kHz-Quarz an GPIO0/GPIO1 (unbestückt), SDIO-Pins GPIO20–25 auf J2 |
| Frei auf J2 (laut Schaltplan) | GPIO2, 3, 35–40, 42–49, 20–25 (SDIO); GPIO0/1 nur ohne 32-kHz-Quarz, GPIO4 ist PHY-Interrupt, 60/61 belegt. GPIO50–57 (Codec) liegen nicht auf J2 |
| Framework | **Nur ESP-IDF ≥ 6.1**. Arduino-Core gibt es für den S31 noch nicht (kommt erst mit arduino-esp32 4.0). |
| Sicherheits-Errata | ECDSA-Secure-Boot auf dem S31 fehlerhaft → **RSA-3072** für Signaturen verwenden |

Konsequenzen:
- Pinzuweisungen unten sind am 24.10.2026 **gegen den Schaltplan geprüft** (siehe Quellen): Keypad/Relais/Kontakte auf GPIO37–42 kollidieren weder mit Codec (GPIO50–57) noch mit Ethernet (GPIO4–19). Alle Pins liegen weiterhin zentral in `main/config.h`.
- PHY-Straps laut Schaltplan (Default-Bestückung): PHYAD[2:0] = 000 → PHY-Adresse 0, RXDLY Pull-up (2 ns RX-Delay hardwareseitig an), TXDLY Pull-down, PLLOFF aus. Die Software setzt die Delays zusätzlich über die Extended-Register; Link 1000 Mbit/s und Datenverkehr funktionieren damit.
- Ein reines Arduino-Projekt (wie bei `esp32_usb_reader`) geht nicht. Wir arbeiten mit **ESP-IDF 6.1 in C**, Projektstruktur so, dass sie mit `idf.py` **und** mit PlatformIO (`framework = espidf`) baut.

### Pinbelegung (gegen Schaltplan geprüft 24.10.2026)

| Funktion | GPIO | J2-Pin | Bemerkung |
|---|---|---|---|
| Keypad UART1 RX | 38 | 22 | über MAX3232 (RS232 ±12 V → 3,3 V) |
| Keypad UART1 TX | 37 | 23 | optional (Keypad-Quittung/Beep), sonst frei lassen |
| Relais-Ansteuerung | 40 | 19 | Low beim Boot, kein Strapping-Pin |
| Türkontakt (Reed, Eingang) | 42 | 20 | optional, Pull-up, Logging „Tür offen/zu“ |
| Austaster innen (REX) | 39 | 21 | optional, löst Relais direkt aus |
| Status-LED | 60 | 6 | WS2812 onboard |
| Werksreset | 61 | 5 | BOOT-Taster 10 s halten |

Externe Beschaltung:
- **RS232-Pegelwandler** MAX3232-Modul (3,3 V). RS232 direkt am ESP zerstört den Pin.
- **Relaismodul** mit Optokoppler (5 V-Versorgung vom J2, Steuerpegel 3,3 V, meist Active-Low → im Code konfigurierbar) oder MOSFET + Relais + Freilaufdiode. Der Schließer (NO) liegt in Serie zum Türöffner (typisch 12 V AC/DC), galvanisch getrennt vom ESP.
- Eigene Versorgung 5 V, idealerweise mit kleiner USV, sonst ist der Türöffner bei Stromausfall tot.

---

## 2. Toolchain / Projektaufbau

**Problem:** Das offizielle PlatformIO-Plattformpaket `espressif32` kennt den ESP32-S31 noch nicht (Issue platformio/platform-espressif32 #1777, offen). Es gibt:
1. **Fork-Branch** `naelolaiz/platform-espressif32#esp32s31-support` mit Board-Manifest `esp32-s31-function-coreboard-1` (ESP-IDF 6.1, Toolchain riscv32 15.2, esptool ≥ 5.3). Hello-World wurde laut Issue auf genau diesem Board getestet.
2. **pioarduino** liefert ein ESP-IDF-6.1-Paket (`pioarduino/esp-idf` Release v6.1.0), aber kein S31-Board.
3. **Natives ESP-IDF 6.1** über `idf.py` / VS-Code-ESP-IDF-Extension – der verlässlichste Weg bei einem so neuen Chip.

**Entscheidung:** Projekt im **ESP-IDF-Standardlayout** (`CMakeLists.txt`, `main/`, `components/`, `sdkconfig.defaults`, `partitions.csv`). `platformio.ini` zeigt auf den Fork-Branch mit `src_dir = main`, sodass beides funktioniert. Erster Meilenstein ist das Bring-up (LED blinkt) über beide Wege. Wenn der Fork hakt, bleibt `idf.py` der Build-Weg, und PlatformIO wird nachgezogen, sobald das offizielle Paket den S31 enthält.

**Stand 23.10.2026 – Phase 0 (Build) erledigt:** Der Fork-Branch funktioniert. PlatformIO 6.1.19 hat ESP-IDF 6.1.0 (`framework-espidf 4.60100.0`), RISC-V-Toolchain 15.2, esptool 5.4, CMake und Ninja selbst installiert; das Gerüst baut in ca. 3 min (Erstbuild) und erzeugt ein signiertes ESP32-S31-Image. Erkenntnisse:
- PlatformIO legt die generierte Konfiguration als `sdkconfig.coreboard1` ab (gitignored). Nach Änderungen an `sdkconfig.defaults` diese Datei löschen, damit sie neu erzeugt wird.
- PSRAM wird automatisch als 16 MB Octal / 200 MHz erkannt; Partitionstabelle wird 64-KB-aligned erzeugt (ota_0 @ 0x30000, ota_1 @ 0x430000, LittleFS 7 MB).
- Der PlatformIO-Builder signiert App-Images nur bei Hardware-Secure-Boot. Für unseren Modus übernimmt `tools/sign_firmware.py` (Post-Build-Hook) das Secure-Padding und die RSA-3072-Signatur; die Signatur wurde mit `espsecure verify-signature` geprüft.
- mbedTLS-X.509-Write (`CONFIG_MBEDTLS_X509_CREATE_C`, `CONFIG_MBEDTLS_X509_CRT_WRITE_C`) ist in IDF 6.1 per Default aus und wurde in `sdkconfig.defaults` aktiviert.
- Noch offen in Phase 0: Flashen und Boot-Test, sobald das Board angeschlossen ist.

### Verzeichnisstruktur (Ziel)

```
esp32_key_system/
├── platformio.ini            PlatformIO-Env (Fork-Plattform, espidf)
├── CMakeLists.txt            ESP-IDF-Projekt
├── sdkconfig.defaults        IPv6, HTTPS-Server, signierte Apps, PSRAM, Partitionstabelle
├── partitions.csv            16 MB: nvs, otadata, phy, coredump, ota_0/ota_1 (je 4 MB), storage (LittleFS)
├── main/
│   ├── CMakeLists.txt        registriert Quellen + eingebettete Web-Dateien
│   ├── idf_component.yml     Abhängigkeiten (mdns, littlefs, led_strip)
│   ├── config.h          alle GPIOs an einer Stelle
│   ├── main.c                Boot-Reihenfolge, Tasks starten
│   ├── settings.[ch]         NVS-Konfiguration (Netz, Zert, Relais, Keypad, Admin)
│   ├── net_eth.[ch]          RGMII/YT8531-Init, IPv4/IPv6, Hostname, mDNS, Events
│   ├── time_sync.[ch]        SNTP, Zeitzone, „Zeit gültig?“-Flag
│   ├── cert_store.[ch]       TLS-Zert/Key laden, selbstsigniert erzeugen, Upload übernehmen
│   ├── web_server.[ch]       esp_https_server, Routen, HTTP→HTTPS-Redirect
│   ├── auth.[ch]             Login, Sessions, CSRF, Passwort-Hash, Rate-Limit
│   ├── api_*.c               REST-Handler je Bereich (codes, network, cert, firmware, system, log)
│   ├── codes_db.[ch]         Code-Speicher + Gültigkeitsprüfung
│   ├── keypad_rs232.[ch]     UART-Task, Rahmen-Parser, Lernmodus
│   ├── door_relay.[ch]       Relais-Impuls (Timer), Türkontakt, REX
│   ├── ota_update.[ch]       Upload → esp_ota_*, Signaturprüfung, Rollback
│   ├── event_log.[ch]        Ringpuffer + persistente Protokolldatei
│   └── web/                  index.html, app.js, style.css (gzip, eingebettet)
├── tools/
│   ├── gen_signing_key.sh    RSA-3072-Schlüssel erzeugen (einmalig, bleibt außerhalb Git)
│   └── sign_firmware.sh      Build-Binary signieren / Signatur prüfen
├── keys/                     nur .gitkeep – private Schlüssel nie einchecken
└── docs/                     Hardware-Notizen, API-Beschreibung, Testplan
```

---

## 3. Netzwerk (Ethernet, IPv4/IPv6)

- **Ethernet primär** über internen EMAC + RGMII + generischen PHY-Treiber (`esp_eth_phy_new_generic`). Nötige YT8531-Besonderheiten (aus dem ESP-IDF-Beispiel `ethernet/basic`): nach dem Reset Autonegotiation per `ETH_CMD_S_AUTONEGO` wieder einschalten und RX/TX-Clock-Delay ≈ 2 ns über die Extended-Register 0x1E/0x1F setzen (Reg 0xA001 Bit 8 für RX, Reg 0xA003 Delay-Stufe 13 für TX). Ohne das kommen keine Pakete durch.
- **IPv4:** DHCP (Default) oder statisch (IP, Maske, Gateway, DNS1/DNS2).
- **IPv6:** Link-Local immer; SLAAC (`CONFIG_LWIP_IPV6_AUTOCONFIG`) ein/aus; zusätzlich **statische IPv6-Adresse** + Präfixlänge + Gateway; DNS via RDNSS oder stateless DHCPv6 (`CONFIG_LWIP_IPV6_DHCP6`). Stateful DHCPv6 bietet lwIP nicht, das wird auf der Admin-Seite so benannt.
- **Hostname + mDNS** (`<hostname>.local`), damit das Gerät ohne feste IP gefunden wird.
- **Zeit:** SNTP (2 Server konfigurierbar, IPv6-fähig), Zeitzone als POSIX-TZ-String (Default `CET-1CEST,M3.5.0,M10.5.0/3`). Ein Flag „Zeit gültig“ steuert, ob Zeitcodes akzeptiert werden.
- Wi-Fi: nicht in Phase 1. Kann als Fallback nachgerüstet werden (Hardware vorhanden).

---

## 4. HTTPS-Webserver

- `esp_https_server` (mbedTLS), TLS ≥ 1.2, 4 parallele TLS-Sockets (≈ 40 KB RAM je Socket → mbedTLS-Puffer in PSRAM: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`).
- Lauscht dual-stack (IPv6-Socket nimmt IPv4-mapped an); Port 443, dazu Port 80 nur als Redirect auf HTTPS.
- **Zertifikat – gleiches Verfahren wie Casambi_Gateway / Casambi_Logic** (`network_webserver_cert.cpp`, Entscheidung 23.10.2026):
  - Zwei Dateien `/webserver_cert.pem` und `/webserver_key.pem` auf der Datenpartition (hier LittleFS statt SPIFFS).
  - Beim Boot `ensure_persistent_cert()`: Liegt ein gültiges Paar vor (hochgeladen oder früher erzeugt), wird es verwendet. Sonst wird einmalig ein **gerätespezifisches, selbstsigniertes EC-P-256-Zertifikat** per mbedTLS erzeugt (CN = `<hostname>-<mac>`, O = „esp32_key_system“, feste Gültigkeit 2025–2045, Zufalls-Seriennummer, Subject-Key-Identifier, Task-WDT wird während der Erzeugung gefüttert). Schlägt die Erzeugung fehl, werden Halbfertig-Dateien gelöscht und der Server startet **HTTP-only** als Notfallmodus.
  - Admin-Seite „Zertifikat“ wie das Casambi-Upload-Formular: getrennter Upload von **Zertifikat** (`.pem/.crt/.cer`) und **Key** (`.pem/.key`), Status-Badge je Datei („hochgeladen, n Byte“ / „gerätespezifisches Zertifikat wird beim Boot erzeugt“), Hinweis „Neustart nötig, Server hält das laufende Zertifikat im RAM“. Zusätzlich: Fingerprint anzeigen, Button „Zertifikat neu erzeugen“ (löscht beide Dateien, Reboot), Download des aktuellen Zertifikats zum Import in den Browser-Truststore.
  - Upload-Validierung vor dem Speichern (PEM-Parse mit mbedTLS, Key passt zum Zertifikat), damit kein kaputtes Paar den HTTPS-Start blockiert.
  - Werksreset und Konfig-Export/Import: Zertifikat und Key werden gelöscht bzw. **nicht** exportiert (wie beim Gateway).
  - Kein ACME/Let's Encrypt im Gerät; wer ein CA-signiertes Zertifikat will, lädt es hoch.
- Web-UI als statische Dateien (HTML/CSS/Vanilla-JS, gzip, per `EMBED_FILES` in die Firmware), Daten über JSON-REST-API. Kein Framework-Overhead, kein Build-Schritt für die UI nötig.
- Live-Protokoll per WebSocket (`CONFIG_HTTPD_WS_SUPPORT`).

### Seiten
1. **Login**
2. **Übersicht:** Netzstatus (IPv4/IPv6/Link-Speed), Uhrzeit/NTP-Status, Türkontakt, letzte Ereignisse, Button „Tür öffnen“
3. **Codes:** Liste (Name, Typ, gültig von/bis, Nutzungen, zuletzt verwendet, aktiv), Anlegen/Bearbeiten/Löschen
4. **Protokoll:** Zutritte, abgelehnte Codes, Logins, Konfigänderungen, OTA; Export als CSV
5. **Netzwerk:** Hostname, IPv4 (DHCP/statisch), IPv6 (SLAAC/statisch), DNS, NTP, Zeitzone
6. **Zertifikat:** siehe oben
7. **Firmware:** laufende Version, Partition, Upload signierter Binary, Fortschritt, Rollback-Status
8. **System:** Admin-Passwort, Relais-Impulsdauer, Keypad-Parameter + Lernmodus, Backup/Restore der Konfiguration (JSON, ohne Key-Material), Neustart, Werksreset

---

## 5. Signierte Firmware-Updates

- ESP-IDF-Option **„Signed App Verification without Hardware Secure Boot“** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`), Schema **RSA-3072-PSS** (ECDSA wegen S31-Errata nicht). Der öffentliche Schlüssel steckt im Signaturblock der laufenden App; `esp_ota_end()` lehnt ein unsigniertes oder falsch signiertes Image ab. Es werden keine eFuses gebrannt, der Vorgang ist reversibel.
- Der Build signiert automatisch (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES`, Schlüssel `keys/ota_signing_key.pem`, steht in `.gitignore`). Alternativ nachträglich mit `tools/sign_firmware.sh`.
- Ablauf im Gerät: Upload über HTTPS (POST `/api/firmware`, Chunks direkt in `esp_ota_write`) → Signaturprüfung → Boot-Partition umschalten → Neustart → neue App meldet sich nach erfolgreichem Netz-Start „gültig“ (`esp_ota_mark_app_valid_cancel_rollback`), sonst **automatischer Rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
- Zwei OTA-Slots à 4 MB. Konfiguration liegt in NVS und bleibt über Updates erhalten; Versionsmigration im `settings`-Modul.
- **Entscheidung 23.10.2026: kein Hardware-Secure-Boot (eFuse) vorerst.** Es bleibt bei der reversiblen Signaturprüfung oben. Der Umstieg auf Secure Boot v2 + Flash-Verschlüsselung ist später ohne Umbau der Signaturkette möglich, weil derselbe RSA-3072-Schlüssel und derselbe Signaturblock weiterverwendet werden; es kommt nur das Brennen der eFuses dazu (irreversibel, JTAG gesperrt).
- Build-Workflow wie bei Casambi_Gateway/Casambi_Logic: privater Schlüssel im Projektroot bzw. über Umgebungsvariable `OTA_SIGNING_KEY` (gitignored), CI signiert mit Secret, unsignierte Images werden immer abgelehnt. Unterschied: statt des eigenen `CSIG`-Trailers mit ECDSA-P-256 nutzen wir den ESP-IDF-eigenen Signaturblock, weil der S31 unter ESP-IDF läuft und `esp_ota_end()` die Prüfung mitbringt (weniger Eigencode, Rollback integriert). Ein separates Schlüsselpaar für dieses Projekt, nicht das der Casambi-Geräte.

---

## 6. Keypad (RS232)

- UART1, Default 9600 8N1 (Baud/Parität einstellbar), RX über MAX3232.
- Da das Keypad-Modell noch nicht feststeht, bekommt der Parser konfigurierbare Rahmen: Abschlusszeichen (CR, LF, `#`, `*`), feste Länge oder Timeout (z. B. 1,5 s ohne Zeichen = Eingabe fertig), Lösch-Taste, optionaler Rahmen-Prefix/Suffix für Keypads mit Protokoll-Overhead.
- **Lernmodus** auf der System-Seite: Rohbytes der letzten Eingaben als Hex/ASCII anzeigen, um das Protokoll des Keypads zu ermitteln.
- Schutz: nach 5 Fehlversuchen 30 s Sperre, danach verdoppelnd bis 10 min; LED/Keypad-Beep (wenn TX genutzt wird) als Rückmeldung.

---

## 7. Codes und Zutrittslogik

Datensatz je Code:
```
id, name, code (4–12 Ziffern), typ, aktiv,
gueltig_von, gueltig_bis (UTC, optional),
wochentage (Bitmaske), zeitfenster_von/bis (Uhrzeit, optional),
max_nutzungen (0 = unbegrenzt), nutzungen, zuletzt_verwendet, angelegt_von
```
Typen:
- **Dauercode** – ohne Ablauf (optional mit Wochentag/Uhrzeit-Fenster, z. B. Reinigung Mo–Fr 6–8 Uhr)
- **Zeitcode** – gültig von/bis (z. B. Handwerker diese Woche)
- **Einmalcode** – max_nutzungen = 1

Prüfung bei Eingabe: Sperre aktiv? → Code vorhanden und aktiv? → Zeitfenster passt (nur wenn Zeit gültig; sonst werden Zeit- und Einmalcodes **abgelehnt**, Dauercodes ohne Zeitfenster funktionieren weiter) → Nutzungszähler → Relais → Protokoll.

Speicherung: NVS-Namespace `codes`, bis ~200 Einträge, Codes im Klartext. Ehrlicher Hinweis: Hashing bringt bei 4–8-stelligen Zahlencodes praktisch nichts (10⁸ Kombinationen sind offline sofort durchprobiert). Realer Schutz ist, dass das Gerät im gesicherten Bereich sitzt, dazu Flash-Verschlüsselung (Ausbaustufe) und das Rate-Limit am Keypad.

---

## 8. Relais / Tür

- Impulsdauer 0,5–30 s konfigurierbar (Default 3 s), Firmware erzwingt die Obergrenze, Timer per `esp_timer`, Relais-Pin beim Boot definiert Low (bzw. inaktiv bei Active-Low-Modul).
- Auslöser: gültiger Code, Button auf der Webseite (protokolliert mit Benutzer), REX-Taster, API-Token für Hausautomation (Abschnitt 8a).
- Türkontakt: Statusanzeige, Protokoll „offen ohne Freigabe“ (Alarmhinweis), Haltezeit-Überwachung.
- Watchdog (Task-WDT) + Brownout-Erkennung, damit das Relais nie hängen bleibt.

---

## 8a. Mehrere Türen und API-Token (Entwurf, Entscheidung 24.10.2026)

**Mehrere Türen**

- `CFG_MAX_DOORS` (Vorschlag 4) in `config.h`; freie J2-Pins laut Schaltplan: GPIO2, 3, 35, 36, 43–49 (GPIO4 ist PHY-Interrupt, GPIO0/1 Quarz-Footprint). Je Tür Relais-Pin, optional Kontakt- und REX-Pin, Pins bleiben Compile-Zeit-Tabelle in `config.h`.
- `settings_door_t` wird zu `doors[CFG_MAX_DOORS]` mit `enabled`, `name[24]`, `relay_pulse_ms`, `relay_active_low`, `door_contact_enable`, `exit_button_enable` → `SETTINGS_VERSION 3` (Defaults statt Migration, noch keine Geräte im Feld).
- Codes bekommen eine Bitmaske `doors` (Bit n = Tür n), Default alle; `codes.json` erweitert, fehlendes Feld = alle Türen (abwärtskompatibel zu bestehenden Backups).
- **Genau ein Keypad pro Gerät** (Entscheidung 24.10.2026), fest einer Tür zugeordnet (`keypad.door`, Default Tür 1, in der UI wählbar). Ein gültiger Code am Keypad öffnet nur diese Tür, und nur wenn seine Türmaske sie enthält. Die übrigen Türen werden ausschließlich über Web-UI, API-Token und REX-Taster bedient.
- `door_relay.c` wird instanzbasiert (`door_open(idx, reason, ms)`), eigener `esp_timer` je Tür; Status-API liefert `doors[]`; Web-UI zeigt je Tür Zustand und Öffnen-Button; Protokoll nennt den Türnamen.

**API-Token (Hausautomation / Home Assistant)**

- **Format** `ks1.<id>.<secret>`: `ks1` = Präfix/Version (erkennbar in Logs und Secret-Scannern), `id` = 6 Byte Zufall als Base64url (8 Zeichen, nur zum Nachschlagen, darf im Protokoll stehen), `secret` = **32 Byte = 256 Bit** Zufall als Base64url (43 Zeichen). Gesamt 55 Zeichen.
- **Warum 256 Bit reichen:** Ein Angreifer kann Token nur online gegen das Gerät probieren. Schon 128 Bit sind bei jedem denkbaren Durchsatz unerratbar; 256 Bit kosten nichts extra und entsprechen dem Session-Cookie. Entscheidend ist nicht die Länge, sondern die Zufallsquelle → Abschnitt 9 (`bootloader_random_enable()`), sonst ist auch ein langes Token vorhersagbar.
- **Speicherung** nur als SHA-256(secret) in `/storage/tokens.json` (id, Name, Hash, Türmaske, Rechte, erstellt, zuletzt benutzt, aktiv, optional IP-Allowlist, optional Ablauf). Kein PBKDF2 nötig, weil das Secret selbst 256 Bit Entropie hat; Vergleich in konstanter Zeit. Das Klartext-Token zeigt die UI genau einmal. Max. 8 Token.
- **Verwendung:** `Authorization: Bearer <token>` auf `POST /api/v1/doors/<n>/open` (Body optional `{pulse_ms}` bis zur Obergrenze) und `GET /api/v1/status` (Türzustände, Kontakt, Link). Nur über HTTPS; niemals in der URL (landet sonst in Logs/Proxies). Kein Cookie, kein CSRF, keine anderen Routen für Token.
- **Rechte je Token:** Türmaske und Aktion (`open`, `status`); optional Quell-IP/Präfix (IPv4 und IPv6) – im LAN der billigste zweite Faktor; optional Ablaufdatum, das nur ausgewertet wird, wenn die Uhr gültig ist (sonst gilt das Token weiter, damit Installationen ohne NTP nicht ausfallen; Default: kein Ablauf).
- **Abwehr:** Rate-Limit pro Quell-IP und global für fehlgeschlagene Bearer-Versuche (wie Login-Sperre), jede Nutzung und jeder Fehlversuch im Protokoll mit Token-id/Name, nie mit Secret. Sofortiges Sperren/Löschen in der UI; Werksreset löscht alle Token.
- **Transport-Vertrauen:** Das Gerätezertifikat ist selbstsigniert. Das Drittsystem soll den SHA-256-Fingerprint pinnen (steht unter `/api/cert`) oder ein eigenes Zertifikat per Upload nutzen; `verify_ssl: false` nur im abgeschotteten LAN.
- Verwaltung nur in der Admin-UI (Session + CSRF): anlegen, Türen zuweisen, sperren, löschen. Werksreset löscht die Token.
- **Home Assistant:** `rest_command` mit `Authorization: Bearer …` auf `/api/v1/doors/1/open`, `rest`-Sensor auf `/api/v1/status` für Türkontakt und Relaiszustand. Funktioniert ohne gültige Uhr.

---

## 9. Sicherheit (Web/Betrieb)

- Erststart: Setup-Assistent erzwingt ein Admin-Passwort (kein Default-Passwort). Passwort-Hash PBKDF2-HMAC-SHA256 (mbedTLS), 100k Iterationen.
- Sessions: 32-Byte-Zufalls-Cookie, `HttpOnly; Secure; SameSite=Strict`, Ablauf nach 30 min Inaktivität; CSRF-Token für alle schreibenden Requests; Login-Rate-Limit.
- Keine Geheimnisse in der Firmware; der Signaturschlüssel liegt nur auf dem Build-Rechner.
- **Zufallsquelle:** Der S31-Bootloader schaltet den TRNG vor dem App-Start ab; ohne Wi-Fi/BT liefert `esp_random()` dann nur ein zählerartiges Muster (am Board gemessen 24.10.2026: `fbceedda fec8eadc e0d5e8df …`). `main.c` ruft deshalb als Erstes `bootloader_random_enable()` auf und lässt die Entropiequelle dauerhaft an. Alle Geheimnisse (Session-Cookies, EC-Schlüssel des Zertifikats, PBKDF2-Salt, TOTP-Secret, API-Token) laufen über `esp_fill_random()` bzw. die PSA-API, die ebenfalls `esp_fill_random()` als Entropie nutzt. Konsequenz: **kein ADC** in dieser Firmware (Konflikt mit der SAR-ADC-Entropiequelle).
- Protokollierung aller sicherheitsrelevanten Ereignisse, Ringpuffer (RAM) + rotierende Datei auf LittleFS.
- Werksreset per BOOT-Taster (10 s) löscht NVS inkl. Zertifikat und Admin-Passwort.

---

## 10. Umsetzungsphasen

| Phase | Inhalt | Ergebnis / Abnahme |
|---|---|---|
| 0 | Toolchain-Bring-up: IDF 6.1 installieren, Gerüst bauen, LED blinkt, Konsole, Partitionstabelle, PlatformIO-Fork testen | **Erledigt (24.10.2026)**: Flash über USB-Serial/JTAG (COM4), Boot sauber, PSRAM 16 MB erkannt, LittleFS gemountet |
| 1 | Ethernet RGMII + YT8531-Workarounds, DHCP/SLAAC, SNTP, mDNS, Hostname | **Auf Hardware getestet (24.10.2026)**: Link 1000 Mbit/s, DHCP-IPv4, SLAAC (link-local + global), `ping keysystem.local` über IPv6 ok. Voraussetzung: RGMII-Sendetakt aus der **APLL** (`CONFIG_ETH_EMAC_RGMII_TX_CLK_SRC_APLL`), siehe Umsetzungsstand. SNTP im Testnetz nicht prüfbar (kein NTP, kein Internet) |
| 2 | HTTPS-Server, selbstsigniertes Zert, Login/Session, UI-Grundgerüst, Übersicht | **Teilweise getestet (24.10.2026)**: HTTP→HTTPS-Redirect, TLS über IPv4 und IPv6, Zertifikat (EC, CN=keysystem-<MAC>, SAN keysystem.local), `/api/session` meldet `setup_required`, geschützte Routen liefern 401. Offen: Ersteinrichtung und Login im Browser |
| 3 | Settings in NVS, Netzwerk-Seite (statisch v4/v6), Zertifikats-Upload, Backup/Restore, DHCP-Rückfall | **Code fertig, kompiliert**; DHCP-Rückfall auf 192.168.1.50/24 nach 20 s ohne Lease (wie Casambi `check_dhcp_fallback`), 192.168.1.50 ist auch die Vorgabe für „statisch“ |
| 4 | Keypad-UART + Lernmodus, Relais-Modul, Codes-DB + UI, Protokoll | **Code fertig, kompiliert**; Test mit echtem Keypad und Relais offen |
| 5 | Signierte OTA: Signierskripte, Upload, Verifikation, Rollback | **Code fertig, kompiliert**; Test: unsigniertes Image ablehnen, Stromausfall während Update |
| 6 | Härtung + Tests: Rate-Limits, WDT, DST-Wechsel, IPv6-only-Netz, Zert-Rotation, Lasttest TLS, Doku | Rate-Limits, Werksreset per BOOT-Taster, Protokolldatei erledigt; Tests warten auf Hardware |
| 7 | **2FA** (Entscheidung 24.10.2026): TOTP RFC 6238 + 8 Recovery-Codes, strenge Variante ohne synchrone Uhr, Einrichtung über Admin-Seite mit QR-Code, keine externe RTC | **Code fertig, kompiliert**; Test mit Authenticator-App offen |
| 8 | **API-Token + mehrere Türen** (Entscheidung 24.10.2026, Abschnitt 8a): bis zu 4 Türen als Array in Settings und Codes, Relais-/Kontakt-/REX-Pins je Tür, ein Keypad fest einer Tür zugeordnet, Token-Verwaltung in der UI, `/api/v1/doors/<n>/open` mit Bearer-Token | **in Arbeit**, Reihenfolge: erst Türen-Datenmodell (Settings-Version 3), dann Token |

### Umsetzungsstand 24.10.2026

Alle Module aus Abschnitt 2 sind implementiert und bauen fehlerfrei mit ESP-IDF 6.1 (Flash 1,04 MB von 4 MB, RAM 35 KB statisch). **Erster Hardwaretest am 24.10.2026** (Board an COM4, Testnetz 192.168.1.0/24 ohne Internet und ohne NTP): Boot, Ethernet, HTTPS und mDNS funktionieren; Keypad, Relais, OTA und 2FA sind noch ungetestet. Befunde aus dem Hardwaretest:

- **EMAC-Takt:** Mit Octal-PSRAM bei 200 MHz ist die MPLL fest auf 400 MHz; der EMAC-Treiber wählt im Modus „Auto“ zuerst die MPLL und scheitert (400/3 = 133 MHz statt 125 MHz, `emac_set_freq_ref_out_clock` → `ESP_FAIL`, kein MAC). Die CPLL (max. 320 MHz, 40-MHz-Raster) kann 125 MHz ebenfalls nicht liefern. Lösung: `CONFIG_ETH_EMAC_RGMII_TX_CLK_SRC_APLL=y` (APLL-Maximum ist genau 125 MHz; der 8-Bit-Referenzteiler liefert daraus auch 25 und 2,5 MHz für 100/10 Mbit). Alternative wäre PSRAM mit 250 MHz (MPLL 500 MHz), nicht getestet.
- **GPIO40-„conflict“:** war kein Pinkonflikt, sondern das zweite `gpio_config()` auf dem Relais-Pin (main.c `relay_safe_init` und `door_init`). `door_init` setzt jetzt nur noch den Pegel.
- Der S31 hat zwei RGMII-Pad-Sätze (GPIO8–19 und GPIO36–47); das Board nutzt Satz 0, deshalb sind GPIO36–47 am J2 frei. Die MSPI-Pins des S31 sind dedizierte Pads, die in `spi_pins.h` eingetragenen GPIO26–32 werden trotzdem vom Treiber reserviert (IDF-Fehler, für uns ohne Folgen).
- Die Coredump-Partition enthielt Altdaten („Incorrect size of core dump image“); einmalig gelöscht mit `esptool erase_region 0x12000 0x10000`.
- **Schwacher Zufall behoben:** `esp_random()` lieferte vor `bootloader_random_enable()` ein Zählermuster (siehe Abschnitt 9). Das am 24.10.2026 zuvor erzeugte Gerätezertifikat war damit unbrauchbar; die Storage-Partition wurde gelöscht und das Zertifikat mit echtem Zufall neu erzeugt. Geräte, die mit einer Firmware vor dieser Korrektur eingerichtet wurden, müssen Zertifikat, Passwort und 2FA neu anlegen (Werksreset).
- pyserial öffnet den Port standardmäßig mit DTR/RTS aktiv und löst damit über USB-Serial/JTAG einen Reset aus; zum Mitlesen ohne Reset `dtr=False, rts=False` vor `open()` setzen.

Besonderheiten gegenüber dem ursprünglichen Plan:

- mbedTLS 4.1 (IDF 6.1): Schlüsselerzeugung und Hash/HMAC laufen über die PSA-API (`psa_generate_key`, `psa_hash_compute`, `psa_mac_*`), weil `ctr_drbg`, `md_hmac_*`, `pk_get_type/name` nicht mehr öffentlich sind.
- Web-Assets werden wie bei Casambi_Logic per `tools/embed_web_assets.py` gzip-komprimiert in einen Header gebettet; PlatformIO kam mit `EMBED_TXTFILES` nicht zurecht.
- Codes liegen als `codes.json` auf LittleFS (nicht in NVS): die NVS-Partition ist nur 24 KB, und das Format dient zugleich als Backup.
- Statische IPv6-Adresse wird gesetzt, der Standard-Router kommt aber immer aus Router Advertisements (lwIP bietet keine API für statische IPv6-Gateways).
- Stateless DHCPv6 ist konfiguriert (`CONFIG_LWIP_IPV6_DHCP6`), DNS über IPv6 kommt derzeit nur per RDNSS; der explizite `dhcp6_enable_stateless()`-Aufruf fehlt noch.
- Noch zu prüfen auf Hardware: Ersteinrichtung/Login im Browser, Keypad-Lernmodus, Relaismodul-Polarität, OTA-Upload mit signiertem/unsigniertem Image, 2FA mit Authenticator-App (braucht NTP), WS2812-Farben, Pinkonflikte mit dem ES8311-Codec.

Grobe Einschätzung: Phase 0–1 sind wegen des neuen Chips das größte Risiko (Pinkonflikte, PHY-Tuning, Toolchain). Phasen 2–5 sind Standard-ESP-IDF-Arbeit.

---

## 11. Offene Entscheidungen (bitte beantworten)

1. **Keypad-Modell/Protokoll:** Welches Keypad genau? (Datenblatt spart den Umweg über den Lernmodus.) Soll der TX-Kanal zum Keypad genutzt werden (Beep/LED-Quittung)?
2. **Relais/Türöffner:** Spannung/Strom des Türöffners, vorhandenes Relaismodul (Active-Low?), Fail-safe- oder Fail-secure-Öffner?
3. ~~Secure Boot per eFuse~~ **Entschieden (23.10.2026): vorerst nicht, eventuell später.** Signierte Updates ohne eFuse (Abschnitt 5).
4. ~~Zertifikatsquelle~~ **Entschieden (23.10.2026): wie Casambi_Gateway / Casambi_Logic** – gerätespezifisch selbstsigniert beim ersten Boot, Upload von Zertifikat + Key über die Admin-Seite (Abschnitt 4).
5. ~~Wi-Fi als Fallback~~ **Entschieden (24.10.2026): nein**, nur Ethernet.
6. ~~Zusätzliche Auslöser?~~ **Entschieden (24.10.2026): ja, beides kommt rein** – API-Token für Hausautomation und mehrere Türen/Relais, Entwurf in Abschnitt 8a, Phase 8. Ein Keypad pro Gerät, bis zu 4 Türen (entschieden 24.10.2026). **Umsetzung freigegeben.**
7. ~~Schaltplan des CoreBoards vorhanden?~~ **Erledigt (24.10.2026):** Schaltplan liegt vor (Quellen), Pins geprüft, keine Konflikte.
8. ~~Soll ich mit Phase 0 starten~~ **Erledigt (24.10.2026), Board läuft.**

---

## 12. Quellen

- Board-User-Guide: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/user_guide.html
- Board-Schaltplan (V1.0, 13.05.2026): [docs/esp32-s31-function-coreboard-1-schematics.pdf](docs/esp32-s31-function-coreboard-1-schematics.pdf), Original https://dl.espressif.com/schematics/esp32-s31-function-coreboard-1-schematics.pdf
- ESP-IDF Ethernet-Beispiel (YT8531-Init): https://github.com/espressif/esp-idf/blob/master/examples/ethernet/basic/
- ESP32-S31 EMAC-Pins: `components/esp_hal_emac/esp32s31/emac_periph.c` (ESP-IDF master)
- ESPHome-PR mit S31-RGMII-Pins: https://github.com/esphome/esphome/pull/17277
- PlatformIO S31-Support (offen): https://github.com/platformio/platform-espressif32/issues/1777
- pioarduino ESP-IDF 6.1: https://github.com/pioarduino/esp-idf/releases
- Secure Boot v2 / signierte Apps ohne Secure Boot (S31): https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/security/secure-boot-v2.html
- esp_https_server: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/protocols/esp_https_server.html
- lwIP IPv6 (SLAAC/DHCPv6): https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-guides/lwip.html
- S31-Errata (ECDSA-Secure-Boot): https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32s31/
