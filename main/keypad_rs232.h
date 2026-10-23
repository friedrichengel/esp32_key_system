/*
 * keypad_rs232.h – Keypad an UART1 (RS232 über MAX3232), Rahmen-Parser, Lernmodus
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef void (*keypad_code_cb_t)(const char *code);

esp_err_t keypad_start(keypad_code_cb_t cb);
void      keypad_apply_settings(void);       /* Baud/Parität/Rahmen nach Settings-Änderung */
/* Lernmodus: letzte Rohbytes als Hex ("41 42 0D ...") und ASCII (nicht druckbar = '.') */
size_t    keypad_get_raw(char *hex, size_t hex_len, char *ascii, size_t ascii_len);
int64_t   keypad_last_rx_uptime_s(void);     /* 0 = noch nie */
uint32_t  keypad_rx_count(void);
/* Optionale Rückmeldung ans Keypad (TX), z. B. Beep-Kommando; no-op ohne TX-Pin */
void      keypad_send(const uint8_t *data, size_t len);
