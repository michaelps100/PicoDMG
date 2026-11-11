/**
 * pico-pcePlus - PC Engine / TurboGrafx-16 emulator for Raspberry Pi Pico
 *
 * Based on pce-go from retro-go (https://github.com/ducalex/retro-go)
 * Using the video/audio framework from pico-infonesPlus
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/divider.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "util/work_meter.h"
#include "ff.h"
#include "tusb.h"
#include "gamepad.h"
#include "menu.h"
#include "nespad.h"
#include "wiipad.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "FrensFonts.h"
#include "vumeter.h"
#include "mytypes.h"
#include "PicoPlusPsram.h"
#include "menu_settings.h"
#include "state.h"
#include "soundrecorder.h"
#include "pce_diag.h"

extern "C"
{
#include "pce-go.h"
#include "pce.h"
#include "psg.h"
#include "cd.h"
#include "cd_chd.h"
#include "gfx.h"
}

#if HSTX
#include "drivers/pico_hdmi/hstx.h"
#endif

#if USE_ILI9341
#include "ili9341.h"
#include "crc32.h"
#include "hardware/pwm.h"
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"
#endif

bool isFatalError = false;
static FATFS fs;
char *romName;
bool showSettings = false;
bool loadSaveStateMenu = false;
SaveStateTypes quickSaveAction = SaveStateTypes::NONE;
static uint32_t start_tick_us = 0;
static uint32_t fps = 0;
static uint8_t framesbeforeAutoStateIsLoaded = 0;
static char fpsString[16] = "00";
static int fpsStringLen = 2;

#define AUDIOBUFFERSIZE 4096
#define PCE_AUDIO_RATE 44100
// Native PCE NTSC frame rate is 59.826 Hz (263 lines * 1365 master clocks at
// 21.47727 MHz), not 60. At 44100 Hz audio that's 44100/59.826 â‰ˆ 737.14
// samples per frame; we emit 737 most frames and 738 occasionally via an
// exact rational accumulator so audio production locks to 44100 Hz long-term.
// Buffer sizes use the ceiling.
#define PCE_SAMPLES_PER_FRAME_MAX 738
// PCE NTSC frame rate scaled by 10000 (avoids floats in the hot path).
#define PCE_FPS_X10000 598261u

#if USE_ILI9341
#define EMULATOR_CLOCKFREQ_KHZ 320000
#else
#define EMULATOR_CLOCKFREQ_KHZ 250000
#endif
static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

#ifndef CHECK_BIOS_AT_BOOT
#define CHECK_BIOS_AT_BOOT 1
#endif


// Per-scanline indexed line buffer: pce-go renders one scanline at a time.
// 16-byte scratch padding on each side (see render_lines() in gfx.c).
static uint8_t pce_line_buffer_storage[XBUF_WIDTH + 64];
static uint8_t *const pce_line_buffer = pce_line_buffer_storage + 16;
int osd_gfx_render_line = 0;

// Lookup table: 8-bit palette index -> RGB565 (ILI9341), RGB555 (HSTX) or RGB444 (DVI)
uint16_t paletteLUT[256];

// Screen geometry â€” updated by osd_gfx_framebuffer() each time render_lines fires
static int current_screen_w = 256;
static int current_screen_h = 240;
static int current_x_offset = 32;
static int current_y_offset = 0;

// Settings visibility for PCE. Designated initializers keep each value bound to
// its enum tag, so inserting a new MOPT_* cannot silently shift this list.
const int8_t g_settings_visibility_pce[MOPT_COUNT] = {
    [MOPT_EXIT_GAME]                = 0,                  // Exit Game, or back to menu. Always visible when in-game.
    [MOPT_RESET_GAME]               = 0,                  // Reset Game
    [MOPT_REBOOT_TO_LOADER]         = BOOTLOADER_BUILD,   // Return to emuLoader picker (only when built for the loader)
    [MOPT_SAVE_RESTORE_STATE]       = 0,                  // Save / Restore State
    [MOPT_SCREENMODE]               = 1,                  // Screen Mode
    [MOPT_SCANLINES]                = 0,                  // Scanlines toggle (superseded by Screen Mode)
    [MOPT_SCANLINE_TYPE]            = HSTX,               // Scanline Type (HSTX only)
    [MOPT_FPS_OVERLAY]              = 1,                  // FPS Overlay
    [MOPT_AUDIO_ENABLE]             = 0,                  // Audio Enable
    [MOPT_FRAMESKIP]                = 0,                  // Frame Skip
    [MOPT_DISPLAY_MODE]             = HSTX && ENABLEDVI,  // Display Mode (HDMI or DVI, only when HSTX is enabled, because non-HSTX builds always use HDMI)
    [MOPT_EXTERNAL_AUDIO]           = EXT_AUDIO_IS_ENABLED, // External Audio
    [MOPT_FONT_COLOR]               = 1,                  // Font Color
    [MOPT_FONT_BACK_COLOR]          = 1,                  // Font Back Color
    [MOPT_FRUITJAM_VUMETER]         = ENABLE_VU_METER,    // VU Meter
    [MOPT_FRUITJAM_VOLUME_CONTROL]  = (HW_CONFIG == 8),   // Fruit Jam Volume Control
    [MOPT_DMG_PALETTE]              = 0,                  // DMG Palette (not applicable for PCE)
    [MOPT_BORDER_MODE]              = 0,                  // Border Mode (Super Gameboy style borders not applicable for PCE)
    [MOPT_RAPID_FIRE_ON_A]          = 0,                  // Rapid Fire on A
    [MOPT_RAPID_FIRE_ON_B]          = 0,                  // Rapid Fire on B
    [MOPT_AUTO_INSERT_FDS_DISK_A]   = 0,                  // Auto Insert Disk A, enabled at runtime on RP2350
    [MOPT_AUTO_SWAP_FDS_DISK]       = 0,                  // Auto Swap FDS, enabled at runtime on RP2350
    [MOPT_FDS_DISK_SWAP]            = 0,                  // FDS Disk Swap (toggled on after fdsParse succeeds)
    [MOPT_OVERCLOCK]                = HSTX,               // Overclock (CPU high clock toggle)
    [MOPT_FM_AUDIO]                 = 0,                  // YM Audio, SMS only
    [MOPT_ENTER_BOOTSEL_MODE]       = 1,                  // Enter bootsel mode
    [MOPT_CONTROLLER_TEST]          = 1,                  // Controller Test
    [MOPT_RECENT_GAMES]             = 0,                  // Recently played (menu.cpp force-shows this in the rom browser)
    [MOPT_USB_DRIVE_MODE]           = 0,                  // USB drive mode (menu.cpp force-shows this in the rom browser)
};

const uint8_t g_available_screen_modes_pce[] = {
    1, // SCANLINE_8_7    (HSTX stretches the centred 256-px content; 320-px modes auto-fall-back to 1:1)
    1, // NOSCANLINE_8_7
    1, // SCANLINE_1_1
    1  // NOSCANLINE_1_1
};

#define fpsfgcolor 0
#define fpsbgcolor 0xFFF

#define FPSSTART 0
#define FPSEND 8

static bool reset = false;
static bool resetGame = false;

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
static uint16_t wiipad_raw_cached = 0;
#endif

// Build the palette lookup table mapping 8-bit PCE color indices to RGB
// PCE palette index encoding: GGG_RRR_BB (3-bit green, 3-bit red, 2-bit blue)
static void buildPaletteLUT()
{
    for (int i = 0; i < 256; i++)
    {
        int g3 = (i >> 5) & 7;
        int r3 = (i >> 2) & 7;
        int b2 = (i >> 0) & 3;
#if USE_ILI9341
        // RGB565
        int r5 = (r3 << 2) | (r3 >> 1);
        int g6 = (g3 << 3) | g3;
        int b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        paletteLUT[i] = (r5 << 11) | (g6 << 5) | b5;
#elif !HSTX
        // RGB444
        int r4 = (r3 << 1) | (r3 >> 2);
        int g4 = (g3 << 1) | (g3 >> 2);
        int b4 = (b2 << 2) | b2;
        paletteLUT[i] = (r4 << 8) | (g4 << 4) | b4;
#else
        // RGB555
        int r5 = (r3 << 2) | (r3 >> 1);
        int g5 = (g3 << 2) | (g3 >> 1);
        int b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        paletteLUT[i] = (r5 << 10) | (g5 << 5) | b5;
#endif
    }
}
#if USE_ILI9341
static uint8_t persistent_volume_level = 3;     /* 1..5 */
#endif
static char selectedRom[FF_MAX_LFN] = "";

static int16_t pce_audio_buffer[PCE_SAMPLES_PER_FRAME_MAX * 2]; // stereo interleaved
static int audio_pos = 0;   // PSG samples synthesised so far this frame
static int pce_samples_this_frame = 737;

// Returns the number of audio samples this frame should produce. Exact rational
// accumulator: target rate = PCE_AUDIO_RATE / 59.8261 Hz. Drift-free long-term.
static int __not_in_flash_func(advance_samples_this_frame)(void)
{
    static uint32_t accum = 0;
    accum += (uint32_t)PCE_AUDIO_RATE * 10000u;
    int n = (int)(accum / PCE_FPS_X10000);
    accum -= (uint32_t)n * PCE_FPS_X10000;
    return n;  // ~737 or 738 per call; averages 737.143
}

#define AUDIO_SAMPLES_PER_SCANLINE 4

// pce-go internal cycles per frame: 263 scanlines * cycles_per_line (113).
#define PCE_CYCLES_PER_FRAME (263 * 113)

// Sample-accurate PSG: generate audio up to the frame-relative CPU cycle
// position of the register write that triggered this (called from pce_writeIO
// before the write is applied). Each register state is thus rendered for
// exactly the cycles it was active â€” short SFX / DDA are no longer lost to
// coarse per-batch sampling. The frame always completes to pce_samples_this_frame
// samples via the frame-end flush in osd_psg_scanline.
extern "C" void __not_in_flash_func(osd_psg_sync)(int frame_cycle)
{
    int target = (int)((uint64_t)frame_cycle * pce_samples_this_frame / PCE_CYCLES_PER_FRAME);
    if (target > pce_samples_this_frame)
        target = pce_samples_this_frame;
    int n = target - audio_pos;
    if (n > 0) {
        psg_update(pce_audio_buffer + audio_pos * 2, n, 0x3F);
        audio_pos += n;
    }
}

extern "C" void __not_in_flash_func(osd_psg_scanline)(void)
{
    // Frame-end flush: synthesise the remainder of this frame's samples even
    // if the game wrote no PSG registers late in the frame.
    if (PCE.Scanline >= 262)
        osd_psg_sync(PCE_CYCLES_PER_FRAME);
}

