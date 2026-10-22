////////////////////////////////////////////////////////////////////////////////
// Memory
////////////////////////////////////////////////////////////////////////////////
// Notes: need to optimize cpu_writemem20
//
//
//
//
//
//////////////////////////////////////////////////////////////////////////////

#include <string.h>
#include <stdbool.h>
#include <io.h>
#include "rom.h"
#include "./nec/nec.h"
#include "gpu.h"
#include "memory.h"

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
#define IO_ROM_BANK_BASE_SELECTOR    0xC0


static uint8 *ws_rom;
__aligned(4) uint8 internalRam[0x10000];
__aligned(4) uint8 ws_staticRam[0x10000];
__aligned(4) uint8 externalEeprom[0x2000];


uint16 ws_rom_checksum;

uint32 sramAddressMask;
uint32 externalEepromAddressMask;
uint32 romAddressMask;
static uint32 romSize;
static bool s_is_conan = false;



////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
/* Direct pointers for the 64 KB pages the V30 core can reach (0x00000..0x10FFFF
 * once a segment base and an offset are added). A non-NULL entry means every byte
 * of that page can be read straight from memory; NULL sends the access down the
 * full cpu_readmem20() path (SRAM bank switching, mono RAM mirror, Conan patch).
 * Rebuilt whenever something it depends on changes: the ROM bank registers
 * 0xC0..0xC3, the colour/mono system, a reset or a state load. */
const uint8_t *ws_page[17];
/* Bumped whenever cartridge SRAM or the external EEPROM really changes. main.cpp watches it to
 * write the .sav shortly after the game saves, instead of only when the game is left. */
volatile uint32_t ws_sram_epoch;
unsigned ws_prof_rom;

void ws_memory_bank_changed(void) {
    for (int i = 0; i < 17; i++) ws_page[i] = NULL;
#ifdef WS_NO_PAGES
    return;
#endif
    if (!ws_rom || romSize < 0x10000 || (romSize & 0xFFFF) || romAddressMask < 0xFFFF) return;

    /* Bank 0: 64 KB of internal RAM only on a WonderSwan Color (the mono model
     * has 16 KB and reads 0x90 past it, which needs the slow path). */
    if (ws_gpu_operatingInColor) ws_page[0] = internalRam;

    /* Bank 1: cartridge SRAM. With 64 KB or more the 16-bit offset indexes the
     * array directly (the bank register only moves the window inside a larger
     * chip, and the array is 64 KB); smaller chips wrap inside the page and keep
     * the slow path. The scheduler idle loop of Digimon Tamers reads here. */
    if (sramAddressMask >= 0xFFFFu) ws_page[1] = ws_staticRam;

    for (uint32_t bank = 2; bank <= 16; bank++) {
        uint32_t base;
        if (bank <= 3) {
            base = ((uint32_t)ws_ioRam[IO_ROM_BANK_BASE_SELECTOR + bank] << 16) & romAddressMask;
        } else {
            unsigned int romBank = (256 - (((ws_ioRam[IO_ROM_BANK_BASE_SELECTOR] & 0xf) << 4) | (bank & 0xf)));
            base = (uint32_t)(romSize - (romBank << 16)) & romAddressMask;
        }
        if (base >= romSize) base %= romSize;
        /* The Detective Conan patch lives in the last 64 KB of the image */
        if (s_is_conan && base == 0xF0000u) continue;
        ws_page[bank] = ws_rom + base;
    }
}

