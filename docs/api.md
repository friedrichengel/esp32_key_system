# REST-API

Alle Antworten sind JSON (`application/json`, `Cache-Control: no-store`). Fehler: `{"error": "Text"}` mit passendem HTTP-Status. Es gibt zwei Zugangsarten:

- **Web-API** (`/api/...`): Session-Cookie `ks_session` (HttpOnly, Secure, SameSite=Strict). Schreibende Routen verlangen zusätzlich den Header `X-CSRF-Token` aus `/api/session` bzw. der Login-Antwort.
- **API v1 für Drittsysteme** (`/api/v1/...`): Header `Authorization: Bearer ks1.<id>.<secret>`, kein Cookie, kein CSRF. Token werden in der Admin-UI (Seite „API“) angelegt.

Türen sind in der API **1-basiert** (`door: 1..4`), Türmasken sind Bitmasken (Bit 0 = Tür 1).

## Rollen

- **Admin**: das Hauptkonto (uid 0, im Gerät) oder ein Benutzer mit Rolle `admin`. Darf alles.
- **Benutzer** (uid 1..16): sieht nur Codes, die er besitzt oder die ihm freigegeben sind. Admin-Routen antworten mit 403 „Nur fuer Administratoren“. Rechte werden bei jedem Request aus den aktuellen Benutzerdaten gelesen; Sperren wirkt sofort.
- Mit gesetztem Startpasswort (`must_change_pw`) sind nur `POST /api/account/password` und `POST /api/logout` erlaubt (sonst 403).

Benutzer-Felder in Session- und Login-Antwort: `uid`, `role` (`admin`|`user`), `code_types` (Bitmaske: 1 = Dauer, 2 = Zeit, 4 = Einmal), `doors` (Türmaske), `perms: {edit_shared, open_doors}`, `max_codes`, `must_change_pw`, `twofa_enabled`.

## Session

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/session` | `{authenticated, setup_required, tls, hostname, site:{name, support_email, info_text}}`; angemeldet zusätzlich `user, csrf` und die Benutzer-Felder oben. `site` ist **ohne Anmeldung** sichtbar (Loginseite) |
| POST | `/api/setup` | nur solange kein Admin-Passwort gesetzt: `{user, password}` → Session |
| POST | `/api/login` | `{user, password}` → `{ok, user, csrf, …}` + Cookie; bei aktiver 2FA stattdessen `{twofa_required:true, ticket, time_valid, recovery_remaining}`; 403 bei gesperrtem Benutzer, 429 bei Sperre |
| POST | `/api/login/2fa` | `{ticket, code}` – `code` ist TOTP (6 Ziffern, nur bei synchroner Zeit) oder Recovery-Code `ABCDE-FGHJK` → Session wie beim Login |
| POST | `/api/logout` | Session beenden |

## Eigenes Konto (jeder Benutzer)

| Methode | Pfad | Beschreibung |
|---|---|---|
| POST | `/api/account/password` | `{old_password, new_password}`; setzt `must_change_pw` zurück, beendet alle Sessions des Benutzers |

## Zwei-Faktor-Authentifizierung (TOTP RFC 6238, SHA-1, 30 s, 6 Stellen) – eigenes Konto

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/twofa` | `{enabled, time_valid, recovery_remaining, recovery_total, issuer}` |
| POST | `/api/twofa/setup` | `{password}` → `{secret (Base32), otpauth, recovery_codes[8], expires_s}`; nur bei synchroner Zeit; Einrichtung bleibt 10 min „pending“ (eine gleichzeitig) |
| POST | `/api/twofa/confirm` | `{code}` aktueller App-Code → aktiviert 2FA |
| POST | `/api/twofa/cancel` | pending Einrichtung verwerfen |
| POST | `/api/twofa/disable` | `{password, code}` (TOTP oder Recovery) |
| POST | `/api/twofa/recovery` | `{password, code}` → `{recovery_codes[8]}` neu, alte ungültig |

## Status und Protokoll

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/status` | `admin` (bool), `firmware`, `system` (Uptime, Zeit, TLS; Admin zusätzlich Heap, FS), `keypad` (`door`, `locked`, `lock_remaining_s`; Admin zusätzlich `fail_count`, `rx_count`, `last_rx_uptime_s`, `learn_mode`), `doors[]` (`door`, `name`, `enabled`, `relay_active`, `contact_enabled`, `contact_closed`, `last_open_uptime_s`), `codes_count` (Benutzer: eigene). Nur Admin: `network`, `tokens_count`, `users_count` |
| GET | `/api/log?max=N&after=ID` | **Admin.** Ereignisse neueste zuerst, `{last_id, entries:[{id, ts, uptime, cat, msg}]}` |

## Codes

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/codes` | `{codes:[...], max, total, owned, grace_max_s, time_valid, doors:[{door, name, enabled}], users:[{uid, name, enabled, admin}]}` – nur die für den Benutzer sichtbaren Codes |
| POST | `/api/codes` | neuen Code anlegen → `{ok, id}`; 403 wenn Typ/Türen nicht erlaubt, 507 bei erschöpftem Kontingent |
| PUT | `/api/codes/<id>` | Code ändern (Zähler bleiben); 404 wenn nicht sichtbar, 403 wenn Regel verletzt |
| DELETE | `/api/codes/<id>` | löschen; nur Besitzer oder Admin |

Code-Objekt: `{id, name, code, type: "permanent"|"timed"|"onetime", enabled, doors (Bitmaske, fehlt = alle), valid_from, valid_until, win_enable, weekdays (Bit0 = Mo), win_from, win_to (Minuten), max_uses, uses, last_used, created_at, created_by, owner (uid, 0 = Admin), shared (Bitmaske Bit uid), grace_s (0–600, nur Einmalcode), grace_remaining_s (nur lesend)}`.

