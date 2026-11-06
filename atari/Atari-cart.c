/*
 * File: Atari-cart.c
 * Mimics ROM space (that is, a game cartridge).
 * Supports 2K, 4K, 8K (F8), 16K (F6), 32K (F4) bankswitching.
 */

#include "Atari-cart.h"
#include <string.h>

#define MAX_CART_SIZE 65536

static uint8_t s_cart_ram[MAX_CART_SIZE];
static uint32_t s_cart_size = 0;
static uint8_t s_cart_bank = 0;
static uint8_t s_cart_bank_count = 1;

static void check_bankswitch(uint16_t address) {
    if (s_cart_bank_count <= 1) return;

    if (s_cart_bank_count == 2) {
        /* F8: 8K (2 x 4K banks) */
        if (address == 0x0FF8) s_cart_bank = 0;
        else if (address == 0x0FF9) s_cart_bank = 1;
    } else if (s_cart_bank_count == 4) {
        /* F6: 16K (4 x 4K banks) */
        if (address >= 0x0FF6 && address <= 0x0FF9) {
            s_cart_bank = (uint8_t)(address - 0x0FF6);
        }
    } else if (s_cart_bank_count == 8) {
        /* F4: 32K (8 x 4K banks) */
        if (address >= 0x0FF4 && address <= 0x0FFB) {
            s_cart_bank = (uint8_t)(address - 0x0FF4);
        }
    } else if (s_cart_bank_count == 16) {
        /* EF / 64K: 16 x 4K banks (hotspots ONLY at $0FE0..$0FEF) */
        if (address >= 0x0FE0 && address <= 0x0FEF) {
            s_cart_bank = (uint8_t)(address - 0x0FE0);
        }
    }
}

void cartridge_read(uint16_t address, uint8_t *data) {
    if (s_cart_size == 0) {
        *data = 0xFF;
        return;
    }

    check_bankswitch(address);

    if (s_cart_size <= 2048) {
        /* 2KB ROM mirrored in 4KB address space */
        *data = s_cart_ram[address & 0x07FF];
    } else if (s_cart_bank_count > 1) {
        /* Bankswitched */
        uint32_t offset = (uint32_t)s_cart_bank * 4096u + (address & 0x0FFFu);
        if (offset < s_cart_size) {
            *data = s_cart_ram[offset];
        } else {
            *data = 0xFF;
        }
    } else {
        /* Standard 4KB */
        *data = s_cart_ram[address & 0x0FFF];
    }
}

void cartridge_write(uint16_t address, uint8_t data) {
    (void)data;
    if (s_cart_size == 0) return;
    check_bankswitch(address);
}

uint8_t *cartridge_get_buffer(size_t *max_size) {
    if (max_size) *max_size = MAX_CART_SIZE;
    return s_cart_ram;
}

bool cartridge_load(const uint8_t *cart, size_t size) {
    if (!cart || size == 0) return false;

    /* Detect and strip headers (e.g. 128-byte A26/Stella header, or 32/64 byte headers) */
    if ((size % 1024) == 128 && size > 128) {
        cart += 128;
        size -= 128;
    } else if ((size % 1024) == 32 && size > 32) {
        cart += 32;
        size -= 32;
    } else if ((size % 1024) == 64 && size > 64) {
        cart += 64;
        size -= 64;
    }

    if (size > MAX_CART_SIZE) {
        size = MAX_CART_SIZE;
    }

    if (s_cart_ram != cart) {
        memmove(s_cart_ram, cart, size);
    }
    s_cart_size = (uint32_t)size;

    if (s_cart_size <= 2048) {
        s_cart_bank_count = 1;
        s_cart_bank = 0;
    } else if (s_cart_size <= 4096) {
        s_cart_bank_count = 1;
        s_cart_bank = 0;
    } else if (s_cart_size <= 8192) {
        s_cart_bank_count = 2;
        s_cart_bank = 1; /* Atari F8 starts in bank 1 where reset vector is */
    } else if (s_cart_size <= 16384) {
        s_cart_bank_count = 4;
        s_cart_bank = 3; /* Atari F6 starts in bank 3 */
    } else if (s_cart_size <= 32768) {
        s_cart_bank_count = 8;
        s_cart_bank = 7; /* Atari F4 starts in bank 7 */
    } else {
        s_cart_bank_count = 16;
        s_cart_bank = 15; /* Atari EF (64K) starts in bank 15 */
    }

    return true;
}

void cartridge_eject(void) {
    memset(s_cart_ram, 0xFF, sizeof(s_cart_ram));
    s_cart_size = 0;
    s_cart_bank = 0;
    s_cart_bank_count = 1;
}

uint8_t cartridge_get_bank(void) {
    return s_cart_bank;
}

void cartridge_set_bank(uint8_t bank) {
    if (bank < s_cart_bank_count) {
        s_cart_bank = bank;
    }
}

uint32_t cartridge_get_size(void) {
    return s_cart_size;
}
