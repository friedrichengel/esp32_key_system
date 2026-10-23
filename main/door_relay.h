/*
 * door_relay.h – Relais (Türöffner), Türkontakt, Austaster (REX)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

esp_err_t door_init(void);
/* Relais für pulse_ms (0 = Wert aus Settings) anziehen; reason landet im Protokoll. */
esp_err_t door_open(const char *reason, uint32_t pulse_ms);
bool      door_relay_active(void);
bool      door_contact_closed(void);          /* true wenn kein Kontakt konfiguriert */
int64_t   door_last_open_uptime_s(void);
void      door_apply_settings(void);          /* Polarität nach Settings-Änderung */