extern "C" void __not_in_flash_func(osd_gfx_lines_rendered)(int first_line, int last_line)
{
    (void)last_line;
#if USE_ILI9341
    if (settings.flags.displayFrameRate && first_line >= 8 && first_line < 16)
    {
        int rowInChar = first_line - 8;
        for (int i = 0; i < fpsStringLen; i++)
        {
            char fontSlice = getcharslicefrom8x8font(fpsString[i], rowInChar);
            int base_x = 8 + i * 8;
            for (int bit = 0; bit < 8; bit++)
            {
                if (base_x + bit < current_screen_w)
                {
                    pce_line_buffer[base_x + bit] = (fontSlice & 1) ? 0xFF : 0x00;
                }
                fontSlice >>= 1;
            }
        }
    }
    ili9341_stream_pce_line(first_line, pce_line_buffer, paletteLUT);
    return;
#else
    int display_y = first_line + current_y_offset;
    if (display_y < 0 || display_y >= 240) return;

    uint16_t *dst;
#if HSTX
    dst = hstx_getlineFromFramebuffer(display_y);
#elif FRAMEBUFFERISPOSSIBLE
    auto *b = Frens::isFrameBufferUsed() ? nullptr : dvi_->getLineBuffer();
    dst = b ? b->data() : &Frens::framebuffer[display_y * 320];
#else
    auto *b = dvi_->getLineBuffer();
    dst = b->data();
#endif

    if (current_x_offset > 0)
        memset(dst, 0, current_x_offset * sizeof(uint16_t));

    // 4 indexed pixels per iteration: one uint32_t load, four LUT lookups,
    // two uint32_t stores. pce_line_buffer and dst+offset are both 4-byte
    // aligned (PCE widths are multiples of 8, so x_offset is a multiple of 4).
    const uint32_t *src32 = (const uint32_t *)pce_line_buffer;
    uint32_t *dst32 = (uint32_t *)(dst + current_x_offset);
    int chunks = current_screen_w >> 2;
    for (int i = 0; i < chunks; i++)
    {
        uint32_t s = src32[i];
        uint32_t p0 = paletteLUT[s & 0xFF];
        uint32_t p1 = paletteLUT[(s >> 8) & 0xFF];
        uint32_t p2 = paletteLUT[(s >> 16) & 0xFF];
        uint32_t p3 = paletteLUT[(s >> 24)];
        dst32[i * 2]     = p0 | (p1 << 16);
        dst32[i * 2 + 1] = p2 | (p3 << 16);
    }
    // Defensive: handle a non-multiple-of-4 width (shouldn't happen on PCE).
    for (int x = chunks << 2; x < current_screen_w; x++)
        dst[x + current_x_offset] = paletteLUT[pce_line_buffer[x]];

    int right = current_x_offset + current_screen_w;
    if (right < 320)
        memset(dst + right, 0, (320 - right) * sizeof(uint16_t));

    if (settings.flags.displayFrameRate &&
        display_y >= current_y_offset + FPSSTART &&
        display_y <  current_y_offset + FPSEND)
    {
        WORD *fpsBuffer = dst + current_x_offset + 4;
        int rowInChar = display_y - current_y_offset - FPSSTART;
        for (int i = 0; i < fpsStringLen; i++)
        {
            char fontSlice = getcharslicefrom8x8font(fpsString[i], rowInChar);
            for (int bit = 0; bit < 8; bit++)
            {
                if (fontSlice & 1)
                    *fpsBuffer++ = fpsfgcolor;
                else
                    *fpsBuffer++ = fpsbgcolor;
                fontSlice >>= 1;
            }
        }
    }

#if !HSTX
#if FRAMEBUFFERISPOSSIBLE
    if (b) dvi_->setLineBuffer(display_y, b);
#else
    dvi_->setLineBuffer(display_y, b);
#endif
#endif
#endif
}

#if PICO_RP2350
// Heap-allocated for the lifetime of a CD-game session so the menu doesn't
// hold ~3 KB it can't use.
static int16_t *cd_audio_buffer = nullptr;
#endif

// Audio-pipeline diagnostics: once per second during CD-DA playback, print
// the minimum levels reached by each buffer in the chain (CD-DA prefetch
// ring, HDMI data-island queue, I2S ring) plus the underrun delta. The
// buffer whose minimum approaches zero around an audible crackle is the
// one to fix. Compile with -DCD_AUDIO_DIAG=1; default off.
#ifndef CD_AUDIO_DIAG
#define CD_AUDIO_DIAG 0
#endif
#if CD_AUDIO_DIAG && PICO_RP2350
#if USE_I2S_AUDIO
extern "C" int audio_i2s_get_fill_permille();
#endif
static void __not_in_flash_func(cdAudioDiagTick)()
{
    static uint32_t min_cdring = UINT32_MAX, min_di = UINT32_MAX;
    static int min_i2s = INT32_MAX;
    static uint32_t last_underruns = 0;
    static int frames = 0;
    static bool played = false;

    if (!CD.cd_attached)
        return;
    if (CD.audio_status == 0)
        played = true;
    uint32_t rc = CD.audio_ring_count;
    if (rc < min_cdring) min_cdring = rc;
#if HSTX
    uint32_t di = hstx_di_queue_get_level();
    if (di < min_di) min_di = di;
#endif
#if USE_I2S_AUDIO
    int fp = audio_i2s_get_fill_permille();
    if (fp < min_i2s) min_i2s = fp;
#endif
    if (++frames < 60)
        return;
    if (played) {
        printf("[adiag] cdring_min=%lu/%u di_min=%lu i2s_min=%d underruns+%lu\n",
               (unsigned long)min_cdring, CD_AUDIO_RING_SECTORS,
               min_di == UINT32_MAX ? 0UL : (unsigned long)min_di,
               min_i2s == INT32_MAX ? -1 : min_i2s,
               (unsigned long)(cd_audio_underruns - last_underruns));
        last_underruns = cd_audio_underruns;
    }
    frames = 0;
    played = false;
    min_cdring = UINT32_MAX;
    min_di = UINT32_MAX;
    min_i2s = INT32_MAX;
}
#endif

#if !HSTX && PICO_RP2350
#if USE_I2S_AUDIO
extern "C" int audio_i2s_get_fill_permille();
#endif
// Fill level (permille, 0..1000) of whichever audio output is currently
// active â€” the DVI/HDMI audio ring or the I2S ring. Drives the audio-clock
// pace and frame-skip so both behave identically regardless of audio route.
static int __not_in_flash_func(cdAudioFillPermille)()
{
#if USE_ILI9341
    return 500;
#elif EXT_AUDIO_IS_ENABLED
    if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
    {
#if USE_I2S_AUDIO
        return audio_i2s_get_fill_permille();
#else
        return 500; // other ext-audio drivers expose no fill level
#endif
    }
#endif
#if !USE_ILI9341
    auto &r = dvi_->getAudioRingBuffer();
    uint32_t sz = r.getBufferSize();
    return sz ? (int)((uint64_t)r.getFullReadableSize() * 1000u / sz) : 0;
#endif
}
#endif

