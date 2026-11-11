#include "state.h"
#include <cstdio>
#include <cstring>
#include "ff.h"
#include "hardware/watchdog.h"

extern "C" {
#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "ppu.h"
#include "apu.h"
#include "spc700.h"
#include "soundux.h"
#include "fxemu.h"
#include "fxinst.h"
#include "sa1.h"
#include "srtc.h"
#include "dsp.h"

extern FxRegs_s GSU;
extern int32_t OBC1_Address;
extern int32_t OBC1_BasePtr;
extern int32_t OBC1_Shift;
}

/* SAVESTATES FOR THE UPSTREAM CORE (snes9x_src/).
 *
 * The upstream core ships no snapshot.c, and the old fork's src/snapshot.c walks
 * field tables written for different struct revisions. Instead of porting those
 * tables, the state is a binary dump of the core's own structs, tied to the build
 * that wrote it -- the same contract as PicoGBC's .ss (root AGENTS.md sec. 24.7):
 * a struct that changes size changes the build signature, and the loader refuses
 * the file instead of restoring garbage.
 *
 * The structs carry pointers (PC, PCBase, the APU's RAM and direct page, the
 * sound channels' block and echo pointers, the GSU's bank tables, the SA-1 maps).
 * Those are never taken from the file: each is either preserved from the running
 * core, which already allocated the same buffers for this cart, or rebuilt from
 * the saved offsets by the core's own post-load helpers.
 *
 * Saving happens from the in-game menu, which runs between frames, after
 * S9xMainLoop() has packed ICPU.Registers.PC and IAPU.Registers.PC and the status
 * flags (cpuexec.c, end of S9xMainLoop). That is what makes Registers.PC the
 * authoritative program counter here.
 *
 * Not saved: the S-DSP echo ring (restarts silent, at most one click) and MSU-1
 * playback position. */

#define SNES_STATE_MAGIC   "PSNS"
#define SNES_STATE_VERSION 1u
#define SNES_STATE_CHUNK   (16u * 1024u)

enum {
    STATE_HAS_SUPERFX = 1u << 0,
    STATE_HAS_SA1     = 1u << 1,
    STATE_HAS_SRTC    = 1u << 2,
    STATE_HAS_OBC1    = 1u << 3,
};

typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t build_sig;     /* hash of every struct size written below */
    uint32_t rom_id;        /* header name + checksums of the loaded cart */
    uint32_t sram_bytes;
    uint32_t dsp_bytes;
    uint32_t flags;
    uint32_t payload_bytes; /* everything after this header */
} snes_state_header_t;

/* The GSU state that lives outside the register space. Everything else -- R0..R15,
 * SFR, the bank registers, CBR, SCMR -- is read back from FillRAM+0x3000 at the
 * start of every FxEmulate() call (fxemu.c, fx_readRegisterSpaceForUse), and the
 * FillRAM block is in the state. pvDreg/pvSreg are stored as register indices. */
typedef struct {
    uint32_t vColorReg;
    uint32_t vPlotOptionReg;
    uint32_t vLastRamAdr;
    uint32_t vPrevMode;
    uint32_t vPrevScreenHeight;
    uint32_t dreg;
    uint32_t sreg;
    uint8_t  vRomBuffer;
    uint8_t  vPipe;
    uint8_t  pad[2];
} snes_gsu_state_t;

static FIL s_snes_fil;
static bool s_write_ok;

static uint32_t sig_mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

static uint32_t snes_build_signature(void) {
    uint32_t h = 2166136261u;
    h = sig_mix(h, SNES_STATE_VERSION);
    h = sig_mix(h, (uint32_t)sizeof(SCPUState));
    h = sig_mix(h, (uint32_t)sizeof(SICPU));
    h = sig_mix(h, (uint32_t)sizeof(SPPU));
    h = sig_mix(h, (uint32_t)sizeof(SDMA));
    h = sig_mix(h, (uint32_t)sizeof(SAPU));
    h = sig_mix(h, (uint32_t)sizeof(SIAPU));
    h = sig_mix(h, (uint32_t)sizeof(SSoundData));
    h = sig_mix(h, (uint32_t)sizeof(snes_gsu_state_t));
    h = sig_mix(h, (uint32_t)sizeof(SSA1));
    h = sig_mix(h, (uint32_t)sizeof(SRTC_DATA));
    return h;
}

