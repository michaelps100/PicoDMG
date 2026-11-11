/**
 * PicoSNES for Raspberry Pi RP2350 + ILI9341 SPI TFT + MAX98357A I2S
 *
 * Super Nintendo Entertainment System (SNES) Emulator with Clean Light UI,
 * In-Game Pause Menu, Save/Load States, Scanlines Effect,
 * 60 FPS Interpolated Display Pipeline, and SD Card Storage.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <algorithm>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/divider.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/spi.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
/* qmi_hw: the flash XIP timing register the safe-flash wrappers below fix up. */
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/pwm.h"

#include "ff.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "external_audio.h"
#include "state.h"

#if USE_ILI9341
#include "ili9341.h"

/* Shared Clean Light UI, panel selection, combo layer.
 *
 * The whole local UI that used to live in this file (ROM browser, in-game pause
 * menu, save/load slots, config persistence, volume HUD, button reading, the
 * SELECT+START / START+A / START+B combos) now comes from shared_menu, which is
 * what the other nine emulators use. What stays here is what is genuinely
 * SNES-specific: the descriptor, the S9x callbacks, and the Image submenu. */
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"
#endif

/* Snes9x emulator core headers (upstream pico-snesPlus tree, snes9x_src/).
 * Note the paths changed: src/ held the old fork, snes9x_src/ holds the core we
 * actually build. The old fork's src/snes9x.h, src/ppu.h and src/snapshot.h
 * are gone; ppu.c and gfx.c replace ppu_.c. Savestates are state.cpp's own
 * struct dump, since the upstream core has no snapshot code. */
extern "C" {
#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "ppu.h"
#include "apu.h"
#include "soundux.h"
#include "gfx.h"
#include "display.h"
#include "port.h"
#if ENABLE_MSU1
#include "msu1.h"
#endif
#include "hardware/structs/scb.h"
}

extern "C" void __not_in_flash_func(hardfault_report)(uint32_t *sp, uint32_t exc_lr)
{
    armv8m_scb_hw_t *scb = scb_hw;
    printf("\n*** HARDFAULT core%u ***\n", (unsigned)get_core_num());
    printf("  r0=%08lx r1=%08lx r2=%08lx  r3=%08lx\n",
           (unsigned long)sp[0], (unsigned long)sp[1],
           (unsigned long)sp[2], (unsigned long)sp[3]);
    printf(" r12=%08lx lr=%08lx pc=%08lx psr=%08lx\n",
           (unsigned long)sp[4], (unsigned long)sp[5],
           (unsigned long)sp[6], (unsigned long)sp[7]);
    printf("  sp=%08lx exc_lr=%08lx\n", (unsigned long)(uintptr_t)sp,
           (unsigned long)exc_lr);
    printf("  CFSR=%08lx HFSR=%08lx MMFAR=%08lx BFAR=%08lx\n",
           (unsigned long)scb->cfsr, (unsigned long)scb->hfsr,
           (unsigned long)scb->mmfar, (unsigned long)scb->bfar);
    while (true)
        __breakpoint();
}

extern "C" __attribute__((naked)) void __not_in_flash_func(isr_hardfault)(void)
{
    __asm volatile(
        "tst lr, #4          \n"
        "ite eq              \n"
        "mrseq r0, msp       \n"
        "mrsne r0, psp       \n"
        "mov r1, lr          \n"
        "b hardfault_report  \n");
}

/* =========================================================================
 * Build Target Configuration (PicoSNES)
 * ========================================================================= */
#define APP_TITLE            "PicoSNES"
#define CONFIG_MAGIC         0x534E4553u /* "SNES" */
#define CONFIG_PATH          "0:/saves/snes/picosnes.cfg"
#define CONFIG_PATH_FALLBACK "/saves/snes/picosnes.cfg"
#define SAVES_DIR            "0:/saves/snes"
#define SAVES_DIR_FALLBACK   "/saves/snes"
#define ROMS_DIR             "0:/roms/snes"
#define ROMS_DIR_FALLBACK    "/roms/snes"
#define DIAG_PATH            "0:/saves/snes/diag.txt"

#define SNES_ROM_CACHE_HEADROOM 0x180000u /* 1.5 MB offset, safe beyond binary size */
#define SNES_ROM_CACHE_MAX      0x600000u /* 6 MB max SNES ROM */
#define SNES_ROM_CACHE_ABS      (0x10080000u + SNES_ROM_CACHE_HEADROOM)
#define SNES_ROM_CACHE_OFF      (SNES_ROM_CACHE_ABS - 0x10000000u)

/* Load the cartridge into PSRAM (1) or keep programming the XIP flash window (0).
 *
 * The two are not equivalent, and which one works has to be established on
 * hardware rather than assumed. Snes9x 2002 reaches ROM through the pointer
 * tables in Memory.Map[], so a PSRAM-resident cartridge is addressable in
 * principle -- but anything the core *writes* to ROM behaves differently: stores
 * to the flash XIP window are silently dropped, while the same store to PSRAM
 * actually lands. ApplyROMFixes() is one such writer, so games needing a header
 * patch behave differently between the two paths.
 *
 * Default is 1 (PSRAM), which is the intended console behaviour. Build with
 * -DSNES_LOAD_ROM_IN_PSRAM=0 to get the flash path for comparison. */
#ifndef SNES_LOAD_ROM_IN_PSRAM
#define SNES_LOAD_ROM_IN_PSRAM 1
#endif

/* microSD rate. Reachable because clk_peri is pinned to 288 MHz in main(): 288 / (2 x 8) = 18.000 MHz. Same order as the 17.5 MHz the root AGENTS.md sec. 9.3 records as the safe ceiling for this SD wiring. */
#ifndef PERIPH_CLOCK_DECOUPLE
#define PERIPH_CLOCK_DECOUPLE 0
#endif


#if PICO_RP2350
/* 360 MHz at 1.30 V.
 *
 * Back to these values after trying upstream's 378 MHz / 1.50 V (their
 * EMULATOR_CLOCKFREQ_KHZ and their stated "1.6V is the safe limit for 378 Mhz").
 * The higher clock/voltage pair stopped being stable here: the cartridge loaded
 * to 100% and the screen went black, then the run ended and returned to the ROM
 * selector. Upstream validated 378 MHz on its own boards, not on this one, and
 * their own README records that the overclock option "can cause instability" --
 * so this is a board difference, not a misreading of their value.
 *
 * The gain was small anyway: 360 -> 378 is +5%, which moves avg_emu by the same
 * 5% and does not close a 20 ms vs 16.667 ms gap on its own. Their CHANGELOG says
 * the bottleneck is PSRAM bandwidth, not the CPU clock, and the [prof] line
 * agrees: avg_emu swings 9.5..27 ms within one game, which is traffic, not
 * cycles. So this cost stability for a couple of fps. */
#define EMULATOR_CLOCKFREQ_KHZ 378000
#define EMULATOR_VOLTAGE       VREG_VOLTAGE_1_50
#else
#define EMULATOR_CLOCKFREQ_KHZ 252000
#define EMULATOR_VOLTAGE       VREG_VOLTAGE_1_30
#endif
static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

static bool isFatalError = false;

/* Completed-frame counter, bumped by the S9x video hook. That hook sits far
 * above the diagnostics that report it, so it is declared up here. */
static volatile uint32_t s_diag_frames = 0;

/* Not static: msu1_port.cpp (MSU-1 audio/data backend) reads this to find the
 * <romname>.msu and <romname>-<n>.pcm files next to the cartridge. It is still
 * only ever written here -- the shared_menu selector fills it. */
char selectedRom[FF_MAX_LFN];
static uint32_t last_flashed_rom_size = 0;

static uint32_t fps = 0;
static char fpsString[8] = "60";

/* Audio rate: 32000 Hz, matching native SNES DSP output rate and the hardware
 * I2S DAC (MAX98357A / PCM5102). This eliminates upsampling overhead and reduces
 * S9xMixSamples workload by 27.3% per frame. */
#define SNES_AUDIO_RATE 32000

/* Buffer of 2048 int16 provides ample room for one frame of stereo samples
 * (532..534 stereo pairs = 1064..1068 int16 at 32000 Hz NTSC, or 640 pairs PAL). */
static int16_t s_audio_buffer[2048] __attribute__((aligned(4)));

/* Acumuladores do log de profile: microsegundos de emulacao pura vs total de
 * quadro, somados por segundo. Ver snes_frame_diagnostics(). */
static uint32_t s_prof_emu = 0;
static uint32_t s_prof_total = 0;
static uint32_t s_prof_frames = 0;
/* Inicio da janela do profile, em time_us_32(). Medir o FPS por 
 * frames/segundo-assumido esconde quadro estourando o orcamento. */
static uint32_t s_prof_window_start = 0;
/* Cost of writing the log line itself, and the slice of the current frame that
 * the write occupied. Kept apart from emu/post because snes_frame_diagnostics()
 * runs INSIDE the frame loop (main.cpp:1935) while `elapsed` is measured after it
 * (main.cpp:1949) -- so the write was landing in the frame's `elapsed`, and thus
 * in s_prof_total, one window later than the report that printed it.
 *
 * That is not cosmetic. A single f_sync() costing 3.7 s showed up as
 * post=3745616 in a window of 1009577 us -- post > window, arithmetically
 * impossible -- and the same 3.7 s also appeared as unexplained wall time in the
 * PREVIOUS window. One event, counted twice, in two different windows. Every
 * reading of `post` from that log was wrong. See AGENTS.md 42. */
static uint32_t s_prof_diag_us = 0;      /* accumulated, reported as diag= */
static uint32_t s_diag_exclude_us = 0;    /* subtracted from the next `elapsed` */

/* Samples of audio one NTSC frame is worth.
 *
 * NTSC is 60.0988 Hz, not 60, and getting this wrong is what made the sound
 * garbled. The mixer has to be handed a whole number of samples per frame, so
 * (32000 / 60) = 533.33 was being truncated to 533 -- and the frame budget is
 * 532.44. Handing the mixer 533 while the DMA consumes 532.44 adds ~0.56 samples
 * to the ring every frame, ~34 per second. The ring fills, the write path starts
 * dropping, and the result is the crackle/underrun the mixer cannot fix from its
 * side: it is being fed more audio per second than the I2S clock takes out.
 *
 * Fractional rates are handled by alternating 532 and 533 so the long-run average
 * lands on 532.44 rather than on either integer. Exactly the fix
 * AGENTS.md sec.16 records for the Game Boy, where the same truncation was worth
 * ~77 us of drift per frame.
 *
 * 44100, not 32000, and this has to match DVIAUDIOFREQ -- which is what the I2S
 * hardware is actually clocked at (FrensHelpers.cpp passes DVIAUDIOFREQ straight
 * to EXT_AUDIO_SETUP). The two rates are not interchangeable: the ring is filled
 * at the core's rate and drained by the DMA at the hardware's, so any mismatch is
 * a monotone drift, not a resampling artefact. At 32000 against a 44100 DMA the
 * ring loses 12100 samples every second and never recovers, which is a
 * permanently underflowing stream -- the crackle, and it cannot be fixed from the
 * mixer side no matter how many samples it is handed.
 *
 * 32000 was chosen here on the reasoning that a lower rate is fewer samples for
 * the same wall-clock and the ring would have more slack. That reasoning ignored
 * that the consumer's rate is not ours to pick. The upstream uses 44100 for the
 * same reason. */
#define SNES_FRAMES_PER_SEC 60.0988
static int32_t s_audio_frac_acc = 0;   /* residual, in thousandths of a sample */

/* Whole samples due this frame, carrying the fraction so the average is exact.
 *
 * The fixed-point representation matters. NTSC is 60.0988 Hz, so one frame is
 *
 *     32000 / 60,0988 = 532,4439 samples
 *
 * which is not a whole number, and the mixer has to be handed a whole number.
 * Truncating to 532 feeds the DMA 0,44 samples per frame less than it removes,
 * and rounding to 533 feeds it more -- either way the ring drifts and the DMA
 * eventually underruns, which is the crackle. So the fraction is carried and the
 * count alternates to land on 532,4439 over any window.
 *
 * SCALE is 1000 and UNITS is 1000: SNES_AUDIO_FRAME_UNITS below is the per-frame
 * cost *in thousandths of a sample*, so one whole sample is 1000 of those units.
 * The two have to agree.
 *
 * Two revisions got this wrong, both in the same direction -- a divisor ten or a
 * thousand times too large, so the mixer was handed almost nothing:
 *
 *   1. (RATE * 1000) / 100 = 320000, divided by 100000 -> 3 samples per frame.
 *      Audio came out as a buzz.
 *   2. The frame cost computed correctly (532,455 thousandths) but divided by
 *      1,000,000 on the theory that a sample was a millionth -> 0,53 samples per
 *      frame, i.e. a sample every other frame, alternating 0 and 1.
 *
 * The second is the more dangerous of the two because the function still returns
 * a plausible-looking number and the audio still comes out of the speakers; it
 * just arrives at a thirtieth of the rate the DMA is draining, so the ring
 * underflows continuously. This is the same failure as a rate mismatch, which
 * means it would have been misdiagnosed as "the audio rate is wrong" -- which it
 * was, just not in the direction the name suggests.
 *
 * The check that catches it is the one in the mix call below: at 44100 a frame is
 * 733 stereo pairs = 1466 int16, and that must be both >= the buffer and
 * consistent with what the hardware is clocked at. Those two numbers agreeing is
 * the invariant; the fixed-point arithmetic only has to agree with itself.
 */
#define SNES_AUDIO_SCALE      1000
#define SNES_AUDIO_UNITS      1000

/* NTSC frame rate in milli-Hz: 60,0988 Hz -> 60099.
 *
 * Kept as an explicit rational rather than a float so the per-frame cost below is
 * exact integer arithmetic. The previous form divided by 6009, i.e. it assumed
 * 60,09 Hz and produced 532,533 thousandths per frame instead of 532,443 -- a
 * surplus of 0,09 samples per frame, or ~5 samples per second, which is the same
 * class of drift as the 533 it was meant to fix. The 64-bit cast is required:
 * 32000 * 1000 * 1000 overflows int32. */
#define SNES_NTSC_MILLI_HZ    60099

/* Per-frame cost in thousandths of a sample: RATE / 60,0988 * 1000 = 733,754.
 *
 * Residual stays below SNES_AUDIO_UNITS by construction, so the accumulator
 * cannot overflow int32 no matter how long the console runs. */
#define SNES_AUDIO_FRAME_UNITS \
    ((int32_t)(((int64_t)SNES_AUDIO_RATE * SNES_AUDIO_SCALE * 1000) / SNES_NTSC_MILLI_HZ))