void cpu_writemem20(uint32_t addr, uint8_t value) {
    uint32 offset = addr & 0xffff;
    uint32 bank = addr >> 16;
    if (addr < 0x2000u) { /* plain RAM: no tile/palette side effects */
        if (internalRam[addr] != value) { internalRam[addr] = value; ws_mc++; }
        return;
    }

    // 0 - RAM - 16 KB (WS) / 64 KB (WSC) internal RAM
    if (!bank) {
        /* The mono WonderSwan has no RAM at 0x4000-0xFFFF: writes go nowhere (ares Bus::write) */
        if (!ws_gpu_operatingInColor && offset >= 0x4000) return;
        if (internalRam[offset] != value) ws_mc++;
        ws_gpu_write_byte(offset, value);
//		ws_audio_write_byte(offset,value);
    } else if (bank == 1) {
        // 1 - SRAM (cart)
        if (sramAddressMask) {
            uint32_t sram_addr = (offset + (((uint32_t)ws_ioRam[0xC1]) << 16)) & sramAddressMask;
            uint8 *dst = &ws_staticRam[sram_addr & (sizeof(ws_staticRam) - 1)];
            if (*dst != value) { *dst = value; ws_mc++; ws_sram_epoch++; }
        }
    }

    // other banks are read-only
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
static uint8_t __attribute__((noinline)) cpu_readmem20_slow(uint32_t addr);

uint8_t cpu_readmem20(uint32_t addr) {
    const uint32_t pg = addr >> 16;
    const uint8_t *p = (pg <= 16u) ? ws_page[pg] : NULL;
    if (__builtin_expect(p != NULL, 1)) return p[addr & 0xFFFFu];
    return cpu_readmem20_slow(addr);
}

static uint8_t cpu_readmem20_slow(uint32_t addr) {
    uint32 offset = addr & 0xffff;
    uint32 bank = addr >> 16;

    switch (bank) {
        case 0:        // 0 - RAM - 16 KB (WS) / 64 KB (WSC) internal RAM
            if (ws_gpu_operatingInColor || offset < 0x4000)
                return (internalRam[offset]);
            /* Mono WonderSwan: 0x4000-0xFFFF is unpopulated and reads 0x90 (ares Bus::read).
             * Klonoa depends on it: its stop-sound routine (ROM 0x4DDDD) forgets to mask the
             * priority nibble of the track number, so stopping sound 0x0D indexes a track at
             * 0x4358. Reading 0xFF there made the game take the voice-stop branch and write
             * the clobbered AL (0x8D) to ports 0x88-0x8F, moving the wavetable base (0x8F)
             * off the instruments: the music changed or went silent after the Moo grab.
             * With 0x90 the same code only zeroes two channel volumes, as on hardware. */
            return (0x90);

        case 1:    // 1 - SRAM (cart)
            if (sramAddressMask) {
                uint32_t sram_addr = (offset + (((uint32_t)ws_ioRam[0xC1]) << 16)) & sramAddressMask;
                return ws_staticRam[sram_addr & (sizeof(ws_staticRam) - 1)];
            }
            return (0xff);
        case 2:
        case 3: {
            uint32_t idx = (offset + (((uint32_t)ws_ioRam[IO_ROM_BANK_BASE_SELECTOR + bank]) << 16)) & romAddressMask;
            if (idx >= romSize && romSize) idx %= romSize;
            return ws_rom[idx];
        }
        default: {
            unsigned int romBank = (256 - (((ws_ioRam[IO_ROM_BANK_BASE_SELECTOR] & 0xf) << 4) | (bank & 0xf)));
            uint32_t roff = (uint32_t)(offset + romSize - (romBank << 16));
            uint32_t idx = roff & romAddressMask;
            if (idx >= romSize && romSize) idx %= romSize;
            if (s_is_conan && idx >= 0xfffe8 && idx <= 0xfffec) {
                static const uint8_t conan_patch[5] = { 0xea, 0x00, 0x00, 0x00, 0x20 };
                return conan_patch[idx - 0xfffe8];
            }
            return ws_rom[idx];

        }
    }
    return (0xff);
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_memory_init(uint8 *rom, uint32 wsRomSize) {
    ws_romHeaderStruct *ws_romHeader;

    ws_rom = rom;
    romSize = wsRomSize;
    ws_romHeader = ws_rom_getHeader(ws_rom, romSize);
    ws_rom_checksum = ws_romHeader->checksum;
    s_is_conan = (ws_romHeader && ws_romHeader->developerId == 0x01 && ws_romHeader->cartId == 0x27);

    uint32_t sram_sz = ws_rom_sramSize(ws_rom, romSize);
    if (sram_sz > 0) {
        uint32_t smask = 1;
        while (smask < sram_sz) smask = (smask << 1) | 1;
        sramAddressMask = smask;
    } else {
        sramAddressMask = 0;
    }

    uint32_t eep_sz = ws_rom_eepromSize(ws_rom, romSize);
    if (eep_sz > 0) {
        uint32_t emask = 1;
        while (emask < eep_sz) emask = (emask << 1) | 1;
        externalEepromAddressMask = emask;
    } else {
        externalEepromAddressMask = 0;
    }

    uint32_t mask = 1;
    while (mask < romSize) mask = (mask << 1) | 1;
    romAddressMask = mask;

    if (ws_romHeader->minimumSupportSystem == WS_SYSTEM_COLOR)
        ws_gpu_operatingInColor = 1;
    else
        ws_gpu_operatingInColor = 0;

    ws_memory_bank_changed();
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_memory_reset(void) {
    memset(internalRam, 0, sizeof(internalRam));
    memset(ws_staticRam, 0, sizeof(ws_staticRam));
    memset(externalEeprom, 0, sizeof(externalEeprom));
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_memory_done(void) {
#if 0
    free(ws_rom);
    free(ws_staticRam);
    free(internalRam);
    free(externalEeprom);
#endif
}
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint8 *memory_getRom(void) {
    return (ws_rom);
}
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint32 memory_getRomSize(void) {
    return (romSize);
}
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint16 memory_getRomCrc(void) {
    return (ws_rom_checksum);
}

