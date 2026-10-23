/*
 * codes_db.h – Zutrittscodes (Dauer-, Zeit- und Einmalcodes)
 *
 * Persistiert als JSON auf LittleFS (/storage/codes.json), im RAM gehalten,
 * thread-sicher über Mutex. Codes stehen im Klartext (Begründung in PLAN.md §7).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"
#include "cJSON.h"
#include "config.h"

typedef enum {
    CODE_TYPE_PERMANENT = 0,   /* ohne Ablauf, optional Wochentag-/Uhrzeitfenster */
    CODE_TYPE_TIMED     = 1,   /* gültig von/bis                                  */
    CODE_TYPE_ONETIME   = 2,   /* genau eine Nutzung                              */
} code_type_t;

typedef struct {
    uint32_t id;
    char     name[32];
    char     code[CFG_KEYPAD_MAX_CODE_LEN + 1];
    uint8_t  type;                 /* code_type_t */
    bool     enabled;
    int64_t  valid_from;           /* Unix-Sekunden, 0 = offen */
    int64_t  valid_until;          /* Unix-Sekunden, 0 = offen */
    bool     win_enable;           /* Wochentag-/Uhrzeitfenster aktiv */
    uint8_t  weekdays;             /* Bit0 = Montag ... Bit6 = Sonntag */
    uint16_t win_from_min;         /* Minuten seit Mitternacht, lokal */
    uint16_t win_to_min;           /* exklusiv; < win_from = über Mitternacht */
    uint32_t max_uses;             /* 0 = unbegrenzt */
    uint32_t uses;
    int64_t  last_used;
    int64_t  created_at;
    char     created_by[32];
} code_entry_t;

typedef enum {
    CODE_OK = 0,
    CODE_UNKNOWN,
    CODE_DISABLED,
    CODE_NOT_YET_VALID,
    CODE_EXPIRED,
    CODE_OUTSIDE_WINDOW,
    CODE_USES_EXHAUSTED,
    CODE_TIME_UNKNOWN,             /* Zeitprüfung nötig, aber Uhr nicht synchron */
} code_result_t;

esp_err_t     codes_init(void);
size_t        codes_count(void);
size_t        codes_list(code_entry_t *out, size_t max);
bool          codes_get(uint32_t id, code_entry_t *out);
esp_err_t     codes_add(code_entry_t *e);            /* vergibt id/created_at, speichert */
esp_err_t     codes_update(const code_entry_t *e);
esp_err_t     codes_delete(uint32_t id);
esp_err_t     codes_clear(void);

/* Prüft einen eingegebenen Code; bei CODE_OK werden uses/last_used erhöht und gespeichert. */
code_result_t codes_check_and_use(const char *code, code_entry_t *matched);
const char   *codes_result_text(code_result_t r);
const char   *codes_type_name(code_type_t t);
bool          codes_type_from_name(const char *s, code_type_t *out);
bool          codes_valid_code_string(const char *code);

/* JSON-Konvertierung eines Eintrags (API und Backup) */
cJSON        *codes_entry_to_json(const code_entry_t *e);
bool          codes_entry_from_json(const cJSON *o, code_entry_t *e);

/* Backup/Restore */
cJSON        *codes_to_json(void);
esp_err_t     codes_from_json(const cJSON *arr, bool replace);
