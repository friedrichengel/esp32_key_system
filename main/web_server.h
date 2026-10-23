/*
 * web_server.h – HTTPS-Server (esp_https_server), HTTP->HTTPS-Redirect, REST-API, Web-UI
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t web_server_start(void);
bool      web_server_is_tls(void);
/* Verzögerter Neustart (für /api/system/reboot), damit die Antwort noch rausgeht. */
void      web_server_schedule_reboot(uint32_t delay_ms);