// Per-frame: push audio, clear display borders, render FPS overlay.
static void __not_in_flash_func(pushAudioAndOverlay)()
{
#if PICO_RP2350
    if (CD.cd_attached) {
        // CD audio SD prefetch (cd_audio_update):
        //  - HSTX: core1 background task.
        //  - framebuffer DVI: during the PaceFrames60fps slack-wait (the
        //    vsyncWaitTask), overlapping the wait instead of adding frame time.
        //  - non-framebuffer DVI: no slack-wait, so prefetch here on core0.
#if !HSTX
        if (CD.audio_status == 0 && (!Frens::isFrameBufferUsed() || USE_ILI9341))
            cd_audio_update();
#endif
        // CD-DA: generate into a scratch buffer and mix into the PSG output.
        if (CD.audio_status == 0 && cd_audio_buffer) {
            int n = cd_audio_generate_samples(cd_audio_buffer, pce_samples_this_frame);
            for (int i = 0; i < n * 2; i++) {
                int32_t v = (int32_t)pce_audio_buffer[i] + (int32_t)cd_audio_buffer[i];
                pce_audio_buffer[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        // ADPCM: decode + resample, summed directly into the PSG output.
        cd_adpcm_generate_samples(pce_audio_buffer, pce_samples_this_frame, PCE_AUDIO_RATE);
    }
#endif

    int audio_idx = 0;

#if USE_ILI9341
    if (audio_pos < pce_samples_this_frame)
    {
        psg_update(pce_audio_buffer + audio_pos * 2, pce_samples_this_frame - audio_pos, 0x3F);
        audio_pos = pce_samples_this_frame;
    }
    static uint32_t s_packed_audio[PCE_SAMPLES_PER_FRAME_MAX];
    for (int i = 0; i < audio_pos; i++)
    {
        int32_t l = pce_audio_buffer[i * 2];
        int32_t r = pce_audio_buffer[i * 2 + 1];
        l = (l * (int32_t)persistent_volume_level) / 5;
        r = (r * (int32_t)persistent_volume_level) / 5;
        s_packed_audio[i] = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
    }
    audio_i2s_enqueue_samples(s_packed_audio, audio_pos);
#elif HSTX
#if EXT_AUDIO_IS_ENABLED
    const bool routeToExtAudio = settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected();
#endif
    for (int y = 0; y < 240; y++)
    {
        for (int a = 0; a < AUDIO_SAMPLES_PER_SCANLINE && audio_idx < pce_samples_this_frame; a++, audio_idx++)
        {
            int16_t l = pce_audio_buffer[audio_idx * 2];
            int16_t r = pce_audio_buffer[audio_idx * 2 + 1];
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter)
                addSampleToVUMeter(l);
#endif
#if EXT_AUDIO_IS_ENABLED
            if (routeToExtAudio)
                EXT_AUDIO_ENQUEUE_SAMPLE(l, r);
            else
#endif
                hstx_push_audio_sample(l, r);
        }
    }
    // Clear top/bottom border lines in HSTX framebuffer
    for (int y = 0; y < current_y_offset; y++)
        memset(hstx_getlineFromFramebuffer(y), 0, 320 * sizeof(uint16_t));
    for (int y = current_y_offset + current_screen_h; y < 240; y++)
        memset(hstx_getlineFromFramebuffer(y), 0, 320 * sizeof(uint16_t));

#elif !HSTX
    // DVI audio (framebuffer and line-streaming paths)
#if EXT_AUDIO_IS_ENABLED
    if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
    {
        for (int a = 0; a < audio_pos; a++)
            EXT_AUDIO_ENQUEUE_SAMPLE(pce_audio_buffer[a * 2], pce_audio_buffer[a * 2 + 1]);
    } else {
#endif
        // Batch push to the DVI audio ring buffer.
        int remaining = audio_pos;
        int pos = 0;
        while (remaining > 0)
        {
            auto &ring = dvi_->getAudioRingBuffer();
            auto n = std::min<int>(remaining, ring.getWritableSize());
            if (!n)
                break;
            auto p = ring.getWritePointer();
            for (int a = 0; a < n; a++)
                *p++ = {pce_audio_buffer[(pos + a) * 2],
                        pce_audio_buffer[(pos + a) * 2 + 1]};
            ring.advanceWritePointer(n);
            remaining -= n;
            pos += n;
        }
#if EXT_AUDIO_IS_ENABLED
    }
#endif
#if FRAMEBUFFERISPOSSIBLE
    if (Frens::isFrameBufferUsed())
    {
        // Clear top/bottom border lines in DVI framebuffer
        for (int y = 0; y < current_y_offset; y++)
            memset(&Frens::framebuffer[y * 320], 0, 320 * sizeof(uint16_t));
        for (int y = current_y_offset + current_screen_h; y < 240; y++)
            memset(&Frens::framebuffer[y * 320], 0, 320 * sizeof(uint16_t));
    }
#endif
#endif

#if CD_AUDIO_DIAG && PICO_RP2350
    cdAudioDiagTick();
#endif
    // FPS overlay is rendered inline in osd_gfx_lines_rendered()
}

// OSD callbacks required by pce-go
extern "C" uint8_t *__not_in_flash_func(osd_gfx_framebuffer)(int width, int height)
{
    current_screen_w = (width > 320) ? 320 : width;
    current_screen_h = (height > 240) ? 240 : height;
    current_x_offset = (320 - current_screen_w) / 2;
    current_y_offset = (240 - current_screen_h) / 2;

#if USE_ILI9341
    ili9341_pce_set_geometry(current_screen_w, current_screen_h);
#endif

#if HSTX
    static int last_w = -1;
    if (current_screen_w != last_w)
    {
        hstx_setAspectRatio87((scaleMode8_7_ && current_screen_w <= 256) ? 1 : 0);
        last_w = current_screen_w;
    }
#endif

    return pce_line_buffer - osd_gfx_render_line * XBUF_WIDTH;
}

extern "C" void osd_vsync(void)
{
}

extern "C" void osd_input_read(uint8_t joypads[8])
{
    // Input is read explicitly in the main loop
}

static inline int ProcessAfterFrameIsRendered()
{
    Frens::pollHeadPhoneJack();
#if !USE_ILI9341
    // Pulse-vsync wait for non-CD games (HuCards) on PicoDVI â€” locks to the
    // DVI's 60Hz pulse, same as before. CD games override this via the
    // audio-clock pace registered with setAudioPaceQuery.
    Frens::PaceFrames60fps(false, !CD.cd_attached);
#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
    (void)count;
    Frens::blinkLed(false);
#if NES_PIN_CLK != -1
    nespad_read_finish();
#endif
#else
    static uint32_t count = 0;
    count++;
    static uint32_t next_frame_time_us = 0;
    uint32_t now_us = time_us_32();
    if (next_frame_time_us == 0) {
        next_frame_time_us = now_us + 16667u;
    } else {
        while ((int32_t)(next_frame_time_us - time_us_32()) > 0) {
            tight_loop_contents();
        }
        if ((int32_t)(time_us_32() - next_frame_time_us) > 16667) {
            next_frame_time_us = time_us_32() + 16667u;
        } else {
            next_frame_time_us += 16667u;
        }
    }
    Frens::blinkLed(false);
#endif
    tuh_task();
    if (settings.flags.displayFrameRate)
    {
        // 1-second windowed average. The previous per-frame ceil(1e6/interval)
        // misread slack-paced frames: in heavy scenes a no-wait frame (~15.5ms)
        // alternates with a longer waiting frame, flashing 63-65 on screen even
        // though the average is locked at ~59.83fps.
        static uint32_t fpsFrames = 0;
        fpsFrames++;
        uint32_t elapsed_us = Frens::time_us() - start_tick_us;
        if (elapsed_us >= 1000000)
        {
            fps = (fpsFrames * 1000000ull + elapsed_us / 2) / elapsed_us;
            fpsFrames = 0;
            start_tick_us = Frens::time_us();
            int nchars = 0;
            fpsString[nchars++] = '0' + (fps / 10);
            fpsString[nchars++] = '0' + (fps % 10);
#if HSTX
            // Append the HSTX auto-resync count so display glitches are visible.
            fpsString[nchars++] = ' ';
            fpsString[nchars++] = 'R';
            int resync = get_video_output_resync_count();
            char digits[10];
            int nd = 0;
            do
            {
                digits[nd++] = '0' + (resync % 10);
                resync /= 10;
            } while (resync > 0 && nd < (int)sizeof(digits));
            while (nd > 0 && nchars < (int)sizeof(fpsString))
            {
                fpsString[nchars++] = digits[--nd];
            }
#endif
            fpsStringLen = nchars;
        }
    }
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipad_raw_cached = wiipad_read();
#endif
#if ENABLE_VU_METER
    if (isVUMeterToggleButtonPressed())
    {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        turnOffAllLeds();
    }
#endif
#if !USE_ILI9341
    if (showSettings)
    {
        showSettings = false;
        int rval = showSettingsMenu(true);
        if (rval == 3)
        {
            reset = true;
            if (isAutoSaveStateConfigured())
            {
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::SAVE_AND_EXIT;
            }
        }
        if (rval == 4)
        {
            loadSaveStateMenu = true;
            quickSaveAction = SaveStateTypes::NONE;
        }
        if (rval == 5)
        {
            reset = resetGame = true;
        }
    }
    if (loadSaveStateMenu)
    {
        if (quickSaveAction == SaveStateTypes::LOAD_AND_START)
        {
            if (framesbeforeAutoStateIsLoaded > 0)
            {
                --framesbeforeAutoStateIsLoaded;
            }
        }
        else
        {
            framesbeforeAutoStateIsLoaded = 0;
        }
        if (framesbeforeAutoStateIsLoaded == 0)
        {
            char msg[24];
            snprintf(msg, sizeof(msg), "CRC %08X", Frens::getCrcOfLoadedRom());
            if (showSaveStateMenu(Emulator_SaveState, Emulator_LoadState, msg, quickSaveAction) == false)
            {
                reset = true;
            };
            loadSaveStateMenu = false;
        }
    }
#endif
    return count;
}

#if USE_ILI9341
/* =========================================================================
 * Clean Light UI Common & Emulator Descriptor (shared_menu)
 * ========================================================================= */
static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_scanlines_enabled = false;
static bool persistent_shimmerless_enabled = true;
static bool persistent_interleave = true;
static bool persistent_display_fps = false;
static uint8_t persistent_brightness_level = 5; /* 1..5 */
static uint8_t persistent_turbo_mode = 0;       /* 0 = 1x, 1 = 2x */
static uint8_t persistent_save_slot = 1;        /* 1..4 */
static uint8_t persistent_load_slot = 1;        /* 1..4 */

static bool s_request_pause_menu = false;
static bool s_request_exit_game = false;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;

/* Combo hooks, defined next to the descriptor that stores them. */
static void pce_save_config(void);
static bool pce_get_fps(void);
static void pce_apply_fps(bool on);

static void pce_apply_interleave(bool interleave_on) {
    /* Was NULL in the descriptor, which meant dmg_config_load() never pushed the
     * stored value into persistent_interleave -- so "Screen: Progressive |
     * Interleave" came back as the compile-time default after every reboot, and
     * nothing the Display submenu wrote could ever be read back. */
    persistent_interleave = interleave_on;
    ili9341_set_interleave(persistent_interleave);
}


static void set_backlight_brightness(uint8_t level) {
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    persistent_brightness_level = level;
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

static void set_sound_volume_level(uint8_t level) {
    if (level > 5) level = 5;
    persistent_volume_level = level;
}

static void ensure_save_directories(void) {
    f_mkdir("0:/saves");
    f_mkdir("0:/saves/pce");
    f_mkdir("/saves");
    f_mkdir("/saves/pce");
    f_mkdir("saves");
    f_mkdir("saves/pce");
}

static char current_rom_dir[64] = "0:/roms/pce";

static bool pce_is_rom_file(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return false;
    if (strcasecmp(dot, ".pce") == 0 ||
        strcasecmp(dot, ".sgx") == 0) {
        return true;
    }
#if ENABLE_CHD
    // CD-ROM games need PSRAM for the ~2.6 MB of CD/SCD/ADPCM buffers plus
    // the CHD hunk cache, so only offer them on boards that have it.
    if ((strcasecmp(dot, ".chd") == 0 || strcasecmp(dot, ".cue") == 0) &&
        Frens::isPsramEnabled()) {
        return true;
    }
#endif
    return false;
}

static bool pce_save_state(const char *game_name, uint8_t slot) {
    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/pce", game_name, slot, state_path, sizeof(state_path));
    ensure_save_directories();
    return (Emulator_SaveState(state_path) == 0);
}

static bool pce_load_state(const char *game_name, uint8_t slot) {
    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/pce", game_name, slot, state_path, sizeof(state_path));
    int res = Emulator_LoadState(state_path);
    if (res != 0 && slot == 1) {
        char base[64];
        dmg_sanitize_basename(game_name, base, sizeof(base));
        char legacy_path[DMG_STATE_PATH_MAX];
        snprintf(legacy_path, sizeof(legacy_path), "0:/saves/pce/%s.ss", base);
        res = Emulator_LoadState(legacy_path);
        if (res != 0) {
            snprintf(legacy_path, sizeof(legacy_path), "/saves/pce/%s.ss", base);
            res = Emulator_LoadState(legacy_path);
        }
    }
    return (res == 0);
}

/* Slot probe for the shared menu's Load State row: the three paths
 * pce_load_state() above can open, in the same order it tries them. */
static bool pce_state_exists(const char *game_name, uint8_t slot) {
    if (slot < 1 || slot > 4) return false;

    char state_path[DMG_STATE_PATH_MAX];
    dmg_build_state_path("0:/saves/pce", game_name, slot, state_path, sizeof(state_path));
    if (dmg_state_file_present(state_path)) return true;

    if (slot == 1) {
        char base[64];
        dmg_sanitize_basename(game_name, base, sizeof(base));

        char legacy_path[DMG_STATE_PATH_MAX];
        snprintf(legacy_path, sizeof(legacy_path), "0:/saves/pce/%s.ss", base);
        if (dmg_state_file_present(legacy_path)) return true;

        snprintf(legacy_path, sizeof(legacy_path), "/saves/pce/%s.ss", base);
        if (dmg_state_file_present(legacy_path)) return true;
    }
    return false;
}

static void pce_apply_scale(dmg_scale_mode_t scale_mode) {
    persistent_scale_mode = (ili9341_scale_mode_t)scale_mode;
    ili9341_set_scale_mode(persistent_scale_mode);
}

static void pce_apply_effect(bool effect_on) {
    persistent_scanlines_enabled = effect_on;
    ili9341_set_scanlines(effect_on);
}

static void pce_apply_speed(bool turbo_on) {
    persistent_turbo_mode = turbo_on ? 1 : 0;
}

static void pce_apply_volume(uint8_t volume) {
    persistent_volume_level = volume;
    set_sound_volume_level(volume);
}

static void pce_apply_brightness(uint8_t brightness) {
    persistent_brightness_level = brightness;
    set_backlight_brightness(brightness);
}

static void pce_apply_filter(bool shimmerless_on) {
    persistent_shimmerless_enabled = shimmerless_on;
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}

static bool pce_on_load_rom(const struct dmg_emulator_desc *desc, const char *filename) {
    (void)desc;
    (void)filename;
    const char *active = dmg_get_current_rom_dir();
    if (active && active[0] != '\0') {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", active);
    }
    return true;
}

static const char *s_pce_candidate_dirs[] = {
    "0:/roms/pce",
    "0:/roms/tg16",
    "0:/roms",
    "/roms/pce",
    "/roms",
    NULL
};

static void pce_display_submenu(const dmg_emulator_desc_t *desc, const char *game_name);

#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
/* Resume intro chime, pushed from core 1 into the I2S ring (dmg_boot_intro.c). */
static int  pce_audio_free(void) { return EXT_AUDIO_GET_FREE(); }
static void pce_audio_push(int16_t l, int16_t r) { EXT_AUDIO_ENQUEUE_SAMPLE(l, r); }
static void pce_audio_restore(void) {
    /* After the resume intro: ring empty, DMA idle, emulator's rate. */
    audio_i2s_flush();
    audio_i2s_update_pio_frequency(44100);
}
#endif

/* CD backup RAM to the SD before a standby (dmg_power_off_with_resume): the
 * exit branch writes it, and a standby never takes that branch. HuCards have
 * no battery save in this emulator. */
static bool s_cd_running = false;
static void pce_flush_saves(void) {
    if (!s_cd_running) return;
    char bramPath[50];
    snprintf(bramPath, sizeof(bramPath), SAVESTATEDIR);
    f_mkdir(bramPath);
    snprintf(bramPath, sizeof(bramPath), SAVESTATEDIR "/%s", FrensSettings::getEmulatorTypeString());
    f_mkdir(bramPath);
    snprintf(bramPath, sizeof(bramPath), SAVESTATEDIR "/%s/%08X",
             FrensSettings::getEmulatorTypeString(), Frens::getCrcOfLoadedRom());
    f_mkdir(bramPath);
    snprintf(bramPath, sizeof(bramPath), SAVESTATEDIR "/%s/%08X/bram.sav",
             FrensSettings::getEmulatorTypeString(), Frens::getCrcOfLoadedRom());
    cd_bram_save(bramPath);
}

static const dmg_emulator_desc_t s_pce_desc = {
    .app_title = "PicoPCE",
    .default_rom_dir = "0:/roms/pce",
    .candidate_dirs = s_pce_candidate_dirs,
    .config_path = "0:/saves/pce/picopce.cfg",
    .config_fallback = "/saves/pce/picopce.cfg",
    .saves_dir = "0:/saves/pce",
    .config_magic = 0x50434531u, /* 'PCE1' */
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
    .is_rom_file = pce_is_rom_file,
    .on_load_rom = pce_on_load_rom,
    .save_state = pce_save_state,
    .load_state = pce_load_state,
    .apply_scale = pce_apply_scale,
    .apply_effect = pce_apply_effect,
    .apply_interleave = pce_apply_interleave,
    .apply_speed = pce_apply_speed,
    .apply_volume = pce_apply_volume,
    .apply_brightness = pce_apply_brightness,
    .apply_filter = pce_apply_filter,
    .reset_game = NULL,
    .exit_game = NULL,
    .custom_submenu_title = "Display >",
    .custom_submenu = pce_display_submenu,
    .save_config = pce_save_config,
    .get_fps = pce_get_fps,
    .apply_fps = pce_apply_fps,
    .state_exists = pce_state_exists,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_free           = pce_audio_free,
    .audio_push           = pce_audio_push,
#endif
    .flush_saves          = pce_flush_saves,
#if USE_ILI9341 && EXT_AUDIO_IS_ENABLED
    .audio_restore        = pce_audio_restore
#endif
};

static bool pce_get_fps(void) {
    return settings.flags.displayFrameRate != 0;
}

static void pce_apply_fps(bool on) {
    settings.flags.displayFrameRate = on;
    persistent_display_fps = on;
}

static void pce_save_config(void) {
    /* The emulator keeps its settings in persistent_*, while dmg_config_save()
     * writes g_dmg_config. Those are two different copies of the same state, and
     * the Display submenu only ever updated the first one before calling save.
     * So the file was written with whatever g_dmg_config happened to hold, and
     * every Scale / Effect / Screen change came back at the previous value on
     * the next boot. Copying them across first is what makes the save mean
     * anything -- this is the shape save_config() already had in PicoWS and
     * save_emulator_config() in PicoGBC. */
    g_dmg_config.scale_mode       = (uint32_t)persistent_scale_mode;
    g_dmg_config.grid_effect      = persistent_scanlines_enabled ? 1u : 0u;
    g_dmg_config.interleave       = persistent_interleave ? 1u : 0u;
    g_dmg_config.turbo_mode       = persistent_turbo_mode;
    g_dmg_config.volume_level     = persistent_volume_level;
    g_dmg_config.brightness_level = persistent_brightness_level;
    g_dmg_config.save_slot        = persistent_save_slot;
    g_dmg_config.load_slot        = persistent_load_slot;
    g_dmg_config.extra_opt1       = persistent_display_fps ? 1u : 0u;
    g_dmg_config.extra_opt2       = persistent_shimmerless_enabled ? 0u : 1u;
    dmg_config_save(&s_pce_desc);
}

static void pce_load_config(void) {
    /* dmg_config_load() pushes scale/effect/interleave/speed/volume/brightness
     * back into persistent_* through the descriptor callbacks. extra_opt1 has no
     * callback of its own -- dmg_config_t has no fps field -- so the FPS overlay
     * is read and applied by hand here. */
    dmg_config_load(&s_pce_desc);
    persistent_display_fps = (g_dmg_config.extra_opt1 != 0u);
    if (s_pce_desc.apply_fps != NULL) s_pce_desc.apply_fps(persistent_display_fps);
    persistent_shimmerless_enabled = (g_dmg_config.extra_opt2 == 0u);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
}

typedef dmg_action_t ingame_action_e;
#define INGAME_ACTION_RESUME DMG_ACTION_RESUME
#define INGAME_ACTION_RESET  DMG_ACTION_RESET
#define INGAME_ACTION_EXIT   DMG_ACTION_EXIT
#define ingame_menu(game)    dmg_ingame_menu(&s_pce_desc, (game))
#define wait_for_all_ui_buttons_released() dmg_wait_for_all_buttons_released()
#define save_picopce_config() pce_save_config()
#define load_picopce_config() pce_load_config()
#define read_ui_buttons dmg_read_ui_buttons
#define init_ui_buttons dmg_init_ui_buttons

static void draw_pce_display_row(uint8_t idx, bool selected) {
    if (idx >= 4) return;
    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();
    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
    uint16_t bg = selected ? DMG_RGB565_SEL_COLOR : DMG_RGB565_BG_COLOR;
    ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

    char buf[48];
    buf[0] = '\0';
    if (idx == 0) {
        const char *s = "Fit";
        if (persistent_scale_mode == SCALE_STRETCH) s = "Stretch";
        else if (persistent_scale_mode == SCALE_INTEGER) s = "Integer";
        snprintf(buf, sizeof(buf), "Scaling:      < %s >", s);
    } else if (idx == 1) {
        snprintf(buf, sizeof(buf), "Scanlines:    %s", persistent_scanlines_enabled ? "< On >" : "< Off >");
    } else if (idx == 2) {
        snprintf(buf, sizeof(buf), "Filter:       %s", persistent_shimmerless_enabled ? "< Shimmerless >" : "< Sharp >");
    } else if (idx == 3) {
        snprintf(buf, sizeof(buf), "Interleave:   %s", persistent_interleave ? "< On >" : "< Off >");
    }
    if (buf[0] != '\0') {
        ili9341_text(buf, wx + 10, y + 5, selected ? DMG_RGB565_TEXT_SEL_COLOR : DMG_RGB565_TEXT_COLOR, bg);
    }
    ili9341_flush_rows(y, y + DMG_UI_ITEM_HEIGHT - 1);
}

static void pce_display_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
    uint8_t cursor = 0;
    bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
    bool prev_a = true, prev_b = true;

    uint16_t wx = dmg_get_window_x();
    uint16_t ww = dmg_get_window_w();

    dmg_wait_for_all_buttons_released();

    if (dmg_is_gb_mode()) {
        ili9341_fill_screen(0x0000);
        ili9341_fill_rect(wx, 0, ww, 240, DMG_RGB565_BG_COLOR);
    } else {
        ili9341_fill_screen(DMG_RGB565_BG_COLOR);
    }
    dmg_draw_top_bar(desc, desc->app_title, "Display Settings");
    dmg_draw_game_bar(desc, game_name);
    dmg_draw_bottom_bar(desc, "L/R / A: Adjust", "B: Back");

    for (uint8_t i = 0; i < 4; i++) {
        draw_pce_display_row(i, i == cursor);
    }
    for (uint8_t i = 4; i < 8; i++) {
        uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)i * DMG_UI_ITEM_HEIGHT);
        ili9341_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, DMG_RGB565_BG_COLOR);
    }
    ili9341_flush(true);

    while (true) {
        bool btn_up, btn_down, btn_left, btn_right, btn_a, btn_b, btn_select, btn_start;
        dmg_read_ui_buttons(&btn_up, &btn_down, &btn_left, &btn_right, &btn_a, &btn_b, &btn_select, &btn_start);

        bool pushed_up    = btn_up && !prev_up;
        bool pushed_down  = btn_down && !prev_down;
        bool pushed_left  = btn_left && !prev_left;
        bool pushed_right = btn_right && !prev_right;
        bool pushed_a     = btn_a && !prev_a;
        bool pushed_b     = btn_b && !prev_b;

        prev_up    = btn_up;
        prev_down  = btn_down;
        prev_left  = btn_left;
        prev_right = btn_right;
        prev_a     = btn_a;
        prev_b     = btn_b;

        if (pushed_up) {
            uint8_t prev = cursor;
            cursor = (cursor == 0) ? 3 : (cursor - 1);
            draw_pce_display_row(prev, false);
            draw_pce_display_row(cursor, true);
        } else if (pushed_down) {
            uint8_t prev = cursor;
            cursor = (cursor == 3) ? 0 : (cursor + 1);
            draw_pce_display_row(prev, false);
            draw_pce_display_row(cursor, true);
        }

        if (pushed_left || pushed_right || pushed_a) {
            if (cursor == 0) {
                bool allow_integer = !dmg_is_gb_mode() || (current_screen_w <= 266);
                int num_modes = allow_integer ? 3 : 2;
                if (pushed_right || pushed_a) {
                    persistent_scale_mode = (ili9341_scale_mode_t)(((int)persistent_scale_mode + 1) % num_modes);
                } else {
                    persistent_scale_mode = (ili9341_scale_mode_t)(((int)persistent_scale_mode == 0) ? (num_modes - 1) : ((int)persistent_scale_mode - 1));
                }
                ili9341_set_scale_mode(persistent_scale_mode);
                save_picopce_config();
                draw_pce_display_row(0, true);
            } else if (cursor == 1) {
                persistent_scanlines_enabled = !persistent_scanlines_enabled;
                ili9341_set_scanlines(persistent_scanlines_enabled);
                save_picopce_config();
                draw_pce_display_row(1, true);
            } else if (cursor == 2) {
                persistent_shimmerless_enabled = !persistent_shimmerless_enabled;
                ili9341_set_shimmerless(persistent_shimmerless_enabled);
                save_picopce_config();
                draw_pce_display_row(2, true);
            } else if (cursor == 3) {
                persistent_interleave = !persistent_interleave;
                ili9341_set_interleave(persistent_interleave);
                save_picopce_config();
                draw_pce_display_row(3, true);
            }
        }

        if (pushed_b) {
            dmg_wait_for_all_buttons_released();
            return;
        }

        tight_loop_contents();
        sleep_ms(20);
    }
}