static uint32_t snes_rom_id(void) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(Memory.ROMName); i++)
        h = sig_mix(h, (uint8_t)Memory.ROMName[i]);
    h = sig_mix(h, Memory.ROMChecksum);
    h = sig_mix(h, Memory.ROMComplementChecksum);
    h = sig_mix(h, Memory.CalculatedSize);
    return h;
}

static void snes_fill_header(snes_state_header_t *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, SNES_STATE_MAGIC, 4);
    hdr->version = SNES_STATE_VERSION;
    hdr->build_sig = snes_build_signature();
    hdr->rom_id = snes_rom_id();
    hdr->sram_bytes = Memory.SRAM ? s9x_port_sram_alloc : 0;

    void *dsp_block;
    size_t dsp_size;
    S9xDSPGetStateBlock(&dsp_block, &dsp_size);
    hdr->dsp_bytes = (uint32_t)dsp_size;

    if (Settings.SuperFX) hdr->flags |= STATE_HAS_SUPERFX;
    if (Settings.SA1)     hdr->flags |= STATE_HAS_SA1;
    if (Settings.SRTC)    hdr->flags |= STATE_HAS_SRTC;
    if (Settings.OBC1)    hdr->flags |= STATE_HAS_OBC1;

    uint32_t n = 0;
    n += sizeof(SCPUState) + sizeof(SICPU) + sizeof(SPPU) + sizeof(DMA);
    n += sizeof(SAPU) + sizeof(SIAPU) + sizeof(SSoundData);
    n += RAM_SIZE + VRAM_SIZE + FILLRAM_SIZE + 0x10000u;
    n += hdr->sram_bytes;
    if (hdr->flags & STATE_HAS_SUPERFX) n += sizeof(snes_gsu_state_t);
    if (hdr->flags & STATE_HAS_SA1)     n += sizeof(SSA1) + sizeof(uint32_t);
    if (hdr->flags & STATE_HAS_SRTC)    n += sizeof(SRTC_DATA);
    if (hdr->flags & STATE_HAS_OBC1)    n += 3u * sizeof(int32_t);
    n += hdr->dsp_bytes;
    hdr->payload_bytes = n;
}

/* Chunked so a 128 KB block from PSRAM does not hold the card for the whole
 * transfer without feeding the watchdog. A short write latches s_write_ok false
 * and every later write is skipped, so the caller checks once at the end. */
static void put(const void *data, uint32_t size) {
    const uint8_t *p = (const uint8_t *)data;
    while (s_write_ok && size) {
        const UINT n = size > SNES_STATE_CHUNK ? SNES_STATE_CHUNK : size;
        UINT bw = 0;
        if (f_write(&s_snes_fil, p, n, &bw) != FR_OK || bw != n) s_write_ok = false;
        p += n;
        size -= n;
        watchdog_update();
    }
}

static bool get(void *data, uint32_t size) {
    uint8_t *p = (uint8_t *)data;
    while (size) {
        const UINT n = size > SNES_STATE_CHUNK ? SNES_STATE_CHUNK : size;
        UINT br = 0;
        if (f_read(&s_snes_fil, p, n, &br) != FR_OK || br != n) return false;
        p += n;
        size -= n;
        watchdog_update();
    }
    return true;
}

