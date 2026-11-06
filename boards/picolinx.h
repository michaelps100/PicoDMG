#pragma once
#include "boards/pico2.h"

// 16MB flash
#define PICO_FLASH_SIZE_BYTES 16777216

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

// TFT ILI9341 (SPI0/PIO: CS17 SCK18 MOSI19 DC20 RST21 BL22)
#define TFT_CS_PIN  17
#define TFT_RST_PIN 21
#define TFT_LED_PIN 22
#define TFT_DC_PIN  20
#define TFT_DATA_PIN 19   /* MOSI */
#define TFT_CLK_PIN  18   /* SCK */

#define SMS_SINGLE_FILE 1

// Sound I2S MAX98357A (DIN26 BCLK27 LRCLK28)
#define AUDIO_DATA_PIN  26
#define AUDIO_CLOCK_PIN 27