static uint32_t last_flashed_rom_size = 0;

// Tracks whether ROM_FILE_ADDR currently points at a live PSRAM HuCard buffer
// pce_load_rom_to_psram() allocated. PSRAM lives at 0x11000000, which is why
// the old `>= 0x20000000u` address-range test never matched it and why the
// flag is authoritative instead of any pointer comparison.
static bool s_rom_in_psram = false;

#ifndef FLASH_BLOCK_SIZE
#define FLASH_BLOCK_SIZE  65536u
#endif
#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 4096u
#endif
#ifndef FLASH_PAGE_SIZE
#define FLASH_PAGE_SIZE   256u
#endif

#define PCE_ROM_CACHE_HEADROOM 0x80000u
#define PCE_ROM_CACHE_ABS      (0x10080000u + PCE_ROM_CACHE_HEADROOM) /* 0x10100000, 64KB aligned */
#define PCE_ROM_CACHE_OFF      (PCE_ROM_CACHE_ABS - XIP_BASE)         /* 0x00100000 */
#define PCE_ROM_CACHE_MAX      0x280000u                              /* 2.5 MB cap (max HuCard size) */

static char s_full_path[256];
static FIL s_fil;
static uint8_t s_page[FLASH_BLOCK_SIZE];

static uint8_t pce_reverse_nibbles(uint8_t value)
{
    static const uint8_t reverse[16] = {
        0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15
    };
    return (uint8_t)((reverse[value & 15u] << 4) | reverse[value >> 4]);
}

#define draw_clean_light_loading_screen(t, g, s) dmg_draw_loading_screen(&s_pce_desc, t, g, s)

