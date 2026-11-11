/**
 * Original source code from https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico
 * Copyright 2021 Carl John Kugler III
 * 
 * Licensed under the Apache License, Version 2.0 (the License); you may not use 
 * this file except in compliance with the License. You may obtain a copy of the 
 * License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0 
 * Unless required by applicable law or agreed to in writing, software distributed 
 * under the License is distributed on an AS IS BASIS, WITHOUT WARRANTIES OR 
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the 
 * specific language governing permissions and limitations under the License.
 * 
 */

#pragma once

#include <pico/stdlib.h>
#include <pico/stdio.h>
#include <string.h>
#include "my_debug.h"
#include "f_util.h"
#include "ff.h"
#include "sd_card.h"
#include "diskio.h"
#include "rtc.h"

void spi_dma_isr();

// SD SPI clock. This used to be a hardcoded 12.5 MHz, which is what the
// emulator has always run at: SD_SPI_BAUD_RATE was defined in CMake for years
// but never read by any source file, so the build line said 20 MHz while the
// hardware sat at 12.5. It is now the single source of truth.
//
// 20 MHz is the value the rest of the console already uses for the SD (PicoNES,
// PicoPCE). If reads ever become flaky, this is the first number to lower --
// 12.5 MHz is a safe fallback and costs ~0.15 s on a 4 MB cart.
#ifndef GB_SD_SPI_BAUD_HZ
#define GB_SD_SPI_BAUD_HZ 20000000u
#endif

static spi_t spis[]={
    {
        .hw_inst=spi1,   /* SD bus; GB_SD_SPI is a string, not a peripheral */
        .miso_gpio=GB_SD_PIN_MISO,
        .mosi_gpio=GB_SD_PIN_MOSI,
        .sck_gpio=GB_SD_PIN_SCK,
        .baud_rate=GB_SD_SPI_BAUD_HZ,
        .dma_isr=spi_dma_isr
    }
};

static sd_card_t sd_cards[]={
    {
        .pcName="0:",
        .spi=&spis[0],
        .ss_gpio=GB_SD_PIN_CS,
        .use_card_detect=false,
        .m_Status=STA_NOINIT
    }
};

void spi_dma_isr() {
    spi_irq_handler(&spis[0]);
}

size_t sd_get_num() {
    return count_of(sd_cards);
}

sd_card_t *sd_get_by_num(size_t num) {
    if (num<=sd_get_num()) {
        return &sd_cards[num];
    } else {
        return NULL;
    }
}

size_t spi_get_num() {
    return count_of(spis);
}

spi_t *spi_get_by_num(size_t num) {
    if (num <= sd_get_num()) {
        return &spis[num];
    } else {
        return NULL;
    }
}
