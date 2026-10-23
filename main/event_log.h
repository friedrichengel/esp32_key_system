/*
 * event_log.h – Ereignisprotokoll (Ringpuffer im PSRAM, thread-sicher)
 *
 * Jede sicherheitsrelevante Aktion landet hier: Zutritt, abgelehnter Code, Login,
 * Konfigurationsänderung, OTA, Netzwerkstatus. Die Web-UI liest den Puffer über die
 * API aus; die Persistierung auf LittleFS kommt in einer späteren Phase dazu.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

typedef enum {
    EV_SYSTEM = 0,
    EV_NETWORK,
    EV_ACCESS,      /* Code akzeptiert / Tür geöffnet */
    EV_DENIED,      /* Code abgelehnt / Sperre        */
    EV_LOGIN,
    EV_CONFIG,
    EV_OTA,
    EV_CATEGORY_COUNT
} event_category_t;

#define EVENT_MSG_LEN 96

typedef struct {
    uint32_t id;                 /* fortlaufend seit Boot            */
    time_t   ts;                 /* 0 = Zeit war noch nicht gültig   */
    uint32_t uptime_s;
    uint8_t  category;
    char     msg[EVENT_MSG_LEN];
} event_entry_t;

esp_err_t   event_log_init(void);
/* Zusätzlich jede Zeile nach CFG_LOG_PATH schreiben (nach dem Mount von LittleFS aktivieren). */
void        event_log_enable_file(bool enable);
void        event_log(event_category_t cat, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const char *event_category_name(event_category_t cat);

/* Liest bis zu max Einträge, neueste zuerst. Rückgabe: Anzahl kopierter Einträge. */
size_t      event_log_read(event_entry_t *out, size_t max, uint32_t after_id);
uint32_t    event_log_last_id(void);
