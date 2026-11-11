#ifndef PICO_SNES_MEM_H
#define PICO_SNES_MEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PSRAM Support Toggle:
 * 0 = Internal SRAM (520 KB) only, 0 bytes RAM for ROM (Flash XIP direct execution).
 * 1 = Extended PSRAM support on boards equipped with PSRAM.
 */
#ifndef USE_PSRAM
#define USE_PSRAM 0
#endif

#define SNES_SCREEN_WIDTH   256
#define SNES_SCREEN_HEIGHT  224
#define SNES_SCREEN_LINES   8

/* Buffer sizes */
#define SNES_WRAM_SIZE      0x20000u /* 128 KB */
#define SNES_VRAM_SIZE      0x10000u /* 64 KB */
#define SNES_ARAM_SIZE      0x10000u /* 64 KB */
#define SNES_SRAM_MAX_SIZE  0x4000u  /* 16 KB */
#define SNES_FILLRAM_SIZE   0x8000u  /* 32 KB */

#define SNES_SCREEN_PITCH   (SNES_SCREEN_WIDTH * 2) /* 512 bytes */
#define SNES_ZPITCH         256 /* 256 bytes per scanline for zbuffer */
#define SNES_ZBUFFER_SIZE   1024 /* 256 bytes main + 256 bytes sub + padding */

#define SNES_COMPACT_TILE_COUNT 512
#define SNES_COMPACT_TILE_CACHE_SIZE (SNES_COMPACT_TILE_COUNT * 64) /* 32 KB */
#define SNES_TILE_KEY_COUNT 7168

extern uint8_t  s_tile_cache[SNES_COMPACT_TILE_CACHE_SIZE];
extern uint16_t s_tile_slot[SNES_TILE_KEY_COUNT];
extern uint16_t s_slot_to_tile[SNES_COMPACT_TILE_COUNT];
extern uint8_t  s_slot_blank[SNES_COMPACT_TILE_COUNT];
extern uint16_t s_next_tile_slot;

void pico_snes_reset_tile_cache(void);
static inline void pico_snes_invalidate_tile(uint32_t address) {
    uint32_t k2 = (address >> 4) & 4095;
    uint16_t s2 = s_tile_slot[k2];
    if (s2 != 0xFFFF) {
        s_tile_slot[k2] = 0xFFFF;
        s_slot_to_tile[s2] = 0xFFFF;
    }
    uint32_t k4 = 4096 + ((address >> 5) & 2047);
    uint16_t s4 = s_tile_slot[k4];
    if (s4 != 0xFFFF) {
        s_tile_slot[k4] = 0xFFFF;
        s_slot_to_tile[s4] = 0xFFFF;
    }
    uint32_t k8 = 6144 + ((address >> 6) & 1023);
    uint16_t s8 = s_tile_slot[k8];
    if (s8 != 0xFFFF) {
        s_tile_slot[k8] = 0xFFFF;
        s_slot_to_tile[s8] = 0xFFFF;
    }
}

/* Memory management API */
bool pico_snes_init_memory(void);
void pico_snes_reset_memory(void);

uint8_t*  pico_snes_get_wram(void);
uint8_t*  pico_snes_get_vram(void);
uint8_t*  pico_snes_get_aram(void);
uint8_t*  pico_snes_get_sram(void);
uint8_t*  pico_snes_get_fillram(void);
uint16_t* pico_snes_get_screen(void);
uint8_t*  pico_snes_get_zbuffer(void);

uint8_t*  pico_snes_get_tilecache(int depth);
uint8_t*  pico_snes_get_tilecached(int depth);

bool      pico_snes_has_psram(void);
void      pico_snes_enable_psram(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* PICO_SNES_MEM_H */
