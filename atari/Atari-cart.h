/*
 * File: Atari-cart.h
 * Mimics ROM space (game cartridge) with banking support for 2K, 4K, 8K, 16K, 32K.
 */

#ifndef _ATARI_CART_H
#define _ATARI_CART_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void cartridge_read(uint16_t address, uint8_t *data);
void cartridge_write(uint16_t address, uint8_t data);
bool cartridge_load(const uint8_t *cart, size_t size);
void cartridge_eject(void);
uint8_t *cartridge_get_buffer(size_t *max_size);

uint8_t cartridge_get_bank(void);
void cartridge_set_bank(uint8_t bank);
uint32_t cartridge_get_size(void);

#ifdef __cplusplus
}
#endif

#endif /* _ATARI_CART_H */
