#ifndef _SDCARD_H_
#define _SDCARD_H_

#include "pico/stdlib.h"

/* SPI pin assignment for SD card (Waveshare RP2350 Plus: SPI1 on MISO 12, CS 13, SCK 14, MOSI 15) */

#ifndef SDCARD_SPI_BUS
#define SDCARD_SPI_BUS spi1
#endif

#ifndef SDCARD_PIN_SPI0_CS
#define SDCARD_PIN_SPI0_CS     13
#endif

#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK    14
#endif

#ifndef SDCARD_PIN_SPI0_MOSI
#define SDCARD_PIN_SPI0_MOSI   15
#endif

#ifndef SDCARD_PIN_SPI0_MISO 
#define SDCARD_PIN_SPI0_MISO   12
#endif

#endif // _SDCARD_H_
