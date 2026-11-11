/*
 * File: Atari-memmap.c
 * Author: dgrubb
 * Date: 06/28/2017
 *
 * Delegates access to memory mapped devices.
 */
// TODO remove
#include <stdio.h>


#include "Atari-memmap.h"
#include "Atari-cart.h"
#include "Atari-TIA.h"
#include "../mos6507/mos6507.h"
#include "../mos6532/mos6532.h"

#define IS_TIA(x)  (x >= MEMMAP_TIA_START && x <= MEMMAP_TIA_END)
#define IS_RIOT(x) ((x >= MEMMAP_RIOT_RAM_START && x <= MEMMAP_RIOT_RAM_END) || \
                    (x >= MEMMAP_RIOT_RAM_MIRROR_START && x <= MEMMAP_RIOT_RAM_MIRROR_END) || \
                    (x >= MEMMAP_RIOT_PERIPH_START && x <= MEMMAP_RIOT_PERIPH_END) || \
                    (x >= MEMMAP_RIOT_PERIPH_MIRROR_START && x <= MEMMAP_RIOT_PERIPH_MIRROR_END))
#define IS_CART(x) (x >= MEMMAP_CART_START && x <= MEMMAP_CART_END)

void memmap_map_address(uint16_t *address)
{
    /* The 6507 only has 13 address lines (A0..A12), so largest address is 0x1FFF */
    *address &= 0x1FFFu;
}

/* Hardware-accurate Atari 2600 bus decoding:
 * A12 = 1: Cartridge (0x1000..0x1FFF)
 * A12 = 0: TIA and RIOT (0x0000..0x0FFF, mirrored every 1KB):
 *   A7 = 0: TIA (registers at addr & 0x3F for write, addr & 0x0F for read)
 *   A7 = 1: RIOT (MOS 6532):
 *     A9 = 0: RIOT RAM (128 bytes: addr & 0x7F)
 *     A9 = 1: RIOT I/O & Timers (0x0280 | (addr & 0x7F))
 */
void memmap_write(void)
{
    uint16_t address;
    uint8_t data;
    mos6507_get_data_bus(&data);
    mos6507_get_address_bus(&address);
    address &= 0x1FFFu;

    if (address & 0x1000u) {
        /* Cartridge space */
        cartridge_write(address & 0x0FFFu, data);
    } else {
        /* Internal console space */
        if (!(address & 0x0080u)) {
            /* TIA (A7 == 0) */
            TIA_write_register((uint8_t)(address & 0x3Fu), data);
        } else {
            /* RIOT (A7 == 1) */
            if (!(address & 0x0200u)) {
                /* RIOT RAM (A9 == 0, 128 bytes) */
                mos6532_write(address & 0x007Fu, data);
            } else {
                /* RIOT I/O and Timers (A9 == 1) */
                mos6532_write(0x0280u | (address & 0x007Fu), data);
            }
        }
    }
}

void memmap_read(uint8_t *data)
{
    uint16_t address;
    mos6507_get_address_bus(&address);
    address &= 0x1FFFu;

    if (address & 0x1000u) {
        /* Cartridge space */
        cartridge_read(address & 0x0FFFu, data);
    } else {
        /* Internal console space */
        if (!(address & 0x0080u)) {
            /* TIA (A7 == 0) */
            TIA_read_register((uint8_t)(address & 0x0Fu), data);
        } else {
            /* RIOT (A7 == 1) */
            if (!(address & 0x0200u)) {
                /* RIOT RAM (A9 == 0, 128 bytes) */
                mos6532_read(address & 0x007Fu, data);
            } else {
                /* RIOT I/O and Timers (A9 == 1) */
                mos6532_read(0x0280u | (address & 0x007Fu), data);
            }
        }
    }

    mos6507_set_data_bus(*data);
}

void memmap_map_riot_address(uint16_t *address)
{
    if (*address >= MEMMAP_RIOT_RAM_START && *address <= MEMMAP_RIOT_RAM_END) {
        *address = *address - MEMMAP_RIOT_RAM_START;
    }
    if (*address >= MEMMAP_RIOT_RAM_MIRROR_START && *address <= MEMMAP_RIOT_RAM_MIRROR_END) {
        *address = *address - MEMMAP_RIOT_RAM_MIRROR_START;
    }
    if (*address >= MEMMAP_RIOT_PERIPH_MIRROR_START && *address <= MEMMAP_RIOT_PERIPH_MIRROR_END) {
        *address = *address - 0x100;
    }
}