// PSRAM HuCard loader, mirroring PicoNES' nes_load_rom_to_psram(). A PSRAM
// board never programs the ROM into flash -- the HuCard is f_malloc'd in
// PSRAM and passed straight to LoadCard(). Keeps the PCE-specific predecode:
// US-encrypted ROMs are detected on the first chunk and nibble-reversed in
// place from data_offset onward, and SetPCECachedRomInfo()/setCrcOfLoadedRom()
// receive the same (predecoded, raw CRC) contract as the flash path so
// LoadCard() can match known-ROM flags.
static bool pce_load_rom_to_psram(const char *filename, uint32_t rom_file_size) {
    if (rom_file_size == 0) {
        printf("[E] Refusing to load a zero-byte ROM into PSRAM\n");
        return false;
    }

    // f_malloc() panics on failure, so the size has to be checked up front
    // rather than discovered by the allocator. Same 512 KB margin as PicoNES
    // covers the menu's own scratch allocations plus lwmem bookkeeping.
    const uint32_t margin = 512u * 1024u;
    const uint32_t avail = (uint32_t)Frens::GetAvailableMemory();
    if (avail > margin && rom_file_size > (avail - margin)) {
        printf("[E] ROM needs %u bytes, only %u usable in PSRAM\n",
               (unsigned)rom_file_size, (unsigned)(avail - margin));
        return false;
    }

    // Release the previous HuCard before allocating, otherwise switching
    // between games leaks each ROM and eventually starves the allocator.
    if (s_rom_in_psram && ROM_FILE_ADDR != 0) {
        printf("[I] Freeing previous PSRAM ROM at 0x%08x\n",
               (unsigned)ROM_FILE_ADDR);
        Frens::f_free((void *)ROM_FILE_ADDR);
        ROM_FILE_ADDR = 0;
        s_rom_in_psram = false;
    }

    uint8_t *buf = (uint8_t *)Frens::f_malloc(rom_file_size);
    if (!buf) {
        printf("[E] PSRAM allocation of %u bytes failed\n", (unsigned)rom_file_size);
        return false;
    }

    // Published before the read: LoadCard() and the loader both key off
    // ROM_FILE_ADDR, and a failed load has to be able to recognise it.
    ROM_FILE_ADDR = (uintptr_t)buf;
    s_rom_in_psram = true;

    draw_clean_light_loading_screen("CARREGANDO HU-CARD", filename, rom_file_size);
    Frens::blinkLed(true);

    bool predecoded = false;
    uint32_t raw_crc = 0;
    const uint32_t data_offset = rom_file_size & 0x1fffu;
    const uint32_t PSRAM_READ_CHUNK = 16u * 1024u;
    uint32_t bytes_loaded = 0;
    bool ok = true;
    f_lseek(&s_fil, 0);

    while (bytes_loaded < rom_file_size) {
        uint32_t want = rom_file_size - bytes_loaded;
        if (want > PSRAM_READ_CHUNK) want = PSRAM_READ_CHUNK;
        UINT br = 0;
        FRESULT fr = f_read(&s_fil, buf + bytes_loaded, want, &br);
        if (fr != FR_OK || br == 0) {
            printf("[E] f_read failed at %u/%u: %d\n",
                   (unsigned)bytes_loaded, (unsigned)rom_file_size, (int)fr);
            ok = false;
            break;
        }

        // CRC runs over the raw (pre-predecode) bytes exactly like the flash
        // path, where update_crc32() executes before the nibble reversal.
        raw_crc = update_crc32(raw_crc, buf + bytes_loaded, br);

        if (bytes_loaded == 0 && br > data_offset + 0x1fffu &&
            buf[data_offset + 0x1fffu] < 0xe0u) {
            predecoded = true;
            printf("[I] Detected US encrypted ROM, predecoding nibbles...\n");
        }

        if (predecoded) {
            for (uint32_t i = 0; i < br; ++i) {
                if (bytes_loaded + i >= data_offset) {
                    buf[bytes_loaded + i] = pce_reverse_nibbles(buf[bytes_loaded + i]);
                }
            }
        }

        bytes_loaded += br;
        dmg_update_loading_progress(&s_pce_desc, bytes_loaded, rom_file_size);
    }

    Frens::blinkLed(false);

    if (!ok) {
        Frens::f_free(buf);
        ROM_FILE_ADDR = 0;
        s_rom_in_psram = false;
        return false;
    }

    last_flashed_rom_size = rom_file_size;
    SetPCECachedRomInfo(predecoded, raw_crc);
    Frens::setCrcOfLoadedRom(raw_crc);
    printf("[I] PCE ROM ready in PSRAM: %u bytes, Raw CRC: 0x%08X (predecoded=%d)\n",
           (unsigned)rom_file_size, (unsigned)raw_crc, predecoded ? 1 : 0);
    if (!dmg_boot_intro_active()) ili9341_fill_screen(0x0000);
    if (!dmg_boot_intro_active()) ili9341_flush(true);
    return true;
}