/* SOUND_BUFFER_SIZE in soundux.h is 2 * 33040 / 50 = 1321 int32, and S9xMixSamples
 * uses sample_count as a bare index into MixBuffer and EchoBuffer -- it memsets and
 * writes that many entries. So a single call may not exceed 1321.
 *
 * At 44100 Hz one NTSC frame is 733,754 stereo pairs = 1466 int16, which is over
 * that limit by 145 entries, i.e. 580 bytes written past the end of two globals
 * that sit in .data. The overflow is silent: S9xMixSamples has no bounds check and
 * nothing validates the count.
 *
* Above 1320 is a memory-corruption bug, not a tuning question -- and it was very
   * nearly mistaken for "44100 is too high, go back to 32000", which would have
   * hidden the real defect while leaving the underflow in place.
   *
   * EVEN, not 1321. A chunk boundary must never fall between the two halves of a
   * stereo pair. S9xMixSamples() restarts its own index at 0 every call and
   * resolves the channel with `master_volume[J & 1]`, so it assumes sample 0 is a
   * LEFT sample. 1466 = 733 pairs: chunk 1 would end on sample 1320, which is even,
   * i.e. the left half of pair 660 -- and chunk 2 would then present the right half
   * of that pair as if it were a left one. 72 stereo pairs per frame come out
   * channel-swapped, 60 times a second. */
  #define S9X_MIX_MAX_SAMPLES   1320

static inline int snes_samples_this_frame(void) {
    if (Settings.PAL) {
        return SNES_AUDIO_RATE / 50;
    }
    s_audio_frac_acc += SNES_AUDIO_FRAME_UNITS;
    const int n = (int)(s_audio_frac_acc / SNES_AUDIO_UNITS);
    s_audio_frac_acc -= (int32_t)n * SNES_AUDIO_UNITS;
    return n;
}

/* =========================================================================
 * Persistent Configuration
 *
 * The emulator's own settings live in persistent_*; dmg_config_t
 * (g_dmg_config, in shared_menu) is what actually gets serialised. Those are
 * two copies of the same state, and only the second one reaches the disk --
 * which is why every write goes through snes_save_config() below rather than
 * straight to dmg_config_save().
 * ========================================================================= */
static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_scanlines_enabled = false;
static bool persistent_interleave = false;
static bool persistent_display_fps = false;
static uint8_t persistent_volume_level = 3;      /* 0..5, 0 = mute */
static uint8_t persistent_brightness_level = 5;  /* 1..5, matching shared_menu */
static uint8_t persistent_turbo_mode = 0;        /* 0: 1x, 1: 2x */
static uint8_t persistent_save_slot = 1;         /* 1..4 */
static uint8_t persistent_load_slot = 1;         /* 1..4 */
enum snes_frameskip_mode_t {
    FRAMESKIP_OFF  = 0,
    FRAMESKIP_AUTO = 1,
    FRAMESKIP_ON   = 2
};
static uint8_t persistent_frameskip_mode = FRAMESKIP_OFF;

/* Auto frameskip tuning (see the pacing block in run_game). Skip the next
 * frame's render only once we are more than ~1 ms behind real time, and never
 * more than 3 in a row (worst case 15 rendered fps during a burst, never a
 * frozen picture). */
#define SNES_SKIP_LATE_US          1000u
#define SNES_MAX_CONSECUTIVE_SKIPS 3u

static bool s_request_pause_menu = false;
static bool s_request_exit_game = false;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;

/* Descriptor callbacks, defined below next to the descriptor that stores them.
 *
 * s_snes_desc itself is forward declared because snes_save_config() -- defined
 * earlier, it is one of the descriptor's own callbacks -- has to pass it to
 * dmg_config_save(). */
/* s_snes_desc is forward declared because snes_save_config() -- defined earlier,
 * it is one of the descriptor's own callbacks -- has to pass it to
 * dmg_config_save().
 *
 * extern, not static: in C++ a namespace-scope `static const T x;` with no
 * initialiser is a definition, not a tentative declaration, so the forward form
 * would collide with the real one below. */
extern const dmg_emulator_desc_t s_snes_desc;
static void snes_save_config(void);
static void snes_load_config(void);
static void snes_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name);
static void ensure_save_directories(void);
static void set_backlight_brightness(uint8_t level);

/* =========================================================================
 * Clean Light UI
 *
 * The palette, the bars, the paged ROM browser and the in-game pause menu all
 * come from shared_menu now. There is deliberately no local copy left: this
 * file used to carry a fourth variant of the same 700 lines, with its own
 * palette (still mint green, never migrated to #BF0000), its own slot handling
 * and its own combo predicates.
 *
 * Static buffers kept for the ROM/load path, to keep large framebuffer-sized
 * locals off the stack.
 * ========================================================================= */
static char current_rom_dir[64] = ROMS_DIR;
static char s_full_path[FF_MAX_LFN + 32];
static FIL  s_io_fil;

/* =========================================================================
 * Physical Controls
 *
 * Buttons are read by shared_menu (GP2..GP9, active low, internal pull-ups),
 * which is also what the menu code reads -- so there is exactly one mapping of
 * a physical button to a logical name in the whole project.
 *
 * Console face buttons -> SNES buttons:
 *   console A (GP6) -> SNES B   (confirm in menus)
 *   console B (GP7) -> SNES Y   (cancel in menus)
 *   d-pad, SELECT and START map straight through.
 *
 * SNES A, X, L and R are intentionally not wired: the shell only carries two
 * face buttons, and mapping a third one onto an already-used console button
 * would double up with the combos layer. Games needing six buttons or a
 * shoulder throw need a real controller port, which is separate work.
 * ========================================================================= */

/* =========================================================================
 * Backlight Control (PWM / SIO)
 * ========================================================================= */
static void set_backlight_brightness(uint8_t level) {
    /* 1..5, not the 0..10 this file used to use.
     *
     * The Brightness row belongs to shared_menu, which steps 1..5 for every
     * emulator in the project. A 0..10 scale here would have meant the same
     * menu row moving in half-steps on this emulator only -- and level 0 would
     * have been "screen off", which the shared row never offers. */
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    persistent_brightness_level = level;
#if USE_ILI9341
    if (ILI9341_PIN_BL != 0xFF) {
        if (level >= 5) {
            gpio_set_function(ILI9341_PIN_BL, GPIO_FUNC_SIO);
            gpio_set_dir(ILI9341_PIN_BL, GPIO_OUT);
            gpio_put(ILI9341_PIN_BL, 1);
        } else {
            gpio_set_function(ILI9341_PIN_BL, GPIO_FUNC_PWM);
            uint slice_num = pwm_gpio_to_slice_num(ILI9341_PIN_BL);
            pwm_config config = pwm_get_default_config();
            pwm_config_set_clkdiv(&config, 4.0f);
            pwm_config_set_wrap(&config, 1000);
            pwm_init(slice_num, &config, true);
            static const uint16_t duty_table[6] = { 0, 200, 400, 600, 800, 1000 };
            pwm_set_gpio_level(ILI9341_PIN_BL, duty_table[level]);
        }
    }
#endif
}

/* =========================================================================
 * Save Directories & Config File Persistence
 * ========================================================================= */
static void ensure_save_directories(void) {
    f_mkdir("0:/saves");
    f_mkdir(SAVES_DIR);
    f_mkdir("/saves");
    f_mkdir(SAVES_DIR_FALLBACK);
}

/* "Write my config now", as the descriptor and the combo layer call it.
 *
 * The copy from persistent_* into g_dmg_config is the whole point. dmg_config_save()
 * serialises g_dmg_config, and the Image submenu only ever updates persistent_*,
 * so saving without copying would write a record whose scale/effect/screen
 * fields are the previous session's values -- the file would be valid and stale,
 * which is exactly the failure the shared menu had across six emulators before
 * this shape was agreed on. */
static void snes_save_config(void) {
    g_dmg_config.scale_mode       = (uint32_t)persistent_scale_mode;
    g_dmg_config.grid_effect      = persistent_scanlines_enabled ? 1u : 0u;
    g_dmg_config.interleave       = persistent_interleave ? 1u : 0u;
    g_dmg_config.turbo_mode       = persistent_turbo_mode;
    g_dmg_config.volume_level     = persistent_volume_level;
    g_dmg_config.brightness_level = persistent_brightness_level;
    g_dmg_config.save_slot        = persistent_save_slot;
    g_dmg_config.load_slot        = persistent_load_slot;
    /* dmg_config_t has no FPS field; extra_opt1 is the agreed transport. */
    g_dmg_config.extra_opt1       = persistent_display_fps ? 1u : 0u;
    /* Frameskip mode: extra_opt2 is the agreed transport (0: Off, 1: Auto, 2: On). */
    g_dmg_config.extra_opt2       = persistent_frameskip_mode;
    dmg_config_save(&s_snes_desc);
}

/* Applied when a game is entered, not only at boot.
 *
 * Safe to repeat because every menu visit commits before it closes, so the file
 * always reflects what was on screen when the menu was last dismissed. This is
 * what makes the config file the source of truth rather than a snapshot from
 * whenever the console was last powered on. */
static void snes_load_config(void) {
    /* dmg_config_load() pushes scale/effect/interleave/speed/volume/brightness
     * back into persistent_* through the descriptor callbacks. extra_opt1 has no
     * callback of its own, so the FPS overlay is applied by hand. */
    dmg_config_load(&s_snes_desc);
    persistent_display_fps = (g_dmg_config.extra_opt1 != 0u);
    ili9341_set_fps_overlay(persistent_display_fps, fpsString);
    if (g_dmg_config.extra_opt2 <= 2u) {
        persistent_frameskip_mode = (uint8_t)g_dmg_config.extra_opt2;
    }
}

static void get_sanitized_basename(const char *path, char *out, size_t out_len) {
    if (!path || !out || out_len == 0) return;
    const char *slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    const char *fname = slash ? (slash + 1) : path;
    strncpy(out, fname, out_len - 1);
    out[out_len - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    for (char *p = out; *p != '\0'; p++) {
        if (*p == ' ' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            *p = '_';
        }
    }
}

/* How many bytes of Memory.SRAM this cartridge actually has.
 *
 * The old fork kept SRAM in a fixed static buffer and saved a compile-time
 * constant. This core does not: memmap.c allocates SRAM_SIZE (128 KB, in PSRAM)
 * up front and then sets Memory.SRAMSize from the cart header ($7FE0 / 0x28),
 * which is the battery size in KB. So the honest number is the header's, and
 * clamping to the allocation is what keeps a lying header from turning into an
 * out-of-bounds write into PSRAM.
 *
 * Zero means "no battery": nothing is read and nothing is written, which is what
 * a plain ROM should do. */
static uint32_t snes_battery_sram_bytes(void) {
    if (!Memory.SRAM || !Memory.SRAMSize) return 0;
    /* Memory.SRAMSize is the header's size CODE, not KB: bytes = 1 KB << code,
     * exactly as memmap.c derives SRAMMask ((1 << (code + 3)) * 128 - 1). Reading
     * it as KB saved code*1 KB -- 1 KB of SMW's 2 KB, 3 KB of an 8 KB cart such
     * as Zelda ALttP, 5 KB of Yoshi's Island's 32 KB -- so the rest of every
     * battery save was silently dropped. Older, shorter .srm files still load:
     * system_load_sram() reads min(file, cart). */
    uint32_t n = 1024u << Memory.SRAMSize;
    if (n > s9x_port_sram_alloc) n = s9x_port_sram_alloc;
    return n;
}

static void system_load_sram(const char *game_name) {
    const uint32_t want = snes_battery_sram_bytes();
    if (!want) return;
    char base[64];
    get_sanitized_basename(game_name, base, sizeof(base));

    char sram_path[FF_MAX_LFN];
    snprintf(sram_path, sizeof(sram_path), "%s/%s.srm", SAVES_DIR, base);
    FRESULT fr = f_open(&s_io_fil, sram_path, FA_READ);
    if (fr != FR_OK) {
        snprintf(sram_path, sizeof(sram_path), "%s/%s.srm", SAVES_DIR_FALLBACK, base);
        fr = f_open(&s_io_fil, sram_path, FA_READ);
    }
    if (fr == FR_OK) {
        UINT br = 0;
        /* Take the smaller of what the cart declares and what the file holds.
         * Reading `want` unconditionally would run off the end of a short .srm
         * -- the same class of bug as the truncated .ss path in AGENTS.md sec. 32,
         * where a one-byte-short buffer silently ate the last character of a
         * filename. */
        const FSIZE_t have = f_size(&s_io_fil);
        const UINT to_read = (UINT)(have < (FSIZE_t)want ? have : (FSIZE_t)want);
        if (to_read) f_read(&s_io_fil, Memory.SRAM, to_read, &br);
        f_close(&s_io_fil);
        printf("[I] Battery SRAM loaded (%u of %u bytes) from %s\n",
               (unsigned)br, (unsigned)want, sram_path);
    }
}

static void system_save_sram(const char *game_name) {
    const uint32_t want = snes_battery_sram_bytes();
    if (!want) return;
    ensure_save_directories();

    char base[64];
    get_sanitized_basename(game_name, base, sizeof(base));

    char sram_path[FF_MAX_LFN];
    snprintf(sram_path, sizeof(sram_path), "%s/%s.srm", SAVES_DIR, base);
    FRESULT fr = f_open(&s_io_fil, sram_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        snprintf(sram_path, sizeof(sram_path), "%s/%s.srm", SAVES_DIR_FALLBACK, base);
        fr = f_open(&s_io_fil, sram_path, FA_WRITE | FA_CREATE_ALWAYS);
    }
    if (fr == FR_OK) {
        UINT bw = 0;
        f_write(&s_io_fil, Memory.SRAM, want, &bw);
        f_close(&s_io_fil);
        printf("[I] Battery SRAM saved (%u bytes) to %s\n", (unsigned)bw, sram_path);
    }
}

/* =========================================================================
 * Audio Processing (Snes9x S-DSP -> MAX98357A I2S)
 * ========================================================================= */
static inline int16_t snes_sat16(int32_t v) {
#if defined(__arm__) || defined(__thumb__)
    int32_t out;
    __asm__("ssat %0, #16, %1" : "=r"(out) : "r"(v));
    return (int16_t)out;
#else
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
#endif
}

static void play_snes_audio(int16_t *samples, size_t count) {
    static const int32_t vol_table[6] = { 0, 51, 102, 154, 205, 256 };
    uint8_t v_idx = persistent_volume_level > 5 ? 5 : persistent_volume_level;
    int32_t vol = vol_table[v_idx];
    if (count > 0) {
        size_t pairs = count / 2;
        uint32_t *batch = (uint32_t *)samples;
        if (vol == 0) {
            /* Mute mode: stream silence to maintain uninterrupted 32 kHz I2S DMA timing
             * and avoid starvation/underrun artifacts upon unmuting. */
            memset(batch, 0, pairs * sizeof(uint32_t));
        } else if (vol == 256) {
            /* Max volume: pack Left (high 16) and Right (low 16) into 32-bit I2S FIFO word */
            for (size_t i = 0; i < pairs; i++) {
                int16_t l = samples[i * 2];
                int16_t r = samples[i * 2 + 1];
                batch[i] = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
            }
        } else {
            for (size_t i = 0; i < pairs; i++) {
                int32_t l = ((int32_t)samples[i * 2] * vol) >> 8;
                int32_t r = ((int32_t)samples[i * 2 + 1] * vol) >> 8;
                batch[i] = ((uint32_t)(uint16_t)snes_sat16(l) << 16) | (uint16_t)snes_sat16(r);
            }
        }
        EXT_AUDIO_ENQUEUE_SAMPLES(batch, pairs);
    }
}

/* =========================================================================
 * Video Scanline Callback (S9xDeinitUpdate -> Core 1 Blitter)
 * ========================================================================= */
int one_c = 6;
int slow_one_c = 8;
int two_c = 12;
bool overclock_cycles = false;

unsigned char *s9x_stream_buffer = NULL;
unsigned long long s9x_stream_buffer_size = 0;
bool ROMAPUEnabled = false;

/* The old fork's port hooks -- S9xDeinitUpdate, S9xInitUpdate,
 * S9xGetFilename, S9xMessage and S9xLoadSDD1Data -- are gone, and their absence
 * is not a missing feature. This core revision has no update-callback
 * architecture at all: gfx.c drives the display directly and calls
 * s9x_port_strip_copyout() when a block of scanlines is finished (see
 * port_glue.cpp), which is where the panel is fed now. Defining them here
 * would have created symbols the core never calls -- and S9xDeinitUpdate in
 * particular was calling ili9341_stream_snes_frame_end(), so the driver's
 * end-of-frame bookkeeping has moved with it.
 *
 * S9xGetFilename's job -- telling the core where to write the battery save --
 * is still needed, and is provided by the core itself in this revision; the
 * save path is snes9x_set_sram_callback-style plumbing we do not override. */

/* =========================================================================
 * Emulator Descriptor (shared_menu)
 *
 * Everything the shared UI needs to know about this emulator, in one place.
 *
 * The display adapter is wired straight to the driver entry points: their
 * signatures already match dmg_display_t, so there is no wrapper struct and no
 * chance of the two drifting apart.
 *
 * This file used to carry its own copy of the ROM browser, the pause menu, the
 * slot handling and the combo predicates -- roughly 640 lines, a fourth variant
 * of the same UI, still on the old mint selection colour. All of it is now
 * shared, which is what puts this emulator's menu in step with the other nine.
 * ========================================================================= */

static void snes_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name);

