/*
 * access.h – Zutrittslogik: Keypad-Code -> Prüfung -> Relais, Sperre nach Fehlversuchen
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t access_init(void);                       /* startet Keypad und Relais */
void      access_on_keypad_code(const char *code); /* vom Keypad-Task aufgerufen */
bool      access_keypad_locked(uint32_t *remaining_s);
uint32_t  access_fail_count(void);
