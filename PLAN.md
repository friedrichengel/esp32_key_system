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
| USB | Type-C USB-Serial/JTAG, Type-C USB-UART-Bridge (Konsole auf GPIO58/59), USB-A Host |
| Sonstiges | WS2812-LED an GPIO60, BOOT an GPIO61, ES8311-Audiocodec + Mikro/Lautsprecher (Pins nicht dokumentiert), SDIO-Pins GPIO20–25 auf J2 |
| Frei auf J2 (voraussichtlich) | GPIO0–4, 35–49, 20–25 (SDIO); 60/61 belegt |
| Framework | **Nur ESP-IDF ≥ 6.1**. Arduino-Core gibt es für den S31 noch nicht (kommt erst mit arduino-esp32 4.0). |
| Sicherheits-Errata | ECDSA-Secure-Boot auf dem S31 fehlerhaft → **RSA-3072** für Signaturen verwenden |

Konsequenzen:
- Alle Pinzuweisungen unten sind **Vorschläge** und müssen gegen den Schaltplan des CoreBoards geprüft werden (die Audiocodec-Pins sind nirgends dokumentiert). Deshalb liegen alle Pins zentral in `main/config.h`.
- Ein reines Arduino-Projekt (wie bei `esp32_usb_reader`) geht nicht. Wir arbeiten mit **ESP-IDF 6.1 in C**, Projektstruktur so, dass sie mit `idf.py` **und** mit PlatformIO (`framework = espidf`) baut.

### Vorgeschlagene Pinbelegung (zu verifizieren)

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
- Auslöser: gültiger Code, Button auf der Webseite (protokolliert mit Benutzer), REX-Taster, später optional API-Token für Hausautomation.
- Türkontakt: Statusanzeige, Protokoll „offen ohne Freigabe“ (Alarmhinweis), Haltezeit-Überwachung.
- Watchdog (Task-WDT) + Brownout-Erkennung, damit das Relais nie hängen bleibt.

---

## 9. Sicherheit (Web/Betrieb)

- Erststart: Setup-Assistent erzwingt ein Admin-Passwort (kein Default-Passwort). Passwort-Hash PBKDF2-HMAC-SHA256 (mbedTLS), 100k Iterationen.
- Sessions: 32-Byte-Zufalls-Cookie, `HttpOnly; Secure; SameSite=Strict`, Ablauf nach 30 min Inaktivität; CSRF-Token für alle schreibenden Requests; Login-Rate-Limit.
- Keine Geheimnisse in der Firmware; der Signaturschlüssel liegt nur auf dem Build-Rechner.
- Protokollierung aller sicherheitsrelevanten Ereignisse, Ringpuffer (RAM) + rotierende Datei auf LittleFS.
- Werksreset per BOOT-Taster (10 s) löscht NVS inkl. Zertifikat und Admin-Passwort.

---

## 10. Umsetzungsphasen

| Phase | Inhalt | Ergebnis / Abnahme |
|---|---|---|
| 0 | Toolchain-Bring-up: IDF 6.1 installieren, Gerüst bauen, LED blinkt, Konsole, Partitionstabelle, PlatformIO-Fork testen | **Build erledigt (23.10.2026)**, Flash-/Boot-Test wartet auf das Board |
| 1 | Ethernet RGMII + YT8531-Workarounds, DHCP/SLAAC, SNTP, mDNS, Hostname | **Code fertig, kompiliert (24.10.2026)**; Hardwaretest offen: `ping -4/-6 <host>.local`, Uhrzeit, Link 1000 Mbit |
| 2 | HTTPS-Server, selbstsigniertes Zert, Login/Session, UI-Grundgerüst, Übersicht | **Code fertig, kompiliert**; Test: Login über IPv4 und IPv6, Zert-Details im Browser |
| 3 | Settings in NVS, Netzwerk-Seite (statisch v4/v6), Zertifikats-Upload, Backup/Restore, DHCP-Rückfall | **Code fertig, kompiliert**; DHCP-Rückfall auf 192.168.1.50/24 nach 20 s ohne Lease (wie Casambi `check_dhcp_fallback`), 192.168.1.50 ist auch die Vorgabe für „statisch“ |
| 4 | Keypad-UART + Lernmodus, Relais-Modul, Codes-DB + UI, Protokoll | **Code fertig, kompiliert**; Test mit echtem Keypad und Relais offen |
| 5 | Signierte OTA: Signierskripte, Upload, Verifikation, Rollback | **Code fertig, kompiliert**; Test: unsigniertes Image ablehnen, Stromausfall während Update |
| 6 | Härtung + Tests: Rate-Limits, WDT, DST-Wechsel, IPv6-only-Netz, Zert-Rotation, Lasttest TLS, Doku | Rate-Limits, Werksreset per BOOT-Taster, Protokolldatei erledigt; Tests warten auf Hardware |
| 7 | **2FA** (Entscheidung 24.10.2026): TOTP RFC 6238 + 8 Recovery-Codes, strenge Variante ohne synchrone Uhr, Einrichtung über Admin-Seite mit QR-Code, keine externe RTC | **Code fertig, kompiliert**; Test mit Authenticator-App offen |