static const char *s_snes_candidate_dirs[] = {
    "0:/roms/snes",
    "/roms/snes",
    "0:/roms",
    "/roms",
    NULL
};

/* Bottom-bar text on the loading screen. Points at the right message because the
 * destination depends on the board: a PSRAM board never programs flash, and
 * telling the player it is "Writing flash cache..." while it reads the SD card
 * is a lie they can act on. Refreshed in main() once PSRAM is known. */
static char s_loading_status[48] = "Writing flash cache...";

static bool snes_is_rom_file(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".smc") == 0 ||
            strcasecmp(dot, ".sfc") == 0 ||
            strcasecmp(dot, ".bin") == 0 ||
            strcasecmp(dot, ".fig") == 0);
}

/* Records which directory the browser was actually in.
 *
 * The selector returns a bare filename; anything that needs a real path has to
 * join it against the active directory. */
static bool snes_on_load_rom(const struct dmg_emulator_desc *desc, const char *filename) {
    (void)desc;
    (void)filename;
    const char *active = dmg_get_current_rom_dir();
    if (active && active[0] != '\0') {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", active);
    }
    return true;
}

/* --- Save states ----------------------------------------------------- */

/* DMG_STATE_PATH_MAX, not 64. See the definition for why 64 is one byte short
 * of a real game's name; the shared helper now logs the intended name instead of
 * silently truncating, but the caller still has to provide room for it. */
static bool snes_save_state(const char *game_name, uint8_t slot) {
    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path(SAVES_DIR, game_name, slot, state_path, sizeof(state_path));
    ensure_save_directories();
    return (Emulator_SaveState(state_path) == 0);
}

/* Only the path above is opened -- no legacy unnumbered fallback.
 *
 * That is deliberate rather than an omission: the shared menu's slot probe
 * (desc->state_exists left NULL) looks for exactly this one name, and a probe
 * has to match what the loader can reach. One name, used by both, cannot
 * disagree.
 *
 * A load that fails after state.cpp has started overwriting the core leaves the
 * game half restored; the frame loop then takes the Reset path instead of
 * running it (s_state_load_broke). */
static bool s_state_load_broke = false;

static bool snes_load_state(const char *game_name, uint8_t slot) {
    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path(SAVES_DIR, game_name, slot, state_path, sizeof(state_path));
    const int rc = Emulator_LoadState(state_path);
    if (rc == SNES_STATE_LOAD_PARTIAL) s_state_load_broke = true;
    return rc == 0;
}

/* --- Emulation / display callbacks ----------------------------------- */

static void snes_apply_scale(dmg_scale_mode_t scale_mode) {
    persistent_scale_mode = (ili9341_scale_mode_t)scale_mode;
    ili9341_set_scale_mode(persistent_scale_mode);
}

static void snes_apply_effect(bool effect_on) {
    persistent_scanlines_enabled = effect_on;
    ili9341_set_scanlines(effect_on);
}

/* Not a stub. With a NULL callback here, dmg_config_load() would never push the
 * stored value into persistent_interleave, so "Scan Mode" came back as the
 * compile-time default on every boot no matter what was written. */
static void snes_apply_interleave(bool interleave_on) {
    persistent_interleave = interleave_on;
    ili9341_set_interleave(interleave_on);
    s9x_set_interleave(interleave_on);
}

static void snes_apply_speed(bool turbo_on) {
    persistent_turbo_mode = turbo_on ? 1 : 0;
}

static void snes_apply_volume(uint8_t volume) {
    if (volume > 5) volume = 5;
    persistent_volume_level = volume;
}

static void snes_apply_brightness(uint8_t brightness) {
    set_backlight_brightness(brightness);
}

static bool snes_get_fps(void) {
    return persistent_display_fps;
}

static void snes_apply_fps(bool on) {
    persistent_display_fps = on;
    ili9341_set_fps_overlay(on, fpsString);
}

/* Reset and Exit are left NULL on purpose.
 *
 * dmg_ingame_menu() does not call reset_game -- it returns DMG_ACTION_RESET and
 * lets the emulator's frame loop perform the reset between frames, which is the
 * only safe point for S9xReset(). Exit is handled by the same return value, so
 * wiring exit_game as well would just add a second, redundant path. */
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
/* Resume intro chime, pushed from core 1 into the I2S ring (dmg_boot_intro.c). */
static int  snes_audio_free(void) { return EXT_AUDIO_GET_FREE(); }
static void snes_audio_push(int16_t l, int16_t r) { EXT_AUDIO_ENQUEUE_SAMPLE(l, r); }
static void snes_audio_restore(void) {
    /* After the resume intro: ring empty, DMA idle, emulator's rate. */
    audio_i2s_flush();
    audio_i2s_update_pio_frequency(SNES_AUDIO_RATE);
}
#endif

/* Battery SRAM to the SD before a standby (dmg_power_off_with_resume). */
static void snes_flush_saves(void) { system_save_sram(selectedRom); }

const dmg_emulator_desc_t s_snes_desc = {
    .app_title = APP_TITLE,
    .default_rom_dir = ROMS_DIR,
    .candidate_dirs = s_snes_candidate_dirs,
    .config_path = CONFIG_PATH,
    .config_fallback = CONFIG_PATH_FALLBACK,
    .saves_dir = SAVES_DIR,
    .config_magic = CONFIG_MAGIC,
    /* The Snes9x engine works internally in RGB555 (see PIXEL_FORMAT in
     * CMakeLists.txt) but the shared menu palette is selected by flag, not by
     * what the frame buffer happens to hold. This emulator's panel is RGB565,
     * like every other board in the project. */
    .is_rgb555 = false,
    .display = {
        .fill_rect = ili9341_fill_rect,
        .draw_text = ili9341_text,
        .flush_rows = ili9341_flush_rows,
        .fill_screen = ili9341_fill_screen,
        .flush = ili9341_flush,
        .stop_blitter = ili9341_blitter_stop,
        .start_blitter = ili9341_blitter_start,
        .write_rect_565 = ili9341_write_rect_565,
    },
    .is_rom_file = snes_is_rom_file,
    .on_load_rom = snes_on_load_rom,
    .save_state = snes_save_state,
    .load_state = snes_load_state,
    .apply_scale = snes_apply_scale,
    .apply_effect = snes_apply_effect,
    .apply_interleave = snes_apply_interleave,
    .apply_speed = snes_apply_speed,
    .apply_volume = snes_apply_volume,
    .apply_brightness = snes_apply_brightness,
    .reset_game = NULL,
    .exit_game = NULL,
    .custom_submenu_title = "Image >",
    .custom_submenu = snes_image_submenu,
    .loading_status = s_loading_status,
    .save_config = snes_save_config,
    .get_fps = snes_get_fps,
    .apply_fps = snes_apply_fps,
    /* NULL: the generic probe looks for <saves_dir>/<game>_<slot>.ss, the one
     * path snes_load_state() opens. */
    .state_exists = NULL,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_free           = snes_audio_free,
    .audio_push           = snes_audio_push,
#endif
    .flush_saves          = snes_flush_saves,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_restore        = snes_audio_restore
#endif
};

/* =========================================================================
 * Image Submenu (Scale / Effect / Scan Mode)
 *
 * The shared menu owns the pause menu and hands off to this for the three
 * settings that are specific to how Snes9x is fed to the display. The rest --
 * bars, palette, selection colour, button repeat, the red selection bar and its
 * white-on-red text -- comes from shared_menu's drawing helpers, so this screen
 * cannot drift from the other emulators' equivalents.
 * ========================================================================= */
static void snes_text(const dmg_emulator_desc_t *desc, const char *s, uint16_t y,
                      uint16_t fg, uint16_t bg) {
    (void)desc;
    uint16_t x = (uint16_t)(dmg_is_gb_mode() ? (DMG_GB_WINDOW_X + 12) : 12);
    ili9341_text(s, x, y, fg, bg);
}

static void snes_image_row(const dmg_emulator_desc_t *desc, uint8_t idx, bool selected) {
    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);
    const uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + idx * DMG_UI_ITEM_HEIGHT);
    const uint16_t bg = selected ? pal.sel : pal.bg;
    /* Text on the selection bar is its own colour, not pal.text: the bar is
     * #BF0000, against which black is a 3.2:1 contrast ratio and white 6.5:1. */
    const uint16_t fg = selected ? pal.text_sel : pal.text;

    ili9341_fill_rect(0, y, DMG_UI_SCREEN_WIDTH, DMG_UI_ITEM_HEIGHT, bg);

    char buf[48];
    buf[0] = '\0';
    const char *scale_names[3] = { "< Fit >", "< Stretch >", "< Integer >" };
    const char *fs_names[3] = { "< Off >", "< Auto >", "< On >" };
    switch (idx) {
        case 0: snprintf(buf, sizeof(buf), "Scale:        %s",
                         scale_names[persistent_scale_mode & 3]); break;
        case 1: snprintf(buf, sizeof(buf), "Effect:       %s",
                         persistent_scanlines_enabled ? "< Scanlines >" : "< Normal >"); break;
        case 2: snprintf(buf, sizeof(buf), "Scan Mode:    %s",
                         persistent_interleave ? "< Interleave >" : "< Progressive >"); break;
        case 3: snprintf(buf, sizeof(buf), "Frameskip:    %s",
                         fs_names[persistent_frameskip_mode % 3]); break;
        default: break;
    }
    snes_text(desc, buf, (uint16_t)(y + 7u), fg, bg);
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void snes_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    uint8_t cursor = 0;
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;

    dmg_palette_t pal;
    dmg_get_palette(desc, &pal);

    desc->display.fill_screen(pal.bg);
    dmg_draw_top_bar(desc, APP_TITLE, "Image Settings");
    dmg_draw_game_bar(desc, game_name);
    dmg_draw_bottom_bar(desc, "L/R: Adjust  A: Change  B: Back", NULL);

    for (uint8_t i = 0; i < 4; i++) snes_image_row(desc, i, i == 0);
    dmg_draw_status(desc, "", pal.text);

    dmg_wait_for_all_buttons_released();

    while (true) {
        bool up, down, left, right, a, b, select, start;
        dmg_read_ui_buttons(&up, &down, &left, &right, &a, &b, &select, &start);
        (void)select; (void)start;

        if (b && !prev_b) {
            /* Commit and apply.
             *
             * The scale change is applied to the driver here, not deferred:
             * unlike the PicoGBC case (root AGENTS.md sec. 28) this driver's
             * scale setter does not repaint the panel, so there is nothing to
             * defer and no black-screen window to guard. */
            snes_apply_scale((dmg_scale_mode_t)persistent_scale_mode);
            snes_apply_effect(persistent_scanlines_enabled);
            snes_apply_interleave(persistent_interleave);
            dmg_config_commit(desc);
            break;
        }

        if (up && !prev_up && cursor > 0) {
            snes_image_row(desc, cursor, false);
            cursor--;
            snes_image_row(desc, cursor, true);
        }
        if (down && !prev_down && cursor < 3) {
            snes_image_row(desc, cursor, false);
            cursor++;
            snes_image_row(desc, cursor, true);
        }

        const bool change = (a && !prev_a) || (left && !prev_left) || (right && !prev_right);
        if (change) {
            if (cursor == 0) {
                if (left && !prev_left)
                    persistent_scale_mode = (persistent_scale_mode == SCALE_FIT)
                        ? SCALE_INTEGER : (ili9341_scale_mode_t)(persistent_scale_mode - 1);
                else if (right && !prev_right)
                    persistent_scale_mode = (persistent_scale_mode == SCALE_INTEGER)
                        ? SCALE_FIT : (ili9341_scale_mode_t)(persistent_scale_mode + 1);
                else
                    persistent_scale_mode = (persistent_scale_mode >= SCALE_INTEGER)
                        ? SCALE_FIT
                        : (ili9341_scale_mode_t)(persistent_scale_mode + 1);
            } else if (cursor == 1) {
                persistent_scanlines_enabled = !persistent_scanlines_enabled;
            } else if (cursor == 2) {
                persistent_interleave = !persistent_interleave;
            } else if (cursor == 3) {
                if (left && !prev_left)
                    persistent_frameskip_mode = (persistent_frameskip_mode == 0) ? 2 : (persistent_frameskip_mode - 1);
                else
                    persistent_frameskip_mode = (persistent_frameskip_mode >= 2) ? 0 : (persistent_frameskip_mode + 1);
            }
            snes_image_row(desc, cursor, true);
        }

        prev_up = up; prev_down = down; prev_left = left; prev_right = right;
        prev_a = a; prev_b = b;
        sleep_ms(20);
    }
    /* The loop only leaves through the B branch above (A is captured by the
     * shared layer, which returns to the parent menu without reaching here), and
     * that branch commits. This is belt-and-braces for any future exit path: the
     * settings changed inside this submenu are in persistent_* only until
     * dmg_config_commit runs, so leaving without it loses them and the next boot
     * comes back on the previous session's values. */
    snes_apply_scale((dmg_scale_mode_t)persistent_scale_mode);
    snes_apply_effect(persistent_scanlines_enabled);
    snes_apply_interleave(persistent_interleave);
    dmg_config_commit(desc);
    dmg_wait_for_all_buttons_released();
}

