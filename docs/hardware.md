# Hardware-Notizen

## Board: ESP32-S31-Function-CoreBoard-1

- Modul ESP32-S31-WROOM-3, 16 MB Flash, 16 MB PSRAM, 320 MHz Dual-Core RISC-V
- Gigabit-Ethernet: YT8531-PHY an RGMII; MDC GPIO5, MDIO GPIO6, Reset GPIO7, PHY-Adresse 0
- RGMII-Datenpins (IO-MUX-Set 0 laut ESP-IDF `esp32s31/emac_periph.c`):
  TX_CLK 13, TX_CTL 12, TXD0-3 8/9/10/11, RX_CLK 14, RX_CTL 15, RXD0-3 19/18/17/16
- YT8531 nach Reset: Autonegotiation wieder einschalten, RX/TX-Delay ≈ 2 ns über Extended-Register 0x1E/0x1F (0xA001 Bit 8; 0xA003 Stufe 13)
- WS2812-LED GPIO60, BOOT GPIO61, Konsole UART0 GPIO58/59
- Audiocodec ES8311 + Mikro/Lautsprecher onboard: **Pins unbekannt**, Schaltplan besorgen, bevor externe Pins festgezurrt werden

## J2-Header (40-polig, laut User-Guide)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | GND | 2 | GND |
| 3 | U0TXD GPIO58 | 4 | U0RXD GPIO59 |
| 5 | GPIO61 (BOOT) | 6 | GPIO60 (RGB-LED) |
| 7 | GPIO2 | 8 | GND |
| 9 | GPIO0 | 10 | GPIO3 |
| 11 | GPIO49 | 12 | GPIO1 |
| 13 | GPIO47 | 14 | GPIO48 |
| 15 | GPIO45 | 16 | GPIO46 |
| 17 | GPIO43 | 18 | GPIO44 |
| 19 | GPIO40 | 20 | GPIO42 |
| 21 | GPIO39 | 22 | GPIO38 |
| 23 | GPIO37 | 24 | GPIO36 |
| 25 | GPIO35 | 26 | GPIO20 (SDIO D0) |
| 27 | GPIO21 (SDIO D1) | 28 | GPIO22 (SDIO D2) |
| 29 | GPIO23 (SDIO D3) | 30 | GPIO24 (SDIO CLK) |
| 31 | GPIO25 (SDIO CMD) | 32 | GPIO4 |
| 33 | GND | 34 | GND |
| 35 | 3V3 | 36 | 3V3 |
| 37 | GND | 38 | GND |
| 39 | 5V | 40 | 5V |

## Externe Beschaltung (Vorschlag)

```
Keypad (RS232) ──► MAX3232-Modul (3,3 V) ──► GPIO38 (UART1 RX), optional GPIO37 (TX)
GPIO40 ──► Relaismodul (Optokoppler, 5 V vom J2) ──► NO-Kontakt in Serie zum Türöffner (12 V)
GPIO42 ◄── Reed-Kontakt Tür (gegen GND, interner Pull-up)
GPIO39 ◄── Austaster innen (gegen GND, interner Pull-up)
```

Stromversorgung: 5 V über J2 oder USB-C; Türöffner aus eigenem Netzteil, nie aus dem Board.
