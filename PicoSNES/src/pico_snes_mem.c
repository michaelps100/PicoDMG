#include "pico_snes_mem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PSRAM_BASE_ADDR ((uint8_t*)0x11000000)

/* Cartridge save RAM sits at 7 MB into PSRAM, not 2 MB.
 *
 * The ROM is allocated by Frens::f_malloc, which hands out PSRAM from the
 * bottom of the window upward, and a SNES ROM can be up to SNES_ROM_CACHE_MAX
 * (6 MB). At the old 2 MB offset the save RAM landed INSIDE that range, so a
 * ROM larger than 2 MB overwrote it -- silently corrupting the save of the game
 * currently loaded, since system_save_sram() writes into whatever the core
 * mapped and the ROM bytes had already landed there.
 *
 * 7 MB clears the largest possible ROM with 1 MB to spare, and still fits the
 * 16 KB of SRAM inside the Core2350B's 8 MB. */
#define PICOS_SNES_PSRAM_SRAM_OFF 0x700000u

/*
 * Static Memory Allocation for RP2350 internal 520 KB SRAM:
 * - WRAM: 128 KB
 * - VRAM: 64 KB
 * - ARAM: 64 KB
 * - SRAM: 32 KB (Cartridge Save RAM)
 * - FillRAM: 32 KB (PPU/DMA/System hardware registers $2000-$5FFF)
 * - Screen Buffer: 120 KB (256x240 RGB565)
 * - ZBuffer: 60 KB (256x240 8-bit ZBuffer)
 * - Compact Tile Cache: 32 KB
 */
static struct {
    uint8_t wram[SNES_WRAM_SIZE];
    uint8_t fillram[SNES_FILLRAM_SIZE];
} __attribute__((aligned(4))) s_wram_combo;

#define s_wram    (s_wram_combo.wram)
#define s_fillram (s_wram_combo.fillram)

static uint8_t  s_vram[SNES_VRAM_SIZE] __attribute__((aligned(4)));
static uint8_t  s_aram[SNES_ARAM_SIZE] __attribute__((aligned(4)));

/* Cartridge save RAM.
 *
 * pico_snes_get_sram() returns a PSRAM address whenever USE_PSRAM is set, so on a
 * board with PSRAM this array is allocated and then never read or written: the
 * save file is loaded straight into PSRAM and read back from there. Compiling it
 * out in that case reclaims 16 KB that the linker had no way to see was dead --
 * static allocation cannot be garbage collected.
 *
 * The right side effect is that switching to a different game cannot leak a
 * stale copy of the previous game's save: there is no second copy to leak. */
#if USE_PSRAM
static bool s_psram_active = true;
#else
static uint8_t  s_sram[SNES_SRAM_MAX_SIZE] __attribute__((aligned(4)));
static bool s_psram_active = false;
#endif

static uint16_t s_screen[SNES_SCREEN_WIDTH * SNES_SCREEN_LINES] __attribute__((aligned(4)));
static uint8_t  s_zbuffer[SNES_ZBUFFER_SIZE] __attribute__((aligned(4)));

/* Compact shared Tile Cache (32 KB) + dynamic frame allocation table */
uint8_t  s_tile_cache[SNES_COMPACT_TILE_CACHE_SIZE] __attribute__((aligned(4)));
uint16_t s_tile_slot[SNES_TILE_KEY_COUNT] __attribute__((aligned(4)));
uint16_t s_slot_to_tile[SNES_COMPACT_TILE_COUNT] __attribute__((aligned(4)));
uint8_t  s_slot_blank[SNES_COMPACT_TILE_COUNT] __attribute__((aligned(4)));
uint16_t s_next_tile_slot = 0;

static uint8_t  s_tile_cached_2bit[4096] __attribute__((aligned(4)));
static uint8_t  s_tile_cached_4bit[2048] __attribute__((aligned(4)));
static uint8_t  s_tile_cached_8bit[1024] __attribute__((aligned(4)));

void pico_snes_reset_tile_cache(void) {
    uint16_t count = (s_next_tile_slot > SNES_COMPACT_TILE_COUNT) ? SNES_COMPACT_TILE_COUNT : s_next_tile_slot;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t key = s_slot_to_tile[i];
        if (key != 0xFFFF && key < SNES_TILE_KEY_COUNT) {
            s_tile_slot[key] = 0xFFFF;
        }
        s_slot_to_tile[i] = 0xFFFF;
    }
    s_next_tile_slot = 0;
}

