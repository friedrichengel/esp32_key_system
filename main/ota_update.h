/*
 * ota_update.h – Signierte Firmware-Updates über die Web-UI
 *
 * Das Image wird in die inaktive OTA-Partition geschrieben; esp_ota_end() prüft die
 * RSA-3072-Signatur (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) gegen den
 * öffentlichen Schlüssel im Signaturblock der laufenden App. Nach dem Neustart
 * bestätigt ota_confirm_running() die neue Firmware, sonst Rollback durch den Bootloader.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

typedef enum {
    OTA_IDLE = 0,
    OTA_RECEIVING,
    OTA_VERIFYING,
    OTA_DONE,          /* Boot-Partition umgeschaltet, Neustart ausstehend */
    OTA_FAILED,
} ota_state_t;

esp_err_t ota_init(void);                 /* beim Boot: Rollback-Status prüfen, Bestätigung planen */
void      ota_confirm_running(void);      /* markiert laufende App als gültig (idempotent) */

esp_err_t ota_begin(size_t image_size);   /* 0 = unbekannt */
esp_err_t ota_write(const void *data, size_t len);
esp_err_t ota_end(void);                  /* verifiziert + setzt Boot-Partition */
void      ota_abort(void);

ota_state_t ota_state(void);
const char *ota_last_error(void);
size_t      ota_bytes_written(void);

/* Infos für die Firmware-Seite: laufende/nächste Partition, Versionen, Rollback-Option */
cJSON      *ota_info_json(void);
esp_err_t   ota_rollback(void);           /* bootet die andere Partition, falls gültig */
