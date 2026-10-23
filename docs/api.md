# REST-API

Alle Antworten JSON. Schreibende Aufrufe (POST/PUT/DELETE) brauchen das Session-Cookie `ks_session` **und** den Header `X-CSRF-Token` (Wert aus `/api/session` bzw. der Login-Antwort). Fehler: `{"error": "..."}` mit passendem HTTP-Status.

## Session

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/session` | `{authenticated, setup_required, tls, hostname, user?, csrf?}` |
| POST | `/api/setup` | nur solange kein Passwort gesetzt: `{user, password}` → Session |
| POST | `/api/login` | `{user, password}` → `{ok, user, csrf}` + Cookie; bei aktiver 2FA stattdessen `{twofa_required:true, ticket, time_valid, recovery_remaining}`; 429 bei Sperre |
| POST | `/api/login/2fa` | `{ticket, code}` – `code` ist TOTP (6 Ziffern, nur bei synchroner Zeit) oder Recovery-Code `ABCDE-FGHJK` → Session wie beim Login |
| POST | `/api/logout` | Session beenden |

## Zwei-Faktor-Authentifizierung (TOTP RFC 6238, SHA-1, 30 s, 6 Stellen)

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/twofa` | `{enabled, time_valid, recovery_remaining, recovery_total, issuer}` |
| POST | `/api/twofa/setup` | `{password}` → `{secret (Base32), otpauth, recovery_codes[8], expires_s}`; nur bei synchroner Zeit; Einrichtung bleibt 10 min „pending“ |
| POST | `/api/twofa/confirm` | `{code}` aktueller App-Code → aktiviert 2FA |
| POST | `/api/twofa/cancel` | pending Einrichtung verwerfen |
| POST | `/api/twofa/disable` | `{password, code}` (TOTP oder Recovery) |
| POST | `/api/twofa/recovery` | `{password, code}` → `{recovery_codes[8]}` neu, alte ungültig |

Strenge Variante: Ohne synchrone Uhrzeit werden TOTP-Codes nicht geprüft (Fehler 401 mit Hinweis), nur Recovery-Codes sind dann gültig. Replay-Schutz: ein TOTP-Zeitschritt wird nur einmal akzeptiert.

## Status und Protokoll

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/status` | `firmware`, `system` (Uptime, Heap, Zeit, TLS), `network` (Link, IPv4, IPv6[]), `door` (Relais, Kontakt, Keypad-Sperre, Codes) |
| GET | `/api/log?max=N&after=ID` | Ereignisse neueste zuerst, `{last_id, entries:[{id, ts, uptime, cat, msg}]}` |

## Codes

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/codes` | `{codes:[...], max, time_valid}` |
| POST | `/api/codes` | neuen Code anlegen → `{ok, id}` |
| PUT | `/api/codes/<id>` | Code ändern (Zähler bleiben) |
| DELETE | `/api/codes/<id>` | löschen |

Code-Objekt:

```json
{"id":1,"name":"Hausmeister","code":"4711","type":"permanent|timed|onetime","enabled":true,
 "valid_from":0,"valid_until":0,"win_enable":false,"weekdays":127,"win_from":360,"win_to":1200,
 "max_uses":0,"uses":0,"last_used":0,"created_at":0,"created_by":"admin"}
```

`valid_*` Unix-Sekunden (0 = offen), `weekdays` Bit0 = Montag, `win_*` Minuten seit Mitternacht (lokal).

## Tür und Keypad

| Methode | Pfad | Beschreibung |
|---|---|---|
| POST | `/api/door/open` | Relais-Impuls mit eingestellter Dauer |
| GET | `/api/keypad/raw` | Lernmodus: `{count, rx_total, last_rx_uptime_s, hex, ascii}` |

## Netzwerk, System, Zertifikat

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET/POST | `/api/network` | Hostname, IPv4 (dhcp/static), DNS, IPv6 (SLAAC, statisch), NTP, TZ → `reboot_required` |
| GET/POST | `/api/system` | `{door:{relay_pulse_ms, relay_active_low, door_contact_enable, exit_button_enable}, keypad:{baud, parity, frame_mode, terminator, clear_key, fixed_len, timeout_ms, learn_mode}}` |
| POST | `/api/system/password` | `{user, old_password, new_password}`; alle Sessions enden |
| POST | `/api/system/reboot` | Neustart nach 1,5 s |
| POST | `/api/system/factory-reset` | Settings, Codes, Zertifikat löschen, Neustart |
| GET | `/api/cert` | Zertifikatsinfo (Subject, Gültigkeit, SHA-256, Key passt, Dateistatus) |
| GET | `/api/cert/download` | Zertifikat als PEM |
| POST | `/api/cert/upload?type=cert\|key` | Body = PEM (text/plain); aktiv nach Neustart |
| POST | `/api/cert/regenerate` | Dateien löschen, Neuerzeugung beim Neustart |

## Backup, Firmware

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/backup` | `{format:"esp32_key_system_backup", version, settings:{net, door, keypad}, codes:[...]}` – ohne Passwort, Zertifikat, Key |
| POST | `/api/restore` | Backup-JSON, optional `restore_network:false`; Codes werden ersetzt |
| GET | `/api/firmware` | laufende/nächste Partition, Versionen, `pending_verify`, `rollback_possible` |
| POST | `/api/firmware` | Body = signiertes `firmware.bin` (application/octet-stream); Signaturprüfung nach Upload |
| POST | `/api/firmware/rollback` | andere Partition als Boot-Partition setzen |
