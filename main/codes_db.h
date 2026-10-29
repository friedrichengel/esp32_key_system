/*
 * codes_db.h – Zutrittscodes (Dauer-, Zeit- und Einmalcodes)
 *
 * Persistiert als JSON auf LittleFS (/storage/codes.json), im RAM gehalten,
 * thread-sicher über Mutex. Codes stehen im Klartext (Begründung in PLAN.md §7).
 *
 * Besitz und Freigabe (PLAN.md §8b): owner_uid (0 = Admin) und shared (Bit uid) bestimmen,
 * welcher Benutzer einen Code sieht. Einmalcodes können ein Nutzungsfenster grace_s haben
 * (PLAN.md §8d): ab der ersten erfolgreichen Eingabe so viele Sekunden beliebig oft nutzbar.
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
    CODE_TYPE_ONETIME   = 2,   /* genau eine Nutzung (bzw. Nutzungsfenster)       */
} code_type_t;

#define CODE_TYPE_BIT(t) (1u << (t))

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
    uint32_t doors;                /* Bitmaske: Bit n = Tür n+1 darf mit diesem Code geöffnet werden */
    uint32_t max_uses;             /* 0 = unbegrenzt */
    uint32_t uses;
    int64_t  last_used;
    int64_t  created_at;
    char     created_by[32];
    uint8_t  owner_uid;            /* 0 = Admin, sonst Benutzer-uid */
    uint32_t shared;               /* Freigabe: Bit uid (1..CFG_USERS_MAX) */
    uint16_t grace_s;              /* Einmalcode: Nutzungsfenster nach erster Eingabe, 0 = einmal */
    int64_t  grace_until_us;       /* nur RAM (Laufzeituhr): Fensterende, 0 = kein Fenster aktiv */
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
    CODE_WRONG_DOOR,               /* Code gilt nicht für diese Tür */
} code_result_t;

esp_err_t     codes_init(void);
size_t        codes_count(void);
size_t        codes_count_owned(uint8_t owner_uid);
size_t        codes_list(code_entry_t *out, size_t max);
bool          codes_get(uint32_t id, code_entry_t *out);
esp_err_t     codes_add(code_entry_t *e);            /* vergibt id/created_at, speichert */
esp_err_t     codes_update(const code_entry_t *e);   /* Zähler, Herkunft, laufendes Fenster bleiben */
esp_err_t     codes_delete(uint32_t id);
esp_err_t     codes_clear(void);

/* Sichtbarkeit: Admin alles, sonst Besitzer oder freigegeben */
bool          codes_visible_to(const code_entry_t *e, uint8_t uid, bool is_admin);
/* Benutzer wird gelöscht: Codes übernehmen (to_uid) oder löschen, Freigabe-Bit überall entfernen */
esp_err_t     codes_user_removed(uint8_t uid, bool delete_codes, uint8_t to_uid);

/* Prüft einen eingegebenen Code für Tür door_idx; bei CODE_OK werden uses/last_used erhöht und gespeichert.
 * in_grace (optional): true, wenn die Nutzung innerhalb eines laufenden Einmalcode-Fensters erfolgte. */
code_result_t codes_check_and_use(const char *code, int door_idx, code_entry_t *matched, bool *in_grace);
#define CODES_ALL_DOORS CFG_ALL_DOORS_MASK
uint32_t      codes_grace_remaining_s(const code_entry_t *e);
const char   *codes_result_text(code_result_t r);
const char   *codes_type_name(code_type_t t);
bool          codes_type_from_name(const char *s, code_type_t *out);
bool          codes_valid_code_string(const char *code);

/* JSON-Konvertierung eines Eintrags (API und Backup) */
cJSON        *codes_entry_to_json(const code_entry_t *e);
bool          codes_entry_from_json(const cJSON *o, code_entry_t *e);

/* Backup/Restore; codes_to_json_for liefert nur die für uid sichtbaren Codes */
cJSON        *codes_to_json(void);
cJSON        *codes_to_json_for(uint8_t uid, bool is_admin);
esp_err_t     codes_from_json(const cJSON *arr, bool replace);