/* =========================================================================
 * ROM Browser
 * ========================================================================= */
static bool rom_file_selector(char *out_selected_rom, size_t max_len) {
    /* Returns false when the player backs out (return to bootloader), true with
     * a bare filename when a ROM is chosen. out_selected_rom must be FF_MAX_LFN
     * bytes: the selector writes a bare filename, and sizeof() on the pointer
     * would be a pointer size. */
    return dmg_rom_selector(&s_snes_desc, out_selected_rom, max_len);
}

/* =========================================================================
 * Safe flash programming
 *
 * The RP2350 runs this firmware at 360 MHz. The flash XIP controller's clock
 * divider must be at least 4 at that speed (90 MHz), and the bootrom routines
 * that erase and program the chip reset that divider to 2 (180 MHz) when they
 * finish. Re-enabling interrupts and returning to code that executes from flash
 * with the divider at 2 corrupts memory reads immediately -- HardFault, reboot,
 * or a stall on the loading screen.
 *
 * These wrappers run from RAM, disable interrupts across the whole operation,
 * and re-apply the safe timing before interrupts come back. Identical to the
 * fix the other flash-backed emulators in this project needed (root AGENTS.md
 * sec. 9.3); the symptom there was a red screen or a silent return to the ROM
 * list on the second game.
 * ========================================================================= */
#if PICO_RP2350
static void __no_inline_not_in_flash_func(apply_safe_qmi_timing)(void) {
    const uint32_t max_flash_khz = 94500;
    uint32_t flash_div = (CPUFreqKHz + max_flash_khz - 1) / max_flash_khz;
    if (flash_div < 4) flash_div = 4;
    uint32_t rxdelay = (3 * CPUFreqKHz + 189000) / 378000;
    if (rxdelay > 7) rxdelay = 7;
    uint32_t ints = save_and_disable_interrupts();
    qmi_hw->m[0].timing = 0x60000000u | (14u << 11) | (rxdelay << 8) | flash_div;
    __asm volatile("nop\nnop\nnop\nnop");
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(safe_flash_range_erase)(uint32_t offset, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, count);
    apply_safe_qmi_timing();
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(safe_flash_range_program)(uint32_t offset, const uint8_t *data, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, data, count);
    apply_safe_qmi_timing();
    restore_interrupts(ints);
}
#else
static inline void apply_safe_qmi_timing(void) {}
static inline void safe_flash_range_erase(uint32_t offset, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, count);
    restore_interrupts(ints);
}
static inline void safe_flash_range_program(uint32_t offset, const uint8_t *data, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, data, count);
    restore_interrupts(ints);
}
#endif

/* =========================================================================
 * ROM Loading
 *
 * Two destinations, chosen by what the board has:
 *
 *   PSRAM present -> the cartridge is read straight from the SD card into PSRAM
 *                    and Memory.ROM points at it. Nothing is programmed, so a
 *                    4 MB ROM costs one SD read instead of an erase plus
 *                    4 MB of QSPI programming -- which is what used to run past
 *                    the 6 s hardware watchdog on the larger carts.
 *
 *   No PSRAM      -> the existing XIP flash window at SNES_ROM_CACHE_ABS, which
 *                    costs 0 bytes of SRAM and keeps working on the RP2350 Plus.
 *
 * s_rom_ptr is the single place either path publishes its result, so run_game()
 * never has to know which one ran.
 * ========================================================================= */
#ifndef ENABLE_SD_DIAGNOSTICS
#define ENABLE_SD_DIAGNOSTICS 0
#endif

static void snes_diag(const char *fmt, ...);   /* defined with the rest of the diagnostics below */
static void snes_diag_begin(void);

static uint8_t *s_rom_ptr = (uint8_t *)SNES_ROM_CACHE_ABS;
static bool s_rom_in_psram = false;

#if PICO_RP2350 && PSRAM_CS_PIN
/* PSRAM_CS_PIN is 0 on the RP2350 Plus, which compiles this whole path out:
 * a 0 chip select is indistinguishable from "this board has none". */
static bool snes_load_rom_to_psram(const char *filename, uint32_t rom_size) {
    if (rom_size == 0) {
        printf("[E] Refusing to load a zero-byte ROM into PSRAM\n");
        return false;
    }

    /* f_malloc() panics on failure, so the size has to be checked up front
     * rather than discovered by the allocator. The 512 KB margin covers the
     * menu's own allocations plus the Snes9x working set. */
    const uint32_t margin = 512u * 1024u;
    const uint32_t avail = (uint32_t)Frens::GetAvailableMemory();
    if (avail > margin && rom_size > (avail - margin)) {
        printf("[E] ROM needs %u bytes, only %u usable in PSRAM\n",
               (unsigned)rom_size, (unsigned)(avail - margin));
        return false;
    }

    /* Release the previous cartridge first, otherwise switching between games
     * leaks each ROM whole until the allocator is starved. */
    if (s_rom_in_psram && s_rom_ptr != NULL && s_rom_ptr != (uint8_t *)SNES_ROM_CACHE_ABS) {
        printf("[I] Freeing previous PSRAM ROM at %p\n", (void *)s_rom_ptr);
        Frens::f_free(s_rom_ptr);
        s_rom_ptr = (uint8_t *)SNES_ROM_CACHE_ABS;
        s_rom_in_psram = false;
    }

    uint8_t *buf = (uint8_t *)Frens::f_malloc(rom_size);
    if (buf == NULL) {
        printf("[E] PSRAM allocation of %u bytes failed\n", (unsigned)rom_size);
        return false;
    }

    /* Published before the read: a failed load still has to leave ROM_FILE_ADDR
     * style state recognisable, and LoadROM() keys off s_rom_ptr. */
    s_rom_ptr = buf;
    s_rom_in_psram = true;

    dmg_draw_loading_screen(&s_snes_desc, APP_TITLE, filename, rom_size);

    static const uint32_t PSRAM_READ_CHUNK = 16u * 1024u;
    uint32_t bytes_loaded = 0;
    bool ok = true;

    /* Own handle, opened here. The caller (load_cart_rom_file) closed s_io_fil
     * before handing off, so this function reading s_io_fil was reading a closed
     * FIL -- FatFS answers FR_OK and copies nothing, which is a silent no-op that
     * leaves the freshly-malloc'd PSRAM buffer uninitialised. That is precisely
     * what produced magic=ffff / vec=ffffffff: the core ran on PSRAM garbage.
     *
     * (Same signature, and the same buffer, so the readback hash on the next
     * lines was comparing uninitialised memory against the file and calling it
     * a mismatch.)
     *
     * Read in 16 KB blocks into a SRAM staging buffer and then copy into PSRAM,
     * rather than f_read straight into the PSRAM window: the SDRAM is reached
     * through the XIP aperture, and the FatFS read path here uses DMA, which
     * cannot target that window. A 16 KB static is affordable now that the core's
     * static working set moved to port_alloc (SRAM usage fell from 512 KB to
     * ~61 KB). */
    uint8_t *stage = (uint8_t *)malloc(PSRAM_READ_CHUNK);
    if (!stage) {
        printf("[E] Failed to allocate staging buffer for ROM load\n");
        Frens::f_free(buf);
        s_rom_ptr = (uint8_t *)SNES_ROM_CACHE_ABS;
        s_rom_in_psram = false;
        return false;
    }

    FIL pf;
    if (f_open(&pf, filename, FA_READ) != FR_OK) {
        printf("[E] Failed to open ROM for PSRAM load: %s\n", filename);
        free(stage);
        Frens::f_free(buf);
        s_rom_ptr = (uint8_t *)SNES_ROM_CACHE_ABS;
        s_rom_in_psram = false;
        return false;
    }
    f_lseek(&pf, 0);

    while (bytes_loaded < rom_size) {
        watchdog_update();
        uint32_t want = rom_size - bytes_loaded;
        if (want > PSRAM_READ_CHUNK) want = PSRAM_READ_CHUNK;
        UINT br = 0;
        FRESULT fr = f_read(&pf, stage, want, &br);
        if (fr != FR_OK || br == 0) {
            printf("[E] f_read failed at %u/%u: %d\n",
                   (unsigned)bytes_loaded, (unsigned)rom_size, (int)fr);
            ok = false;
            break;
        }
        memcpy(buf + bytes_loaded, stage, br);
        /* A short read ends the loop rather than leaving the tail of the buffer
         * uninitialised, which the core would then read as ROM. */
        bytes_loaded += br;
        dmg_update_loading_progress(&s_snes_desc, bytes_loaded, rom_size);
    }
    f_close(&pf);
    free(stage);

    if (!ok) {
        Frens::f_free(buf);
        s_rom_ptr = (uint8_t *)SNES_ROM_CACHE_ABS;
        s_rom_in_psram = false;
        return false;
    }

    last_flashed_rom_size = rom_size;
    printf("[I] SNES ROM ready in PSRAM at %p: %u bytes\n",
           (void *)s_rom_ptr, (unsigned)rom_size);
    return true;
}
#endif /* PICO_RP2350 && PSRAM_CS_PIN */

/* Program the cartridge into the XIP flash window. See the safe-flash note
 * above for why this is not a plain flash_range_* call. */
