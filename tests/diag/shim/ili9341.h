#pragma once
#include <stdint.h>
void ili9341_fill_screen(uint16_t);
void ili9341_text(const char *, uint16_t, uint16_t, uint16_t, uint16_t);
void ili9341_flush(bool);