static bool load_cart_rom_file(const char *filename) {
    if (strchr(filename, '/') != NULL || current_rom_dir[0] == '\0') {
        snprintf(s_full_path, sizeof(s_full_path), "%s", filename);
    } else {
        size_t dlen = strlen(current_rom_dir);
        if (dlen > 0 && current_rom_dir[dlen - 1] == '/') {
            snprintf(s_full_path, sizeof(s_full_path), "%s%s", current_rom_dir, filename);
        } else {
            snprintf(s_full_path, sizeof(s_full_path), "%s/%s", current_rom_dir, filename);
        }
    }

    printf("[I] ROM loading starts: %s\n", s_full_path);

    ili9341_blitter_stop();

    if (!dmg_boot_intro_active()) ili9341_fill_screen(0x0000);
    if (!dmg_boot_intro_active()) ili9341_text("Loading...", 20, 75, 0x7FFF, 0x0000);
    {
        char short_name[44];
        const char *slash = strrchr(filename, '/');
        const char *base = slash ? (slash + 1) : filename;
        strncpy(short_name, base, sizeof(short_name) - 1);
        short_name[sizeof(short_name) - 1] = '\0';
        char *dot = strrchr(short_name, '.');
        if (dot) *dot = '\0';
        for (char *p = short_name; *p; p++) {
            if (*p == '_') *p = ' ';
        }
        if (!dmg_boot_intro_active()) ili9341_text(short_name, 20, 100, 0x07E0, 0x0000);
    }
    if (!dmg_boot_intro_active()) ili9341_flush(true);

    FRESULT fr = f_open(&s_fil, s_full_path, FA_READ);
    if (fr != FR_OK) {
        printf("E f_open(%s) error: %d\n", s_full_path, fr);
        if (!dmg_boot_intro_active()) ili9341_text("Cannot open file!", 20, 135, 0xF800, 0x0000);
        if (!dmg_boot_intro_active()) ili9341_flush(true);
        sleep_ms(2000);
        return false;
    }

    uint32_t rom_file_size = (uint32_t)f_size(&s_fil);
    if (rom_file_size < 0x2000u || rom_file_size > PCE_ROM_CACHE_MAX) {
        printf("E Invalid PCE ROM size: %u\n", (unsigned)rom_file_size);
        if (!dmg_boot_intro_active()) ili9341_text("Invalid ROM size!", 20, 135, 0xF800, 0x0000);
        if (!dmg_boot_intro_active()) ili9341_flush(true);
        f_close(&s_fil);
        sleep_ms(2000);
        return false;
    }

    // A PSRAM board never programs the HuCard into flash: mirror PicoNES'
    // nes_load_rom_to_psram() so the ROM runs straight out of PSRAM. Flash
    // erase/program is slow and destructive, and the shared_menu selector was
    // never the one that programmed it anyway -- on PSRAM the intended path is
    // an f_malloc'd copy, exactly like every other PSRAM board in the set.
    if (Frens::isPsramEnabled()) {
        bool ok = pce_load_rom_to_psram(filename, rom_file_size);
        f_close(&s_fil);
        return ok;
    }

    draw_clean_light_loading_screen("CARREGANDO HU-CARD", filename, rom_file_size);
    Frens::blinkLed(true);
    ROM_FILE_ADDR = PCE_ROM_CACHE_ABS;
    bool predecoded = false;
    uint32_t raw_crc = 0;
    const uint32_t data_offset = rom_file_size & 0x1fffu;

    printf("[I] clk_sys=%lu kHz clk_peri=%lu kHz\n",
           (unsigned long)clock_get_hz(clk_sys) / 1000,
           (unsigned long)clock_get_hz(clk_peri) / 1000);
    printf("[I] Programming flash at 0x%08X (offset 0x%08X) for PCE ROM (%u bytes)...\n",
           (unsigned)PCE_ROM_CACHE_ABS, (unsigned)PCE_ROM_CACHE_OFF, (unsigned)rom_file_size);

    uint32_t offset = 0;

    while (offset < rom_file_size) {
        const uint32_t chunk = (rom_file_size - offset > sizeof(s_page)) ? sizeof(s_page) : (rom_file_size - offset);
        printf("[F] chunk 0x%08X (%u B)\n", (unsigned)offset, (unsigned)chunk);
        UINT read = 0;
        fr = f_read(&s_fil, s_page, chunk, &read);
        if (fr != FR_OK || read != chunk) {
            printf("E f_read failed at offset %u: fr=%d read=%u chunk=%u\n",
                   (unsigned)offset, fr, (unsigned)read, (unsigned)chunk);
            f_close(&s_fil);
            if (!dmg_boot_intro_active()) ili9341_text("SD read failed!", 20, 135, 0xF800, 0x0000);
            if (!dmg_boot_intro_active()) ili9341_flush(true);
            sleep_ms(2000);
            Frens::blinkLed(false); return false;
        }

        raw_crc = update_crc32(raw_crc, s_page, chunk);

        if (offset == 0 && chunk > data_offset + 0x1fffu && s_page[data_offset + 0x1fffu] < 0xe0u) {
            predecoded = true;
            printf("[I] Detected US encrypted ROM, predecoding nibbles...\n");
        }

        if (predecoded) {
            for (uint32_t i = 0; i < chunk; ++i) {
                if (offset + i >= data_offset) {
                    s_page[i] = pce_reverse_nibbles(s_page[i]);
                }
            }
        }

        const uint32_t programmed = (chunk + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
        if (programmed > chunk) {
            memset(s_page + chunk, 0xff, programmed - chunk);
        }

        /* Block already in flash (always, on a standby resume of the same
         * card)? Skip the erase/program: it is the slow part, and it would
         * freeze the resume intro on core 1. */
        if (memcmp((const void *)(PCE_ROM_CACHE_ABS + offset), s_page, chunk) == 0) {
            printf("[F]   block already in flash\n");
        } else {
            dmg_boot_intro_flash_begin();
            Frens::flashEraseSafe(PCE_ROM_CACHE_OFF + offset, FLASH_BLOCK_SIZE);
            printf("[F]   erase done\n");

            for (uint32_t pos = 0; pos < programmed; pos += FLASH_PAGE_SIZE) {
                Frens::flashProgramSafe(PCE_ROM_CACHE_OFF + offset + pos, s_page + pos,
                                        FLASH_PAGE_SIZE);
            }
            dmg_boot_intro_flash_end();
            printf("[F]   program done\n");
        }

        if (memcmp((const void *)(PCE_ROM_CACHE_ABS + offset), s_page, chunk) != 0) {
            printf("E Flash verification mismatch at offset %u!\n", (unsigned)offset);
            f_close(&s_fil);
            if (!dmg_boot_intro_active()) ili9341_text("Flash verify failed!", 20, 135, 0xF800, 0x0000);
            if (!dmg_boot_intro_active()) ili9341_flush(true);
            sleep_ms(2000);
            Frens::blinkLed(false); return false;
        }
        printf("[F]   verify ok\n");

        offset += chunk;
    }
    f_close(&s_fil);
    last_flashed_rom_size = rom_file_size;

    SetPCECachedRomInfo(predecoded, raw_crc);
    Frens::setCrcOfLoadedRom(raw_crc);
    printf("[I] PCE ROM ready in flash: %u bytes, Raw CRC: 0x%08X (predecoded=%d)\n",
           (unsigned)rom_file_size, (unsigned)raw_crc, predecoded ? 1 : 0);
    if (!dmg_boot_intro_active()) ili9341_fill_rect(0, 135, 320, 25, 0x0000);
    if (!dmg_boot_intro_active()) ili9341_text("FLASH DONE", 20, 135, 0x07E0, 0x0000);
    if (!dmg_boot_intro_active()) ili9341_flush(true);
    Frens::blinkLed(false);
    sleep_ms(150);
    return true;
}

static bool rom_file_selector(char *out_selectedRom, size_t max_len) {
    return dmg_rom_selector(&s_pce_desc, out_selectedRom, max_len);
}
#endif

#define NESPAD_SELECT (0x04)
#define NESPAD_START (0x08)
#define NESPAD_UP (0x10)
#define NESPAD_DOWN (0x20)
#define NESPAD_LEFT (0x40)
#define NESPAD_RIGHT (0x80)
#define NESPAD_A (0x01)
#define NESPAD_B (0x02)

static DWORD prevButtons[2]{};
static DWORD prevButtonssystem[2]{};
static DWORD prevOtherButtons[2]{};

#define OTHER_BUTTON1 (0b1)
#define OTHER_BUTTON2 (0b10)

// PCE button definitions (from pce-go.h):
// JOY_A=0x01, JOY_B=0x02, JOY_SELECT=0x04, JOY_RUN=0x08
// JOY_UP=0x10, JOY_RIGHT=0x20, JOY_DOWN=0x40, JOY_LEFT=0x80

void readInputAndMapToPCE(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem, bool ignorepushed, char *gamepadType)
{
    *pdwPad1 = *pdwPad2 = *pdwSystem = 0;

    int pcesystem[2]{};
    unsigned long pushed, pushedsystem, pushedother;
    bool usbConnected = false;

#if USE_ILI9341
    bool cur_up = false, cur_down = false, cur_left = false, cur_right = false;
    bool cur_a = false, cur_b = false, cur_select = false, cur_start = false;
    read_ui_buttons(&cur_up, &cur_down, &cur_left, &cur_right, &cur_a, &cur_b, &cur_select, &cur_start);

    int gpio_pcebuttons = (cur_left ? JOY_LEFT : 0) |
                          (cur_right ? JOY_RIGHT : 0) |
                          (cur_up ? JOY_UP : 0) |
                          (cur_down ? JOY_DOWN : 0) |
                          (cur_a ? JOY_A : 0) |
                          (cur_b ? JOY_B : 0);
    int gpio_pcesystem = (cur_select ? JOY_SELECT : 0) |
                         (cur_start ? JOY_RUN : 0);
#endif

    for (int i = 0; i < 2; i++)
    {
        int nespadbuttons = 0;
        auto &dst = (i == 0) ? *pdwPad1 : *pdwPad2;
        auto &gp = io::getCurrentGamePadState(i);
        if (i == 0)
        {
            usbConnected = gp.isConnected();
            if (gamepadType)
                strcpy(gamepadType, gp.GamePadName);
        }

        int pcebuttons = (gp.buttons & io::GamePadState::Button::LEFT ? JOY_LEFT : 0) |
                         (gp.buttons & io::GamePadState::Button::RIGHT ? JOY_RIGHT : 0) |
                         (gp.buttons & io::GamePadState::Button::UP ? JOY_UP : 0) |
                         (gp.buttons & io::GamePadState::Button::DOWN ? JOY_DOWN : 0) |
                         (gp.buttons & io::GamePadState::Button::A ? JOY_A : 0) |
                         (gp.buttons & io::GamePadState::Button::B ? JOY_B : 0) | 0;

        int otherButtons = (gp.buttons & io::GamePadState::Button::X ? OTHER_BUTTON1 : 0) |
                           (gp.buttons & io::GamePadState::Button::Y ? OTHER_BUTTON2 : 0) | 0;

        pcesystem[i] =
            (gp.buttons & io::GamePadState::Button::SELECT ? JOY_SELECT : 0) |
            (gp.buttons & io::GamePadState::Button::START ? JOY_RUN : 0) | 0;

#if NES_PIN_CLK != -1
        if (usbConnected)
        {
            if (i == 1)
                nespadbuttons = nespadbuttons | nespad_states[1] | nespad_states[0];
        }
        else
        {
            nespadbuttons |= nespad_states[i];
        }
#endif
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        if (usbConnected)
        {
            if (i == 1)
                nespadbuttons |= wiipad_raw_cached;
        }
        else
        {
            if (i == 0)
                nespadbuttons |= wiipad_raw_cached;
        }
#endif
        if (nespadbuttons > 0)
        {
            pcebuttons |= ((nespadbuttons & NESPAD_UP ? JOY_UP : 0) |
                           (nespadbuttons & NESPAD_DOWN ? JOY_DOWN : 0) |
                           (nespadbuttons & NESPAD_LEFT ? JOY_LEFT : 0) |
                           (nespadbuttons & NESPAD_RIGHT ? JOY_RIGHT : 0) |
                           (nespadbuttons & NESPAD_A ? JOY_A : 0) |
                           (nespadbuttons & NESPAD_B ? JOY_B : 0) | 0);
            pcesystem[i] |= ((nespadbuttons & NESPAD_SELECT ? JOY_SELECT : 0) |
                             (nespadbuttons & NESPAD_START ? JOY_RUN : 0) | 0);
        }

#if USE_ILI9341
        if (i == 0)
        {
            pcebuttons |= gpio_pcebuttons;
            pcesystem[0] |= gpio_pcesystem;
        }
#endif

#if USE_ILI9341
        /* Combos run on the merged pad (console buttons, USB, NES and Wii
         * pads) so every input source gets the same shortcuts.
         *
         * This has to happen BEFORE PCE.Joypad.regs[] is written below. It used
         * to sit after that write, clearing bits in the local pcebuttons /
         * pcesystem copies -- which changed nothing, because the register the
         * game reads had already been assigned. The combo buttons leaked into
         * the PC Engine on every frame they were held. */
        if (i == 0)
        {
            dmg_pad_t pad_in;
            pad_in.up     = (pcebuttons & JOY_UP) != 0;
            pad_in.down   = (pcebuttons & JOY_DOWN) != 0;
            pad_in.left   = (pcebuttons & JOY_LEFT) != 0;
            pad_in.right  = (pcebuttons & JOY_RIGHT) != 0;
            pad_in.a      = (pcebuttons & JOY_A) != 0;
            pad_in.b      = (pcebuttons & JOY_B) != 0;
            pad_in.select = (pcesystem[0] & JOY_SELECT) != 0;
            pad_in.start  = (pcesystem[0] & JOY_RUN) != 0;

            dmg_combo_events_t ev;
            dmg_combo_frame(&pad_in, &s_pce_desc, &ev);

            pcebuttons = (pcebuttons & ~(JOY_UP | JOY_DOWN | JOY_LEFT | JOY_RIGHT |
                                         JOY_A | JOY_B)) |
                         (pad_in.up ? JOY_UP : 0) |
                         (pad_in.down ? JOY_DOWN : 0) |
                         (pad_in.left ? JOY_LEFT : 0) |
                         (pad_in.right ? JOY_RIGHT : 0) |
                         (pad_in.a ? JOY_A : 0) |
                         (pad_in.b ? JOY_B : 0);
            pcesystem[0] = (pcesystem[0] & ~(JOY_SELECT | JOY_RUN)) |
                           (pad_in.select ? JOY_SELECT : 0) |
                           (pad_in.start ? JOY_RUN : 0);

            if (ev.request_exit_game) {
                s_request_exit_game = true;
                reset = true;
            }
            if (ev.request_pause_menu) s_request_pause_menu = true;
            if (ev.request_power_off) {
                s_request_power_off = true;
                s_power_reason = ev.power_reason;
            }
            if (ev.speed_toggled) {
                printf("[I] Combo SELECT+A: speed = %s\n",
                       persistent_turbo_mode ? "2x" : "1x");
            }
        }
#endif

        // Write combined buttons to PCE joypad registers
        PCE.Joypad.regs[i] = pcebuttons | pcesystem[i];

        auto p1 = pcesystem[i];
        if (ignorepushed == false)
        {
            pushed = pcebuttons & ~prevButtons[i];
            pushedsystem = pcesystem[i] & ~prevButtonssystem[i];
            pushedother = otherButtons & ~prevOtherButtons[i];
        }
        else
        {
            pushed = pcebuttons;
            pushedsystem = pcesystem[i];
            pushedother = otherButtons;
        }

#if USE_ILI9341
        (void)0;
#else
        // SELECT + RUN -> settings menu
        if (p1 & JOY_SELECT)
        {
            if (pushedsystem & JOY_RUN)
            {
                FrensSettings::savesettings();
                showSettings = true;
            }
            else if (pushed & JOY_LEFT)
            {
#if EXT_AUDIO_IS_ENABLED && !HSTX
                settings.flags.useExtAudio = !settings.flags.useExtAudio;
#else
                settings.flags.useExtAudio = 0;
#endif
            }
#if ENABLE_VU_METER
            else if (pushed & JOY_RIGHT)
            {
                settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
                turnOffAllLeds();
            }
#endif
        }
        // RUN + button combos
        if (p1 & JOY_RUN)
        {
            if (pushed & JOY_A)
            {
                settings.flags.displayFrameRate = !settings.flags.displayFrameRate;
            }
            else if (pushed & JOY_B)
            {
#if PICO_RP2350
                if (Frens::isPsramEnabled() && !SoundRecorder::isRecording())
                    SoundRecorder::startRecording();
#endif
            }
            else if (pushed & JOY_UP)
            {
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::LOAD;
            }
            else if (pushed & JOY_DOWN)
            {
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::SAVE;
            }
            else if (pushed & JOY_LEFT)
            {
#if HW_CONFIG == 8
                settings.fruitjamVolumeLevel = std::max(-63, settings.fruitjamVolumeLevel - 1);
                EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
#endif
            }
            else if (pushed & JOY_RIGHT)
            {
#if HW_CONFIG == 8
                settings.fruitjamVolumeLevel = std::min(23, settings.fruitjamVolumeLevel + 1);
                EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
#endif
            }
        }
#endif

        prevButtons[i] = pcebuttons;
        prevButtonssystem[i] = pcesystem[i];
        prevOtherButtons[i] = otherButtons;
        if (pushed)
            dst = pcebuttons;
        if (pushedother)
        {
            if (pushedother & OTHER_BUTTON1)
                printf("Other 1\n");
            if (pushedother & OTHER_BUTTON2)
                printf("Other 2\n");
        }
    }

    // Mirror Player 1 across joypad registers so multitap counter cycling reads valid inputs
    for (int p = (usbConnected ? 2 : 1); p < 5; p++) {
        PCE.Joypad.regs[p] = PCE.Joypad.regs[0];
    }

    *pdwSystem = pcesystem[0] | pcesystem[1];
    if (reset)
    {
        // No SRAM to save for PCE (battery-backed RAM handled differently)
    }
}

void __not_in_flash_func(process)(void)
{
    DWORD pdwPad1, pdwPad2, pdwSystem;
    bool skipRender = false;
    uint32_t frameWorkStartUs = time_us_32();
    while (reset == false)
    {
#if USE_ILI9341
        if (s_request_pause_menu)
        {
            s_request_pause_menu = false;
            ili9341_blitter_stop();
#if PCE_DIAGNOSTICS
            pce_diag_pause();
#endif
            ingame_action_e act = ingame_menu(selectedRom);
            if (act == INGAME_ACTION_RESET) {
                reset = true;
                resetGame = true;
            } else if (act == INGAME_ACTION_EXIT) {
                reset = true;
                resetGame = false;
                selectedRom[0] = '\0';
            }
#if PCE_DIAGNOSTICS
            pce_diag_resume();
#endif
            ili9341_blitter_start();
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            /* One all-black frame before the game resumes.
             *
             * The menu drew straight to the panel over the whole surface, but
             * the game only streams the rows inside its own vertical range. The
             * letterbox rows above and below that range are filled only on a
             * geometry change, so a 224-line game kept 16 rows of menu pixels on
             * screen for the rest of the session -- the menu looked like it had
             * never closed. */
            ili9341_clear_black_frame();
            /* Clear the combo edge latches. The menu is driven by the same pad,
             * so a START+B still held on return would otherwise re-open it. */
            dmg_combo_reset();
            wait_for_all_ui_buttons_released();
            audio_pos = 0;
            frameWorkStartUs = time_us_32();
            if (reset) break;
        }
#endif
        readInputAndMapToPCE(&pdwPad1, &pdwPad2, &pdwSystem, false, nullptr);
#if USE_ILI9341
        /* Power switch OFF (or battery cutoff): save at this frame boundary and
         * sleep. Never returns -- the wake reboots back into the resume path. */
        if (s_request_power_off) {
            s_request_power_off = false;
            ili9341_blitter_stop();
#if PCE_DIAGNOSTICS
            /* A standby is an intentional stop, not a crash. Without this the
             * frame breadcrumb is still in watchdog scratch[0..3] when the wake
             * reboots, and pce_diag_boot() reports an "INTERRUPTED RUN" on top
             * of a resume that worked. Same call as a normal game exit. */
            pce_diag_shutdown();
#endif
            dmg_power_off_with_resume(&s_pce_desc, (dmg_power_reason_t)s_power_reason);
        }
#endif
        audio_pos = 0;
        pce_samples_this_frame = advance_samples_this_frame();
        gfx_set_skip_render(skipRender);
        pce_diag_stage(PCE_DIAG_FRAME_BEGIN);
        pce_diag_stage(PCE_DIAG_CPU);
        pce_run();
#if USE_ILI9341
        pce_diag_stage(PCE_DIAG_VIDEO_SUBMIT);
        ili9341_stream_pce_frame_end();
#endif
        pce_diag_stage(PCE_DIAG_AUDIO);
        pushAudioAndOverlay();
#if !HSTX && PICO_RP2350
        // Audio-clock pacing for CD games on the framebuffer DVI path, for both
        // HDMI and I2S (the I2S ring is enlarged to 4096 so it has the same
        // >=2-frame depth the audio-clock lock needs). cdAudioFillPermille()
        // reports whichever output is active. Disabled only for ext-audio
        // drivers that expose no fill level.
        bool cdFb = CD.cd_attached && Frens::isFrameBufferUsed();
#if EXT_AUDIO_IS_ENABLED && !USE_I2S_AUDIO
        if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
            cdFb = false; // SPI/other ext audio: no fill query â†’ timer pace
#endif
        Frens::setAudioPaceQuery(cdFb ? cdAudioFillPermille : nullptr);
#endif
        // Emulation+render time for this frame, EXCLUDING the pace wait below.
        // The skip trigger must compare work done against the frame budget;
        // measuring across the pace wait makes the interval equal the paced
        // frame period (~16.7ms), which exceeds any sub-frame threshold and
        // made skipRender alternate every frame regardless of load.
        uint32_t frameWorkUs = time_us_32() - frameWorkStartUs;
        ProcessAfterFrameIsRendered();
        pce_diag_frame_done();
        frameWorkStartUs = time_us_32();
#if USE_ILI9341
        /* Standby wake: put the game back where the switch left it, now that one
         * frame has run, through the same steps the pause menu's Load State
         * takes. Loading before the loop (as before) diverged from that path:
         * the sound engine or the pacing came up wrong (no audio on the MD,
         * slow SMS/WS) until a Reset. */
        if (s_resume_pending) {
            s_resume_pending = false;
            ili9341_blitter_stop();
            const bool resumed = pce_load_state(selectedRom, DMG_RESUME_SLOT);
            printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
            ili9341_blitter_start();
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_clear_black_frame();
            audio_pos = 0;
            frameWorkStartUs = time_us_32();
            dmg_resume_consume();
            dmg_power_arm_menu_loaded(1000);
        }
#endif
#if USE_ILI9341
        skipRender = false;
#elif PICO_RP2350
        // Frame-skip when the emulator can't hold the frame rate. Originally
        // for CD games (audio underrun â†’ HDMI drops / I2S crackles); now also
        // applied to HuCard / SGX scenes with heavy sprite work where the
        // PSG audio crackles for the same reason. Skipping rendering keeps
        // CPU+audio running at full speed while only the picture stalls.
        // Never skip twice in a row â†’ â‰¥30fps video.
        //
        // PicoDVI CD-FB path keeps the precise audio-ring "behind" signal
        // (only available there). Everything else falls back to the work-time
        // overrun: PCE_FRAME_SKIP_OVERRUN_US is tunable from CMakeLists.txt â€”
        // proactive (just under one 59.826Hz frame, 16715 Âµs) so we skip
        // before the audio queue actually underruns.
#ifndef PCE_FRAME_SKIP_OVERRUN_US
#define PCE_FRAME_SKIP_OVERRUN_US 16250u
#endif
#if !HSTX
        if (CD.cd_attached)
        {
            // PicoDVI CD: audio-ring fill is the authoritative signal.
            bool behind = cdFb && cdAudioFillPermille() < 250;
            skipRender = behind && !skipRender;
        }
        else
        {
            // PicoDVI HuCard / SGX: no audio-ring query â†’ work-time overrun.
            bool behind = frameWorkUs > PCE_FRAME_SKIP_OVERRUN_US;
            skipRender = behind && !skipRender;
        }
#else
        // HSTX (CD or HuCard / SGX): work-time overrun. CD has no audio-ring
        // query on core0 either, so both use the same trigger.
        bool behind = frameWorkUs > PCE_FRAME_SKIP_OVERRUN_US;
        skipRender = behind && !skipRender;
#endif

        // A/B experiment switch: build with -DPCE_DISABLE_FRAMESKIP=1 to never
        // skip rendering. Use it to decide whether the Sapphire "freeze" during
        // intensive action is the frameskip itself (freeze disappears, possibly
        // replaced by audio crackle / sub-60fps slowdown) or something downstream
        // in the display path (freeze persists). See project_sapphire_freeze_phase_a.
#if PCE_DISABLE_FRAMESKIP
        skipRender = false;
#endif
#endif

        // 1 Hz UART diagnostic of frameskip behaviour. Build with
        // -DPCE_FRAMESKIP_DIAG=1. Reports per second: total frames, how many
        // were render-skipped, the worst single-frame emulation+render time,
        // and (CD on PicoDVI) the minimum CD-DA ring fill permille â€” i.e. the
        // exact signals that trigger a skip. Capture this while reproducing the
        // Sapphire freeze to see if skips spike with the visible stall.
#if PCE_FRAMESKIP_DIAG
        {
            static uint32_t fs_frames = 0, fs_skips = 0, fs_maxwork = 0, fs_t0 = 0;
            static uint32_t fs_minfill = 1000;
            fs_frames++;
            if (skipRender) fs_skips++;
            if (frameWorkUs > fs_maxwork) fs_maxwork = frameWorkUs;
#if !HSTX
            if (CD.cd_attached && cdFb)
            {
                uint32_t fill = cdAudioFillPermille();
                if (fill < fs_minfill) fs_minfill = fill;
            }
#endif
            uint32_t fs_now = time_us_32();
            if (fs_t0 == 0) fs_t0 = fs_now;
            if (fs_now - fs_t0 >= 1000000u)
            {
                printf("[fsdiag] frames=%lu skips=%lu maxwork_us=%lu minfill=%lu\n",
                       (unsigned long)fs_frames, (unsigned long)fs_skips,
                       (unsigned long)fs_maxwork, (unsigned long)fs_minfill);
                fs_frames = fs_skips = fs_maxwork = 0;
                fs_minfill = 1000;
                fs_t0 = fs_now;
            }
        }
#endif
    }
    gfx_set_skip_render(false);
}


int main()
{
#if PCE_DIAGNOSTICS
    // Recover before display/SD/PSRAM setup can touch shared SRAM.
    pce_diag_boot();
#endif
    romName = selectedRom;
    ErrorMessage[0] = selectedRom[0] = 0;

    int fileSize = 0;
    vreg_voltage voltage = VREG_VOLTAGE_1_30;
#if SGX
    Frens::FlashParams *flashParams;
    // assign flashParams to point to flash location

    bool freqOverruled = false;
    flashParams = (Frens::FlashParams *)FLASHPARAM_ADDRESS;
    if ( Frens::validateFlashParams(*flashParams) ) {
        CPUFreqKHz = flashParams->cpuFreqKHz;
        voltage = flashParams->voltage;
        freqOverruled = true;
    }
#endif
#if USE_ILI9341
    CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;
#endif
    Frens::setClocksAndStartStdio(CPUFreqKHz, voltage);

    printf("==========================================================================================\n");
    printf("Pico-PCE+ %s\n", SWVERSION);
    printf("Build date: %s\n", __DATE__);
    printf("Build time: %s\n", __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
    printf("Stack size: %d bytes\n", PICO_STACK_SIZE);
    printf("==========================================================================================\n");
    printf("Starting up...\n");

    FrensSettings::initSettings(FrensSettings::emulators::PCE);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0, AUDIOBUFFERSIZE, false, true);
    scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);

#if USE_ILI9341
    ROM_FILE_ADDR = PCE_ROM_CACHE_ABS;
    init_ui_buttons();
    dmg_init_boot_mode();
    load_picopce_config();
    if (dmg_is_gb_mode() && persistent_scale_mode > 1 && current_screen_w > 266) {
        persistent_scale_mode = SCALE_FIT;
    }
    set_backlight_brightness(persistent_brightness_level);
    ili9341_set_scale_mode(persistent_scale_mode);
    ili9341_set_scanlines(persistent_scanlines_enabled);
    ili9341_set_shimmerless(persistent_shimmerless_enabled);
    ili9341_fill_screen(0x0000);
    pce_diag_render_report();
#endif

    // CD BIOS self-test: scan /bios/ once at boot and log what we find.
    // This is purely diagnostic â€” actual BIOS loading happens in LoadDisc()
    // when the user selects a .cue file. Requires PSRAM and a mounted SD.
    //
    // All FatFS-heavy locals are static. The 3 KB main stack would overflow
    // into core1's SCRATCH region with deep call chains here (DIR + FILINFO
    // + multiple FF_MAX_LFN buffers + cd_find_bios's own static-now buffers
    // is fine, but stack-allocating here on top is not).
#if CHECK_BIOS_AT_BOOT
    if (!isFatalError && Frens::isPsramEnabled())
    {
        printf("--- CD BIOS self-test ---\n");
        static char     biosPath[FF_MAX_LFN + 8];
        bios_variant_t  variant = BIOS_UNKNOWN;
        if (cd_find_bios(biosPath, sizeof(biosPath), NULL, &variant) == 0) {
            printf("Found CD BIOS: %s (region=%s)\n",
                   biosPath, cd_bios_is_us() ? "US" : "JP");
        } else {
            printf("No CD BIOS in /bios/ â€” CD games will fail to load until one is added.\n");
        }
        printf("-------------------------\n");

        // CUE parser diagnostic: parse every .cue file in /diag/ (if any).
        // Drop CUE+BIN pairs into /diag/ to validate parsing without booting.
        static DIR     diagDir;
        static FILINFO diagFno;
        static char    diagCuePath[FF_MAX_LFN + 8];
        if (f_opendir(&diagDir, "/diag") == FR_OK) {
            printf("--- CUE parser diagnostic (/diag/) ---\n");
            int count = 0;
            while (f_readdir(&diagDir, &diagFno) == FR_OK && diagFno.fname[0]) {
                if (diagFno.fattrib & (AM_DIR | AM_HID)) continue;
                size_t n = strlen(diagFno.fname);
                if (n < 4 || strcasecmp(diagFno.fname + n - 4, ".cue") != 0) continue;

                snprintf(diagCuePath, sizeof(diagCuePath), "/diag/%s", diagFno.fname);
                printf("[diag] parsing %s\n", diagCuePath);
                if (cd_load_cue(diagCuePath) == 0) {
                    printf("[diag] OK\n");
                } else {
                    printf("[diag] FAILED\n");
                }
                cd_close();
                count++;
            }
            f_closedir(&diagDir);
            if (count == 0) {
                printf("[diag] /diag/ is empty (no .cue files)\n");
            }
            printf("--------------------------------------\n");
        }
    }
#endif

    buildPaletteLUT();

#if USE_ILI9341
    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_pce_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_pce_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_pce_desc);
            snprintf(selectedRom, sizeof(selectedRom), "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_pce_desc)) dmg_boot_intro_finish();
        }
    }
    if (s_resume_pending) {
        snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", dmg_get_current_rom_dir());
    }