static bool snes_load_rom_to_flash(const char *filename, uint32_t size, uint32_t header_offset) {
    /* Own handle. The caller closed s_io_fil before handing off, so using it
     * here would read from a closed FIL -- which FatFS reports as FR_OK while
     * leaving the destination untouched. */
    FIL ff;
    if (f_open(&ff, filename, FA_READ) != FR_OK) {
        printf("[E] Failed to open ROM for flash load: %s\n", filename);
        return false;
    }
    if (header_offset != 0) f_lseek(&ff, header_offset);
    /* Fast probe: if the ROM already in flash matches, there is nothing to do. */
    bool already_flashed = false;
    static uint8_t probe_buf[256];
    UINT got = 0;
    f_lseek(&ff, header_offset);
    if (f_read(&ff, probe_buf, sizeof(probe_buf), &got) == FR_OK && got == sizeof(probe_buf)) {
        already_flashed = (memcmp((const void *)SNES_ROM_CACHE_ABS, probe_buf, sizeof(probe_buf)) == 0);
    }
    f_lseek(&ff, header_offset);

    if (already_flashed) {
        printf("[I] ROM already present in Flash Cache (%u bytes)\n", (unsigned)size);
        last_flashed_rom_size = size;
        last_flashed_rom_size = size;
        return true;
    }

    printf("[I] Flashing %s (%u bytes) to 0x%08X (offset 0x%08X)...\n",
           filename, (unsigned)size, (unsigned)SNES_ROM_CACHE_ABS, (unsigned)SNES_ROM_CACHE_OFF);

    dmg_draw_loading_screen(&s_snes_desc, APP_TITLE, filename, size);
    dmg_boot_intro_flash_begin();

    uint8_t *copy_block = (uint8_t *)malloc(4096);
    if (!copy_block) {
        printf("[E] Failed to allocate copy_block for flash load\n");
        f_close(&ff);
        return false;
    }
    uint32_t offset = 0;

    while (offset < size) {
        watchdog_update();
        uint32_t chunk = (size - offset > 4096u) ? 4096u : (size - offset);
        UINT read = 0;
        if (f_read(&ff, copy_block, chunk, &read) != FR_OK || read != chunk) {
            printf("[E] SD read error at offset %u\n", (unsigned)offset);
            free(copy_block);
            f_close(&ff);
            return false;
        }

        const uint32_t programmed = (chunk + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
        if (programmed > chunk) memset(copy_block + chunk, 0xFF, programmed - chunk);

        /* Erase 64 KB flash blocks as we cross boundaries */
        if ((offset % FLASH_BLOCK_SIZE) == 0u) {
            safe_flash_range_erase(SNES_ROM_CACHE_OFF + offset, FLASH_BLOCK_SIZE);
        }

        for (uint32_t pos = 0; pos < programmed; pos += FLASH_PAGE_SIZE) {
            safe_flash_range_program(SNES_ROM_CACHE_OFF + offset + pos, copy_block + pos, FLASH_PAGE_SIZE);
        }

        if (memcmp((const void *)(SNES_ROM_CACHE_ABS + offset), copy_block, chunk) != 0) {
            printf("[E] Flash verification failed at offset %u!\n", (unsigned)offset);
            free(copy_block);
            f_close(&ff);
            return false;
        }

        offset += chunk;
        dmg_update_loading_progress(&s_snes_desc, offset, size);
    }
    free(copy_block);
    f_close(&ff);

    last_flashed_rom_size = size;
    printf("[I] Flash caching completed successfully\n");
    return true;
}

/* Does a copier header sit in front of this file?
 *
 * The check this replaces was `(fsz & 0x1FF) == 512`, which is dead code:
 * `x & 0x1FF` can only ever be 0..511, so the comparison is never true. A file
 * with a real 512-byte SMC/FIG header therefore had its header silently kept,
 * every bank was mapped 512 bytes out of step, and the game rendered garbage --
 * which is what this was written to fix.
 *
 * Decided from the data instead of the file size. A SNES cartridge header ends
 * with two complement-check pairs: bytes 0x7FDC/0x7FDD must sum to 0xFF, and so
 * must 0x7FDE/0x7FDF. LoROM headers sit at 0x7FB0, HiROM at 0xFFB0. If those
 * hold at offset 0 the file is raw; if they only hold at offset 512, the first
 * 512 bytes are a copier header and have to go.
 *
 * File size cannot decide it: a 1 MB ROM is a multiple of 512 with no header at
 * all, so any size-based test gets one case or the other wrong. */
/* Do the two complement-check pairs at `base + hdr + off` both sum to 0xFF?
 *
 * Reads 4 bytes at a time rather than buffering the header: the RAM budget has
 * about 12 KB of headroom, and a 64 KB probe buffer does not fit in it. */
static bool load_cart_rom_file(const char *filename) {
    /* The browser returns a bare filename, so the active directory has to be
     * joined on here. current_rom_dir is captured by snes_on_load_rom(). */
    const char *dir = current_rom_dir;
    if (dir == NULL || dir[0] == '\0') dir = dmg_get_current_rom_dir();
    if (filename[0] == '/' || (filename[0] != '\0' && filename[1] == ':')) {
        snprintf(s_full_path, sizeof(s_full_path), "%s", filename);
    } else {
        snprintf(s_full_path, sizeof(s_full_path), "%s/%s", dir, filename);
    }
    s_full_path[sizeof(s_full_path) - 1] = '\0';

    if (f_open(&s_io_fil, s_full_path, FA_READ) != FR_OK) {
        printf("[E] Failed to open ROM: %s\n", s_full_path);
        return false;
    }

    FSIZE_t fsz = f_size(&s_io_fil);
    if (fsz < 0x8000 || fsz > SNES_ROM_CACHE_MAX) {
        printf("[E] Invalid SNES ROM file size (%u bytes)\n", (unsigned)fsz);
        f_close(&s_io_fil);
        return false;
    }

    /* The whole file, byte 0 onward, no header stripping. This is the upstream
     * contract and I had it wrong.
     *
     * I had built snes_detect_header_offset() to remove a 512-byte copier header
     * before handing the image to the core, on the reasoning that a kept header
     * shifts every bank. But that reasoning is backwards for THIS core: the
     * copier header is what makes the header land where snes9x's own InitROM()
     * looks for it, and Memmap()/InitROM() already handles a leading header --
     * it is the standard .smc layout, and it is what every upstream user feeds it.
     *
     * Upstream passes f_size() verbatim and Memory.ROM_Offset = 0. So do we.
     *
     * The evidence that this was the defect is in the card log, not in theory:
     *   magic=ffff vec=ffffffff   <- reading the SNES header at 0x7FC0 of an
     *                                  image whose byte 0 was already the copier
     *                                  header, i.e. half a kilobyte out of step
     *   vram_nz=4 (constant)       <- a 65816 executing garbage writes no tiles
     *
     * Stripping is what produced both, on every game, in both the old core and
     * the upstream one. The old "512-byte header test" also had a real bug
     * (x & 0x1FF can never equal 512), but fixing it was treating the symptom:
     * with the whole-file contract there is no header offset here at all. */
    const uint32_t size = (uint32_t)fsz;
    snes_diag("[rom] file_size=%u rom_size=%u (whole file, no strip)\n",
              (unsigned)fsz, (unsigned)size);

    bool ok;
#if PICO_RP2350 && PSRAM_CS_PIN && SNES_LOAD_ROM_IN_PSRAM
    if (Frens::isPsramEnabled()) {
        snprintf(s_loading_status, sizeof(s_loading_status), "Loading into PSRAM...");
        ok = snes_load_rom_to_psram(s_full_path, size);
    } else {
        snprintf(s_loading_status, sizeof(s_loading_status), "Writing flash cache...");
        ok = snes_load_rom_to_flash(s_full_path, size, 0);
    }
#else
    snprintf(s_loading_status, sizeof(s_loading_status), "Writing flash cache...");
    ok = snes_load_rom_to_flash(s_full_path, size, 0);
#endif
    /* Closed here, and both loaders open their own handle -- see the note in
     * snes_load_rom_to_psram(). This handle was previously still open when the
     * loader ran, which meant the loader's reads went through a shared FIL whose
     * position the loader never controlled. */
    f_close(&s_io_fil);

    if (ok) {
        s_rom_ptr = s_rom_in_psram ? s_rom_ptr : (uint8_t *)SNES_ROM_CACHE_ABS;
        const uint8_t *r = (const uint8_t *)s_rom_ptr;
        snes_diag("[rom] name=%s size=%u src=%s ptr=%08x\n",
                  filename, (unsigned)last_flashed_rom_size,
                  s_rom_in_psram ? "PSRAM" : "FLASH",
                  (unsigned)(uintptr_t)s_rom_ptr);
        /* Header magic and reset vector. 0000/0000 here means the bytes never
         * arrived; anything else means they arrived but are wrong. */
        snes_diag("[rom] magic=%02x%02x vec=%02x%02x%02x%02x byte0=%02x\n",
                  r[0x7FC2], r[0x7FC3], r[0x7FC0], r[0x7FC1],
                  r[0x7FC4], r[0x7FC5], r[0]);

        /* Readback: hash the first 64 bytes of the image as it now sits in PSRAM,
         * and the same 64 bytes straight from the SD file, offset 0.
         *
         * Its own FIL, because s_io_fil is closed by now. Reading the closed
         * handle returned FR_OK and left the destination as uninitialised stack,
         * so the "file" side hashed garbage that changed between boots -- the
         * same Aladdin image reported 88202696 on one run and 15f638de on the
         * next -- and this check reported its own bug as a ROM corruption for
         * two rounds. */
        {
            uint8_t back[64], orig[64];
            uint32_t h_back = 2166136261u, h_orig = 2166136261u;
            memset(back, 0, sizeof(back));
            memset(orig, 0, sizeof(orig));

            memcpy(back, s_rom_ptr, sizeof(back));

            FIL rf;
            uint32_t from_file = 0;
            if (f_open(&rf, s_full_path, FA_READ) == FR_OK) {
                UINT got = 0;
                if (f_read(&rf, orig, sizeof(orig), &got) == FR_OK)
                    from_file = got;
                f_close(&rf);
            }

            for (size_t i = 0; i < sizeof(back); i++) {
                h_back = (h_back ^ back[i]) * 16777619u;
                h_orig = (h_orig ^ orig[i]) * 16777619u;
            }
            snes_diag("[rom] readback got=%u psram=%08lx file=%08lx %s\n",
                      (unsigned)from_file,
                      (unsigned long)h_back, (unsigned long)h_orig,
                      (from_file == sizeof(orig) && h_back == h_orig)
                          ? "MATCH" : "*** MISMATCH ***");
        }

        /* Where the core put things -- but only AFTER LoadROM(), because that is
         * when the buffers exist. This used to sit here, before the ROM was
         * loaded, and therefore printed five zeros and sram_size=0 on every boot
         * while looking like a real measurement. A diagnostic that cannot fail is
         * worse than none: it is the reason the memory layout was never compared
         * against the upstream. Moved down next to [loadrom]. */

    } else {
        snes_diag("[rom] FAILED name=%s size=%u\n", filename,
                  (unsigned)last_flashed_rom_size);
    }
    return ok;
}

/* =========================================================================
 * Joypad Input Update
 *
 * Reads the pad, runs the shared combo layer, and forwards whatever survives to
 * the emulated SNES.
 *
 * The combo layer writes the pad back masked, so a button consumed by a combo
 * (SELECT+START, START+A, SELECT+A, SELECT+UP/DOWN) is cleared before the mask
 * is built. That is what stops a combo from reaching the game as a stray press
 * -- SELECT+START in particular cannot become 60 frames of Start held down while
 * the 1 s hold is counting.
 * ========================================================================= */

/* Read by port_glue.cpp's S9xReadJoypad(). The core calls that from inside
 * S9xMainLoop, so this has to be the one place the eight physical buttons are
 * assembled -- keeping the mapping here rather than in the glue is what keeps
 * the SNES-face-button assignment (console A -> SNES B, console B -> SNES Y) in
 * one spot instead of split across two files that could disagree. */
extern "C" uint32_t s_current_pad_mask = 0;

static void update_snes_input(void) {
    dmg_pad_t pad;
    dmg_read_ui_buttons(&pad.up, &pad.down, &pad.left, &pad.right,
                        &pad.a, &pad.b, &pad.select, &pad.start);

    dmg_combo_events_t ev;
    dmg_combo_frame(&pad, &s_snes_desc, &ev);

    /* START + B opens the pause menu, and the shared layer is what decides that.
     *
     * This used to be detected here instead -- `if (pad.start && pad.b)` -- and
     * it could never fire. dmg_combo_frame() already handles START+B as combo
     * priority 2 and, having consumed it, zeroes the buttons it used:
     *
     *     if (pad->start && pad->b) {
     *         if (!s_last_sta_b && ev) ev->request_pause_menu = true;
     *         s_last_sta_b = true;
     *         pad->a = pad->b = pad->select = pad->start = false;
     *     }
     *
     * So by the time control returned, pad.b and pad.start were both false and
     * the local test was dead code. s_request_pause_menu was therefore only ever
     * cleared, never set, and START+B did nothing at all. The event flag was
     * right there the whole time and was being discarded.
     *
     * Using ev->request_pause_menu also means the edge detection is the shared
     * one, so holding both buttons cannot reopen the menu on every frame after
     * it is dismissed, and the pad arrives already masked -- the START+B press
     * never reaches the game as a stray Start. */
    if (ev.request_pause_menu) {
        s_request_pause_menu = true;
        s_current_pad_mask = 0;
        return;
    }

    /* START+A is already fully handled inside dmg_combo_frame(): it reads
     * get_fps(), applies the inverted value through apply_fps() and persists it
     * with save_config() -- see dmg_combo.c step 3. Toggling again here is not
     * redundant, it is fatal: apply_fps(true) sets persistent_display_fps, and
     * `!persistent_display_fps` immediately evaluates to false, so the overlay
     * was enabled and disabled inside the same frame and the net state never
     * changed. That is why START+A appeared to do nothing.
     *
     * ev.fps_toggled is a notification only. Same shape as PicoSMS:889. */
    if (ev.fps_toggled) {
        snes_diag("[combo] START+A: FPS overlay = %s (fps=%d)\n",
                   persistent_display_fps ? "ON" : "OFF", snes_get_fps());
    }
    if (ev.request_exit_game) {
        s_request_exit_game = true;
        s_current_pad_mask = 0;
        return;
    }
    if (ev.request_power_off) {
        s_request_power_off = true;
        s_power_reason = ev.power_reason;
        s_current_pad_mask = 0;
        return;
    }

    /* Console face buttons -> SNES.
     *
     * Only the two the shell carries are wired, and the assignment is the one
     * the hardware dictates: the console's right-hand face button (GP7, "B")
     * is the SNES Y, and its left-hand one (GP6, "A") is the SNES B. So
     * B-on-the-SNES is the confirm button everywhere in the UI, which is what
     * the rest of the project's menus assume, and Y is the one next to it.
     *
     * The previous mapping here was the other way round (GP6 -> SNES A,
     * GP7 -> SNES B), which put the game face buttons on the wrong pair
     * relative to the menu hints. */
    uint32_t mask = 0;
    if (pad.up)     mask |= SNES_UP_MASK;
    if (pad.down)   mask |= SNES_DOWN_MASK;
    if (pad.left)   mask |= SNES_LEFT_MASK;
    if (pad.right)  mask |= SNES_RIGHT_MASK;
    if (pad.a)      mask |= SNES_B_MASK;   /* GP6 (console A) -> SNES B */
    if (pad.b)      mask |= SNES_Y_MASK;   /* GP7 (console B) -> SNES Y */
    if (pad.select) mask |= SNES_SELECT_MASK;
    if (pad.start)  mask |= SNES_START_MASK;

    s_current_pad_mask = mask;
}

/* =========================================================================
 * Frame diagnostics
 *
 * Written to 0:/saves/snes/diag.txt rather than to the screen or to UART: this
 * console has neither a debug port nor a screen free during gameplay, and a file
 * on the card holds far more than a line of text. Read it by taking the card out
 * and opening the file on a PC.
 *
 * The decision this exists to settle is whether the emulated PPU rasterises
 * anything at all:
 *
 *   deinit=NN    S9xDeinitUpdate calls in the last second = frames completed
 *   nz=NN/2048   non-zero pixels in the scanline buffer
 *   fb=xxxxxxxx  checksum, to tell a static black screen from a changing one
 *
 * deinit=0 means the frame loop never completes. deinit=60 with nz=0 means it
 * completes but rasterises nothing, which is a core fault. nz>0 with a black
 * screen means the fault is downstream of the core, in the display path.
 *
 * It keeps its own file handle on purpose: s_io_fil is in use by the ROM and SRAM
 * paths, and clobbering it there would corrupt whichever of them is running.
 * ========================================================================= */
#if ENABLE_SD_DIAGNOSTICS
static FIL s_diag_fil;
static bool s_diag_open = false;
#endif

static void snes_diag(const char *fmt, ...) {
#if ENABLE_SD_DIAGNOSTICS
    static char line[192];
    if (!s_diag_open) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

    UINT bw = 0;
    (void)f_write(&s_diag_fil, line, (UINT)n, &bw);
#else
    (void)fmt;
#endif
}

/* CREATE_ALWAYS, so each run starts clean instead of appending to the previous
 * one and leaving the interesting run buried. */
static void snes_diag_begin(void) {
#if ENABLE_SD_DIAGNOSTICS
    ensure_save_directories();
    if (f_open(&s_diag_fil, DIAG_PATH, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        s_diag_open = true;
    }
#endif
}

/* Counters owned by port_glue.cpp, where the strips are actually published. The
 * old fork counted inside S9xDeinitUpdate(), which this core revision does not
 * have -- it calls s9x_port_strip_copyout() straight from gfx.c. */
extern "C" uint32_t s9x_port_copyout_calls;
extern "C" uint32_t s9x_port_lines_published;
extern "C" uint32_t s9x_port_strip_px_nonzero;

/* Names the memory window a core buffer lives in. See the comment at the call
 * site: PSRAM is 0x11000000 through the XIP aperture, internal SRAM is
 * 0x20000000, and "below 0x20000000" is not a test for either. */
static const char *snes_mem_window(uintptr_t p) {
    if (p >= 0x11000000u && p < 0x12000000u) return "psram";
    if (p >= 0x20000000u && p < 0x20090000u) return "sram";
    return "neither";
}

static void snes_frame_diagnostics(void) {
#if ENABLE_SD_DIAGNOSTICS
    /* Four counts, deliberately kept apart:
     *   strip = rows the rasteriser published that had any non-zero pixel
     *   drv   = pixels the driver received, and their checksum
     *   vram  = whether the PPU's input data is non-zero at all
     *
     * strip=0, vram=0  -> the core produced nothing (this is what the old fork
     *                     did on every frame: it never wrote VRAM)
     * strip>0, drv=0    -> the line did not survive the trip to the driver
     * both>0, black     -> the driver lost it (scaling, slot queue, Core 1)
     * vram=0            -> VRAM is empty, which explains strip=0
     */
    const uint32_t lines  = ili9341_frame_lines();
    const uint32_t nz     = ili9341_frame_nonzero();
    const uint32_t sum    = ili9341_frame_checksum();
    const uint32_t strip  = s9x_port_strip_px_nonzero;
    const uint32_t calls  = s9x_port_copyout_calls;
    const uint32_t pub    = s9x_port_lines_published;

    ili9341_frame_reset();
    s9x_port_strip_px_nonzero = 0;
    s9x_port_copyout_calls    = 0;
    s9x_port_lines_published  = 0;

    /* The Core 1 -> panel counters. Read and cleared BEFORE the reset above would
     * have any effect on them (they are independent), but they must be sampled in
     * the same window as everything else, so they are taken here rather than in a
     * separate pass.
     *
     * This is the measurement the log was missing. drv_lines/drv_nz sit upstream of
     * snes_band_push() and therefore reported a healthy core while the panel stayed
     * black. c1_* is downstream of the slot queue and upstream of SPI, so:
     *
     *   c1_rows>0, c1_nz>0      -> panel was fed pixels; defect is in the SPI write
     *   c1_rows>0, c1_nz==0     -> panel was fed black; scaling/xmap in snes_band_push
     *   c1_rows==0              -> queue never filled; defect is upstream of the queue
     *   c1_row_changes==1       -> blitter painting a single row; it is not sweeping */
    uint32_t c1_rows = 0, c1_nz = 0, c1_chg = 0;
    ili9341_c1_stats(&c1_rows, &c1_nz, &c1_chg);
    ili9341_c1_stats_reset();

    uint32_t vram_nz = 0;
    snes_diag("[frame] VRAM=%08x PPU.ScreenHeight=%d ippu=%08x\n",
              (unsigned)(uintptr_t)Memory.VRAM, (int)PPU.ScreenHeight,
              (unsigned)(uintptr_t)&IPPU);
    if (Memory.VRAM) {
        for (int i = 0; i < 0x4000; i++) if (Memory.VRAM[i]) vram_nz++;
    }

    snes_diag("[frame] calls=%lu pub=%lu strip=%lu drv_lines=%lu drv_nz=%lu fb=%08lx\n",
              (unsigned long)calls, (unsigned long)pub, (unsigned long)strip,
              (unsigned long)lines, (unsigned long)nz, (unsigned long)sum);
    snes_diag("[frame] C1 rows=%lu nz=%lu row_changes=%lu   <- last stop before SPI\n",
              (unsigned long)c1_rows, (unsigned long)c1_nz, (unsigned long)c1_chg);
    snes_diag("[frame] vram_nz=%lu pitch=%d delta=%d screenH=%d\n",
              (unsigned long)vram_nz, GFX.Pitch, GFX.Delta, (int)PPU.ScreenHeight);

    /* Profile split. emu is emulation; the rest is everything else this port
     * does per frame on core 0. If "post" is large the emulation is not the
     * bottleneck and the fix is here, not in the core.
     *
     * window is the wall-clock span this sample actually covered, and fps is
     * frames/window -- not frames/second-assumed. The distinction is the whole
     * point: if window exceeds 1 s while frames stays near 60, the loop is
     * missing its deadlines and the reported rate is frames-per-window rather
     * than frames-per-second. Counting frames and dividing by a nominal second
     * hides that, which is how a 50 Hz loop gets logged as "60". */
    const uint32_t prof_emu = s_prof_emu;
    const uint32_t prof_total = s_prof_total;
    const uint32_t prof_frames = s_prof_frames;
    const uint32_t prof_diag = s_prof_diag_us;
    const uint32_t prof_now = time_us_32();
    const uint32_t window = prof_now - s_prof_window_start;
    s_prof_emu = 0;
    s_prof_total = 0;
    s_prof_frames = 0;
    s_prof_diag_us = 0;

    /* Time the write itself, then restart the window AFTER it. Both halves
     * matter: the subtraction below keeps the write out of `post` (so `post`
     * stays "real work outside the core", which is what it claims to be), and
     * restarting the window here keeps the windows contiguous in wall time
     * without the write being smeared across two of them. */
    const uint32_t diag_t0 = time_us_32();
    snes_diag("[prof] frames=%lu window=%lu us fps=%.1f | emu=%lu us post=%lu us diag=%lu us emu=%.0f%% avg_emu=%lu us\n",
              (unsigned long)prof_frames, (unsigned long)window,
              window ? (double)prof_frames * 1e6 / (double)window : 0.0,
              (unsigned long)prof_emu,
              (unsigned long)(prof_total > prof_emu ? prof_total - prof_emu : 0),
              (unsigned long)prof_diag,
              prof_total ? (double)prof_emu * 100.0 / (double)prof_total : 0.0,
              (unsigned long)(prof_frames ? prof_emu / prof_frames : 0));

    /* One commit periodically now that snes_diag() no longer commits per
     * line. Slower commit rate prevents microSD write stalls during gameplay. */
    static uint32_t s_diag_sync_count = 0;
    if (++s_diag_sync_count >= 10) {
        s_diag_sync_count = 0;
        (void)f_sync(&s_diag_fil);
    }

    s_diag_exclude_us = time_us_32() - diag_t0;
    s_prof_diag_us += s_diag_exclude_us;
    s_prof_window_start = time_us_32();
#endif
}

/* Hand the cartridge to the core.
 *
 * Ported from pico-snesPlus's snes9x_load_rom_from_psram(). The contract is
 * different from the old fork's LoadROM(void): this core reads Memory.ROM /
 * Memory.ROM_AllocSize / Memory.ROM_Offset and takes NULL as "the image is
 * already in memory, do not open a file". That is the whole point -- the old
 * LoadROM() had no such path, which is why a PSRAM cartridge used to fall into
 * a file-reading branch that memset 4.7 MB inside PSRAM and hung (agents.md
 * sec. 18-19). ROM_AllocSize doubles as the file size in this path, so it has
 * to be the real cartridge size, not an allocation ceiling.
 */
static bool snes9x_load_rom_from_cartridge(void) {
    if (!s_rom_ptr || last_flashed_rom_size < 0x8000) return false;

    Memory.ROM           = s_rom_ptr;
    Memory.ROM_AllocSize = (uint32_t)last_flashed_rom_size;
    Memory.ROM_Offset    = 0;

    if (!LoadROM(NULL)) return false;

    /* Reject the carts whose chips are declared but not implemented. Everything
     * else this core emulates for real and must be allowed through: DSP (dsp.c),
     * SuperFX (fxemu.c/fxinst.c), C4 (c4.c/c4emu.c), OBC1 (obc1.c), S-RTC
     * (srtc.c) and SA-1 (sa1.c/sa1cpu.c). S-DD1 and SPC7110 have no decoder
     * here, so those carts bail out rather than run as a base SNES.
     *
     * InitROM() is what sets these flags, so this has to be after LoadROM(). */
    if (Settings.SDD1 || Settings.SPC7110) return false;

    /* LoadROM() resets the machine; Memory.SRAM is deliberately not cleared, so
     * system_load_sram() afterwards is what restores a battery save. */
    S9xReset();
    return true;
}

/* Super FX cart detection from the raw LoROM header, before the core has parsed
 * it. Cart type byte $FFD6 (file 0x7FD6) 0x13/0x14/0x15/0x1A = GSU, with a LoROM
 * map mode byte (0x20/0x30). Checked at offset 0 and past a 512-byte copier
 * header, since the core strips that header itself later. */
#ifndef SNES_GSU_RAM_IN_SRAM
#define SNES_GSU_RAM_IN_SRAM 1
#endif
static bool snes_rom_is_superfx(void) {
    if (s_rom_ptr == NULL) return false;
    static const uint32_t bases[2] = { 0u, 512u };
    for (int i = 0; i < 2; i++) {
        const uint32_t h = bases[i] + 0x7FC0u;
        if (h + 0x20u > last_flashed_rom_size) continue;
        const uint8_t map = s_rom_ptr[h + 0x15u];
        const uint8_t type = s_rom_ptr[h + 0x16u];
        const bool gsu = (type == 0x13u || type == 0x14u || type == 0x15u || type == 0x1Au);
        if (gsu && (map & 0xEFu) == 0x20u) return true;
    }
    return false;
}

/* =========================================================================
 * Performance report: 0:/saves/snes/perf.txt
 *
 * The console has no UART, and the allocator's SRAM/PSRAM placement log only
 * goes to printf. This writes where every hot buffer landed and how much SRAM
 * heap is left once at game start, and the frame counters once at game exit.
 * The card is NEVER touched during gameplay (agents.md failure #12): the
 * counters live in RAM and are written after the loop. Reuses s_io_fil, which
 * is idle at both points (ROM/SRAM I/O has finished or not started).
 * ========================================================================= */
#ifndef ENABLE_PERF_REPORT
#define ENABLE_PERF_REPORT 1
#endif
#define PERF_PATH SAVES_DIR "/perf.txt"

#if ENABLE_PERF_REPORT
#include <malloc.h>
#include <unistd.h>
extern "C" char __HeapLimit;
#if PROFILE_BUCKETS
extern "C" uint32_t g_prof_us_sfx;
extern "C" uint32_t g_prof_sfx_runs;
extern "C" uint32_t g_prof_fx_instr;
#endif

struct snes_perf_t {
    uint32_t frames, rendered, emu_us, max_emu_us, late_frames, debt_resets;
    uint32_t emu_rendered_us, over33_frames, max_sfx_us, heap_before_core;
};
static snes_perf_t s_perf;

static void perf_printf(bool append, const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    const BYTE mode = FA_WRITE | (append ? FA_OPEN_APPEND : FA_CREATE_ALWAYS);
    if (f_open(&s_io_fil, PERF_PATH, mode) != FR_OK) return;
    UINT bw = 0;
    (void)f_write(&s_io_fil, line, (UINT)n, &bw);
    f_close(&s_io_fil);
}

static uint32_t perf_heap_free(void) {
    struct mallinfo mi = mallinfo();
    const uintptr_t top = (uintptr_t)sbrk(0);
    const uintptr_t lim = (uintptr_t)&__HeapLimit;
    return (uint32_t)mi.fordblks + (uint32_t)(lim > top ? lim - top : 0u);
}

/* Called at the top of run_game(), before the core allocates anything: the
 * difference to heap_free at game start is what the core itself took, and what
 * is already gone here belongs to the firmware (FatFs, menu, USB, ...). */
static void perf_note_heap_before_core(void) {
    s_perf.heap_before_core = perf_heap_free();
}

static void perf_report_start(void) {
    ensure_save_directories();
    const uint32_t heap_before = s_perf.heap_before_core;
    memset(&s_perf, 0, sizeof(s_perf));
    perf_printf(false, "[game] %s sfx=%d sa1=%d dsp1=%d c4=%d\n", selectedRom,
                (int)Settings.SuperFX, (int)Settings.SA1, (int)Settings.DSP1Master,
                (int)Settings.C4);
    perf_printf(true, "[mem] heap_free=%lu B (before core init: %lu B)\n",
                (unsigned long)perf_heap_free(), (unsigned long)heap_before);
    perf_printf(true, "[mem] RAM=%s VRAM=%s SRAM=%s FillRAM=%s Map=%s MapInfo=%s APU=%s\n",
                snes_mem_window((uintptr_t)Memory.RAM), snes_mem_window((uintptr_t)Memory.VRAM),
                snes_mem_window((uintptr_t)Memory.SRAM), snes_mem_window((uintptr_t)Memory.FillRAM),
                snes_mem_window((uintptr_t)Memory.Map), snes_mem_window((uintptr_t)Memory.MapInfo),
                snes_mem_window((uintptr_t)IAPU.RAM));
    perf_printf(true, "[mem] Screen=%s ZBuf=%s ScreenColors=%s TileCached=%s TileCache=%s\n",
                snes_mem_window((uintptr_t)GFX.Screen), snes_mem_window((uintptr_t)GFX.ZBuffer),
                snes_mem_window((uintptr_t)IPPU.ScreenColors),
                snes_mem_window((uintptr_t)IPPU.TileCached),
                snes_mem_window((uintptr_t)IPPU.TileCache));
#if PROFILE_BUCKETS
    g_prof_us_sfx = 0;
    g_prof_sfx_runs = 0;
    g_prof_fx_instr = 0;
#endif
    ili9341_core0_wait_reset();
}

static void perf_report_end(void) {
    const uint32_t f = s_perf.frames ? s_perf.frames : 1u;
    perf_printf(true, "[perf] frames=%lu rendered=%lu (%lu%%) late=%lu debt_resets=%lu\n",
                (unsigned long)s_perf.frames, (unsigned long)s_perf.rendered,
                (unsigned long)(s_perf.rendered * 100u / f),
                (unsigned long)s_perf.late_frames, (unsigned long)s_perf.debt_resets);
    const uint32_t r = s_perf.rendered ? s_perf.rendered : 1u;
    const uint32_t sk = (s_perf.frames > s_perf.rendered) ? (s_perf.frames - s_perf.rendered) : 0u;
    perf_printf(true, "[perf] emu avg=%lu us (rendered %lu, skipped %lu) max=%lu us over33ms=%lu\n",
                (unsigned long)(s_perf.emu_us / f),
                (unsigned long)(s_perf.emu_rendered_us / r),
                (unsigned long)(sk ? (s_perf.emu_us - s_perf.emu_rendered_us) / sk : 0u),
                (unsigned long)s_perf.max_emu_us, (unsigned long)s_perf.over33_frames);
    perf_printf(true, "[perf] core0 SPI wait avg=%lu us/frame\n",
                (unsigned long)(ili9341_core0_wait_us() / f));
#if PROFILE_BUCKETS
    perf_printf(true, "[perf] superfx avg=%lu us/frame max=%lu us/frame runs=%lu\n",
                (unsigned long)(g_prof_us_sfx / f), (unsigned long)s_perf.max_sfx_us,
                (unsigned long)g_prof_sfx_runs);
    perf_printf(true, "[perf] gsu instr=%lu total, %lu per frame, %lu ns per instr\n",
                (unsigned long)g_prof_fx_instr, (unsigned long)(g_prof_fx_instr / f),
                (unsigned long)(g_prof_fx_instr ? (uint64_t)g_prof_us_sfx * 1000u / g_prof_fx_instr : 0u));
#endif
}
#define PERF_ADD(stmt) do { stmt; } while (0)
#else
static inline void perf_report_start(void) {}
static inline void perf_report_end(void) {}
static inline void perf_note_heap_before_core(void) {}
#define PERF_ADD(stmt) do { } while (0)
#endif

/* =========================================================================
 * Main Emulation Execution
 * ========================================================================= */
static void run_game(void) {
    if (last_flashed_rom_size < 0x8000) return;
    perf_note_heap_before_core();

    /* Standby wake: the intro has to be over before anything below touches the
     * panel or the audio. ili9341_clear_black() right after this pushes a whole
     * frame over SPI0, and doing that while core 1 is still drawing the intro
     * interleaves the two command streams -- the panel took a corrupted command
     * and stayed white while the game ran blind underneath. Further down,
     * audio_i2s_update_pio_frequency() and the prefill would also race the
     * intro's chime in the I2S ring. Whether it happened depended on the ROM
     * load finishing before or after the intro, hence "sometimes". No-op on a
     * normal start. */
    dmg_boot_intro_finish();

    /* CRITICAL: Disconnect framebuffer before any memory init or clearing
     * so ili9341_clear_black() does NOT wipe out 153KB over s_wram and s_fillram! */
    ili9341_set_framebuffer(NULL);
    ili9341_clear_black();

    memset(&Settings, 0, sizeof(Settings));
    Settings.CyclesPercentage  = 100;
    Settings.H_Max             = SNES_CYCLES_PER_SCANLINE;
    Settings.HBlankStart       = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.FrameTimePAL      = 20000;
    Settings.FrameTimeNTSC     = 16667;
    Settings.ControllerOption  = SNES_JOYPAD;
    Settings.SoundPlaybackRate = SNES_AUDIO_RATE;
    Settings.SoundInputRate    = SNES_AUDIO_RATE;
    Settings.SoundBufferSize   = 1024;
    Settings.SoundMixInterval  = 0;
    /* Linear interpolation in the mixer costs ~25-30% of MixStereo. Left off
     * while there is no headroom to spare; it mostly benefits chip-music tracks
     * with high-frequency detail. */
    Settings.InterpolatedSound = false;
    Settings.DisableSoundEcho  = false;
    Settings.Mute              = false;
    Settings.APUEnabled        = true;
    Settings.Shutdown          = true;

    /* Note what is NOT here, and why it cannot be: Settings.Transparency,
     * SupportHiRes, SixteenBitSound, Stereo and a uint16_t SoundBufferSize were
     * fields of the OLD fork's Settings. This core revision has bool-typed
     * fields (SoundBufferSize is int32_t here) and no transparency or hi-res
     * knobs at all -- they were compile-time PIXEL_FORMAT/graphics decisions in
     * upstream's arrangement. Do not add them back speculatively: the four lines
     * that were removed compiled to nothing and the build was already telling us
     * the fields do not exist. */

    /* ---- Display first.
     *
     * S9xInitDisplay() is ours (port_glue.cpp) and it is what allocates the four
     * SRAM strips, sets GFX.Pitch and parks GFX.Screen on them. It has to run
     * before S9xInitGFX() because that one resolves the buffers into line
     * pointers, and both have to run before the first frame because the strips
     * are where the rasteriser writes -- there is no separate "screen" buffer to
     * wire up afterwards the way the old fork had.
     *
     * The strips come out of the libc SRAM heap first with a PSRAM fallback
     * (port_alloc_sram/port_alloc_psram). That is why PICO_MALLOC_PANIC=0 is
     * set in CMakeLists: with the default panicking malloc the fallback is
     * unreachable and an OOM kills the process instead of degrading. */
    snes_diag("[init] S9xInitDisplay\n");
    if (!S9xInitDisplay()) {
        snes_diag("[init] S9xInitDisplay FAILED (no SRAM for the gfx strips)\n");
        printf("[E] gfx strips allocation failed\n");
        return;
    }
    if (!S9xInitGFX()) {
        snes_diag("[init] S9xInitGFX FAILED\n");
        printf("[E] Failed to initialize Snes9x graphics!\n");
        S9xDeinitDisplay();
        return;
    }
    snes_diag("[init] display+gfx OK\n");

    /* APU before Memory. Both are large allocations (64 KB APU RAM, then
     * WRAM/VRAM/FillRAM); taking the big one while the libc heap is least
     * fragmented lets the smaller ones fit in the gaps. */
    snes_diag("[init] S9xInitAPU\n");
    if (!S9xInitAPU()) {
        snes_diag("[init] S9xInitAPU FAILED\n");
        S9xDeinitGFX(); S9xDeinitDisplay();
        return;
    }
    /* Super FX layout is chosen before allocation, from the cart header,
     * because LoROM()/InitROM() run only after the buffers exist. */
    s9x_port_gsu_ram_in_sram = SNES_GSU_RAM_IN_SRAM && snes_rom_is_superfx();
    snes_diag("[init] S9xInitMemory\n");
    if (!S9xInitMemory()) {
        snes_diag("[init] S9xInitMemory FAILED\n");
        printf("[E] Failed to initialize SNES memory!\n");
        S9xDeinitAPU(); S9xDeinitGFX(); S9xDeinitDisplay();
        return;
    }
    snes_diag("[init] S9xInitMemory OK\n");

    snes_diag("[init] S9xInitSound\n");
    if (!S9xInitSound(0, 0)) {
        snes_diag("[init] S9xInitSound FAILED (continuing silent)\n");
    }
    /* S9xInitSound leaves so.playback_rate at 0, and with it the mixer's
     * per-channel frequency table uninitialized -- S9xMixSamples then returns
     * pure silence. It must be set after S9xInitSound and before the reset that
     * LoadROM triggers. */
    S9xSetPlaybackRate(SNES_AUDIO_RATE);
    /* Mute lives in Settings.Mute (set above), not a separate call in this revision. */
    snes_diag("[init] sound OK\n");

    snes_diag("[loadrom] entering, rom=%u bytes src=%s ptr=%08x\n",
              (unsigned)last_flashed_rom_size,
              s_rom_in_psram ? "PSRAM" : "FLASH",
              (unsigned)(uintptr_t)s_rom_ptr);
    if (!snes9x_load_rom_from_cartridge()) {
        snes_diag("[loadrom] result=FAIL\n");
        printf("[E] Failed to load ROM (%s)\n",
               s_rom_in_psram ? "PSRAM" : "flash cache");
        sleep_ms(3000);
        S9xDeinitSound(); S9xDeinitAPU(); S9xDeinitMemory();
        S9xDeinitGFX(); S9xDeinitDisplay();
        return;
    }
    snes_diag("[loadrom] result=OK\n");

    /* Where the core actually put things. This has to run after LoadROM():
     * that is what sizes the buffers and builds Memory.Map[], so printing it
     * earlier reports the zeroed initial state rather than the layout.
     *
     * This core names the 128 KB work RAM "RAM" (the old fork called it WRAM),
     * so the label follows the struct field.
     *
     * The addresses are the interesting part, not the names. Everything the
     * rasteriser reads has to sit in the PSRAM window: the SDRAM is reached
     * through the XIP aperture, so a pointer anywhere else would be served from
     * somewhere the PPU never writes. A RAM pointer below 0x20000000 here is the
     * signature of a build that reverted to internal SRAM for the hot set. */
    snes_diag("[mem] ROM=%08x RAM=%08x VRAM=%08x FillRAM=%08x SRAM=%08x sram_size=%u\n",
              (unsigned)(uintptr_t)Memory.ROM,
              (unsigned)(uintptr_t)Memory.RAM,
              (unsigned)(uintptr_t)Memory.VRAM,
              (unsigned)(uintptr_t)Memory.FillRAM,
              (unsigned)(uintptr_t)Memory.SRAM,
              (unsigned)Memory.SRAMSize);
    snes_diag("[mem] HiROM=%d LoROM=%d ROMSize=%d type=%d maps=%d\n",
              (int)Memory.HiROM, (int)Memory.LoROM, (int)Memory.ROMSize,
              (int)Memory.ROMType, MEMMAP_NUM_BLOCKS);
    /* Which window each hot buffer actually landed in.
     *
     * The distinction matters and is easy to get wrong: PSRAM on the RP2350 is
     * reached through the XIP aperture at 0x11000000, and internal SRAM is
     * 0x20000000..0x20081FFF. Testing "below 0x20000000" as a proxy for SRAM
     * therefore classifies every PSRAM buffer as SRAM, which is what an earlier
     * revision of this line did -- it printed RAM=1 VRAM=1 FillRAM=1 for three
     * addresses that were plainly 0x1114xxxx, and read as an alarming finding
     * when the layout was in fact exactly right.
     *
     * Both windows are named explicitly, and anything outside both is reported
     * as neither rather than guessed into one of them. */
    snes_diag("[mem] windows: RAM=%s VRAM=%s FillRAM=%s (psram|sram|neither)\n",
              snes_mem_window((uintptr_t)Memory.RAM),
              snes_mem_window((uintptr_t)Memory.VRAM),
              snes_mem_window((uintptr_t)Memory.FillRAM));

    /* After LoadROM's S9xReset, which does not clear Memory.SRAM -- so loading
     * the battery save now is what makes an in-game Reset preserve progress. */
    system_load_sram(selectedRom);

#if ENABLE_MSU1
    /* Look for <romname>.msu / <romname>-1.pcm next to the cartridge. With no
     * pack present this allocates nothing, opens nothing and touches the card
     * not at all -- a non-MSU1 ROM behaves exactly as it did before. */
    if (msu1_init()) {
        snes_diag("[msu1] pack detected, streaming enabled\n");
    }
#endif


    /* The config file is the source of truth for how the game is displayed, so
     * it is read when a game is entered and not only at boot. */
    snes_load_config();

    /* 256x240, not 256x224. The panel is 240 rows tall and GB_MODE's window is
     * 266x240, so declaring 240 lets the picture and the window occupy the same
     * rows exactly. 224 here would make the driver centre a 224-row image inside
     * 240 (offset 8) while the window expects full height, and the two would
     * disagree about where the picture belongs. */
    ili9341_md_set_geometry(256, 240);
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_scanlines(persistent_scanlines_enabled);
    ili9341_set_interleave(persistent_interleave);
    s9x_set_interleave(persistent_interleave);
    ili9341_set_fps_overlay(persistent_display_fps, fpsString);

    dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */
    ili9341_blitter_start();

    s_request_pause_menu = false;
    s_request_exit_game = false;
    /* Clear the combo hold counters. Without it a half-finished SELECT+START
     * from the previous game carries over, and releasing then re-pressing one
     * frame after a game loads would exit immediately -- which looks exactly
     * like the 1 s hold being ignored. */
    dmg_combo_reset();
    bool reset = false;

    /* Ensure hardware I2S PIO frequency is strictly locked to SNES native 32000 Hz */
    audio_i2s_update_pio_frequency(SNES_AUDIO_RATE);
    s_audio_frac_acc = 0;

    /* Pre-fill the I2S audio ring buffer with 2048 silent stereo samples (~64 ms).
     * This establishes a healthy 50% cushion from frame 0, preventing immediate
     * buffer starvation and popping before emulation warms up. */
    static const uint32_t s_audio_prefill[2048] = {0};
    EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_prefill, 2048);

    perf_report_start();
#if ENABLE_PERF_REPORT && PROFILE_BUCKETS
    uint32_t s_sfx_us_prev = 0;   /* g_prof_us_sfx was just zeroed by perf_report_start() */
#endif
    uint32_t fps_timer_us = time_us_32();
    /* Same instant, so the first prof sample covers one real second rather than
     * the time since the program started. */
    s_prof_window_start = fps_timer_us;
    uint32_t frame_count = 0;
    static uint32_t s_frame_deadline = 0;
    s_frame_deadline = time_us_32();

    watchdog_enable(8388, false);

    while (!s_request_exit_game) {
        watchdog_update();

        if (reset) {
            /* Reload the battery save before resetting.
             *
             * S9xReset() puts the cart back at its reset vector but does not
             * clear Memory.SRAM, so a save already in memory survives -- however
             * what is in memory is whatever the game last wrote this session, and
             * the shared menu's own AGENTS.md sec.29 records that this is the
             * distinction that matters: "Reset" has to mean the same thing as
             * picking the game from the browser, or quitting to the menu loses
             * progress that re-entering the same game would have kept.
             *
             * system_load_sram() is the same call the boot path makes, so both
             * routes land on identical SRAM. */
            S9xReset();
            system_load_sram(selectedRom);
            reset = false;
            EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_prefill, 2048);
            s_frame_deadline = time_us_32();
        }

        update_snes_input();

        /* Power switch OFF (or battery cutoff): save at this frame boundary and
         * sleep. Never returns -- the wake reboots back into the resume path. */
        if (s_request_power_off) {
            s_request_power_off = false;
            ili9341_blitter_stop();
            dmg_power_off_with_resume(&s_snes_desc, (dmg_power_reason_t)s_power_reason);
        }

        if (s_request_pause_menu) {
            s_request_pause_menu = false;
            /* Stop the LCD stream before the menu draws. The shared menu writes
             * straight to the panel from Core 0, and Core 1 is holding the SPI
             * bus with an in-flight band -- the two would interleave commands
             * and the screen would come apart. dmg_ingame_menu() commits the
             * config on the way in and on every one of its three returns, so
             * the file always matches what was on screen when it closed. */
            ili9341_blitter_stop();
            const dmg_action_t action = dmg_ingame_menu(&s_snes_desc, selectedRom);
            ili9341_clear_black();
            ili9341_blitter_start();
            if (action == DMG_ACTION_EXIT) {
                s_request_exit_game = true;
                break;
            } else if (action == DMG_ACTION_RESET) {
                reset = true;
            }
            if (s_state_load_broke) {
                s_state_load_broke = false;
                reset = true;
            }
            EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_prefill, 2048);
            s_frame_deadline = time_us_32();
        }

        /* Run 1 SNES frame with dynamic auto-frameskip / fixed frameskip */
        static bool s_skip_next_render = false;
        static bool s_skip_phase = false;
        const uint32_t t_emu0 = time_us_32();
        if (persistent_frameskip_mode == FRAMESKIP_ON) {
            /* Fixed Frameskip 1: alternating 1 rendered, 1 skipped (pico-snesPlus model) */
            s_skip_phase = !s_skip_phase;
            IPPU.RenderThisFrame = !s_skip_phase;
        } else if (persistent_frameskip_mode == FRAMESKIP_AUTO) {
            if (s_skip_next_render) {
                IPPU.RenderThisFrame = false;
                s_skip_next_render = false;
            } else {
                IPPU.RenderThisFrame = true;
            }
        } else {
            /* FRAMESKIP_OFF: Always render every frame */
            IPPU.RenderThisFrame = true;
        }
        S9xMainLoop();
        const uint32_t t_emu1 = time_us_32();
#if ENABLE_PERF_REPORT
        {
            const uint32_t d = t_emu1 - t_emu0;
            s_perf.emu_us += d;
            if (d > s_perf.max_emu_us) s_perf.max_emu_us = d;
            if (d > 33000u) s_perf.over33_frames++;
            s_perf.frames++;
            if (IPPU.RenderThisFrame) { s_perf.rendered++; s_perf.emu_rendered_us += d; }
#if PROFILE_BUCKETS
            const uint32_t sfx_frame = g_prof_us_sfx - s_sfx_us_prev;
            s_sfx_us_prev = g_prof_us_sfx;
            if (sfx_frame > s_perf.max_sfx_us) s_perf.max_sfx_us = sfx_frame;
#endif
        }
#endif
#if ENABLE_SD_DIAGNOSTICS
        s_prof_emu += (t_emu1 - t_emu0);
#endif

        /* Audio for one emulated frame at 32000 Hz: ~532.44 stereo frames = ~1066 int16.
         * Fits in a single call (1066 <= 1320 S9X_MIX_MAX_SAMPLES), avoiding chunking loops. */
        const int stereo_frames = snes_samples_this_frame();
        const int samples_to_mix = stereo_frames * 2;
        S9xMixSamples((signed short*)s_audio_buffer, samples_to_mix);
#if ENABLE_MSU1
        /* Sum the MSU-1 track on top of the DSP mix. It goes in here rather
         * than in msu1.c's own core1 path because this port mixes on core0:
         * the PCM ring is already warm and msu1_mix() is a plain add-and-clamp
         * over the same buffer we are about to hand the I2S driver. The DSP mix
         * is already clipped to full scale, so the sum genuinely can overflow
         * and the clamp inside msu1_mix() is load-bearing, not decorative. */
        msu1_mix((int16_t *)s_audio_buffer, samples_to_mix / 2);
        /* All SD access MSU-1 needs happens here, once per emulated frame:
         * the deferred track open and the ring refill. Deliberately not inside
         * S9xMainLoop -- the card must never be touched from the CPU
         * interpreter's hot path, and this emulates at ~176 KB/s while playing. */
        msu1_pump();
#endif
        play_snes_audio(s_audio_buffer, samples_to_mix);

        frame_count++;

        /* Standby wake: put the game back where the switch left it, now that one
         * frame has run, through the same steps the pause menu's Load State
         * takes. Loading before the loop (as before) diverged from that path:
         * the sound engine or the pacing came up wrong (no audio on the MD,
         * slow SMS/WS) until a Reset. */
        if (s_resume_pending) {
            s_resume_pending = false;
            ili9341_blitter_stop();
            const bool resumed = snes_load_state(selectedRom, DMG_RESUME_SLOT);
            printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
            ili9341_clear_black();
            ili9341_blitter_start();
            if (s_state_load_broke) {   /* half-restored core: Reset path, as the menu */
                s_state_load_broke = false;
                reset = true;
            }
            EXT_AUDIO_ENQUEUE_SAMPLES(s_audio_prefill, 2048);
            s_frame_deadline = time_us_32();
            dmg_resume_consume();
            dmg_power_arm_menu_loaded(1000);
        }

        /* FPS calculation */
        uint32_t now = time_us_32();
        if (now - fps_timer_us >= 1000000u) {
#if ENABLE_SD_DIAGNOSTICS
            snes_frame_diagnostics();
#endif
            fps = frame_count;
            frame_count = 0;
            fps_timer_us = now;
            snprintf(fpsString, sizeof(fpsString), "%lu", (unsigned long)fps);
            if (persistent_display_fps) {
                ili9341_set_fps_overlay(true, fpsString);
            }
        }

        /* Frame Pacing: 60.0988 Hz NTSC (16639 us) / 50.000 Hz PAL (20000 us).
         * Matches exact SNES APU/DSP audio generation rate: Rate / 60.0988 = 532.444 samples/frame.
         * Running at 16639 us generates exactly 32000 samples/sec to perfectly match hardware I2S DMA. */
        uint32_t frame_budget = Settings.PAL ? 20000u : 16639u;
        if (persistent_turbo_mode) frame_budget /= 2;

        /* Pacing with cumulative deadline to eliminate timer drift */
        if (s_frame_deadline == 0) s_frame_deadline = time_us_32();
        s_frame_deadline += frame_budget;

        int fill = audio_i2s_get_fill_permille();

        /* Dynamic Audio Rate Control with Hysteresis: trim the PIO I2S rate from
         * the audio ring cushion, with a deadband against clkdiv flapping.
         * Trim is kept to +/-0.5% (~9 cents, inaudible). It used to swing up to
         * 2.5% (~43 cents), which was heard as pitch wobble. With the deadline
         * pacing below keeping emulation at 100% speed, the drift it has to
         * absorb is only crystal tolerance, so a narrow trim is enough. */
        static uint32_t s_current_pio_freq = SNES_AUDIO_RATE;
        uint32_t target_freq = s_current_pio_freq;
        if (persistent_volume_level > 0) {
            if (s_current_pio_freq == SNES_AUDIO_RATE) {
                if (fill < 300) target_freq = 31840;
                else if (fill > 750) target_freq = 32160;
            } else if (s_current_pio_freq < SNES_AUDIO_RATE) {
                /* Was running slower to catch up: return to 32000 once cushion restored (> 450) */
                if (fill > 450) target_freq = SNES_AUDIO_RATE;
            } else {
                /* Was running faster to drain: return to 32000 once cushion drops (< 550) */
                if (fill < 550) target_freq = SNES_AUDIO_RATE;
            }
        } else {
            target_freq = SNES_AUDIO_RATE;
        }
        if (target_freq != s_current_pio_freq) {
            audio_i2s_update_pio_frequency(target_freq);
            s_current_pio_freq = target_freq;
        }

        /* Deadline pacing with bounded debt.
         *
         * A frame that runs over its budget leaves its overrun in s_frame_deadline,
         * so the next (lighter or frameskipped) frames pay it back and emulation
         * stays at 100% real time. The old code reset the deadline to `now` on every
         * overrun, which threw that time away: the game ran slower than real time,
         * the audio ring drained, and that is the periodic hiccup. Debt is only
         * dropped past two frame budgets, which means a real stall (SD access,
         * menu) rather than a heavy frame -- catching up from that would fast-forward. */
        int32_t late_us = 0;
        if (!persistent_turbo_mode) {
            now = time_us_32();
            const int32_t wait_us = (int32_t)(s_frame_deadline - now);
            if (wait_us > 0) {
                busy_wait_us_32((uint32_t)wait_us);
            } else {
                late_us = -wait_us;
                PERF_ADD(s_perf.late_frames++);
                if (late_us > (int32_t)(2u * frame_budget)) {
                    PERF_ADD(s_perf.debt_resets++);
                    s_frame_deadline = now;
                    late_us = 0;
                }
            }
        } else {
            s_frame_deadline = time_us_32();
        }

        /* Auto frameskip driven by the deadline itself: while we are behind by more
         * than SNES_SKIP_LATE_US, drop PPU rendering of the next frame (the core then
         * runs in a few ms and pays the debt back), up to SNES_MAX_CONSECUTIVE_SKIPS
         * in a row so the picture never freezes. Small lateness is absorbed by the
         * carried debt instead of costing a frame. The audio ring is a last-resort
         * net only. Skipping is decided from where we are against real time, not
         * from a guess at how long the next frame will take. */
        static uint8_t s_consecutive_skips = 0;
        if (persistent_frameskip_mode == FRAMESKIP_AUTO && !persistent_turbo_mode) {
            const bool behind = late_us > (int32_t)SNES_SKIP_LATE_US;
            const bool audio_low = (persistent_volume_level > 0 && fill < 150);
            if ((behind || audio_low) && s_consecutive_skips < SNES_MAX_CONSECUTIVE_SKIPS) {
                s_skip_next_render = true;
                s_consecutive_skips++;
            } else {
                s_skip_next_render = false;
                s_consecutive_skips = 0;
            }
        } else {
            s_skip_next_render = false;
            s_consecutive_skips = 0;
        }

#if ENABLE_SD_DIAGNOSTICS
        s_prof_total += (now - t_emu0);
        s_prof_frames++;
#endif
    }

    watchdog_disable();

    ili9341_blitter_stop();
    dmg_flush_global_volume();
    system_save_sram(selectedRom);
    perf_report_end();

    /* Tear the whole core down, not just Memory: the gfx strips are SRAM/PSRAM
     * allocations that the ROM selector needs back (the menu has to fit while a
     * game is not loaded), and S9xDeinitGFX/Display are what release them.
     * Order mirrors the init sequence, leaf first. */