bool pico_snes_init_memory(void) {
    memset(s_wram, 0, sizeof(s_wram));
    memset(s_vram, 0, sizeof(s_vram));
    memset(s_aram, 0, sizeof(s_aram));
#if !USE_PSRAM
    memset(s_sram, 0, sizeof(s_sram));
#endif
    memset(s_fillram, 0, sizeof(s_fillram));
    memset(s_screen, 0, sizeof(s_screen));
    memset(s_zbuffer, 0, sizeof(s_zbuffer));
    memset(s_tile_cache, 0, sizeof(s_tile_cache));
    memset(s_tile_cached_2bit, 0, sizeof(s_tile_cached_2bit));
    memset(s_tile_cached_4bit, 0, sizeof(s_tile_cached_4bit));
    memset(s_tile_cached_8bit, 0, sizeof(s_tile_cached_8bit));
    memset(s_tile_slot, 0xFF, sizeof(s_tile_slot));
    memset(s_slot_to_tile, 0xFF, sizeof(s_slot_to_tile));
    memset(s_slot_blank, 0, sizeof(s_slot_blank));
    s_next_tile_slot = 0;

#if USE_PSRAM
    printf("[MEM] PicoSNES compiled with USE_PSRAM=1\n");
#else
    printf("[MEM] PicoSNES compiled with USE_PSRAM=0 (internal 520KB SRAM mode)\n");
#endif

    printf("[MEM] Allocated SNES buffers in SRAM: WRAM=%u, VRAM=%u, ARAM=%u, Screen=%u, ZBuf=%u\n",
           (unsigned)sizeof(s_wram), (unsigned)sizeof(s_vram), (unsigned)sizeof(s_aram),
           (unsigned)sizeof(s_screen), (unsigned)sizeof(s_zbuffer));
#if USE_PSRAM
    printf("[MEM] Cartridge SRAM lives in PSRAM (0x%08X + 2MB), not in SRAM\n",
           (unsigned)PSRAM_BASE_ADDR);
#else
    printf("[MEM] Cartridge SRAM in SRAM: %u bytes\n", (unsigned)sizeof(s_sram));
#endif

    return true;
}

void pico_snes_reset_memory(void) {
    memset(s_wram, 0, sizeof(s_wram));
    memset(s_vram, 0, sizeof(s_vram));
    memset(s_aram, 0, sizeof(s_aram));
    memset(s_fillram, 0, sizeof(s_fillram));
    memset(s_screen, 0, sizeof(s_screen));
    memset(s_zbuffer, 0, sizeof(s_zbuffer));
    memset(s_tile_cached_2bit, 0, sizeof(s_tile_cached_2bit));
    memset(s_tile_cached_4bit, 0, sizeof(s_tile_cached_4bit));
    memset(s_tile_cached_8bit, 0, sizeof(s_tile_cached_8bit));
    memset(s_tile_slot, 0xFF, sizeof(s_tile_slot));
    memset(s_slot_to_tile, 0xFF, sizeof(s_slot_to_tile));
    memset(s_slot_blank, 0, sizeof(s_slot_blank));
    s_next_tile_slot = 0;
}

uint8_t* pico_snes_get_wram(void) {
    return s_wram;
}

uint8_t* pico_snes_get_vram(void) {
    return s_vram;
}

uint8_t* pico_snes_get_aram(void) {
    return s_aram;
}

uint8_t* pico_snes_get_sram(void) {
#if USE_PSRAM
    if (s_psram_active) {
        return (uint8_t*)(PSRAM_BASE_ADDR + PICOS_SNES_PSRAM_SRAM_OFF);
    }
#endif
#if !USE_PSRAM
    return s_sram;
#else
    return (uint8_t*)(PSRAM_BASE_ADDR + PICOS_SNES_PSRAM_SRAM_OFF);
#endif
}

uint8_t* pico_snes_get_fillram(void) {
    return s_fillram;
}

uint16_t* pico_snes_get_screen(void) {
    return s_screen;
}

uint8_t* pico_snes_get_zbuffer(void) {
    return s_zbuffer;
}

uint8_t* pico_snes_get_tilecache(int depth) {
    (void)depth;
    return s_tile_cache;
}

uint8_t* pico_snes_get_tilecached(int depth) {
    switch (depth) {
        case 0: return s_tile_cached_2bit;
        case 1: return s_tile_cached_4bit;
        default: return s_tile_cached_8bit;
    }
}

bool pico_snes_has_psram(void) {
    return s_psram_active;
}

void pico_snes_enable_psram(bool enable) {
    s_psram_active = enable;
}
