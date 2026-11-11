/*
port/buffers.c — see buffers.h.

Also defines the frame-loop globals the core expects the host to own
(scan_line, frame_counter, system_clock).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gwenesis_bus.h"
#include "gwenesis_vdp.h"
#include "ym2612.h"
#include "buffers.h"
#include "gwsram.h"

/* Core-visible memory (extern pointers declared in the vendored core). */
unsigned char *M68K_RAM = NULL; /* 64 KB — 68000 work RAM */
unsigned char *ZRAM = NULL;     /*  8 KB — Z80 RAM */
unsigned char *VRAM = NULL;     /* 64 KB — VDP RAM */

/* Audio sample buffers, one per chip, mono int16 at ~53.267 kHz.
   PAL frames need up to 1061 samples (see GWENESIS_AUDIO_BUFFER_LENGTH_PAL). */
int16_t *gwenesis_ym2612_buffer = NULL;
int16_t *gwenesis_sn76489_buffer = NULL;

/* Chip catch-up bookkeeping (the core declares these extern). */
int ym2612_index;
int ym2612_clock;
int sn76489_index;
int sn76489_clock;

/* Frame-loop globals owned by the host (core references them extern). */
int scan_line;
int frame_counter;
int system_clock;

#if defined(GWENESIS_LUTS_IN_RAM) && GWENESIS_LUTS_IN_RAM != 0
/* RAM working copies of the YM2612 LUTs (~46 KB). The synthesis reads
   them on every sample; leaving them in XIP flash makes core1 (offload)
   or core0 (sync) contend with the 68000 dispatch tables and the PSRAM
   ROM on the shared QMI port — measured as frame drops in DAC-heavy
   games. Heap-allocated per game like everything else here.

   These MUST stay in SRAM, and the QMI note above is why: on a console build
   the cartridge ROM lives in PSRAM, and the PSRAM window is behind the same
   QMI port. Moving the LUTs there too puts a second QMI client on the port,
   and Castlevania Bloodlines drops 60 -> 51 fps from it alone. Tried, measured
   on the console, reverted. The console build finds the memory elsewhere -- the
   ROM and the cart save RAM are the cold buffers that moved to PSRAM. */
static void *ym_tl_ram, *ym_sin_ram;
#endif

/* Called by the sound chips when a caller-supplied timestamp would have
   pushed the per-frame sample index past the end of the audio buffers.
   Reports once per chip per game: the index and timestamp identify how
   far out of range the driving clock went. */
void gwenesis_audio_report_clamp(const char *chip, int index, int target)
{
    static int reported_ym, reported_psg;
    int *seen = (chip[0] == 'y') ? &reported_ym : &reported_psg;
    if (*seen)
        return;
    *seen = 1;
    printf("AUDIO CLAMP: %s index=%d (max %d) target=%d frame=%d line=%d\n",
           chip, index, GWENESIS_AUDIO_BUFFER_MAX, target, frame_counter, scan_line);
}

/* Every buffer is allocated with a trailing guard word. An emulator-side
   overrun would otherwise land on malloc's chunk header and only surface
   much later as a corrupt free list (a hard fault deep inside
   malloc/free/mallinfo), which is impossible to attribute. Checking the
   guards names the culprit at the moment it is detected. */
#define GUARD_MAGIC 0xA5C3F00Du
#define GUARD_BYTES 4

typedef struct {
    void *ptr;
    size_t size;
    const char *name;
} tracked_buf_t;

static tracked_buf_t tracked[16];
static int tracked_count;

static void track(void *p, size_t size, const char *what)
{
    if (tracked_count < (int)(sizeof(tracked) / sizeof(tracked[0]))) {
        tracked[tracked_count].ptr = p;
        tracked[tracked_count].size = size;
        tracked[tracked_count].name = what;
        tracked_count++;
    }
}

/* Emulation memory, in SRAM.
 *
 * These are the buffers the 68000 and the VDP touch on every access, so they
 * stay in SRAM. Their real cost (read off the headers, not estimated) is:
 *
 *   m68k 65536 + z80 8192 + vram 65536 + audio 4288 = 143556
 *
 * and the link leaves a 197488 byte heap, so 53932 remain for everything else.
 * The layout of that 53932 is the whole ballgame:
 *
 *   tl_tab  26624   read per operator per sample   -> SRAM
 *   sin_tab  4096   read per operator per sample   -> SRAM
 *   FIFO      8192   gwsnd, 1024 x gwsnd_event_t(8) -> SRAM
 *   bridge    4096   gwsnd, 1024 stereo frames      -> SRAM
 *   ----------------------------------------------
 *   total    43008                                 -> 10924 to spare
 *
 * lfo_pm_table (16384) used to be a fourth SRAM entry and did not fit: the
 * deficit was 5460 bytes, and gwsnd_offload_start() simply failed its
 * allocation, so the game came up silent and the failure presented as a black
 * screen after the ROM loaded rather than as an error. The ROM list was
 * unaffected, which is what made it hard to read.
 *
 * lfo_pm_table is now left in flash because it is the only one of the three not
 * read per operator per sample -- update_phase_lfo_slot() is reached only under
 * `if (CH->pms)`, i.e. only on channels that use pitch modulation. That buys the
 * two hot tables fast SRAM, which is worth 9 fps on a DAC-heavy game (Castlevania,
 * 60 -> 51 when all three sat in PSRAM sharing the QMI port with the ROM).
 *
 * A failed malloc() is reported rather than stepped over: the SDK's malloc
 * panics, and panic is an infinite loop, which is exactly how a game that will
 * not start presents itself. */
