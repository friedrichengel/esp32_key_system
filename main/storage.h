/*
 * storage.h – LittleFS-Datenpartition ("storage", gemountet unter /storage)
 *
 * Enthält wie bei Casambi_Gateway/_Logic das HTTPS-Zertifikat samt Key sowie das
 * persistente Ereignisprotokoll. Wird beim ersten Start formatiert.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t storage_init(void);
bool      storage_file_exists(const char *path);
/* Liest eine Datei komplett in einen neu allokierten, nullterminierten Puffer (free() durch Aufrufer). */
char     *storage_read_file(const char *path, size_t *len_out);
esp_err_t storage_write_file(const char *path, const void *data, size_t len);
esp_err_t storage_remove(const char *path);
esp_err_t storage_usage(size_t *total, size_t *used);