#if ENABLE_MSU1
    msu1_deinit();   /* closes the .msu/.pcm handles, frees the ring */
#endif
    S9xDeinitGFX();
    S9xDeinitDisplay();
    S9xDeinitSound();
    S9xDeinitAPU();
    S9xDeinitMemory();

#if ENABLE_SD_DIAGNOSTICS
    if (s_diag_open) (void)f_sync(&s_diag_fil);
#endif

    /* Restore framebuffer for Clean Light UI and clear screen */
    ili9341_clear_black();
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */
int main(void) {
    vreg_set_voltage(EMULATOR_VOLTAGE);
    set_sys_clock_khz(CPUFreqKHz, true);
    apply_safe_qmi_timing();

#if PERIPH_CLOCK_DECOUPLE
    /* Decouple the peripheral clock from the CPU -- copied from PicoNGP, which
     * is validated on this console with this panel and this SD wiring.
     *
     * The number that matters is clk_peri = 140 MHz, not the SPI rate. Both SPI0
     * (panel) and SPI1 (microSD) divide down from clk_peri by
     * (prescale x postdiv) with prescale EVEN, so at 140 MHz the panel lands on
     * 140/(2x2) = 70 MHz and the card on 140/(2x4) = 17.5 MHz. That 17.5 is why
     * PicoNGP's CLK_FAST_DEFAULT of "20 MHz" is safe there: 140 is not a multiple
     * of 20, so the request settles one step below it. At the 360 MHz clk_peri
     * this firmware used to run, the same constant means 360/(2x9) = 20.000 MHz --
     * 14% above the ceiling the root AGENTS.md sec. 9.3 records for this wiring.
     *
     * ORDER IS THE POINT, and getting it wrong is what broke the card earlier.
     * This has to run BEFORE Frens::initAll(), because initAll() is what mounts
     * the SD card: if clk_peri is changed afterwards, the card has already been
     * brought up at one peripheral clock and then has the clock pulled out from
     * under it mid-session, with tf_card's cached clk_fast no longer matching the
     * hardware. The card then reads nothing -- no directory listing, and no
     * /boot.txt either, which is also why GB_MODE=1 stopped being honoured.
     *
     * The first clock_configure() to CLK_SYS before the PLL is the same
     * two-step PicoNGP uses: PLL_USB has to be running before it can be selected
     * as the peripheral source.
     *
     * 840 MHz VCO / 6 / 1 = 140 MHz (fbdiv = 840/12 = 70), the arithmetic in
     * PicoNGP's own comment. Post dividers are limited to 1..7, so 1400/10 does
     * not exist and 840/6 is the form that works. */
    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
        clock_get_hz(clk_sys),
        clock_get_hz(clk_sys)
    );
    pll_init(pll_usb, 1, 840 * MHZ, 6, 1);
    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
        140 * MHZ,
        140 * MHZ
    );
