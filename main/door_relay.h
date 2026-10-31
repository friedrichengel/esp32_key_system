/*
 * door_relay.h – Türen: Relais (Türöffner oder Rolltor Auf/Ab), Türkontakt, Austaster (REX)
 *
 * Pins stehen in config.h (CFG_DOOR_*_GPIOS), Laufzeitwerte (aktiv, Name, Impulsdauer,
 * Polarität, Kontakt/REX angeschlossen) in g_settings.doors[], Türtyp und Rolltor-Relais in
 * g_settings.door_ext[]. Index 0 = Tür 1.
 *
 * Die CFG_MAX_DOORS Relais-Pins bilden einen Pool (Relais 1..4). Ein Türöffner nutzt fest das
 * Relais mit seinem Türindex, ein Rolltor zwei frei gewählte (Auf und Ab). Jedes Relais gehört
 * höchstens einer aktiven Tür; bei einem Rolltor sind Auf und Ab gegeneinander verriegelt.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"
#include "settings.h"

typedef enum {
    DOOR_DIR_NONE = 0,
    DOOR_DIR_UP   = 1,                            /* Türöffner: "öffnen", Rolltor: "auf" */
    DOOR_DIR_DOWN = 2,                            /* nur Rolltor: "ab"                   */
} door_dir_t;

/* Beim Boot vor allem anderen: alle Relais-Pins als Ausgang in den inaktiven Zustand
 * (Compile-Zeit-Polarität CFG_RELAY_ACTIVE_LOW, die NVS ist noch nicht gelesen). */
void      door_relay_safe_init(void);

esp_err_t door_init(void);
/* Türöffner: Relais für pulse_ms (0 = Wert aus Settings) anziehen. Rolltor: Relais "Auf".
 * reason landet im Protokoll. ESP_ERR_INVALID_ARG bei ungültigem Index, ESP_ERR_INVALID_STATE
 * wenn die Tür deaktiviert ist oder ihr Relais einer anderen Tür gehört. */
esp_err_t door_open(int idx, const char *reason, uint32_t pulse_ms);
/* Rolltor: Relais "Ab" anziehen; ESP_ERR_NOT_SUPPORTED bei einem Türöffner. */
esp_err_t door_close(int idx, const char *reason, uint32_t pulse_ms);
bool      door_is_enabled(int idx);
bool      door_is_gate(int idx);
bool      door_relay_active(int idx);             /* irgendein Relais dieser Tür angezogen */
door_dir_t door_active_dir(int idx);              /* gerade laufende Richtung              */
door_dir_t door_last_dir(int idx);                /* letzter Fahrbefehl seit Start         */
bool      door_any_relay_active(void);
bool      door_contact_closed(int idx);           /* true wenn kein Kontakt konfiguriert   */
int64_t   door_last_open_uptime_s(int idx);       /* letzter Befehl (auf oder ab)          */
const char *door_name(int idx);
int       door_relay_gpio(int idx);               /* Relais des Pools mit diesem Index     */
int       door_relay_up_gpio(int idx);            /* genutztes Relais für auf/öffnen       */
int       door_relay_down_gpio(int idx);          /* -1 bei Türöffner                      */
int       door_contact_gpio(int idx);
int       door_rex_gpio(int idx);
/* Konfiguration prüfen: Relaisindizes gültig, Auf != Ab, kein Relais bei zwei aktiven Türen.
 * Liefert NULL oder eine deutsche Fehlermeldung (ASCII) in err. */
const char *door_settings_check(const settings_door_t doors[CFG_MAX_DOORS],
                                const settings_door_ext_t ext[CFG_MAX_DOORS], char *err, size_t err_len);
void      door_apply_settings(void);              /* Polarität/Eingänge/Besitzer nach Settings-Änderung */
