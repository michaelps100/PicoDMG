#pragma once
/* The SDK's own Core2350B board, not boards/pico2.h. pico2.h is the RP2350A
 * (30 GPIOs, ADC on 26..29): built on it, this firmware could not read any pin
 * from 30 up -- the power switch and the battery sense on GP40 never worked on
 * the console, and gpio_get() of a high pin always read 0. waveshare_core2350b.h
 * sets PICO_RP2350A 0 (48 GPIOs, ADC on 40..47). */
#include "boards/waveshare_core2350b.h"

/* Waveshare Core2350B (RP2350B, 16 MB flash + 8 MB PSRAM) -- console build.
 *
 * Same TFT/SD/button/audio wiring as boards/picolinx.h (that pinout already
 * matched the console), plus the two things that only exist on the console:
 *   - PSRAM chip select on GPIO 47. GPIO 47 does not exist on the RP2350A
 *     package, which is why this is based on the RP2350B board above.
 *   - onboard LED on GPIO 39.
 *
 * The panel itself (ILI9341 vs ST7789) is a runtime decision read from
 * /boot.txt by shared_menu/dmg_panel.c, not a compile-time constant here, so
 * this one binary drives both panels. See ili9341_init() in drivers/ili9341.
 */

// 16MB flash (the SDK board already says so; kept for the record)
#undef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES 16777216

// PSRAM (8 MB) on the QSPI/XIP bus, CS1. Requires the QMI driver
// (drivers/pico_psram), NOT the legacy PIO-SPI driver in drivers/psram.
#define PSRAM_CS_PIN 47

// SDCARD (SPI1, Waveshare RP2350 Plus / PicoNGP wiring: MISO12 CS13 SCK14 MOSI15)
#define SDCARD_SPI_BUS spi1
#define SDCARD_PIN_SPI0_CS 13
#define SDCARD_PIN_SPI0_SCK 14
#define SDCARD_PIN_SPI0_MOSI 15
#define SDCARD_PIN_SPI0_MISO 12

// User controls: direct GPIO buttons, active-low (PicoNGP layout)
#define UI_BTN_UP      2
#define UI_BTN_DOWN    3
#define UI_BTN_LEFT    4
#define UI_BTN_RIGHT   5
#define UI_BTN_A       6
#define UI_BTN_B       7
#define UI_BTN_SELECT  8
#define UI_BTN_START   9

// TFT ILI9341 or ST7789 (SPI0/PIO: CS17 SCK18 MOSI19 DC20 RST21 BL22)
#define TFT_CS_PIN  17
#define TFT_RST_PIN 21
#define TFT_LED_PIN 22
#define TFT_DC_PIN  20
#define TFT_DATA_PIN 19   /* MOSI */
#define TFT_CLK_PIN  18   /* SCK */

#define SMS_SINGLE_FILE 1

// Sound I2S MAX98357A (DIN26 BCLK27 LRCLK28 -- LRCLK is clock base + 1)
#define AUDIO_DATA_PIN  26
#define AUDIO_CLOCK_PIN 27

// Onboard LED (blink indicates ROM write / activity)
#define LED_PIN 39
