/*
 * status_led.h – Onboard-WS2812 (GPIO60) als Statusanzeige
 */
#pragma once

#include "esp_err.h"

typedef enum {
    LED_BOOT = 0,       /* gelb blinkend: startet                      */
    LED_NO_LINK,        /* rot: kein Ethernet-Link                     */
    LED_NO_IP,          /* orange blinkend: Link, aber keine IP        */
    LED_OK,             /* grün gedimmt: betriebsbereit                */
    LED_DOOR_OPEN,      /* blau: Relais aktiv                          */
    LED_DENIED,         /* rot kurz: Code abgelehnt                    */
    LED_LOCKED,         /* rot blinkend: Keypad gesperrt               */
    LED_OTA,            /* weiß blinkend: Firmware-Update läuft        */
} led_state_t;

esp_err_t status_led_init(void);
void      status_led_set(led_state_t state);
/* Kurzzeitiger Zustand (z. B. DENIED), danach zurück zum vorherigen. */
void      status_led_flash(led_state_t state, uint32_t duration_ms);
