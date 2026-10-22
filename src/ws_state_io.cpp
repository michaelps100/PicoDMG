/**
 * ws_state_io.cpp
 * WonderSwan save state and battery SRAM I/O using FatFS.
 *
 * State layout:
 *   18 × uint32: NEC V30 registers
 *     (NEC_IP, NEC_AW, NEC_CW, NEC_DW, NEC_BW, NEC_SP, NEC_BP,
 *      NEC_IX, NEC_IY, NEC_FLAGS, NEC_ES, NEC_CS, NEC_SS, NEC_DS,
 *      NEC_VECTOR, NEC_PENDING, NEC_NMI_STATE, NEC_IRQ_STATE)
 *   internalRam[65536]
 *   ws_staticRam[65536]
 *   ws_ioRam[256]
 *   ws_paletteColors[8]
 *   ws_palette[16*4*2]
 *   wsc_palette[16*16*2]
 *   ws_videoMode (uint8)
 *   ws_gpu_scanline (uint8)
 *
 * SRAM-only: ws_staticRam[65536]
 */

#include "ws_state_io.h"
#include <string.h>
#include "ff.h"

extern "C" {
#include "oswan/types.h"
#include "oswan/gpu.h"
#include "oswan/memory.h"
#include "oswan/io.h"
#include "oswan/ws_audio.h"
#include "oswan/nec/necintrf.h"
}

/* Register save list – all IDs defined in necintrf.h enum */
static const int k_nec_regs[] = {
    NEC_IP, NEC_AW, NEC_CW, NEC_DW, NEC_BW, NEC_SP, NEC_BP,
    NEC_IX, NEC_IY, NEC_FLAGS, NEC_ES, NEC_CS, NEC_SS, NEC_DS,
    NEC_VECTOR, NEC_PENDING, NEC_NMI_STATE, NEC_IRQ_STATE
};
#define NEC_REG_COUNT ((int)(sizeof(k_nec_regs)/sizeof(k_nec_regs[0])))

/* -------------------------------------------------------------------------- */
static bool ff_write_all(FIL *f, const void *buf, UINT len) {
    UINT bw = 0;
    return (f_write(f, buf, len, &bw) == FR_OK && bw == len);
}
static bool ff_read_all(FIL *f, void *buf, UINT len) {
    UINT br = 0;
    return (f_read(f, buf, len, &br) == FR_OK && br == len);
}

static FIL s_io_f;

/* -------------------------------------------------------------------------- */
bool ws_state_save(const char *path) {
    if (f_open(&s_io_f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    bool ok = true;

    for (int i = 0; i < NEC_REG_COUNT && ok; i++) {
        uint32_t v = (uint32_t)nec_get_reg(k_nec_regs[i]);
        ok = ff_write_all(&s_io_f, &v, sizeof(v));
    }
    if (ok) ok = ff_write_all(&s_io_f, internalRam,     sizeof(internalRam));
    if (ok) ok = ff_write_all(&s_io_f, ws_staticRam,    sizeof(ws_staticRam));
    if (ok) ok = ff_write_all(&s_io_f, ws_ioRam,          sizeof(ws_ioRam));
    if (ok) ok = ff_write_all(&s_io_f, ws_paletteColors,    sizeof(ws_paletteColors));
    if (ok) ok = ff_write_all(&s_io_f, ws_palette,    sizeof(ws_palette));
    if (ok) ok = ff_write_all(&s_io_f, wsc_palette,  sizeof(wsc_palette));
    if (ok) ok = ff_write_all(&s_io_f, &ws_videoMode,       1);
    if (ok) ok = ff_write_all(&s_io_f, &ws_gpu_scanline,    1);
    if (ok) ok = ff_write_all(&s_io_f, externalEeprom,     sizeof(externalEeprom));

    f_close(&s_io_f);
    return ok;
}

/* -------------------------------------------------------------------------- */
bool ws_state_load(const char *path) {
    if (f_open(&s_io_f, path, FA_READ) != FR_OK) return false;
    bool ok = true;

    for (int i = 0; i < NEC_REG_COUNT && ok; i++) {
        uint32_t v = 0;
        ok = ff_read_all(&s_io_f, &v, sizeof(v));
        if (ok) nec_set_reg(k_nec_regs[i], (unsigned)v);
    }

    uint8_t new_vm = 0, new_sl = 0;
    if (ok) ok = ff_read_all(&s_io_f, internalRam,     sizeof(internalRam));
    if (ok) ok = ff_read_all(&s_io_f, ws_staticRam,    sizeof(ws_staticRam));
    if (ok) ok = ff_read_all(&s_io_f, ws_ioRam,          sizeof(ws_ioRam));
    if (ok) ok = ff_read_all(&s_io_f, ws_paletteColors,    sizeof(ws_paletteColors));
    if (ok) ok = ff_read_all(&s_io_f, ws_palette,    sizeof(ws_palette));
    if (ok) ok = ff_read_all(&s_io_f, wsc_palette,  sizeof(wsc_palette));
    if (ok) ok = ff_read_all(&s_io_f, &new_vm,             1);
    if (ok) ok = ff_read_all(&s_io_f, &new_sl,             1);
    if (ok) ok = ff_read_all(&s_io_f, externalEeprom,     sizeof(externalEeprom));
    f_close(&s_io_f);
    ws_memory_bank_changed();
    ws_gpu_colorActive = (ws_gpu_operatingInColor && (ws_ioRam[0x60] & 0x80)) ? 1 : 0;

    if (ok) {
        ws_videoMode    = new_vm;
        ws_gpu_scanline = new_sl;
        ws_gpu_clearCache();
        ws_audio_sync_from_ioram();
    }
    return ok;
}

/* -------------------------------------------------------------------------- */
bool ws_sram_save(const char *path) {
    if (f_open(&s_io_f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    bool ok = ff_write_all(&s_io_f, ws_staticRam, sizeof(ws_staticRam));
    if (ok) ok = ff_write_all(&s_io_f, externalEeprom, sizeof(externalEeprom));
    f_close(&s_io_f);
    return ok;
}

bool ws_sram_load(const char *path) {
    if (f_open(&s_io_f, path, FA_READ) != FR_OK) return false;
    bool ok = ff_read_all(&s_io_f, ws_staticRam, sizeof(ws_staticRam));
    if (ok) ok = ff_read_all(&s_io_f, externalEeprom, sizeof(externalEeprom));
    f_close(&s_io_f);
    if (ok) ws_gpu_clearCache();
    return ok;
}

