# esp32_key_system

Zutrittssystem auf dem **ESP32-S31-Function-CoreBoard-1**: Keypad (RS232) → Code-Prüfung mit Dauer-/Zeit-/Einmalcodes → Relais öffnet die Tür (bis zu 4 Türen). Verwaltung per HTTPS-Webseite (IPv4/IPv6) mit Admin und bis zu 16 Benutzern, die nur ihre eigenen oder freigegebenen Codes sehen; 2FA (TOTP) je Konto, API-Token für Hausautomation, signierte Firmware-Updates, gerätespezifisches TLS-Zertifikat mit Upload-Möglichkeit.

**Status (28.10.2026):** Alle Phasen aus [PLAN.md](PLAN.md) implementiert und mit ESP-IDF 6.1 gebaut. Auf dem Board getestet: Boot, Ethernet (1 Gbit/s, DHCP, SLAAC), HTTPS über IPv4/IPv6, mDNS und Keypad-Empfang. Noch ungetestet: Relais, Türkontakt/REX, OTA, 2FA. Pins liegen gesammelt in [main/config.h](main/config.h) (siehe [docs/hardware.md](docs/hardware.md)).

## Bauen

Der ESP32-S31 braucht **ESP-IDF ≥ 6.1**. Zwei Wege:

```bash
# A) PlatformIO (Fork-Plattform mit S31-Board, bis das offizielle Paket nachzieht)
pio run -t upload
pio device monitor

# B) natives ESP-IDF
idf.py set-target esp32s31
idf.py build flash monitor
```

Vor dem ersten Build den Signaturschlüssel erzeugen (bleibt lokal, siehe `.gitignore`):

```bash
tools/gen_signing_key.sh
```

Der Build signiert `firmware.bin` danach automatisch (`tools/sign_firmware.py`, RSA-3072, Schlüssel aus `OTA_SIGNING_KEY` oder `keys/ota_signing_key.pem`). Ohne Schlüssel bleibt das Image unsigniert: Es bootet per USB, nimmt aber keine OTA-Updates an. Den privaten Schlüssel sichern, Geräte akzeptieren nur damit signierte Images.

Hinweise zu PlatformIO: Die generierte Konfiguration liegt in `sdkconfig.coreboard1` (gitignored). Nach Änderungen an `sdkconfig.defaults` diese Datei löschen, damit sie neu erzeugt wird.

## Erster Start

1. Board per Ethernet anschließen, flashen. LED: gelb blinkend = Start, rot = kein Link, orange blinkend = keine IP, grün = bereit.
2. Browser: `https://keysystem.local` (oder die per DHCP vergebene IP). Das selbstsignierte Zertifikat einmal akzeptieren oder über die Zertifikat-Seite herunterladen und importieren.
3. Ersteinrichtung: Admin-Benutzer und Passwort (mind. 8 Zeichen) festlegen.
4. System → Keypad: Lernmodus einschalten, Tasten drücken, Rahmen/Abschlusszeichen anhand der Rohdaten einstellen.
5. Codes anlegen, Relais-Impulsdauer und Polarität unter System prüfen, „Tür öffnen“ testen.
6. Optional: unter Benutzer weitere Konten mit erlaubten Code-Typen, Türen und Rechten anlegen; unter System → „Anlage und Betreuer“ Kontakt und Adresse für die Anmeldeseite hinterlegen.

Werksreset ohne Web-Zugang: BOOT-Taster 10 s halten (LED blinkt ab 2 s rot).

## Struktur

| Pfad | Inhalt |
|---|---|
| `PLAN.md` | Umsetzungsplan, Entscheidungen, Stand |
| `CLAUDE.md` | Projektkonventionen, Modulübersicht |
| `platformio.ini` | PlatformIO-Env (`framework = espidf`, `src_dir = main`, Build-Hooks) |
| `sdkconfig.defaults` | IPv6, HTTPS-Server, signierte OTA-Images, PSRAM, Version |
| `partitions.csv` | 16 MB: zwei OTA-Slots à 4 MB + LittleFS 7 MB |
| `main/` | Firmware-Module (siehe CLAUDE.md), `main/web/` Web-UI |
| `tools/` | Web-Assets einbetten, Signaturschlüssel erzeugen, Firmware signieren |
| `docs/hardware.md` | Pinbelegung, J2-Header, externe Beschaltung |
| `docs/api.md` | REST-API |

## Lizenz

MIT, siehe [LICENSE](LICENSE). Enthaltene Fremdkomponenten: `main/web/qrcode.js` (MIT, Kazuhiko Arase); die YT8531-Initialisierung in `main/net_eth.c` orientiert sich am ESP-IDF-Ethernet-Beispiel (Apache-2.0, Espressif). ESP-IDF und die Komponenten aus der ESP Component Registry (mdns, littlefs, led_strip, cJSON) werden beim Build geladen und bleiben unter ihren eigenen Lizenzen.