#else
    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
        clock_get_hz(clk_sys),
        clock_get_hz(clk_sys)
    );
#endif

    stdio_init_all();
    dmg_init_ui_buttons();

    selectedRom[0] = '\0';

    FrensSettings::initSettings(FrensSettings::emulators::SNES);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0, 1024, false, true);
    audio_i2s_update_pio_frequency(SNES_AUDIO_RATE);

#if USE_ILI9341
    /* Read /boot.txt and the ROM/save layout only after the SD card is mounted:
     * dmg_panel_config_init() and dmg_init_boot_mode() both try to open files on
     * it, and caching a "file missing" answer from before the mount is how an
     * emulator ends up permanently on the wrong panel. */
    dmg_panel_config_init();
    dmg_init_boot_mode();

    ili9341_init();

    snes_load_config();
    set_backlight_brightness(persistent_brightness_level);
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_scanlines(persistent_scanlines_enabled);
    ili9341_set_interleave(persistent_interleave);
    s9x_set_interleave(persistent_interleave);
    ili9341_clear_black();
#endif

    ensure_save_directories();

    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_snes_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_snes_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_snes_desc);
            snprintf(selectedRom, sizeof(selectedRom), "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            audio_i2s_update_pio_frequency(44100);   /* the chime is 44.1 kHz */
        if (dmg_boot_intro_start(&s_snes_desc)) dmg_boot_intro_finish();
        audio_i2s_update_pio_frequency(SNES_AUDIO_RATE);
        }
    }
    if (s_resume_pending) {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", dmg_get_current_rom_dir());
        /* The intro chime is 44.1 kHz; run_game() sets the SNES 32 kHz again. */
        audio_i2s_update_pio_frequency(44100);
    }

    printf("\n==================================================\n");
    printf("  %s Super Nintendo (SNES) for RP2350\n", APP_TITLE);
    printf("  CPU: %lu MHz | clk_peri %lu MHz | SPI0 panel %lu MHz | SPI1 microSD %lu MHz\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000),
           (unsigned long)(clock_get_hz(clk_peri) / 1000000),
           (unsigned long)(spi_get_baudrate(ILI9341_SPI) / 1000000),
           (unsigned long)(spi_get_baudrate(spi1) / 1000000));
