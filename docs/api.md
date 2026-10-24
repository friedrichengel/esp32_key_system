# REST-API

Alle Antworten sind JSON (`application/json`, `Cache-Control: no-store`). Fehler: `{"error": "Text"}` mit passendem HTTP-Status. Es gibt zwei Zugangsarten:

- **Admin-API** (`/api/...`): Session-Cookie `ks_session` (HttpOnly, Secure, SameSite=Strict). Schreibende Routen verlangen zusätzlich den Header `X-CSRF-Token` aus `/api/session` bzw. der Login-Antwort.
- **API v1 für Drittsysteme** (`/api/v1/...`): Header `Authorization: Bearer ks1.<id>.<secret>`, kein Cookie, kein CSRF. Token werden in der Admin-UI (Seite „API“) angelegt.

Türen sind in der API **1-basiert** (`door: 1..4`), Türmasken sind Bitmasken (Bit 0 = Tür 1).

## Session

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/session` | `{authenticated, setup_required, tls, hostname, twofa_enabled, user?, csrf?}` |
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

## Status und Protokoll

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/status` | `firmware`, `system` (Uptime, Heap, Zeit, TLS), `network` (Link, IPv4, IPv6[]), `keypad` (`door`, `locked`, `lock_remaining_s`, `fail_count`, `rx_count`, `last_rx_uptime_s`, `learn_mode`), `doors[]` (`door`, `name`, `enabled`, `relay_active`, `contact_enabled`, `contact_closed`, `last_open_uptime_s`), `codes_count`, `tokens_count` |
| GET | `/api/log?max=N&after=ID` | Ereignisse neueste zuerst, `{last_id, entries:[{id, ts, uptime, cat, msg}]}` |

## Codes

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/codes` | `{codes:[...], max, time_valid, doors:[{door, name, enabled}]}` |
| POST | `/api/codes` | neuen Code anlegen → `{ok, id}` |
| PUT | `/api/codes/<id>` | Code ändern (Zähler bleiben) |
| DELETE | `/api/codes/<id>` | löschen |

Code-Objekt: `{id, name, code, type: "permanent"|"timed"|"onetime", enabled, doors (Bitmaske, fehlt = alle), valid_from, valid_until, win_enable, weekdays (Bit0 = Mo), win_from, win_to (Minuten), max_uses, uses, last_used, created_at, created_by}`. Am Keypad öffnet ein Code nur die dem Keypad zugeordnete Tür, und nur wenn seine Maske sie enthält.

## Tür und Keypad

| Methode | Pfad | Beschreibung |
|---|---|---|
| POST | `/api/door/open` | Body optional `{door: n}` (Default: Tür des Keypads); Relais-Impuls mit eingestellter Dauer → `{ok, door, name}`; 404 unbekannte Tür, 409 Tür deaktiviert |
| GET | `/api/keypad/raw` | Lernmodus: `{count, rx_total, last_rx_uptime_s, hex, ascii}` |

## API-Token (Admin)

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/tokens` | `{tokens:[...], max, time_valid, doors:[{door, name, enabled}]}` – ohne Hash, ohne Klartext |
| POST | `/api/tokens` | `{name, doors, can_open, can_status, enabled, allow_ip, expires_at}` → `{ok, token, entry}`; **`token` ist der Klartext und wird nie wieder ausgegeben** |
| PUT | `/api/tokens/<id>` | Felder wie oben ändern (Secret bleibt) |
| DELETE | `/api/tokens/<id>` | löschen, sofort wirksam |

Token-Objekt: `{id, name, doors, can_open, can_status, enabled, allow_ip, expires_at, created_at, last_used, use_count}`. `allow_ip` leer = jede Quelle, sonst Adresse oder Präfix (`192.168.1.10`, `192.168.1.0/24`, `fd00::/64`). `expires_at` (Unix-Sekunden, 0 = nie) wird nur geprüft, solange die Uhr gültig ist. Max. 8 Token.

## API v1 (Drittsysteme, Bearer-Token)

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/v1/status` | `{hostname, firmware, uptime_s, time, time_valid, link, keypad_locked, doors:[...]}`; `doors` enthält nur aktive Türen, die das Token sehen darf. Recht `can_status` |
| GET | `/api/v1/doors` | `{doors:[...]}` wie oben. Recht `can_status` |
| POST | `/api/v1/doors/<n>/open` | Body optional `{pulse_ms}` (wird auf 500–30000 begrenzt) → `{ok, door, name}`. Recht `can_open` und Tür in der Maske; 403 sonst, 404 unbekannte Tür, 409 Tür deaktiviert |

Fehler: 401 (Token fehlt, Format ungültig, unbekannt), 403 (gesperrt, abgelaufen, Quelladresse nicht erlaubt, Recht fehlt), 429 mit `Retry-After` nach 10 Fehlversuchen je Quell-IP (Sperre 60 s, verdoppelt sich bis 15 min). Jede Nutzung und jeder Fehlversuch steht im Protokoll mit Token-Name bzw. Quell-IP, nie mit dem Secret.

```bash
curl -k -X POST https://keysystem.local/api/v1/doors/1/open \
     -H "Authorization: Bearer ks1.xxxxxxxx.yyyy…" -H "Content-Type: application/json" -d '{"pulse_ms":3000}'
```

## Netzwerk, System, Zertifikat

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET/POST | `/api/network` | Hostname, IPv4 (dhcp/static), DNS, IPv6 (SLAAC, statisch), NTP, TZ → `reboot_required` |
| GET/POST | `/api/system` | `{doors:[{door, enabled, name, relay_pulse_ms, relay_active_low, door_contact_enable, exit_button_enable, pins:{relay, contact, rex}}], max_doors, keypad:{baud, parity, frame_mode, terminator, clear_key, fixed_len, timeout_ms, learn_mode, door}, admin_user, pins}`; POST nimmt `doors` (Array, Teilmenge erlaubt) und/oder `keypad` |
| POST | `/api/system/password` | `{user, old_password, new_password}`; alle Sessions enden |
| POST | `/api/system/reboot` | Neustart nach 1,5 s |
| POST | `/api/system/factory-reset` | Settings, Codes, Token, Zertifikat löschen, Neustart |
| GET | `/api/cert` | Zertifikatsinfo (Subject, Gültigkeit, SHA-256, Key passt, Dateistatus) |
| GET | `/api/cert/download` | Zertifikat als PEM |
| POST | `/api/cert/upload?type=cert\|key` | Body = PEM (text/plain); aktiv nach Neustart |
| POST | `/api/cert/regenerate` | Dateien löschen, Neuerzeugung beim Neustart |

## Backup, Firmware

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/backup` | `{format:"esp32_key_system_backup", version: 2, settings:{net, doors[], keypad}, codes:[...]}` – ohne Passwort, Token, Zertifikat, Key. Backups der Version 1 (`settings.door`) werden beim Einspielen auf Tür 1 abgebildet |
| POST | `/api/restore` | Backup-JSON, optional `restore_network:false`; Codes werden ersetzt |
| GET | `/api/firmware` | laufende/nächste Partition, Versionen, `pending_verify`, `rollback_possible` |
| POST | `/api/firmware` | Body = signiertes `firmware.bin` (application/octet-stream); Signaturprüfung nach Upload |
| POST | `/api/firmware/rollback` | andere Partition als Boot-Partition setzen |