Regeln (serverseitig):

- Admin darf alles und kann `owner` setzen.
- Besitzer darf ändern, löschen und über `shared` freigeben; `type` muss in seinen `code_types`, `doors` in seiner Türmaske liegen.
- Freigegebene Benutzer mit Recht `edit_shared` dürfen alle Felder außer `owner` und `shared` ändern (gleiche Typ-/Türgrenzen); ohne das Recht wird nur `enabled` übernommen.
- Am Keypad öffnet ein Code nur die dem Keypad zugeordnete Tür, und nur wenn seine Maske sie enthält.
- **Einmalcode mit Nutzungsfenster:** `grace_s > 0` erlaubt nach der ersten gültigen Eingabe so viele Sekunden lang beliebig viele Eingaben; danach ist der Code verbraucht. Das Fenster läuft auf der Laufzeituhr; ein Neustart beendet es.

## Tür und Keypad

| Methode | Pfad | Beschreibung |
|---|---|---|
| POST | `/api/door/open` | Body optional `{door: n}` (Default: Tür des Keypads); Relais-Impuls mit eingestellter Dauer → `{ok, door, name}`; Benutzer brauchen `open_doors` und die Tür in ihrer Maske (403); 404 unbekannte Tür, 409 Tür deaktiviert |
| GET | `/api/keypad/raw` | **Admin.** Lernmodus: `{count, rx_total, last_rx_uptime_s, hex, ascii}` |

## Benutzer (Admin)

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/users` | `{users:[...], max, admin_user, me (uid), doors}`; jedes Objekt zusätzlich `codes_owned` |
| POST | `/api/users` | `{name, password, role, enabled, must_change_pw (Default true), code_types, doors, edit_shared, open_doors, max_codes}` → `{ok, uid}`; 409 Name vergeben, 507 voll |
| PUT | `/api/users/<uid>` | Felder wie oben ohne `password`; eigenes Konto kann nicht gesperrt/herabgestuft werden (409). Sperren oder Rollenwechsel beendet die Sessions des Benutzers |
| POST | `/api/users/<uid>/password` | `{password, must_change_pw (Default true)}`; beendet seine Sessions |
| POST | `/api/users/<uid>/twofa-reset` | 2FA des Benutzers abschalten (Handy verloren) |
| DELETE | `/api/users/<uid>?codes=transfer\|delete` | löschen; `transfer` (Default) übergibt seine Codes an den handelnden Admin, `delete` löscht sie; Freigaben an ihn werden überall entfernt |

Benutzer-Objekt: `{uid, name, role, enabled, must_change_pw, code_types, doors, edit_shared, open_doors, max_codes, created_at, created_by, last_login, twofa_enabled}`. Max. 16 Benutzer zusätzlich zum Admin. Das Admin-Hauptkonto erscheint nicht in der Liste.

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

## Netzwerk, System, Zertifikat (Admin)

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET/POST | `/api/network` | Hostname, IPv4 (dhcp/static), DNS, IPv6 (SLAAC, statisch), NTP, TZ → `reboot_required` |
| GET/POST | `/api/system` | `{doors:[{door, enabled, name, relay_pulse_ms, relay_active_low, door_contact_enable, exit_button_enable, pins:{relay, contact, rex}}], max_doors, keypad:{baud, parity, frame_mode, terminator, clear_key, fixed_len, timeout_ms, learn_mode, door}, site:{name, support_email, info_text}, admin_user, pins}`; POST nimmt `doors` (Array, Teilmenge erlaubt), `keypad` und/oder `site`. `terminator`/`clear_key`: ein Zeichen, `CR`, `LF`, `BS`, `none` oder Hex wie `0x08`. `site`: Name max. 31, E-Mail max. 63 (einfache Formatprüfung), Text max. 511 Zeichen |
| POST | `/api/system/password` | nur Admin-Hauptkonto: `{user, old_password, new_password}` – Name und/oder Passwort; Name darf keinem Benutzer gehören (409) |
| POST | `/api/system/reboot` | Neustart nach 1,5 s |
| POST | `/api/system/factory-reset` | Settings, Benutzer, Codes, Token, Zertifikat löschen, Neustart |
| GET | `/api/cert` | Zertifikatsinfo (Subject, Gültigkeit, SHA-256, Key passt, Dateistatus) |
| GET | `/api/cert/download` | Zertifikat als PEM (auch für Benutzer) |
| POST | `/api/cert/upload?type=cert\|key` | Body = PEM (text/plain); aktiv nach Neustart |
| POST | `/api/cert/regenerate` | Dateien löschen, Neuerzeugung beim Neustart |

## Backup, Firmware (Admin)

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/backup` | `{format:"esp32_key_system_backup", version: 3, settings:{net, doors[], keypad, site}, users:[...], codes:[...]}` – `users` **mit Passwort-Hash, Salt und 2FA-Daten**, ohne Admin-Passwort, Token, Zertifikat, Key. Die Datei enthält Zutrittscodes im Klartext. Backups der Versionen 1 und 2 werden weiter eingespielt (Codes gehören dann dem Admin) |
| POST | `/api/restore` | Backup-JSON (max. 96 KB), optional `restore_network:false`; Codes und, falls enthalten, Benutzer werden ersetzt (deren Sessions enden) |
| GET | `/api/firmware` | laufende/nächste Partition, Versionen, `pending_verify`, `rollback_possible` |
| POST | `/api/firmware` | Body = signiertes `firmware.bin` (application/octet-stream); Signaturprüfung nach Upload |
| POST | `/api/firmware/rollback` | andere Partition als Boot-Partition setzen |
