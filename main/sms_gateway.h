/*
 * sms_gateway.h – SMS-Versand über die REST-API eines Mobilfunkrouters (Teltonika RutOS 7.x)
 *
 * Ablauf je SMS: POST <url>/api/login {username, password} -> Token (5 min gültig),
 * dann POST <url>/api/messages/actions/send {data:{number, message, modem}} mit Bearer-Token.
 * Zugangsdaten stehen in g_settings.sms (ab Werk aus). Der Nachrichtentext wird nie
 * protokolliert, er enthält in der Regel einen Zutrittscode.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

bool      sms_gateway_enabled(void);
/* Blockiert bis zu ~2 x CFG_SMS_HTTP_TIMEOUT_MS. err (optional) erhält eine deutsche Fehlermeldung (ASCII). */
esp_err_t sms_gateway_send(const char *number, const char *text, char *err, size_t err_len);
/* Telefonnummer plausibel: optional "+", danach 6..20 Ziffern, Leerzeichen/Bindestriche werden ignoriert */
bool      sms_gateway_valid_number(const char *number);