static void *alloc_or_report(size_t size, const char *what)
{
    void *p = malloc(size + GUARD_BYTES);
    if (!p) {
        printf("init_emulator_mem: SRAM alloc of %u bytes for %s FAILED\n",
               (unsigned)size, what);
        return NULL;
    }
    *(volatile uint32_t *)((uint8_t *)p + size) = GUARD_MAGIC;
    track(p, size, what);
    return p;
}

/* Returns the number of buffers whose guard word was clobbered. */
int check_emulator_mem(const char *when)
{
    int bad = 0;
    for (int i = 0; i < tracked_count; i++) {
        if (!tracked[i].ptr)
            continue;
        uint32_t g = *(volatile uint32_t *)((uint8_t *)tracked[i].ptr + tracked[i].size);
        if (g != GUARD_MAGIC) {
            printf("HEAP GUARD CLOBBERED (%s): %s overran, guard=%08x\n",
                   when, tracked[i].name, (unsigned)g);
            bad++;
        }
    }
    return bad;
}

bool init_emulator_mem(void)
{
    free_emulator_mem();

    M68K_RAM = alloc_or_report(MAX_RAM_SIZE, "M68K_RAM");
    ZRAM = alloc_or_report(MAX_Z80_RAM_SIZE, "ZRAM");
    VRAM = alloc_or_report(VRAM_MAX_SIZE, "VRAM");
    gwenesis_ym2612_buffer =
        alloc_or_report(GWENESIS_AUDIO_BUFFER_LENGTH_PAL * sizeof(int16_t), "ym buffer");
    gwenesis_sn76489_buffer =
        alloc_or_report(GWENESIS_AUDIO_BUFFER_LENGTH_PAL * sizeof(int16_t), "psg buffer");

    if (!M68K_RAM || !ZRAM || !VRAM || !gwenesis_ym2612_buffer || !gwenesis_sn76489_buffer) {
        free_emulator_mem();
        return false;
    }

#if defined(GWENESIS_LUTS_IN_RAM) && GWENESIS_LUTS_IN_RAM != 0
    /* In SRAM, so the synthesis reads them at full speed. lfo_pm_table (16 KB)
     * is not among them: it is only reached under `if (CH->pms)` and stays in
     * flash, which is what leaves room for these two plus the sound FIFO. */
    ym_tl_ram  = alloc_or_report(YM2612_TL_TAB_BYTES, "ym tl_tab");
    ym_sin_ram = alloc_or_report(YM2612_SIN_TAB_BYTES, "ym sin_tab");
    if (!ym_tl_ram || !ym_sin_ram) {
        free_emulator_mem();
        return false;
    }
    ym2612_luts_init_ram(ym_tl_ram, ym_sin_ram);
#endif

    /* Cartridge save RAM is deliberately NOT allocated here: its size comes
       from the cartridge rather than the emulator, and SGDK declares a 32 KB
       one in every game it builds whether the game uses it or not. gwsram.c
       allocates on first use instead.

       The window still has to be mapped, though, or no access would ever
       reach gwsram.c to trigger that allocation. gwsram_detect() set it up,
       and the free_emulator_mem() at the top of this function tore it down
       again, so put it back. */
    if (gwsram_span)
        gwsram_live = gwsram_bankable ? 0 : gwsram_span;

    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);
    memset(VRAM, 0, VRAM_MAX_SIZE);
    memset(gwenesis_ym2612_buffer, 0, GWENESIS_AUDIO_BUFFER_LENGTH_PAL * sizeof(int16_t));
    memset(gwenesis_sn76489_buffer, 0, GWENESIS_AUDIO_BUFFER_LENGTH_PAL * sizeof(int16_t));
    return true;
}

void free_emulator_mem(void)
{
    if (tracked_count)
        check_emulator_mem("free");
    tracked_count = 0;
    free(M68K_RAM);
    free(ZRAM);
    free(VRAM);
    free(gwenesis_ym2612_buffer);
    free(gwenesis_sn76489_buffer);
    gwsram_release();
    M68K_RAM = NULL;
    ZRAM = NULL;
    VRAM = NULL;
    gwenesis_ym2612_buffer = NULL;
    gwenesis_sn76489_buffer = NULL;
#if defined(GWENESIS_LUTS_IN_RAM) && GWENESIS_LUTS_IN_RAM != 0
        free(ym_tl_ram);
        free(ym_sin_ram);
        ym_tl_ram = ym_sin_ram = NULL;
#endif
}