#endif

    bool showSplash = true;
    g_settings_visibility = g_settings_visibility_pce;
    g_available_screen_modes = g_available_screen_modes_pce;

    while (true)
    {
        if (strlen(selectedRom) == 0 || reset == true)
        {
#if USE_ILI9341
            if (s_resume_pending) {
                s_resume_pending = false;
                dmg_resume_consume();
            }
            dmg_boot_intro_finish();
            if (!rom_file_selector(selectedRom, sizeof(selectedRom))) {
                selectedRom[0] = '\0';
                continue;
            }
            printf("Selected rom: %s\n", selectedRom);
#else
            // CD-ROM games (.cue) require PSRAM for the additional ~2.6MB of
            // CD/SCD/ADPCM/Arcade Card buffers.
            // SuperGrafx (.sgx) needs an extra 64 KB VRAM2 that doesn't fit in
            // the SRAM heap alongside the 150 KB framebuffer; we route it to
            // PSRAM and therefore require PSRAM for .sgx as well. Most SGX
            // carts are 1 MB anyway, which already needs PSRAM to hold the ROM.
            const char *menuExts;
#if PICO_RP2350
#if ENABLE_CHD
            menuExts = Frens::isPsramEnabled() ? ".pce .cue .chd .sgx" : ".pce";
#else
            menuExts = Frens::isPsramEnabled() ? ".pce .cue .sgx" : ".pce";
#endif
#else
            menuExts = ".pce";
#endif
            menu("Pico-PCE+", ErrorMessage, isFatalError, showSplash, menuExts, selectedRom);
            printf("Selected rom from menu: %s\n", selectedRom);
#endif
        }
        reset = false;
        fileSize = 0;

        // Detect CD-ROM / SuperGrafx / HuCard by file extension.
        bool isCDGame = false;
        bool isSgxGame = false;
        if (strlen(selectedRom) > 0) {
            char ext[8];
            Frens::getextensionfromfilename(selectedRom, ext, sizeof(ext));
            isCDGame  = (strcasecmp(ext, ".cue") == 0
#if ENABLE_CHD
                      || strcasecmp(ext, ".chd") == 0
#endif
                        );
            isSgxGame = (strcasecmp(ext, ".sgx") == 0);
        }

        if (isCDGame)
        {
            // A HuCard previously loaded to PSRAM (pce_load_rom_to_psram)
            // leaves its f_malloc'd buffer at ROM_FILE_ADDR (0x11000000).
            // Free it before LoadDisc() carves out ~2.6 MB of CD/SCD/ADPCM/
            // Arcade Card buffers from the same PSRAM heap. s_rom_in_psram is
            // authoritative: the old `>= 0x20000000u` range test never matched
            // the 0x11000000 PSRAM region, and ROM_FILE_ADDR may otherwise be a
            // flash XIP address (PCE_ROM_CACHE_ABS under ILI9341), which must
            // NOT be passed to f_free.
#if PICO_RP2350 && PSRAM_CS_PIN
#if USE_ILI9341
            // A HuCard previously loaded to PSRAM (pce_load_rom_to_psram)
            // leaves its f_malloc'd buffer at ROM_FILE_ADDR (0x11000000).
            // Free it before LoadDisc() carves out ~2.6 MB of CD/SCD/ADPCM/
            // Arcade Card buffers from the same PSRAM heap. s_rom_in_psram is
            // authoritative: the old `>= 0x20000000u` range test never matched
            // the 0x11000000 PSRAM region, and ROM_FILE_ADDR may otherwise be a
            // flash XIP address (PCE_ROM_CACHE_ABS), which must NOT be freed.
            if (Frens::isPsramEnabled() && s_rom_in_psram && ROM_FILE_ADDR) {
                printf("[I] Freeing PSRAM HuCard ROM before CD load\n");
                Frens::f_free((void *)ROM_FILE_ADDR);
                ROM_FILE_ADDR = 0;
                s_rom_in_psram = false;
            }
#else
            // DVI/HSTX menu's PSRAM loader copied the (small) .cue text into
            // SRAM/PSRAM at ROM_FILE_ADDR (>= 0x20000000u), and LoadDisc()
            // re-parses the CUE directly from SD, so that copy is freed first.
            if (Frens::isPsramEnabled() && ROM_FILE_ADDR && (uintptr_t)ROM_FILE_ADDR >= 0x20000000u) {
                Frens::f_free((void *)ROM_FILE_ADDR);
                ROM_FILE_ADDR = 0;
            }
#endif
#endif
            // The shared_menu selector returns the bare filename; LoadDisc()
            // expects a full path so cd_find_bios/cd_load_cue/cd_chd_open can
            // locate the BIOS in the disc's folder and open the CUE/CHD via SD.
            if (strchr(selectedRom, '/') != NULL || current_rom_dir[0] == '\0') {
                snprintf(s_full_path, sizeof(s_full_path), "%s", selectedRom);
            } else {
                size_t dlen = strlen(current_rom_dir);
                if (dlen > 0 && current_rom_dir[dlen - 1] == '/') {
                    snprintf(s_full_path, sizeof(s_full_path), "%s%s", current_rom_dir, selectedRom);
                } else {
                    snprintf(s_full_path, sizeof(s_full_path), "%s/%s", current_rom_dir, selectedRom);
                }
            }
            printf("Now playing (CD): %s\n", s_full_path);
        }
#if USE_ILI9341
        else
        {
            if (!load_cart_rom_file(selectedRom)) {
                printf("E load_cart_rom_file failed for '%s'\n", selectedRom);
                selectedRom[0] = '\0';
                continue;
            }
            fileSize = last_flashed_rom_size;
            /* The config file is the source of truth for how the game is
             * displayed, so it is re-read when a game is entered, not only at
             * boot. Safe to repeat because every menu visit commits first, so
             * the file always matches what was on screen when the menu closed.
             */
            load_picopce_config();
            if (dmg_is_gb_mode() && persistent_scale_mode > 1 && current_screen_w > 266) {
                persistent_scale_mode = SCALE_FIT;
            }
            ili9341_set_scale_mode(persistent_scale_mode);
            ili9341_set_scanlines(persistent_scanlines_enabled);
            ili9341_set_shimmerless(persistent_shimmerless_enabled);
            ili9341_set_interleave(persistent_interleave);
            set_backlight_brightness(persistent_brightness_level);
        }
#else
        else if (Frens::isPsramEnabled())
        {
#if PICO_RP2350 && PSRAM_CS_PIN
            PicoPlusPsram &psram_ = PicoPlusPsram::getInstance();
            fileSize = psram_.GetSize((void *)ROM_FILE_ADDR);
            if (fileSize == 0)
            {
                printf("No rom loaded, continuing to menu\n");
                selectedRom[0] = 0;
                continue;
            }
#endif
        }
        else
        {
            FRESULT fr;
            FIL file;
            fr = f_open(&file, selectedRom, FA_READ);
            if (fr != FR_OK)
            {
                snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot open rom %d", fr);
                printf("%s\n", ErrorMessage);
                selectedRom[0] = 0;
                continue;
            }
            fileSize = f_size(&file);
            f_close(&file);
        }
#endif

        if (!isCDGame)
            printf("Now playing: %s (%d bytes)\n", selectedRom, fileSize);

        if (isAutoSaveStateConfigured())
        {
            char tmpPath[40];
            getAutoSaveStatePath(tmpPath, sizeof(tmpPath));
            if (Frens::fileExists(tmpPath))
            {
                printf("Auto-save state found: %s\n", tmpPath);
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::LOAD_AND_START;
                framesbeforeAutoStateIsLoaded = 120;
            }
        }

        do
        {
            reset = resetGame = false;
            SetSgxModePCE(isSgxGame);
            InitPCE(PCE_AUDIO_RATE, true);
            int loadResult;
            if (isCDGame) {
                loadResult = LoadDisc(s_full_path);
                // Distinguish the failing step: a single "CD load failed" said
                // nothing about which of the eight steps in LoadDisc() gave up.
                switch (loadResult) {
                    case DISC_OK:                  break;
                    case DISC_ERR_NO_BIOS:         snprintf(ErrorMessage, ERRORMESSAGESIZE, "No BIOS in /bios/"); break;
                    case DISC_ERR_BIOS_OPEN:       snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot open BIOS"); break;
                    case DISC_ERR_BIOS_SIZE:       snprintf(ErrorMessage, ERRORMESSAGESIZE, "BIOS size invalid"); break;
                    case DISC_ERR_BIOS_ALLOC:      snprintf(ErrorMessage, ERRORMESSAGESIZE, "No PSRAM for BIOS"); break;
                    case DISC_ERR_BIOS_READ:       snprintf(ErrorMessage, ERRORMESSAGESIZE, "BIOS read failed"); break;
                    case DISC_ERR_CD_ALLOC:        snprintf(ErrorMessage, ERRORMESSAGESIZE, "No PSRAM for CD RAM"); break;
                    case DISC_ERR_IMAGE_OPEN:      snprintf(ErrorMessage, ERRORMESSAGESIZE, "Disc image open failed"); break;
                    default:                       snprintf(ErrorMessage, ERRORMESSAGESIZE, "CD load failed %d", loadResult); break;
                }
            } else {
                loadResult = LoadCard((uint8_t *)ROM_FILE_ADDR, fileSize);
                if (loadResult != 0) {
                    snprintf(ErrorMessage, ERRORMESSAGESIZE, "ROM load failed");
                }
            }
            if (loadResult != 0)
            {
                printf("%s\n", ErrorMessage);
                PCE.ROM = NULL;
                break;
            }

            if (isCDGame) {
                char bramPath[50];
                snprintf(bramPath, sizeof(bramPath),
                         SAVESTATEDIR "/%s/%08X/bram.sav",
                         FrensSettings::getEmulatorTypeString(),
                         Frens::getCrcOfLoadedRom());
                cd_bram_load(bramPath);
            }

            printf("Starting game\n");
            // CHD on HSTX needs a larger core1 stack (libchdr's chd_read uses
            // ~6-10 KB; default is 4 KB and overflowing zeroes the hunk-cache
            // pointer array â†’ silent CD audio). Growing the static stack
            // permanently starves SGX's SRAM heap, so swap core1's stack only
            // for the duration of this CHD game by tearing HSTX down,
            // resetting core1, and relaunching it with a fresh 8 KB heap
            // buffer. The pointer is remembered here so the exit branch can
            // restore the default stack and free the heap buffer.
#if HSTX && ENABLE_CHD
            uint32_t *chd_core1_stack = nullptr;
            if (isCDGame && cd_chd_is_active()) {
                chd_core1_stack = (uint32_t *)malloc(8192);
                if (chd_core1_stack) {
                    printf("[hstx] swapping core1 stack: default 4 KB -> 8 KB malloc for CHD\n");
                    hstx_restart_core1(chd_core1_stack, 8192);
                } else {
                    printf("[hstx] WARN: malloc(8192) for CHD core1 stack failed; CD audio will be silent\n");
                }
            }
#endif
            if (isCDGame) {
                cd_audio_buffer = (int16_t *)malloc(PCE_SAMPLES_PER_FRAME_MAX * 2 * sizeof(int16_t));
                if (!cd_audio_buffer)
                    printf("WARN: malloc cd_audio_buffer failed; CD audio will be silent\n");
#if HSTX
                extern void video_output_set_background_task(void (*)(void));
                video_output_set_background_task(cd_audio_update);
#else
                // Framebuffer DVI: prefetch CD audio during the pace slack-wait.
                if (Frens::isFrameBufferUsed())
                    Frens::setVSyncWaitTask(cd_audio_update);
#endif
            }
            Frens::PaceFrames60fps(true, !isCDGame);
#if PCE_DIAGNOSTICS
            pce_diag_game_begin();
#endif
#if USE_ILI9341
            dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */
            /* HuCard and CD alike: the in-game state also restores CD games
             * (validated with Rondo of Blood in CHD). */
            s_cd_running = isCDGame;
            dmg_power_set_game(selectedRom, selectedRom);
            ili9341_blitter_start();
#endif
            process();
#if USE_ILI9341
            dmg_power_set_game(NULL, NULL);
            s_cd_running = false;
#endif
#if USE_ILI9341
            ili9341_blitter_stop();
#endif
#if PCE_DIAGNOSTICS
            pce_diag_shutdown();
#endif
            if (isCDGame) {
#if HSTX
                extern void video_output_set_background_task(void (*)(void));
                video_output_set_background_task(nullptr);
#else
                Frens::setVSyncWaitTask(nullptr);
#endif
                char bramPath[50];
                snprintf(bramPath, sizeof(bramPath), SAVESTATEDIR);
                f_mkdir(bramPath);
                snprintf(bramPath, sizeof(bramPath),
                         SAVESTATEDIR "/%s",
                         FrensSettings::getEmulatorTypeString());
                f_mkdir(bramPath);
                snprintf(bramPath, sizeof(bramPath),
                         SAVESTATEDIR "/%s/%08X",
                         FrensSettings::getEmulatorTypeString(),
                         Frens::getCrcOfLoadedRom());
                f_mkdir(bramPath);
                snprintf(bramPath, sizeof(bramPath),
                         SAVESTATEDIR "/%s/%08X/bram.sav",
                         FrensSettings::getEmulatorTypeString(),
                         Frens::getCrcOfLoadedRom());
                cd_bram_save(bramPath);
                cd_close();
                free(cd_audio_buffer);
                cd_audio_buffer = nullptr;
            } else {
                PCE.ROM = NULL; // prevent ShutdownPCE from freeing flash/PSRAM
            }
#if HSTX && ENABLE_CHD
            if (chd_core1_stack) {
                size_t default_bytes = 0;
                void *default_stack = hstx_default_core1_stack(&default_bytes);
                printf("[hstx] restoring core1 stack to default 4 KB\n");
                hstx_restart_core1((uint32_t *)default_stack, default_bytes);
                free(chd_core1_stack);
                chd_core1_stack = nullptr;
            }
#endif
            ShutdownPCE();
        } while (resetGame);

        selectedRom[0] = 0;
        showSplash = false;
#if ENABLE_VU_METER
        turnOffAllLeds();
#endif
    }

    return 0;
}
