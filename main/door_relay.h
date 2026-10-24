/*
 * door_relay.h – Türen: Relais (Türöffner), Türkontakt, Austaster (REX), je Tür eine Instanz
 *
 * Pins stehen in config.h (CFG_DOOR_*_GPIOS), Laufzeitwerte (aktiv, Name, Impulsdauer,
 * Polarität, Kontakt/REX angeschlossen) in g_settings.doors[]. Index 0 = Tür 1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

/* Beim Boot vor allem anderen: alle Relais-Pins als Ausgang in den inaktiven Zustand
 * (Compile-Zeit-Polarität CFG_RELAY_ACTIVE_LOW, die NVS ist noch nicht gelesen). */
void      door_relay_safe_init(void);

esp_err_t door_init(void);
/* Relais der Tür idx für pulse_ms (0 = Wert aus Settings) anziehen; reason landet im Protokoll.
 * ESP_ERR_INVALID_ARG bei ungültigem Index, ESP_ERR_INVALID_STATE wenn die Tür deaktiviert ist. */
esp_err_t door_open(int idx, const char *reason, uint32_t pulse_ms);
bool      door_is_enabled(int idx);
bool      door_relay_active(int idx);
bool      door_any_relay_active(void);
bool      door_contact_closed(int idx);       /* true wenn kein Kontakt konfiguriert */
int64_t   door_last_open_uptime_s(int idx);
const char *door_name(int idx);
int       door_relay_gpio(int idx);
int       door_contact_gpio(int idx);
int       door_rex_gpio(int idx);
void      door_apply_settings(void);          /* Polarität/Eingänge nach Settings-Änderung */
