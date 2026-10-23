#include "keypad_rs232.h"

#include <ctype.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"

static const char *TAG = "keypad";

static keypad_code_cb_t  s_cb;
static uint8_t           s_raw[CFG_KEYPAD_RAW_BUF];
static size_t            s_raw_head;
static size_t            s_raw_count;
static SemaphoreHandle_t s_raw_lock;
static int64_t           s_last_rx_s;
static uint32_t          s_rx_count;
static volatile bool     s_reconfigure;

static uart_parity_t parity_from_settings(void)
{
    switch (g_settings.keypad.parity) {
    case 1:  return UART_PARITY_EVEN;
    case 2:  return UART_PARITY_ODD;
    default: return UART_PARITY_DISABLE;
    }
}

static void raw_push(const uint8_t *d, size_t n)
{
    if (xSemaphoreTake(s_raw_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    for (size_t i = 0; i < n; i++) {
        s_raw[s_raw_head] = d[i];
        s_raw_head = (s_raw_head + 1) % CFG_KEYPAD_RAW_BUF;
        if (s_raw_count < CFG_KEYPAD_RAW_BUF) s_raw_count++;
    }
    xSemaphoreGive(s_raw_lock);
}

size_t keypad_get_raw(char *hex, size_t hex_len, char *ascii, size_t ascii_len)
{
    if (xSemaphoreTake(s_raw_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    size_t hp = 0, ap = 0;
    for (size_t i = 0; i < s_raw_count; i++) {
        size_t idx = (s_raw_head + CFG_KEYPAD_RAW_BUF - s_raw_count + i) % CFG_KEYPAD_RAW_BUF;
        uint8_t b = s_raw[idx];
        if (hex && hp + 4 < hex_len) hp += snprintf(hex + hp, hex_len - hp, "%02X ", b);
        if (ascii && ap + 2 < ascii_len) ascii[ap++] = isprint(b) ? (char)b : '.';
    }
    if (ascii && ascii_len) ascii[ap] = '\0';
    if (hex && hex_len) hex[hp] = '\0';
    size_t n = s_raw_count;
    xSemaphoreGive(s_raw_lock);
    return n;
}

int64_t keypad_last_rx_uptime_s(void)
{
    return s_last_rx_s;
}

uint32_t keypad_rx_count(void)
{
    return s_rx_count;
}

void keypad_send(const uint8_t *data, size_t len)
{
    if (CFG_KEYPAD_TX_GPIO >= 0 && data && len) {
        uart_write_bytes(CFG_KEYPAD_UART_NUM, data, len);
    }
}

void keypad_apply_settings(void)
{
    s_reconfigure = true;
}

static void apply_uart_params(void)
{
    uart_set_baudrate(CFG_KEYPAD_UART_NUM, g_settings.keypad.baud);
    uart_set_parity(CFG_KEYPAD_UART_NUM, parity_from_settings());
    uart_flush_input(CFG_KEYPAD_UART_NUM);
    ESP_LOGI(TAG, "UART %" PRIu32 " Bd, Paritaet %u, Rahmen %u, Term 0x%02X, Clear 0x%02X, Timeout %u ms",
             g_settings.keypad.baud, g_settings.keypad.parity, g_settings.keypad.frame_mode,
             (unsigned char)g_settings.keypad.terminator, (unsigned char)g_settings.keypad.clear_key,
             g_settings.keypad.timeout_ms);
}

static void emit(char *buf, size_t *len)
{
    if (*len >= CFG_CODE_MIN_LEN) {
        buf[*len] = '\0';
        if (s_cb) s_cb(buf);
    } else if (*len > 0) {
        ESP_LOGD(TAG, "Eingabe zu kurz (%u), verworfen", (unsigned)*len);
    }
    *len = 0;
}

static void keypad_task(void *arg)
{
    (void)arg;
    char code[CFG_KEYPAD_MAX_CODE_LEN + 1];
    size_t len = 0;
    int64_t last_byte_us = 0;
    uint8_t rx[64];

    while (1) {
        if (s_reconfigure) {
            s_reconfigure = false;
            apply_uart_params();
            len = 0;
        }
        const settings_keypad_t *k = &g_settings.keypad;
        uint32_t wait_ms = (k->frame_mode == KEYPAD_FRAME_TIMEOUT) ? k->timeout_ms : 100;
        int n = uart_read_bytes(CFG_KEYPAD_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(wait_ms));
        int64_t now = esp_timer_get_time();

        if (n <= 0) {
            /* Pause: im Timeout-Modus Eingabe abschließen, sonst alte Teileingabe verwerfen */
            if (len > 0) {
                int64_t gap_ms = (now - last_byte_us) / 1000;
                if (k->frame_mode == KEYPAD_FRAME_TIMEOUT && gap_ms >= k->timeout_ms) {
                    emit(code, &len);
                } else if (gap_ms >= CFG_KEYPAD_IDLE_RESET_MS) {
                    ESP_LOGD(TAG, "Teileingabe nach Pause verworfen");
                    len = 0;
                }
            }
            continue;
        }

        raw_push(rx, n);
        s_rx_count += n;
        s_last_rx_s = now / 1000000;
        last_byte_us = now;

        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            if (k->clear_key && c == k->clear_key) {
                len = 0;
                continue;
            }
            if (k->frame_mode == KEYPAD_FRAME_TERMINATOR && k->terminator && c == k->terminator) {
                emit(code, &len);
                continue;
            }
            if (!isalnum((unsigned char)c)) {
                continue;                        /* CR/LF/Steuerzeichen/Protokollrahmen ignorieren */
            }
            if (len < CFG_KEYPAD_MAX_CODE_LEN) {
                code[len++] = c;
            } else {
                /* Überlauf: zu lange Eingabe verwerfen */
                len = 0;
                ESP_LOGD(TAG, "Eingabe zu lang, verworfen");
                continue;
            }
            if (k->frame_mode == KEYPAD_FRAME_FIXED_LEN && len >= k->fixed_len) {
                emit(code, &len);
            }
        }
    }
}

esp_err_t keypad_start(keypad_code_cb_t cb)
{
    s_cb = cb;
    s_raw_lock = xSemaphoreCreateMutex();
    if (!s_raw_lock) return ESP_ERR_NO_MEM;

    uart_config_t cfg = {
        .baud_rate = (int)g_settings.keypad.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = parity_from_settings(),
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(CFG_KEYPAD_UART_NUM, CFG_KEYPAD_RX_BUF_SIZE, 256, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_param_config(CFG_KEYPAD_UART_NUM, &cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_pin(CFG_KEYPAD_UART_NUM, CFG_KEYPAD_TX_GPIO, CFG_KEYPAD_RX_GPIO,
                                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(keypad_task, "keypad", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "Keypad an UART%d: RX GPIO%d, TX GPIO%d, %" PRIu32 " Bd", CFG_KEYPAD_UART_NUM,
             CFG_KEYPAD_RX_GPIO, CFG_KEYPAD_TX_GPIO, g_settings.keypad.baud);
    return ESP_OK;
}
