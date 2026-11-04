/*
 * time_sync.h – SNTP, Zeitzone und das Flag "Zeit gültig"
 *
 * Zeitcodes werden nur akzeptiert, wenn time_sync_is_valid() true liefert.
 * Ein Wächter-Task prüft nach jedem (Neu-)Start des SNTP-Clients, ob innerhalb
 * von CFG_NTP_TIMEOUT_S eine Antwort kam, und schreibt sonst die wahrscheinliche
 * Ursache (keine IP, DNS, keine Antwort) ins Ereignisprotokoll.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef struct {
    bool     running;                 /* SNTP-Client läuft                                  */
    bool     valid;                   /* Uhr gültig (NTP oder plausible RTC)                */
    bool     last_failed;             /* letzter Versuch ohne Antwort                        */
    bool     attempt_pending;         /* Versuch läuft, Wächter wartet noch auf Antwort      */
    time_t   last_sync;               /* 0 = noch nie                                        */
    uint32_t sync_count;
    uint32_t last_attempt_uptime_s;   /* 0 = noch nie                                        */
    uint32_t timeout_s;               /* CFG_NTP_TIMEOUT_S                                   */
    char     server1[64];
    char     server2[64];
    char     last_result[160];        /* letzte Meldung (Erfolg oder Ursache), "" = keine    */
} time_sync_status_t;

esp_err_t time_sync_init(void);        /* setzt TZ, prüft ob die Uhr plausibel ist */
/* Startet SNTP; läuft er schon, wird sofort eine neue Anfrage geschickt (z. B. neue IP). */
esp_err_t time_sync_start(void);
esp_err_t time_sync_restart(void);     /* nach Änderung der NTP-/TZ-Einstellungen   */
/* Manueller Neuversuch (Web-UI); who = Benutzername fürs Protokoll. */
esp_err_t time_sync_retry(const char *who);
bool      time_sync_is_valid(void);
time_t    time_sync_last_sync(void);   /* 0 = noch nie                              */
void      time_sync_get_status(time_sync_status_t *st);
/* Lokale Zeit als "YYYY-MM-DD HH:MM:SS" in buf (mind. 20 Byte). */
void      time_sync_format_local(time_t t, char *buf, size_t len);