### Umsetzungsstand 24.10.2026

Alle Module aus Abschnitt 2 sind implementiert und bauen fehlerfrei mit ESP-IDF 6.1 (Flash 1,04 MB von 4 MB, RAM 35 KB statisch). **Nichts davon ist auf Hardware getestet**, das Board war noch nicht angeschlossen. Besonderheiten gegenüber dem ursprünglichen Plan:

- mbedTLS 4.1 (IDF 6.1): Schlüsselerzeugung und Hash/HMAC laufen über die PSA-API (`psa_generate_key`, `psa_hash_compute`, `psa_mac_*`), weil `ctr_drbg`, `md_hmac_*`, `pk_get_type/name` nicht mehr öffentlich sind.
- Web-Assets werden wie bei Casambi_Logic per `tools/embed_web_assets.py` gzip-komprimiert in einen Header gebettet; PlatformIO kam mit `EMBED_TXTFILES` nicht zurecht.
- Codes liegen als `codes.json` auf LittleFS (nicht in NVS): die NVS-Partition ist nur 24 KB, und das Format dient zugleich als Backup.
- Statische IPv6-Adresse wird gesetzt, der Standard-Router kommt aber immer aus Router Advertisements (lwIP bietet keine API für statische IPv6-Gateways).
- Stateless DHCPv6 ist konfiguriert (`CONFIG_LWIP_IPV6_DHCP6`), DNS über IPv6 kommt derzeit nur per RDNSS; der explizite `dhcp6_enable_stateless()`-Aufruf fehlt noch.
- Erste Hardware-Tests sollten prüfen: RGMII-Delays des YT8531 (Link kommt, aber kein Verkehr = Delay falsch), Pinkonflikte mit dem ES8311-Codec, WS2812-Timing, Relaismodul-Polarität.

Grobe Einschätzung: Phase 0–1 sind wegen des neuen Chips das größte Risiko (Pinkonflikte, PHY-Tuning, Toolchain). Phasen 2–5 sind Standard-ESP-IDF-Arbeit.

---

## 11. Offene Entscheidungen (bitte beantworten)

1. **Keypad-Modell/Protokoll:** Welches Keypad genau? (Datenblatt spart den Umweg über den Lernmodus.) Soll der TX-Kanal zum Keypad genutzt werden (Beep/LED-Quittung)?
2. **Relais/Türöffner:** Spannung/Strom des Türöffners, vorhandenes Relaismodul (Active-Low?), Fail-safe- oder Fail-secure-Öffner?
3. ~~Secure Boot per eFuse~~ **Entschieden (23.10.2026): vorerst nicht, eventuell später.** Signierte Updates ohne eFuse (Abschnitt 5).
4. ~~Zertifikatsquelle~~ **Entschieden (23.10.2026): wie Casambi_Gateway / Casambi_Logic** – gerätespezifisch selbstsigniert beim ersten Boot, Upload von Zertifikat + Key über die Admin-Seite (Abschnitt 4).
5. **Wi-Fi als Fallback** gewünscht?
6. **Zusätzliche Auslöser:** API-Token für Hausautomation/Home Assistant? Mehrere Türen/Relais?
7. **Schaltplan des CoreBoards** vorhanden? Ohne ihn bleiben die Pins für Keypad/Relais vorläufig.
8. Soll ich mit Phase 0 starten (Toolchain einrichten und Build auf dem Board verifizieren), sobald das Board am Rechner hängt?

---

## 12. Quellen

- Board-User-Guide: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/user_guide.html
- ESP-IDF Ethernet-Beispiel (YT8531-Init): https://github.com/espressif/esp-idf/blob/master/examples/ethernet/basic/
- ESP32-S31 EMAC-Pins: `components/esp_hal_emac/esp32s31/emac_periph.c` (ESP-IDF master)
- ESPHome-PR mit S31-RGMII-Pins: https://github.com/esphome/esphome/pull/17277
- PlatformIO S31-Support (offen): https://github.com/platformio/platform-espressif32/issues/1777
- pioarduino ESP-IDF 6.1: https://github.com/pioarduino/esp-idf/releases
- Secure Boot v2 / signierte Apps ohne Secure Boot (S31): https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/security/secure-boot-v2.html
- esp_https_server: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/protocols/esp_https_server.html
- lwIP IPv6 (SLAAC/DHCPv6): https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-guides/lwip.html
- S31-Errata (ECDSA-Secure-Boot): https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32s31/