#if USE_ILI9341
    printf("  Panel: %s (MADCTL=0x%02x, source: %s)\n",
           (dmg_panel_get() == DMG_PANEL_ST7789) ? "ST7789" : "ILI9341",
           (unsigned)dmg_panel_get_madctl(), dmg_panel_source());
    printf("  ROMs: %s | Saves: %s\n", ROMS_DIR, SAVES_DIR);
    printf("  GB_MODE window: %s\n", dmg_is_gb_mode() ? "on (x=45, 266px)" : "off (full 320x240)");
#endif
    printf("  PSRAM: %s\n", Frens::isPsramEnabled() ? "detected - ROMs load into PSRAM"
                                                    : "none - ROMs load into flash XIP");
    printf("==================================================\n\n");

    /* From here on, everything goes to the card. Boot info first, so a log that
     * stops after this line already tells you where it stopped. */
    snes_diag_begin();
    snes_diag("[boot] %s\n", APP_TITLE);
    snes_diag("[boot] clk_sys=%lu clk_peri=%lu spi0=%lu spi1=%lu\n",
              (unsigned long)(clock_get_hz(clk_sys) / 1000),
              (unsigned long)(clock_get_hz(clk_peri) / 1000),
              (unsigned long)(spi_get_baudrate(ILI9341_SPI) / 1000),
              (unsigned long)(spi_get_baudrate(spi1) / 1000));
    snes_diag("[boot] panel=%s madctl=0x%02x src=%s gb_mode=%d\n",
              (dmg_panel_get() == DMG_PANEL_ST7789) ? "ST7789" : "ILI9341",
              (unsigned)dmg_panel_get_madctl(), dmg_panel_source(),
              dmg_is_gb_mode() ? 1 : 0);
    snes_diag("[boot] psram=%d fatal=%d romdir=%s savedir=%s\n",
              Frens::isPsramEnabled() ? 1 : 0, isFatalError ? 1 : 0,
              ROMS_DIR, SAVES_DIR);

    while (true) {

        if (s_resume_pending || rom_file_selector(selectedRom, sizeof(selectedRom))) {
            if (load_cart_rom_file(selectedRom)) {
                dmg_config_commit(&s_snes_desc);
                dmg_power_set_game(selectedRom, selectedRom);
                run_game();
                dmg_power_set_game(NULL, NULL);
                if (s_resume_pending) {   /* run_game() bailed before the state */
                    s_resume_pending = false;
                    dmg_resume_consume();
                }
                dmg_boot_intro_finish();
            } else {
                if (s_resume_pending) {
                    s_resume_pending = false;
                    dmg_resume_consume();
                }
                dmg_boot_intro_finish();
                sleep_ms(1500);
            }
        }
    }

    return 0;
}
