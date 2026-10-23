/*
 * time_sync.h – SNTP, Zeitzone und das Flag "Zeit gültig"
 *
 * Zeitcodes werden nur akzeptiert, wenn time_sync_is_valid() true liefert.
 */
#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

esp_err_t time_sync_init(void);        /* setzt TZ, prüft ob die Uhr plausibel ist */
esp_err_t time_sync_start(void);       /* startet SNTP mit den Servern aus settings */
esp_err_t time_sync_restart(void);     /* nach Änderung der NTP-/TZ-Einstellungen   */
bool      time_sync_is_valid(void);
time_t    time_sync_last_sync(void);   /* 0 = noch nie                              */
/* Lokale Zeit als "YYYY-MM-DD HH:MM:SS" in buf (mind. 20 Byte). */
void      time_sync_format_local(time_t t, char *buf, size_t len);