extern "C" {

int Emulator_SaveState(const char *path) {
    if (!Memory.RAM || !Memory.VRAM || !Memory.FillRAM || !IAPU.RAM) return -1;

    snes_state_header_t hdr;
    snes_fill_header(&hdr);

    /* Written beside the real name and renamed at the end, so a card pulled or a
     * write that fails halfway leaves the previous state intact instead of a
     * truncated .ss that the menu's slot probe (size > 0) would offer. */
    char tmp_path[FF_MAX_LFN];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) return -1;

    if (f_open(&s_snes_fil, tmp_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        printf("[state] cannot create %s\n", tmp_path);
        return -1;
    }
    s_write_ok = true;

    if (Settings.SRTC) S9xSRTCPreSaveState();

    put(&hdr, sizeof(hdr));
    put(&CPU, sizeof(CPU));
    put(&ICPU, sizeof(ICPU));
    put(&PPU, sizeof(PPU));
    put(DMA, sizeof(DMA));
    put(&APU, sizeof(APU));
    put(&IAPU, sizeof(IAPU));
    put(&SoundData, sizeof(SoundData));
    put(Memory.RAM, RAM_SIZE);
    put(Memory.VRAM, VRAM_SIZE);
    put(Memory.FillRAM, FILLRAM_SIZE);
    put(IAPU.RAM, 0x10000u);
    if (hdr.sram_bytes) put(Memory.SRAM, hdr.sram_bytes);

    if (hdr.flags & STATE_HAS_SUPERFX) {
        snes_gsu_state_t g;
        memset(&g, 0, sizeof(g));
        g.vColorReg = GSU.vColorReg;
        g.vPlotOptionReg = GSU.vPlotOptionReg;
        g.vLastRamAdr = GSU.vLastRamAdr;
        g.vPrevMode = GSU.vPrevMode;
        g.vPrevScreenHeight = GSU.vPrevScreenHeight;
        g.dreg = GSU.pvDreg ? (uint32_t)(GSU.pvDreg - GSU.avReg) & 15u : 0;
        g.sreg = GSU.pvSreg ? (uint32_t)(GSU.pvSreg - GSU.avReg) & 15u : 0;
        g.vRomBuffer = GSU.vRomBuffer;
        g.vPipe = GSU.vPipe;
        put(&g, sizeof(g));
    }
    if (hdr.flags & STATE_HAS_SA1) {
        /* The SA-1 loop does not pack its PC on the way out the way S9xMainLoop
         * does for the main CPU, so do it here. */
        SA1.Registers.PC = (uint16_t)(SA1.PC - SA1.PCBase);
        S9xSA1PackStatus();
        put(&SA1, sizeof(SA1));
        const uint32_t bwram_off = (SA1.BWRAM && Memory.SRAM) ? (uint32_t)(SA1.BWRAM - Memory.SRAM) : 0;
        put(&bwram_off, sizeof(bwram_off));
    }
    if (hdr.flags & STATE_HAS_SRTC) put(&rtc, sizeof(rtc));
    if (hdr.flags & STATE_HAS_OBC1) {
        put(&OBC1_Address, sizeof(int32_t));
        put(&OBC1_BasePtr, sizeof(int32_t));
        put(&OBC1_Shift, sizeof(int32_t));
    }
    if (hdr.dsp_bytes) {
        void *dsp_block;
        size_t dsp_size;
        S9xDSPGetStateBlock(&dsp_block, &dsp_size);
        put(dsp_block, hdr.dsp_bytes);
    }

    if (f_close(&s_snes_fil) != FR_OK) s_write_ok = false;
    if (!s_write_ok) {
        f_unlink(tmp_path);
        printf("[state] write failed for %s\n", path);
        return -1;
    }

    f_unlink(path);
    if (f_rename(tmp_path, path) != FR_OK) {
        printf("[state] rename %s -> %s failed\n", tmp_path, path);
        return -1;
    }
    printf("[state] saved %s (%lu bytes)\n", path,
           (unsigned long)(sizeof(hdr) + hdr.payload_bytes));
    return 0;
}

/* Returns 0 on success, -1 when the file was refused before anything in the core
 * was touched, and SNES_STATE_LOAD_PARTIAL when a read failed after the core
 * state had already been overwritten -- the caller must reset the game then. */
int Emulator_LoadState(const char *path) {
    if (!Memory.RAM || !Memory.VRAM || !Memory.FillRAM || !IAPU.RAM) return -1;

    if (f_open(&s_snes_fil, path, FA_READ) != FR_OK) {
        printf("[state] cannot open %s\n", path);
        return -1;
    }

    snes_state_header_t want, hdr;
    snes_fill_header(&want);
    UINT br = 0;
    const char *why = NULL;
    if (f_read(&s_snes_fil, &hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr))
        why = "short header";
    else if (memcmp(hdr.magic, SNES_STATE_MAGIC, 4) != 0)
        why = "not a PicoSNES state";
    else if (hdr.version != want.version || hdr.build_sig != want.build_sig)
        why = "written by a different firmware build";
    else if (hdr.rom_id != want.rom_id)
        why = "belongs to another game";
    else if (hdr.sram_bytes != want.sram_bytes || hdr.dsp_bytes != want.dsp_bytes ||
             hdr.flags != want.flags || hdr.payload_bytes != want.payload_bytes)
        why = "cart layout differs";
    else if (f_size(&s_snes_fil) != (FSIZE_t)(sizeof(hdr) + hdr.payload_bytes))
        why = "file size does not match";
    if (why) {
        f_close(&s_snes_fil);
        printf("[state] refused %s: %s\n", path, why);
        return -1;
    }

    /* From here on the core is overwritten in place. Pointers are kept from the
     * running core (same cart, same allocations) and put back after each read. */
    bool ok = true;

    ok = ok && get(&CPU, sizeof(CPU));

    {
        const SOpcodes *ops = ICPU.S9xOpcodes;
        uint8_t *unused1 = ICPU.UNUSED1;
        ok = ok && get(&ICPU, sizeof(ICPU));
        ICPU.S9xOpcodes = ops;
        ICPU.UNUSED1 = unused1;
    }

    ok = ok && get(&PPU, sizeof(PPU));
    ok = ok && get(DMA, sizeof(DMA));
    ok = ok && get(&APU, sizeof(APU));

    {
        uint8_t *apu_ram = IAPU.RAM;
        ok = ok && get(&IAPU, sizeof(IAPU));
        IAPU.RAM = apu_ram;
        IAPU.PC = IAPU.RAM + IAPU.Registers.PC;
        IAPU.WaitAddress1 = NULL;
        IAPU.WaitAddress2 = NULL;
    }

    ok = ok && get(&SoundData, sizeof(SoundData));
    for (int i = 0; i < NUM_CHANNELS; i++) {
        SoundData.channels[i].block = SoundData.channels[i].decoded;
        SoundData.channels[i].echo_buf_ptr = NULL;
    }

    ok = ok && get(Memory.RAM, RAM_SIZE);
    ok = ok && get(Memory.VRAM, VRAM_SIZE);
    ok = ok && get(Memory.FillRAM, FILLRAM_SIZE);
    ok = ok && get(IAPU.RAM, 0x10000u);
    if (hdr.sram_bytes) ok = ok && get(Memory.SRAM, hdr.sram_bytes);

    snes_gsu_state_t g;
    if (hdr.flags & STATE_HAS_SUPERFX) ok = ok && get(&g, sizeof(g));

    uint32_t sa1_bwram_off = 0;
    if (hdr.flags & STATE_HAS_SA1) {
        const SOpcodes *ops = SA1.S9xOpcodes;
        uint8_t **map = SA1.Map;
        uint8_t **wmap = SA1.WriteMap;
        ok = ok && get(&SA1, sizeof(SA1));
        ok = ok && get(&sa1_bwram_off, sizeof(sa1_bwram_off));
        SA1.S9xOpcodes = ops;
        SA1.Map = map;
        SA1.WriteMap = wmap;
        SA1.WaitAddress = NULL;
        SA1.WaitByteAddress1 = NULL;
        SA1.WaitByteAddress2 = NULL;
    }
    if (hdr.flags & STATE_HAS_SRTC) ok = ok && get(&rtc, sizeof(rtc));
    if (hdr.flags & STATE_HAS_OBC1) {
        ok = ok && get(&OBC1_Address, sizeof(int32_t));
        ok = ok && get(&OBC1_BasePtr, sizeof(int32_t));
        ok = ok && get(&OBC1_Shift, sizeof(int32_t));
    }
    if (hdr.dsp_bytes) {
        void *dsp_block;
        size_t dsp_size;
        S9xDSPGetStateBlock(&dsp_block, &dsp_size);
        ok = ok && dsp_block && get(dsp_block, hdr.dsp_bytes);
    }

    f_close(&s_snes_fil);
    if (!ok) {
        printf("[state] read failed in %s after the core was overwritten\n", path);
        return SNES_STATE_LOAD_PARTIAL;
    }

    /* --- Main CPU ------------------------------------------------------- */
    ICPU.ShiftedPB = (uint32_t)ICPU.Registers.PB << 16;
    ICPU.ShiftedDB = (uint32_t)ICPU.Registers.DB << 16;
    CPU.InDMA = false;
    CPU.WaitAddress = NULL;
    FixROMSpeed();
    S9xSetPCBase(ICPU.ShiftedPB + ICPU.Registers.PC);
    CPU.PCAtOpcodeStart = CPU.PC;
    S9xUnpackStatus();
    S9xFixCycles();
    S9xReschedule();

    /* --- PPU: derived tables and the tile cache --------------------------- */
    /* VRAM was replaced underneath the decoded-tile cache, so every cached tile is
     * stale; clear the flags exactly as S9xResetPPU does. */
    memset(IPPU.TileCached, 0, MAX_2BIT_TILES);
    IPPU.ColorsChanged = true;
    IPPU.OBJChanged = true;
    IPPU.FirstVRAMRead = false;
    S9xFixColourBrightness();
    PPU.RecomputeClipWindows = true;

    /* --- APU / S-DSP ------------------------------------------------------ */
    S9xAPUUnpackStatus();
    IAPU.DirectPage = APUCheckDirectPage() ? IAPU.RAM + 0x100 : IAPU.RAM;
    for (int u = 0; u < NUM_CHANNELS; u++) {
        SoundData.channels[u].env_ind_attack  &= 0xf;
        SoundData.channels[u].env_ind_decay   &= 0x7;
        SoundData.channels[u].env_ind_sustain &= 0x1f;
    }
    /* Clearing echo_enable makes S9xSetEchoEnable() (reached through
     * S9xFixSoundAfterSnapshotLoad -> S9xSetEchoDelay) wipe the echo ring and
     * rebuild each channel's echo_buf_ptr from APU.DSP[EON]. */
    SoundData.echo_enable = 0;
    S9xFixSoundAfterSnapshotLoad();

    /* --- Coprocessors ----------------------------------------------------- */
    if (hdr.flags & STATE_HAS_SUPERFX) {
        GSU.vColorReg = g.vColorReg;
        GSU.vPlotOptionReg = g.vPlotOptionReg;
        GSU.vLastRamAdr = g.vLastRamAdr;
        GSU.vPrevMode = g.vPrevMode;
        GSU.vPrevScreenHeight = g.vPrevScreenHeight;
        GSU.pvDreg = &GSU.avReg[g.dreg & 15u];
        GSU.pvSreg = &GSU.avReg[g.sreg & 15u];
        GSU.vRomBuffer = g.vRomBuffer;
        GSU.vPipe = g.vPipe;
        GSU.vSCBRDirty = true;
    }
    if (hdr.flags & STATE_HAS_SA1) {
        S9xFixSA1AfterSnapshotLoad();
        SA1.PCAtOpcodeStart = SA1.PC;
        if (Memory.SRAM) SA1.BWRAM = Memory.SRAM + sa1_bwram_off;
    }
    if (hdr.flags & STATE_HAS_SRTC) S9xSRTCPostLoadState();
    if (hdr.dsp_bytes) S9xDSPPostLoadState();

    printf("[state] loaded %s\n", path);
    return 0;
}

}
