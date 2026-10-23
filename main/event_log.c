#include "event_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "time_sync.h"

static const char *TAG = "event";

static void append_to_file(const event_entry_t *e);

static bool               s_file_enabled;
static event_entry_t     *s_buf;
static size_t             s_cap;
static size_t             s_head;      /* nächster Schreibindex */
static size_t             s_count;
static uint32_t           s_next_id = 1;
static SemaphoreHandle_t  s_lock;

static const char *s_cat_names[EV_CATEGORY_COUNT] = {
    "system", "network", "access", "denied", "login", "config", "ota",
};

const char *event_category_name(event_category_t cat)
{
    return (cat < EV_CATEGORY_COUNT) ? s_cat_names[cat] : "?";
}

esp_err_t event_log_init(void)
{
    s_cap = CFG_EVENT_LOG_CAPACITY;
    s_buf = heap_caps_calloc(s_cap, sizeof(event_entry_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        s_buf = calloc(s_cap, sizeof(event_entry_t));
    }
    if (!s_buf) {
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void event_log(event_category_t cat, const char *fmt, ...)
{
    event_entry_t e = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);

    e.category = (uint8_t)cat;
    e.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    e.ts = time_sync_is_valid() ? time(NULL) : 0;

    ESP_LOGI(TAG, "[%s] %s", event_category_name(cat), e.msg);

    if (!s_buf || !s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    e.id = s_next_id++;
    s_buf[s_head] = e;
    s_head = (s_head + 1) % s_cap;
    if (s_count < s_cap) {
        s_count++;
    }
    xSemaphoreGive(s_lock);

    if (s_file_enabled) {
        append_to_file(&e);
    }
}

/* ------------------------------------------------------------------------
 * Persistenz auf LittleFS: eine Zeile je Ereignis, Rotation bei CFG_EVENT_LOG_FILE_MAX
 * ---------------------------------------------------------------------- */
static void append_to_file(const event_entry_t *e)
{
    struct stat st;
    if (stat(CFG_LOG_PATH, &st) == 0 && st.st_size > CFG_EVENT_LOG_FILE_MAX) {
        unlink(CFG_LOG_PATH ".1");
        rename(CFG_LOG_PATH, CFG_LOG_PATH ".1");
    }
    FILE *f = fopen(CFG_LOG_PATH, "a");
    if (!f) {
        return;
    }
    char tbuf[24] = "-";
    if (e->ts) {
        time_sync_format_local(e->ts, tbuf, sizeof(tbuf));
    }
    fprintf(f, "%" PRIu32 ";%s;%" PRIu32 ";%s;%s\n", e->id, tbuf, e->uptime_s,
            event_category_name((event_category_t)e->category), e->msg);
    fclose(f);
}

void event_log_enable_file(bool enable)
{
    s_file_enabled = enable;
}

size_t event_log_read(event_entry_t *out, size_t max, uint32_t after_id)
{
    if (!s_buf || !s_lock || !out || max == 0) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max; i++) {
        size_t idx = (s_head + s_cap - 1 - i) % s_cap;   /* neueste zuerst */
        if (s_buf[idx].id <= after_id) {
            break;
        }
        out[n++] = s_buf[idx];
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t event_log_last_id(void)
{
    return s_next_id - 1;
}
