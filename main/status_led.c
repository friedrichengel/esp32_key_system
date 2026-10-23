#include "status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "config.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static volatile led_state_t s_state = LED_BOOT;
static volatile led_state_t s_flash_state;
static volatile int64_t     s_flash_until_us;

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    uint32_t br = CFG_LED_BRIGHTNESS;
    led_strip_set_pixel(s_strip, 0, r * br / 255, g * br / 255, b * br / 255);
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    while (1) {
        led_state_t st = s_state;
        if (s_flash_until_us && esp_timer_get_time() < s_flash_until_us) {
            st = s_flash_state;
        } else {
            s_flash_until_us = 0;
        }
        bool blink = (tick / 5) % 2;   /* 500 ms Takt bei 100 ms Schleife */
        bool fast  = tick % 2;         /* 100 ms Takt                       */
        switch (st) {
        case LED_BOOT:      set_rgb(blink ? 255 : 0, blink ? 180 : 0, 0); break;
        case LED_NO_LINK:   set_rgb(255, 0, 0); break;
        case LED_NO_IP:     set_rgb(blink ? 255 : 0, blink ? 90 : 0, 0); break;
        case LED_OK:        set_rgb(0, 40, 0); break;
        case LED_DOOR_OPEN: set_rgb(0, 60, 255); break;
        case LED_DENIED:    set_rgb(255, 0, 0); break;
        case LED_LOCKED:    set_rgb(fast ? 255 : 0, 0, 0); break;
        case LED_OTA:       set_rgb(blink ? 200 : 0, blink ? 200 : 0, blink ? 200 : 0); break;
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CFG_PIN_LED_WS2812,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 nicht initialisiert: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    return ESP_OK;
}

void status_led_set(led_state_t state)
{
    s_state = state;
}

void status_led_flash(led_state_t state, uint32_t duration_ms)
{
    s_flash_state = state;
    s_flash_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
}
