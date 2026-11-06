/**
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

// Peanut-GB emulator settings
#ifndef ENABLE_LCD
#define ENABLE_LCD 1
#endif
#ifndef ENABLE_SOUND
#define ENABLE_SOUND 1
#endif
#ifndef ENABLE_SDCARD
#define ENABLE_SDCARD 1
#endif
#ifndef ENABLE_NFC
#define ENABLE_NFC 0
#endif
#ifndef USE_DMG_BUTTON_MATRIX
#define USE_DMG_BUTTON_MATRIX 0
#endif
#define AUDIO_DEFAULT_VOLUME 3
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_USE_BIOS 0
/* Enable full Game Boy Color emulation (wram/vram banking, double speed,
 * 15-bit colour palettes). Requires the gbc-rtc-fix Peanut-GB core. */
#ifndef PEANUT_FULL_GBC_SUPPORT
#define PEANUT_FULL_GBC_SUPPORT 1
#endif

#ifndef GBC_IS_DMG_BUILD
#define GBC_IS_DMG_BUILD 0
#endif

#ifndef GB_APP_LABEL
#if GBC_IS_DMG_BUILD
#define GB_APP_LABEL "PicoDMG"
#else
#define GB_APP_LABEL "PicoGBC"
#endif
#endif

#if GBC_IS_DMG_BUILD
#define GB_SAVE_DIR_0           "0:/saves/gb"
#define GB_SAVE_DIR_ROOT        "/saves/gb"
#define GB_CONFIG_PATH_0        "0:/saves/gb/picodmg.cfg"
#define GB_CONFIG_PATH_ROOT     "/saves/gb/picodmg.cfg"
#define GB_CONFIG_FALLBACK_0    "0:/saves/gb/picogbc.cfg"
#define GB_CONFIG_FALLBACK_ROOT "/saves/gb/picogbc.cfg"
#define GB_DEFAULT_ROM_DIR      "0:/roms/gb"
#else
#define GB_SAVE_DIR_0           "0:/saves/gbc"
#define GB_SAVE_DIR_ROOT        "/saves/gbc"
#define GB_CONFIG_PATH_0        "0:/saves/gbc/picogbc.cfg"
#define GB_CONFIG_PATH_ROOT     "/saves/gbc/picogbc.cfg"
#define GB_CONFIG_FALLBACK_0    "0:/saves/gbc/picodmg.cfg"
#define GB_CONFIG_FALLBACK_ROOT "/saves/gbc/picodmg.cfg"
#define GB_DEFAULT_ROM_DIR      "0:/roms/gbc"
#endif

/* Use DMA for all drawing to LCD. Benefits aren't fully realised at the moment
 * due to busy loops waiting for DMA completion. */
#define USE_DMA 0

/**
 * Reducing VSYNC calculation to lower multiple.
 * When setting a clock IRQ to DMG_CLOCK_FREQ_REDUCED, count to
 * SCREEN_REFRESH_CYCLES_REDUCED to obtain the time required each VSYNC.
 * DMG_CLOCK_FREQ_REDUCED = 2^18, and SCREEN_REFRESH_CYCLES_REDUCED = 4389.
 * Currently unused.
 */
#define VSYNC_REDUCTION_FACTOR 16u
#define SCREEN_REFRESH_CYCLES_REDUCED                                          \
  (SCREEN_REFRESH_CYCLES / VSYNC_REDUCTION_FACTOR)
#define DMG_CLOCK_FREQ_REDUCED (DMG_CLOCK_FREQ / VSYNC_REDUCTION_FACTOR)

/* C Headers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RP2040 Headers */
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/flash.h>
#include <hardware/irq.h>
#include <hardware/pio.h>
#include <hardware/pwm.h>
#include <hardware/spi.h>
#include <hardware/sync.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#include <hardware/watchdog.h>
#include <pico/bootrom.h>
#include <pico/multicore.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <sys/unistd.h>

/* pico-bootLoader handshake protocol */
#define LOADER_LAUNCH_MAGIC   0xB007ED01u
#define LOADER_RETURN_MAGIC   0xB007BACEu
#define LOADER_LAUNCH_SCRATCH 6
#define LOADER_RETURN_SCRATCH 7

static inline bool is_launched_from_bootloader(void) {
  return watchdog_hw->scratch[LOADER_LAUNCH_SCRATCH] == LOADER_LAUNCH_MAGIC;
}

static inline void reboot_to_bootloader(void) {
  watchdog_hw->scratch[LOADER_RETURN_SCRATCH] = LOADER_RETURN_MAGIC;
  watchdog_reboot(0, 0, 0);
  for (;;) tight_loop_contents();
}

void __attribute__((noreturn)) _exit(int status) {
  (void)status;
  if (is_launched_from_bootloader()) {
    reboot_to_bootloader();
  }
  watchdog_reboot(0, 0, 0);
  for (;;) tight_loop_contents();
}

/* Project headers */
#include "gbcolors.h"
#include "gb_rumble.h"
#include "hedley.h"
#include "i2s.h"
#include "minigb_apu.h"
#include "mk_ili9225.h"
#include "dmg_menu.h"
#include "dmg_combo.h"
#include "dmg_resume.h"
#include "dmg_boot_intro.h"
#include "peanut_gb.h"
#include "sdcard.h"

/* FatFs SPI driver: redirect SD-card DMA interrupts to DMA_IRQ_1 so they
 * don't collide with the display driver's exclusive handler on DMA_IRQ_0.
 * Declared here instead of including spi.h to avoid header-name ambiguity
 * with hardware/spi.h. */
extern void set_spi_dma_irq_channel(bool useChannel1, bool shared);
#if ENABLE_NFC
#include "mfrc522.h"
#endif
#if ENABLE_SOUND
#include "boot_sound_data.h"
#endif

/* GPIO Connections. */
#if USE_DMG_BUTTON_MATRIX
#define GPIO_P10 6
#define GPIO_P11 2
#define GPIO_P12 5
#define GPIO_P13 4
#define GPIO_P14 3
#define GPIO_P15 7
#else
#define GPIO_UP 2
#define GPIO_DOWN 3
#define GPIO_LEFT 4
#define GPIO_RIGHT 5
#define GPIO_A 6
#define GPIO_B 7
#define GPIO_SELECT 8
#define GPIO_START 9
#endif
#define GPIO_CS 17
#define GPIO_CLK 18
#define GPIO_SDA 19
#define GPIO_RS 20
#define GPIO_RST 21
#define GPIO_LED 22

#define ROM_SELECTOR_PAGE_SIZE 9
#define ROM_SELECTOR_AUTOSTART_US (10ULL * 1000ULL * 1000ULL)
#ifndef DEFAULT_INTERLACE
#define DEFAULT_INTERLACE 0
#endif
#ifndef NFC_MAP_FILE
#define NFC_MAP_FILE "nfc_games.csv"
#endif
#ifndef RP2040_GB_SYS_CLOCK_VCO
#define RP2040_GB_SYS_CLOCK_VCO 1596000000u
#endif
#ifndef RP2040_GB_SYS_CLOCK_DIV1
#define RP2040_GB_SYS_CLOCK_DIV1 6u
#endif
#ifndef RP2040_GB_SYS_CLOCK_DIV2
#define RP2040_GB_SYS_CLOCK_DIV2 1u
#endif

#if defined(DISPLAY_ST7789) || defined(DISPLAY_ILI9341)
#define ROM_SELECTOR_X 42u
#define ROM_SELECTOR_Y 0u
#define LOADING_SCREEN_X 18u
#define LOADING_SCREEN_WIDTH 320u
#define LOADING_SCREEN_HEIGHT 240u
#else
#define ROM_SELECTOR_X 0u
#define ROM_SELECTOR_Y 0u
#define LOADING_SCREEN_X 0u
#define LOADING_SCREEN_WIDTH SCREEN_SIZE_X
#define LOADING_SCREEN_HEIGHT SCREEN_SIZE_Y
#endif

#define LOADING_ANIM_FALL_DURATION_US (3ULL * 1000ULL * 1000ULL)
#define LOADING_ANIM_MIN_VISIBLE_US (4ULL * 1000ULL * 1000ULL)
#define LOADING_ANIM_TEXT "NINTENDO"
#define LOADING_ANIM_TEXT_WIDTH 128u
#define LOADING_ANIM_TEXT_HEIGHT 16u
#define LOADING_ANIM_TARGET_Y                                                  \
  ((LOADING_SCREEN_HEIGHT / 2u) - (LOADING_ANIM_TEXT_HEIGHT / 2u))
#define LOADING_ANIM_START_Y 0u

#if ENABLE_SOUND
/**
 * Global variables for audio task
 * stream contains N=AUDIO_SAMPLES samples
 * each sample is 32 bits
 * 16 bits for the left channel + 16 bits for the right channel in stereo
 * interleaved format) This is intended to be played at AUDIO_SAMPLE_RATE Hz
 */
uint16_t *stream;
static i2s_config_t i2s_config;
/* Standby by save state (shared_menu/dmg_power.h). */
static bool s_request_power_off = false;
static uint8_t s_power_reason = 0;
static bool s_resume_pending = false;
static bool i2s_config_ready = false;
#endif

#ifndef BTLDR_APP_BASE
#define BTLDR_APP_BASE 0x10080000u
#endif
#define ROM_CACHE_HEADROOM (1024 * 1024)
#define ROM_FLASH_MAX_SIZE (4 * 1024 * 1024)
#define FLASH_TARGET_OFFSET_ABS (BTLDR_APP_BASE + ROM_CACHE_HEADROOM)
#define FLASH_TARGET_OFFSET (FLASH_TARGET_OFFSET_ABS - XIP_BASE)
#define ROM_CACHE_LENGTH_ROMS (4 * 1024 * 1024)
#define FLASH_LAST_ROM_MARKER_OFFSET (FLASH_TARGET_OFFSET + ROM_CACHE_LENGTH_ROMS)
#define FLASH_LAST_ROM_MARKER_MAGIC 0x4C524D50u /* 'PRML' */

typedef struct {
  uint32_t magic;
  uint32_t size;
} last_rom_marker_t;

static const uint8_t *rom = (const uint8_t *)(uintptr_t)FLASH_TARGET_OFFSET_ABS;
static uint32_t rom_size = ROM_FLASH_MAX_SIZE;

// ---------------------------------------------------------------------------
// PSRAM cartridge path (console build)
// ---------------------------------------------------------------------------
// On the Core2350B the cartridge ROM is read straight off the SD card into
// PSRAM and the emulator runs from there. The flash cache exists only to hold
// the ROM on boards that have nowhere else to put it, and programming a 512 KB
//..8 MB cart into flash on this board is actively harmful: the erase/program
// cycle is slow (visible as a multi-second "writing" stall), it burns the card
// the 16 MB part is there to provide, and it is pure risk next to the
// firmware's own sectors.
//
// ROM_IN_PSRAM is the single source of truth for "the ROM lives in PSRAM",
// rather than testing pointer addresses: the PSRAM window is at 0x11000000,
// which is neither flash XIP (>= 0x10000000) nor SRAM (0x20000000), so the
// address-based checks the flash path uses would not recognise it.
// Set once the PSRAM read finishes, so the flash path below is skipped. Without
// it both paths would run and the flash would erase sectors the ROM is in.
// Outside the GB_USE_PSRAM guard because load_cart_rom_file() tests it
// unconditionally: on a flash-only build it is simply never set.
static bool s_psram_load_done = false;

#if GB_USE_PSRAM
#include "gb_psram.h"
#include "lwmem/lwmem.h"
static size_t s_psram_bytes = 0;
static uint8_t *s_rom_in_psram = NULL;
// Cleared at boot if no chip answers, so a failed detect falls back to the flash
// cache instead of hanging the load. A PSRAM that is mapped but mis-timed still
// hangs, which is why the detect happens after the final clock is set.
static bool s_use_psram = true;
// Byte counter for the loading bar. File-static because the PSRAM reader runs
// before load_cart_rom_file()'s own local of the same name is in scope.
static uint32_t bytes_loaded = 0;

// Reads the whole cart from the SD card into a PSRAM allocation. Returns true
// on success and leaves s_rom_in_psram / rom / rom_size set.
//
// The descriptor is passed in rather than referenced directly: this function
// sits above s_gb_desc's definition, and hardcoding the name here would need a
// second forward declaration. It is used to redraw the progress bar every chunk
// -- without that the bar only ever jumps from 0 to 100 when the whole read is
// already done, which looks exactly like a hang.
static bool gb_load_rom_to_psram(FIL *fil, uint32_t rom_file_size,
                                 const dmg_emulator_desc_t *desc) {
  if (rom_file_size == 0) {
    return false;
  }
  // Keep a margin so a later allocation does not run the allocator into the
  // wall. The emulator allocates cart RAM from the same pool.
  const size_t margin = 128 * 1024;
  if (s_psram_bytes == 0 || rom_file_size + margin > s_psram_bytes) {
    printf("E ROM too large for PSRAM: %u + %u > %lu\n", rom_file_size,
           (uint32_t)margin, (unsigned long)s_psram_bytes);
    return false;
  }

  // Release the previous cart before allocating the next one, otherwise
  // switching games leaks the old ROM every time.
  if (s_rom_in_psram) {
    lwmem_free(s_rom_in_psram);
    s_rom_in_psram = NULL;
  }

  s_rom_in_psram = (uint8_t *)lwmem_malloc(rom_file_size);
  if (!s_rom_in_psram) {
    printf("E lwmem_malloc(%u) failed\n", rom_file_size);
    return false;
  }

  // Read in chunks so a short read ends the loop instead of leaving the tail of
  // the buffer uninitialised, and so the bar can move during a long read.
  //
  // 64 KB rather than 16 KB: the SD bus already bursts via CMD18 (see
  // in_sd_read_blocks, which uses CMD18 when blockCnt > 1), so this does not
  // change the on-wire transfer. It cuts the f_read call count for a 4 MB cart
  // from 256 to 64, and the measured efficiency of 63% of bus peak at 16 KB was
  // constant across 12.5 and 20 MHz -- a cost that scales with data, not a
  // startup cost -- which points at per-call FatFS overhead (cluster chain walk,
  // FAT sector read) as the thing worth attacking. Writing straight into
  // s_rom_in_psram means no staging buffer and no extra allocation.
  const uint32_t chunk = 64 * 1024;
  uint32_t done = 0;
  // Progress is redrawn on a 2% grid rather than per chunk. Drawing per chunk
  // costs a fill_rect plus an snprintf each time, and the GBC's display adapter
  // has no flush_rows, so the work never reaches the panel anyway. The 2% grid
  // is ~50 updates for a 4 MB cart, still smooth to the eye. The watchdog is fed
  // every chunk regardless: that is about liveness, not display.
  const uint32_t pct_step = rom_file_size / 50u;   /* ~2% */
  uint32_t next_pct_at = 0;
  const uint32_t t0 = time_us_32();
  while (done < rom_file_size) {
    uint32_t want = rom_file_size - done;
    if (want > chunk) want = chunk;
    UINT br = 0;
    if (f_read(fil, s_rom_in_psram + done, want, &br) != FR_OK || br != want) {
      printf("E f_read failed at %lu\n", (unsigned long)done);
      lwmem_free(s_rom_in_psram);
      s_rom_in_psram = NULL;
      return false;
    }
    done += br;
    bytes_loaded = done;
    watchdog_update();
#if ENABLE_LCD
    if (desc && done >= next_pct_at) {
      dmg_update_loading_progress((dmg_emulator_desc_t *)desc, done, rom_file_size);
      next_pct_at = done + pct_step;
    }
#endif
  }

  rom = s_rom_in_psram;
  rom_size = rom_file_size;
  {
    const uint32_t us = time_us_32() - t0;
    printf("[I] ROM loaded to PSRAM: %u bytes at %p in %lu ms\n",
           rom_file_size, (void *)s_rom_in_psram, (unsigned long)(us / 1000u));
  }
  return true;
}
#endif // GB_USE_PSRAM

#if defined(RP2040_GB_HAS_BUILTIN_ROM)
extern const uint8_t rp2040_gb_builtin_rom[];
extern const unsigned int rp2040_gb_builtin_rom_size;
#endif
static unsigned char rom_bank0[65536];
static uint32_t last_flashed_rom_size = 0;
static bool last_rom_marker_loaded = false;

static void load_last_rom_marker(void) {
  const volatile last_rom_marker_t *m =
      (const volatile last_rom_marker_t *)(XIP_BASE +
                                           FLASH_LAST_ROM_MARKER_OFFSET);
  last_flashed_rom_size =
      (m->magic == FLASH_LAST_ROM_MARKER_MAGIC) ? m->size : 0;
  last_rom_marker_loaded = true;
}

static void store_last_rom_marker(uint32_t size) {
  last_rom_marker_t m;
  m.magic = FLASH_LAST_ROM_MARKER_MAGIC;
  m.size = size;
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(FLASH_LAST_ROM_MARKER_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(FLASH_LAST_ROM_MARKER_OFFSET, (const uint8_t *)&m,
                      sizeof m);
  restore_interrupts(ints);
}

/* Cartridge SRAM backing store.
 *
 * 64 KB covers header 0x0149 = 0x05, which is the largest size any real
 * cartridge declares. 0x04 (128 KB) exists in the spec but has no commercial
 * software, and sizing for it would take most of the free heap (~171 KB) and
 * starve the HW16 lwmem allocator.
 *
 * The size matters because the core derives its addressing from 0x0149 alone
 * (num_ram_banks * CRAM_BANK_SIZE) and never checks it against this buffer. A
 * cart claiming 128 KB therefore addresses twice this array, and what sits
 * immediately after it in SRAM is rom_bank0 -- the live 64 KB ROM cache
 * window. Overflow corrupts emulated game code, not just a picture. The
 * callbacks below enforce the bound so a mis-declared cart degrades to a
 * dropped write instead.
 *
 * GB_CART_RAM_MAX_SIZE is passed in by CMake, which passes the matching bank
 * count to the core as GB_MAX_RAM_BANKS so gb_init() clamps to the same
 * number. The two must not drift apart. */
#ifndef GB_CART_RAM_MAX_SIZE
#define GB_CART_RAM_MAX_SIZE 65536u
#endif
#define CART_RAM_MAX_SIZE GB_CART_RAM_MAX_SIZE
static uint8_t ram[CART_RAM_MAX_SIZE];
static palette_t palette; // Colour palette
static uint8_t manual_palette_selected = 0;
static uint8_t persistent_palette_mode = 0; /* 0 = GBC Auto, 1..14 = manual 0..13 */
static volatile bool display_fps = false;
static volatile uint32_t current_fps = 60;
static volatile bool lcd_core_running = false;
static volatile bool lcd_core_ready = false;
static volatile bool lcd_core_parked = false;
static volatile bool loading_anim_active = false;
static volatile bool loading_anim_rom_done = false;
static volatile bool loading_anim_done = false;
static volatile uint64_t loading_anim_start_us = 0;
#if ENABLE_SOUND
static bool loading_anim_boot_sound_played = false;
#endif

static void ensure_lcd_core_running(void);
static void park_lcd_core1(void);
static void unpark_lcd_core1(void);
_Noreturn void main_core1(void);

/* Full-frame Game Boy double buffering (160x144 pixels @ 16-bit RGB565) */
#define GB_FRAME_PIXELS (LCD_WIDTH * LCD_HEIGHT)
static uint16_t s_gb_frame_buf[2][GB_FRAME_PIXELS];
static uint8_t s_gb_write_idx = 0u;

#define LCD_LINE_QUEUE_DEPTH 16u
static uint8_t pixels_buffer[LCD_LINE_QUEUE_DEPTH][LCD_WIDTH];
static volatile uint8_t lcd_line_slot_state[LCD_LINE_QUEUE_DEPTH];
static uint8_t lcd_write_slot = 0u;

#if PEANUT_FULL_GBC_SUPPORT
/* CGB state shared with the LCD core, snapshotted per queue slot so that the
 * same slot handshake (lcd_line_slot_state[]) protects pixels and palette
 * together. cgb_mode_buf[slot]: non-zero when the line came from a CGB game;
 * cgb_palette_buf[slot]: 64-entry RGB555 palette table (BG 0..31, OBJ 0x20..). */
static volatile uint8_t cgb_mode_buf[LCD_LINE_QUEUE_DEPTH];
static uint16_t cgb_palette_buf[LCD_LINE_QUEUE_DEPTH][0x40];

/* Convert a 15-bit RGB555 value (0RRRRRGGGGGBBBBB) to 16-bit RGB565
 * (RRRRRGGGGGGBBBBB) by expanding green from 5 to 6 bits using 3 operations. */
static inline uint16_t rgb555_to_rgb565(uint16_t c) {
  return (uint16_t)(((c & 0x7FE0u) << 1) | ((c >> 4) & 0x0020u) | (c & 0x001Fu));
}
#endif

static struct {
  unsigned a : 1;
  unsigned b : 1;
  unsigned select : 1;
  unsigned start : 1;
  unsigned right : 1;
  unsigned left : 1;
  unsigned up : 1;
  unsigned down : 1;
} prev_joypad_bits;

/* Multicore command structure. */
union core_cmd {
  struct {
    /* Does nothing. */
#define CORE_CMD_NOP 0
    /* Set line "data" on the LCD. Pixel data is in pixels_buffer. */
#define CORE_CMD_LCD_LINE 1
    /* Control idle mode on the LCD. Limits colours to 2 bits. */
#define CORE_CMD_IDLE_SET 2
    /* Set a specific pixel. For debugging. */
#define CORE_CMD_SET_PIXEL 3
    /* Start ROM-loading animation on the LCD core. */
#define CORE_CMD_LOADING_ANIM_START 4
    /* Park the LCD core in a safe flash-free loop while core0 programs flash or draws menu. */
#define CORE_CMD_PARK 5
    /* Render full frame from double buffer (atomic, tear-free). */
#define CORE_CMD_LCD_FRAME 6
    uint8_t cmd;
    uint8_t slot;
    uint8_t unused2;
    uint8_t data;
  };
  uint32_t full;
};

#define putstdio(x) write(1, x, strlen(x))

static void console_startup_sync(void) {
  /* No USB serial CDC wait or sync needed when stdio_usb is disabled (matching PicoNGP). */
}

/* Functions required for communication with the ILI9225. */
void mk_ili9225_set_rst(bool state) { gpio_put(GPIO_RST, state); }

void mk_ili9225_set_rs(bool state) { gpio_put(GPIO_RS, state); }

void mk_ili9225_set_cs(bool state) { gpio_put(GPIO_CS, state); }

static uint8_t persistent_brightness_level = 5; /* 1..5 */

static void set_backlight_brightness(uint8_t level) {
  if (level < 1) level = 1;
  if (level > 5) level = 5;
  persistent_brightness_level = level;

  gpio_set_function(GPIO_LED, GPIO_FUNC_PWM);
  uint slice_num = pwm_gpio_to_slice_num(GPIO_LED);
  pwm_config config = pwm_get_default_config();
  pwm_config_set_clkdiv(&config, 4.0f);
  pwm_config_set_wrap(&config, 1000);
  pwm_init(slice_num, &config, true);

  static const uint16_t duty_table[6] = { 0, 200, 400, 600, 800, 1000 };
  pwm_set_gpio_level(GPIO_LED, duty_table[level]);
}

void mk_ili9225_set_led(bool state) {
  if (state) {
    set_backlight_brightness(persistent_brightness_level);
  } else {
    gpio_set_function(GPIO_LED, GPIO_FUNC_PWM);
    pwm_set_gpio_level(GPIO_LED, 0);
  }
}

void mk_ili9225_spi_write16(const uint16_t *halfwords, size_t len) {
  spi_write16_blocking(spi0, halfwords, len);
}

void mk_ili9225_delay_ms(unsigned ms) { sleep_ms(ms); }

static void read_button_states(bool *up, bool *down, bool *left, bool *right,
                               bool *a, bool *b, bool *select, bool *start) {
#if USE_DMG_BUTTON_MATRIX
  /* DMG joypad matrix:
   * P14 low selects D-Pad: P10=Right, P11=Left, P12=Up, P13=Down
   * P15 low selects buttons: P10=A, P11=B, P12=Select, P13=Start
   * All lines are active-low. */
  gpio_put(GPIO_P14, 0);
  gpio_put(GPIO_P15, 1);
  sleep_us(5);
  *right = gpio_get(GPIO_P10);
  *left = gpio_get(GPIO_P11);
  *up = gpio_get(GPIO_P12);
  *down = gpio_get(GPIO_P13);

  gpio_put(GPIO_P14, 1);
  gpio_put(GPIO_P15, 0);
  sleep_us(5);
  *a = gpio_get(GPIO_P10);
  *b = gpio_get(GPIO_P11);
  *select = gpio_get(GPIO_P12);
  *start = gpio_get(GPIO_P13);

  gpio_put(GPIO_P14, 1);
  gpio_put(GPIO_P15, 1);
#else
  *up = gpio_get(GPIO_UP);
  *down = gpio_get(GPIO_DOWN);
  *left = gpio_get(GPIO_LEFT);
  *right = gpio_get(GPIO_RIGHT);
  *a = gpio_get(GPIO_A);
  *b = gpio_get(GPIO_B);
  *select = gpio_get(GPIO_SELECT);
  *start = gpio_get(GPIO_START);
#endif
}

static void wait_for_all_buttons_released(void) {
  bool up, down, left, right, a, b, select, start;
  uint32_t stable_count = 0;
  uint32_t max_wait = 200; /* 2 seconds maximum timeout */
  while (stable_count < 5 && max_wait > 0) {
    read_button_states(&up, &down, &left, &right, &a, &b, &select, &start);
    if (up && down && left && right && a && b && select && start) {
      stable_count++;
    } else {
      stable_count = 0;
    }
    sleep_ms(10);
    max_wait--;
  }
}

static void init_button_gpio_only(void) {
#if USE_DMG_BUTTON_MATRIX
  gpio_set_function(GPIO_P10, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P11, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P12, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P13, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P14, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P15, GPIO_FUNC_SIO);

  gpio_set_dir(GPIO_P10, false);
  gpio_set_dir(GPIO_P11, false);
  gpio_set_dir(GPIO_P12, false);
  gpio_set_dir(GPIO_P13, false);
  gpio_set_dir(GPIO_P14, true);
  gpio_set_dir(GPIO_P15, true);

  gpio_pull_up(GPIO_P10);
  gpio_pull_up(GPIO_P11);
  gpio_pull_up(GPIO_P12);
  gpio_pull_up(GPIO_P13);

  gpio_put(GPIO_P14, true);
  gpio_put(GPIO_P15, true);
#else
  gpio_set_function(GPIO_UP, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_DOWN, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_LEFT, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_RIGHT, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_A, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_B, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_SELECT, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_START, GPIO_FUNC_SIO);

  gpio_set_dir(GPIO_UP, false);
  gpio_set_dir(GPIO_DOWN, false);
  gpio_set_dir(GPIO_LEFT, false);
  gpio_set_dir(GPIO_RIGHT, false);
  gpio_set_dir(GPIO_A, false);
  gpio_set_dir(GPIO_B, false);
  gpio_set_dir(GPIO_SELECT, false);
  gpio_set_dir(GPIO_START, false);

  gpio_pull_up(GPIO_UP);
  gpio_pull_up(GPIO_DOWN);
  gpio_pull_up(GPIO_LEFT);
  gpio_pull_up(GPIO_RIGHT);
  gpio_pull_up(GPIO_A);
  gpio_pull_up(GPIO_B);
  gpio_pull_up(GPIO_SELECT);
  gpio_pull_up(GPIO_START);
#endif
}

static void check_bootsel_combo(void) {
  /* Disabled: never enter USB bootsel mode automatically on GPIO states.
   * Return to bootloader is handled cleanly via SELECT/B menus and watchdog. */
}

static void select_default_rom_source(void) {
#if defined(RP2040_GB_HAS_BUILTIN_ROM)
  rom = rp2040_gb_builtin_rom;
  rom_size = rp2040_gb_builtin_rom_size;
#else
  rom = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
  rom_size = ROM_FLASH_MAX_SIZE;
#endif
}

#if ENABLE_NFC
static char nfc_hex_digit(uint8_t value) {
  value &= 0x0Fu;
  return (value < 10u) ? (char)('0' + value) : (char)('A' + (value - 10u));
}

static char nfc_ascii_upper(char value) {
  if (value >= 'a' && value <= 'z') {
    return (char)(value - ('a' - 'A'));
  }
  return value;
}

static void nfc_uid_to_hex(const mfrc522_uid_t *uid, char *uid_hex,
                           size_t uid_hex_size) {
  if (uid_hex == NULL || uid_hex_size == 0u) {
    return;
  }
  uid_hex[0] = '\0';
  if (uid == NULL || uid_hex_size < ((size_t)uid->len * 2u + 1u)) {
    return;
  }

  for (uint8_t i = 0; i < uid->len; ++i) {
    uid_hex[i * 2u] = nfc_hex_digit((uint8_t)(uid->bytes[i] >> 4u));
    uid_hex[i * 2u + 1u] = nfc_hex_digit(uid->bytes[i]);
  }
  uid_hex[(size_t)uid->len * 2u] = '\0';
}

static bool nfc_uid_field_matches(const char *field, const char *uid_hex) {
  size_t uid_pos = 0;

  for (const char *p = field;
       *p != '\0' && *p != ';' && *p != ',' && *p != '\r' && *p != '\n'; ++p) {
    if (*p == ' ' || *p == '\t' || *p == ':' || *p == '-') {
      continue;
    }
    if (uid_hex[uid_pos] == '\0' || nfc_ascii_upper(*p) != uid_hex[uid_pos]) {
      return false;
    }
    uid_pos++;
  }

  return uid_hex[uid_pos] == '\0';
}

static void nfc_trim_filename(char *filename) {
  size_t len;
  char *src = filename;

  while (*src == ' ' || *src == '\t') {
    src++;
  }
  if (src != filename) {
    memmove(filename, src, strlen(src) + 1u);
  }

  len = strlen(filename);
  while (len > 0u &&
         (filename[len - 1u] == ' ' || filename[len - 1u] == '\t' ||
          filename[len - 1u] == '\r' || filename[len - 1u] == '\n')) {
    filename[--len] = '\0';
  }
}

static bool nfc_lookup_rom_from_sd(const mfrc522_uid_t *uid, char *rom_filename,
                                   size_t rom_filename_size) {
  char uid_hex[MFRC522_UID_MAX_LEN * 2u + 1u];
  char line[160];
  sd_card_t *pSD = sd_get_by_num(0);
  FIL fil;
  FRESULT fr;

  if (pSD == NULL) {
    printf("NFC: SD driver not available for mapping file\n");
    return false;
  }

  nfc_uid_to_hex(uid, uid_hex, sizeof(uid_hex));
  if (uid_hex[0] == '\0') {
    printf("NFC: invalid UID length for mapping\n");
    return false;
  }

  fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
  if (fr != FR_OK) {
    printf("NFC: f_mount error while opening %s: %s (%d)\n", NFC_MAP_FILE,
           FRESULT_str(fr), fr);
    return false;
  }

  fr = f_open(&fil, NFC_MAP_FILE, FA_READ);
  if (fr != FR_OK) {
    printf("NFC: mapping file %s not available: %s (%d)\n", NFC_MAP_FILE,
           FRESULT_str(fr), fr);
    f_unmount(pSD->pcName);
    return false;
  }

  printf("NFC: looking up UID %s in %s\n", uid_hex, NFC_MAP_FILE);
  while (f_gets(line, sizeof(line), &fil) != NULL) {
    char *cursor = line;
    char *separator;

    while (*cursor == ' ' || *cursor == '\t') {
      cursor++;
    }
    if (*cursor == '\0' || *cursor == '\r' || *cursor == '\n' ||
        *cursor == '#') {
      continue;
    }

    separator = strchr(cursor, ';');
    if (separator == NULL) {
      separator = strchr(cursor, ',');
    }
    if (separator == NULL) {
      continue;
    }

    if (nfc_uid_field_matches(cursor, uid_hex)) {
      strncpy(rom_filename, separator + 1, rom_filename_size - 1u);
      rom_filename[rom_filename_size - 1u] = '\0';
      nfc_trim_filename(rom_filename);
      f_close(&fil);
      f_unmount(pSD->pcName);

      if (rom_filename[0] == '\0') {
        printf("NFC: empty ROM mapping for UID %s\n", uid_hex);
        return false;
      }
      printf("NFC: matched UID %s -> %s\n", uid_hex, rom_filename);
      return true;
    }
  }

  f_close(&fil);
  f_unmount(pSD->pcName);
  printf("NFC: no ROM mapping for UID %s\n", uid_hex);
  return false;
}

static void nfc_print_uid(const mfrc522_uid_t *uid) {
  for (uint8_t i = 0; i < uid->len; ++i) {
    printf("%02X", uid->bytes[i]);
  }
}

#define NFC_BOOT_SCAN_ATTEMPTS 2u
#define NFC_BOOT_SCAN_DELAY_MS 100u
#endif

static void draw_scaled_text(const char *text, uint8_t x, uint8_t y,
                             uint8_t scale, bool bold, uint16_t color,
                             uint16_t bgcolor) {
  uint16_t glyph[8u * 8u];
  uint16_t scaled[16u * 16u];
  size_t len;

  if (text == NULL || scale != 2u) {
    return;
  }

  len = strlen(text);
  for (size_t i = 0; i < len; ++i) {
    memset(scaled, 0, sizeof(scaled));
    mk_ili9225_get_letter(glyph, text[i], color, bgcolor);
    for (uint8_t gy = 0; gy < 8u; ++gy) {
      for (uint8_t gx = 0; gx < 8u; ++gx) {
        uint16_t src = glyph[(gy * 8u) + gx];
        for (uint8_t sy = 0; sy < 2u; ++sy) {
          for (uint8_t sx = 0; sx < 2u; ++sx) {
            uint8_t dx = (uint8_t)(gx * 2u + sx);
            uint8_t dy = (uint8_t)(gy * 2u + sy);
            scaled[(dy * 16u) + dx] = src;
            if (bold && src == color && (dx + 1u) < 16u) {
              scaled[(dy * 16u) + dx + 1u] = color;
            }
          }
        }
      }
    }
    mk_ili9225_blit(scaled, (uint8_t)(x + (uint8_t)(i * 16u)), y, 16u, 16u);
  }
}

#if ENABLE_SOUND
static void play_boot_sound(void) {
  /* Kept completely silent during loading, identical to PicoSMS, PicoNGP, PicoPCE */
}

static void maybe_play_loading_boot_sound(void) {
  uint64_t start_us;
  uint64_t elapsed_us;

  if (loading_anim_boot_sound_played) {
    return;
  }

  start_us = __atomic_load_n(&loading_anim_start_us, __ATOMIC_SEQ_CST);
  if (start_us == 0u) {
    return;
  }

  elapsed_us = time_us_64() - start_us;
  if (elapsed_us < LOADING_ANIM_FALL_DURATION_US) {
    return;
  }

  loading_anim_boot_sound_played = true;
  play_boot_sound();
}
#endif

#if ENABLE_NFC
static bool nfc_try_select_rom(char *rom_filename, size_t rom_filename_size) {
  mfrc522_uid_t uid;
  uint8_t version;
  uint64_t scan_start_us;

  if (rom_filename == NULL || rom_filename_size == 0u) {
    return false;
  }
  rom_filename[0] = '\0';

  printf("NFC: init RC522 on spi1 CS=GP%d RST=GP%d\n", MFRC522_PIN_CS,
         MFRC522_PIN_RST);
  mfrc522_init();
  version = mfrc522_version();
  printf("NFC: RC522 VersionReg=0x%02X\n", version);
  if (version == 0x00u || version == 0xFFu) {
    printf("NFC: RC522 not detected, skipping NFC boot\n");
    return false;
  }

  printf("NFC: waiting for boot tag\n");
  scan_start_us = time_us_64();
  for (uint8_t attempt = 0; attempt < NFC_BOOT_SCAN_ATTEMPTS; ++attempt) {
    if (mfrc522_read_uid(&uid)) {
      uint32_t elapsed_ms = (uint32_t)((time_us_64() - scan_start_us) / 1000u);
      printf(
          "NFC: detected on attempt %u/%u after %lu ms during startup scan\n",
          (unsigned)(attempt + 1u), (unsigned)NFC_BOOT_SCAN_ATTEMPTS,
          (unsigned long)elapsed_ms);
      printf("NFC: UID=");
      nfc_print_uid(&uid);
      printf("\n");
      return nfc_lookup_rom_from_sd(&uid, rom_filename, rom_filename_size);
    }
    sleep_ms(NFC_BOOT_SCAN_DELAY_MS);
  }

  printf("NFC: no tag found, showing ROM selector\n");
  return false;
}
#endif

static void build_save_filename(struct gb_s *gb, char *filename,
                                size_t filename_size) {
  char rom_name[17];   /* 16-char DMG title + NUL (gb_get_rom_name writes 17) */

  if (filename == NULL || filename_size == 0u) {
    return;
  }

  gb_get_rom_name(gb, rom_name);
  for (int i = 0; i < 16 && rom_name[i] != '\0'; i++) {
    if (rom_name[i] == '/' || rom_name[i] == '\\' || rom_name[i] == ':' ||
        rom_name[i] == '*' || rom_name[i] == '?' || rom_name[i] == '"' ||
        rom_name[i] == '<' || rom_name[i] == '>' || rom_name[i] == '|' ||
        rom_name[i] == ' ') {
      rom_name[i] = '_';
    }
  }
  snprintf(filename, filename_size, GB_SAVE_DIR_0 "/%s.sav", rom_name);
}

static inline void rom_selector_text(const char *text, uint16_t row, uint16_t color,
                                     uint16_t bgcolor) {
  mk_ili9225_text((char *)text, ROM_SELECTOR_X, (uint16_t)(ROM_SELECTOR_Y + row * 8u),
                  color, bgcolor);
}

static void refresh_rom_bank0(void) {
  uint32_t copy_size = rom_size;

  if (copy_size > sizeof(rom_bank0)) {
    copy_size = sizeof(rom_bank0);
  }

  memset(rom_bank0, 0xFF, sizeof(rom_bank0));
  if (copy_size > 0) {
    memcpy(rom_bank0, rom, copy_size);
  }
}

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t __not_in_flash_func(gb_rom_read)(struct gb_s *gb, const uint_fast32_t addr) {
  (void)gb;
  if (addr < sizeof(rom_bank0))
    return rom_bank0[addr];

  if (addr < rom_size) {
    return rom[addr];
  }

  return 0xFF;
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t __not_in_flash_func(gb_cart_ram_read)(struct gb_s *gb, const uint_fast32_t addr) {
  (void)gb;
  /* The core can hand us an address past the backing store when the cart
   * header over-declares 0x0149. Unmapped SRAM reads 0xFF on hardware. */
  if (addr >= CART_RAM_MAX_SIZE) return 0xFF;
  return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void __not_in_flash_func(gb_cart_ram_write)(struct gb_s *gb, const uint_fast32_t addr,
                                            const uint8_t val) {
  (void)gb;
  /* Drop the write rather than letting it run into rom_bank0. */
  if (addr >= CART_RAM_MAX_SIZE) return;
  ram[addr] = val;
}

/**
 * Ignore all errors.
 */
void gb_error(struct gb_s *gb, const enum gb_error_e gb_err,
              const uint16_t addr) {
#if 1
  const char *gb_err_str[4] = {"UNKNOWN", "INVALID OPCODE", "INVALID READ",
                               "INVALID WRITE"};
  printf("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err],
         addr);
//	abort();
#endif
}

#if ENABLE_LCD
void core1_lcd_draw_line(const uint_fast8_t slot, const uint_fast8_t line) {
  static uint16_t fb[LCD_WIDTH];
  const uint8_t *line_pixels = pixels_buffer[slot];

#if PEANUT_FULL_GBC_SUPPORT
  if (cgb_mode_buf[slot]) {
    const uint16_t *pal = cgb_palette_buf[slot];
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      fb[x] = rgb555_to_rgb565(pal[line_pixels[x]]);
    }
  } else
#endif
  {
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      fb[x] =
          palette[(line_pixels[x] & LCD_PALETTE_ALL) >> 4][line_pixels[x] & 3];
    }
  }

  /* Display frame rate overlay (8 scanlines: line 8 to 15, matching pico-infonesPlus) */
  if (display_fps && line >= 8 && line < 16) {
    uint32_t f = current_fps;
    char fpsString[2];
    fpsString[0] = (char)('0' + ((f / 10u) % 10u));
    fpsString[1] = (char)('0' + (f % 10u));

    const uint16_t fgc = 0xFFFF; /* White text */
    const uint16_t bgc = 0x0000; /* Black background */

    const int rowInChar = (int)(line - 8);
    int x_start = 8;
    for (int i = 0; i < 2; i++) {
      uint8_t glyph[8];
      mk_ili9225_get_raw_glyph(glyph, fpsString[i]);
      uint8_t fontSlice = glyph[rowInChar];
      for (int bit = 0; bit < 8; bit++) {
        if (fontSlice & (0x80 >> bit)) {
          fb[x_start++] = fgc;
        } else {
          fb[x_start++] = bgc;
        }
      }
    }
  }

  mk_ili9225_set_x(line + 16);
  mk_ili9225_write_pixels(fb, LCD_WIDTH);
  __atomic_store_n(&lcd_line_slot_state[slot], 0u, __ATOMIC_SEQ_CST);
}

static void core1_loading_animation_tick(void) {
  static bool drawn = false;
  static uint8_t prev_y = LOADING_ANIM_START_Y;
  static const char logo_text[] = LOADING_ANIM_TEXT;
  uint64_t start_us = __atomic_load_n(&loading_anim_start_us, __ATOMIC_SEQ_CST);
  uint64_t elapsed_us;
  uint8_t y = LOADING_ANIM_TARGET_Y;
  const uint8_t x =
      (uint8_t)(LOADING_SCREEN_X +
                ((LOADING_SCREEN_WIDTH - LOADING_ANIM_TEXT_WIDTH) / 2u));

  if (start_us == 0u) {
    start_us = time_us_64();
    __atomic_store_n(&loading_anim_start_us, start_us, __ATOMIC_SEQ_CST);
    elapsed_us = 0u;
  } else {
    elapsed_us = time_us_64() - start_us;
  }

  if (elapsed_us < LOADING_ANIM_FALL_DURATION_US) {
    y = (uint8_t)(LOADING_ANIM_START_Y +
                  (((uint64_t)(LOADING_ANIM_TARGET_Y - LOADING_ANIM_START_Y) *
                    elapsed_us) /
                   LOADING_ANIM_FALL_DURATION_US));
  }

  if (!drawn || y != prev_y) {
    if (drawn) {
      draw_scaled_text(logo_text, x, prev_y, 2u, true, 0x0000, 0x0000);
    }
    draw_scaled_text(logo_text, x, y, 2u, true, 0xFFFF, 0x0000);
    prev_y = y;
    drawn = true;
  }

  if (elapsed_us >= LOADING_ANIM_MIN_VISIBLE_US &&
      __atomic_load_n(&loading_anim_rom_done, __ATOMIC_SEQ_CST)) {
    mk_ili9225_fill(0x0000);
    drawn = false;
    prev_y = LOADING_ANIM_START_Y;
    __atomic_store_n(&loading_anim_active, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&loading_anim_done, true, __ATOMIC_SEQ_CST);
  }
}

static void lcd_start_loading_animation(void) {
  union core_cmd cmd;
  __atomic_store_n(&loading_anim_done, false, __ATOMIC_SEQ_CST);
  __atomic_store_n(&loading_anim_rom_done, false, __ATOMIC_SEQ_CST);
  __atomic_store_n(&loading_anim_start_us, 0u, __ATOMIC_SEQ_CST);
#if ENABLE_SOUND
  loading_anim_boot_sound_played = false;
#endif
  __atomic_store_n(&loading_anim_active, true, __ATOMIC_SEQ_CST);
  cmd.cmd = CORE_CMD_LOADING_ANIM_START;
  cmd.data = 0u;
  multicore_fifo_push_blocking(cmd.full);
}

static void lcd_wait_loading_animation_done(void) {
  while (!__atomic_load_n(&loading_anim_done, __ATOMIC_SEQ_CST)) {
#if ENABLE_SOUND
    maybe_play_loading_boot_sound();
#endif
    tight_loop_contents();
  }
}

static void ensure_lcd_core_running(void) {
  if (__atomic_load_n(&lcd_core_running, __ATOMIC_SEQ_CST)) {
    return;
  }

  __atomic_store_n(&lcd_core_ready, false, __ATOMIC_SEQ_CST);
  __atomic_store_n(&lcd_core_running, true, __ATOMIC_SEQ_CST);
  putstdio("CORE1 ");
  multicore_launch_core1(main_core1);
  while (!__atomic_load_n(&lcd_core_ready, __ATOMIC_SEQ_CST)) {
    tight_loop_contents();
  }
  putstdio("LCD ");
}

/* Park LCD core1 gracefully before core0 draws menu or flashes ROM.
 * Hard-resetting core1 mid-transfer wedges the SPI0 bus. Safe to call when
 * core1 isn't running. */
static void park_lcd_core1(void) {
  if (!__atomic_load_n(&lcd_core_running, __ATOMIC_SEQ_CST)) {
    return;
  }
  if (__atomic_load_n(&lcd_core_parked, __ATOMIC_SEQ_CST)) {
    return;
  }
  union core_cmd park;
  park.cmd = CORE_CMD_PARK;
  park.slot = 0u;
  park.unused2 = 0u;
  park.data = 0u;
  multicore_fifo_push_blocking(park.full);
  while (!__atomic_load_n(&lcd_core_parked, __ATOMIC_SEQ_CST)) {
    tight_loop_contents();
  }
}

static void unpark_lcd_core1(void) {
  __atomic_store_n(&lcd_core_parked, false, __ATOMIC_SEQ_CST);
}

_Noreturn void main_core1(void) {
  union core_cmd cmd;

  /* Initialise and control LCD on core 1. */
  mk_ili9225_init();

  /* Clear LCD screen. */
  mk_ili9225_fill(0x0000);
  __atomic_store_n(&lcd_core_ready, true, __ATOMIC_SEQ_CST);

  /* Set LCD window to DMG size. */
#if defined(DISPLAY_ILI9225)
  mk_ili9225_fill_rect(31, 16, LCD_WIDTH, LCD_HEIGHT, 0x0000);
#endif

  // Sleep used for debugging LCD window.
  // sleep_ms(1000);

  /* Handle commands coming from core0. */
  while (1) {
    if (multicore_fifo_rvalid()) {
      cmd.full = multicore_fifo_pop_blocking();
      switch (cmd.cmd) {
      case CORE_CMD_LCD_FRAME:
        mk_ili9225_draw_frame(s_gb_frame_buf[cmd.slot], cmd.data);
        break;

      case CORE_CMD_LCD_LINE:
        core1_lcd_draw_line(cmd.slot, cmd.data);
        break;

      case CORE_CMD_IDLE_SET:
        mk_ili9225_display_control(true, cmd.data);
        break;

      case CORE_CMD_LOADING_ANIM_START:
        mk_ili9225_fill(0x0000);
        break;

      case CORE_CMD_PARK:
        __atomic_store_n(&lcd_core_parked, true, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&lcd_core_parked, __ATOMIC_SEQ_CST)) {
          tight_loop_contents();
        }
        break;

      case CORE_CMD_NOP:
      default:
        break;
      }
    } else if (__atomic_load_n(&loading_anim_active, __ATOMIC_SEQ_CST)) {
      core1_loading_animation_tick();
      sleep_ms(10);
    } else {
      tight_loop_contents();
    }
  }

  HEDLEY_UNREACHABLE();
}
#endif

#if ENABLE_LCD
void __not_in_flash_func(lcd_draw_line)(struct gb_s *gb, const uint8_t pixels[LCD_WIDTH],
                                        const uint_fast8_t line) {
  if (line >= LCD_HEIGHT) return;
  uint16_t *dst = &s_gb_frame_buf[s_gb_write_idx][line * LCD_WIDTH];
#if PEANUT_FULL_GBC_SUPPORT
  if (gb->cgb.cgbMode != 0) {
    const uint16_t *pal = gb->cgb.fixPalette;
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      dst[x] = rgb555_to_rgb565(pal[pixels[x]]);
    }
  } else
#endif
  {
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      dst[x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    }
  }
}
#endif

#if ENABLE_SDCARD
static bool ensure_sd_mounted(void) {
  sd_card_t *pSD = sd_get_by_num(0);
  if (pSD == NULL) {
    return false;
  }
  if (pSD->fatfs.fs_type != 0) {
    return true; /* Already mounted */
  }
  FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
  if (fr != FR_OK) {
    sleep_ms(50);
    fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
  }
  return (fr == FR_OK);
}

static bool sd_storage_ready(void) {
  sd_card_t *pSD = sd_get_by_num(0);
  if (pSD == NULL) {
    printf("W SD disabled: no SD driver config\n");
    return false;
  }
  if (!ensure_sd_mounted()) {
    printf("W SD unavailable. Continuing without SD features.\n");
    return false;
  }
  printf("I SD card mounted successfully\n");
  return true;
}

/* ---------------------------------------------------------------------------
 * Cartridge real-time clock (MBC3 + TIMER, cartridge types 0x0F-0x13)
 *
 * Pokemon Gold/Silver/Crystal and a few other titles read the clock. The core
 * carries the registers but nothing drove them, so every such game saw
 * 00:00:00, day 0, forever.
 *
 * There is no clock hardware on this board to read: the RP2040's RTC
 * peripheral was dropped in the RP2350 (hardware/regs/rtc.h exists under
 * rp2040/ and not under rp2350/), and time_init() is called with no time
 * source. So the seed is taken once from /boot.txt RTC_EPOCH=, or from a fixed
 * reference date when that key is absent, and the clock then free-runs in
 * cartridge SRAM, which is written to the .sav. It only ever counts forward, so
 * it stays correct across sessions without any clock hardware.
 * ------------------------------------------------------------------------ */

/* Trailing 10 bytes appended to the .sav: 5 running + 5 latched RTC registers.
 * A save written before this existed is exactly save_size bytes, and is read
 * back with the clock simply unseeded -- so existing Gold/Silver saves keep
 * loading instead of being rejected as malformed. */
#define GB_RTC_STATE_BYTES 10

/* Default seed when /boot.txt carries no RTC_EPOCH=. A Wednesday, matching the
 * 1 Jan 2000 that Gold/Silver were released on, so the in-game weekday is
 * sensible on a fresh install without anyone having to set a date. */
#define GB_RTC_DEFAULT_EPOCH_YEAR 2000
#define GB_RTC_DEFAULT_EPOCH_MON 1
#define GB_RTC_DEFAULT_EPOCH_MDAY 1

/* Days elapsed from the epoch year to the start of each month, non-leap. The
 * RTC stores a day-of-year counter, and MBC3's has no notion of months or
 * leap years -- the game computes the date itself -- so only the conversion
 * from a calendar date to that counter is needed here. */
static int rtc_days_from_year_start(int year, int month, int mday) {
  static const int cum[12] = {0,   31,  59,  90,  120, 151,
                              181, 212, 243, 273, 304, 334};
  int y = year - GB_RTC_DEFAULT_EPOCH_YEAR;
  int days = y * 365 + (y / 4) - (y / 100) + (y / 400);
  days += cum[month - 1];
  if (month > 2) {
    int leap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
    days += leap ? 1 : 0;
  }
  return days + (mday - 1);
}

/* Parse "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS" into a struct tm.
 * Returns false on anything malformed, leaving the caller to use the default,
 * so a typo in boot.txt costs the user their chosen date and nothing else. */
static bool rtc_parse_epoch(const char *s, struct tm *out) {
  int year, mon, mday, hour = 0, min = 0, sec = 0;
  if (s == NULL) return false;

  if (sscanf(s, "%4d-%2d-%2d %2d:%2d:%2d", &year, &mon, &mday, &hour, &min,
             &sec) < 3) {
    if (sscanf(s, "%4d-%2d-%2d", &year, &mon, &mday) != 3) return false;
  }
  if (year < 1980 || year > 2127) return false;
  if (mon < 1 || mon > 12) return false;
  if (mday < 1 || mday > 31) return false;
  if (hour < 0 || hour > 23) return false;
  if (min < 0 || min > 59) return false;
  if (sec < 0 || sec > 59) return false;

  memset(out, 0, sizeof(*out));
  out->tm_year = year - 1900;
  out->tm_mon = mon - 1;
  out->tm_mday = mday;
  out->tm_hour = hour;
  out->tm_min = min;
  out->tm_sec = sec;
  out->tm_yday = rtc_days_from_year_start(year, mon, mday);
  return true;
}

/* Seed the clock for a cartridge that has one. Called once, before the game
 * runs, and only when the cartridge actually carries an RTC. */
static void rtc_seed_if_needed(struct gb_s *gb) {
  struct tm seed;

  if (!gb_has_rtc(gb)) return;

  /* The .sav is the source of truth once written: this runs before the save is
   * loaded, so an existing save overwrites whatever is seeded here. */
  if (!rtc_parse_epoch(dmg_panel_get_rtc_epoch(), &seed)) {
    const char *cfg = dmg_panel_get_rtc_epoch();
    if (cfg != NULL) {
      printf("W RTC_EPOCH \"%s\" is not YYYY-MM-DD[ HH:MM:SS]; using default\n",
             cfg);
    }
    memset(&seed, 0, sizeof(seed));
    seed.tm_year = GB_RTC_DEFAULT_EPOCH_YEAR - 1900;
    seed.tm_mon = GB_RTC_DEFAULT_EPOCH_MON - 1;
    seed.tm_mday = GB_RTC_DEFAULT_EPOCH_MDAY;
    seed.tm_yday = 0;
  }

  gb_set_rtc(gb, &seed);
  printf("I cartridge RTC seeded: %04d-%02d-%02d %02d:%02d:%02d\n",
         seed.tm_year + 1900, seed.tm_mon + 1, seed.tm_mday, seed.tm_hour,
         seed.tm_min, seed.tm_sec);
}


/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s *gb) {
  char filename[64];
  uint_fast32_t save_size;
  UINT br;

  build_save_filename(gb, filename, sizeof(filename));
  save_size = gb_get_save_size(gb);
  if (save_size > 0) {
    if (!ensure_sd_mounted()) {
      printf("E SD not mounted for read_cart_ram_file\n");
      return;
    }

    FIL fil;
    FRESULT fr = f_open(&fil, filename, FA_READ);
    if (fr == FR_OK) {
      /* Bound by save_size, not by the file length: an oversized or truncated
       * .sav would otherwise f_read straight past ram[] into the ROM cache.
       * save_size is itself clamped to sizeof(ram) below. */
      uint_fast32_t want = (save_size < CART_RAM_MAX_SIZE) ? save_size : CART_RAM_MAX_SIZE;
      f_read(&fil, ram, want, &br);

      /* Trailing RTC bytes, present only in saves this build wrote. Absent --
       * i.e. the file is exactly save_size -- means an older save, which stays
       * valid and simply keeps the seeded clock. That case is the normal one
       * for every existing .sav, so it must not be treated as an error. */
      if (gb_has_rtc(gb) && f_size(&fil) == (FSIZE_t)(want + GB_RTC_STATE_BYTES)) {
        uint8_t rtc_state[GB_RTC_STATE_BYTES];
        f_read(&fil, rtc_state, sizeof(rtc_state), &br);
        if (br == sizeof(rtc_state)) {
          gb_set_rtc_state(gb, rtc_state);
          printf("I cartridge RTC restored from %s\n", filename);
        }
      } else if (gb_has_rtc(gb)) {
        printf("W %s has no RTC block; clock restarts from the seed date\n",
               filename);
      }
      f_close(&fil);
    } else if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
      printf("W save file %s does not exist yet; starting with empty cartridge RAM\n",
             filename);
    } else {
      printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
    }
  }
  printf("I read_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename,
         save_size);
}

/**
 * Write a save file to the SD card
 */
void write_cart_ram_file(struct gb_s *gb) {
  char filename[64];
  uint_fast32_t save_size;
  UINT bw;

  build_save_filename(gb, filename, sizeof(filename));
  save_size = gb_get_save_size(gb);
  if (save_size > CART_RAM_MAX_SIZE) {
    printf("W save_size %lu exceeds cartridge RAM buffer (%u); clamping\n",
           save_size, (unsigned)CART_RAM_MAX_SIZE);
    save_size = CART_RAM_MAX_SIZE;
  }
  if (save_size > 0) {
    if (!ensure_sd_mounted()) {
      printf("E SD not mounted for write_cart_ram_file\n");
      return;
    }

    /* Ensure target directory exists on drive 0: and root */
    f_mkdir("0:/saves");
    f_mkdir(GB_SAVE_DIR_0);
    f_mkdir("/saves");
    f_mkdir(GB_SAVE_DIR_ROOT);

    FIL fil;
    FRESULT fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_OK) {
      f_write(&fil, ram, save_size, &bw);

      /* The RTC lives in the cartridge's own registers rather than in SRAM, so
       * it would otherwise be lost every time the game was closed -- which is
       * what made Gold/Silver report a different date on each launch. It is
       * appended after the SRAM so a save without a clock cartridge stays
       * byte-identical to what older builds wrote. */
      if (gb_has_rtc(gb)) {
        uint8_t rtc_state[GB_RTC_STATE_BYTES];
        gb_get_rtc_state(gb, rtc_state);
        f_write(&fil, rtc_state, sizeof(rtc_state), &bw);
      }
      f_close(&fil);
    } else {
      printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
    }
  }
  printf("I write_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename,
         save_size);
}

#define SAVE_STATE_MAGIC "PBGBC_SS"

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t gb_size;
  uint32_t ram_size;
  uint32_t palette_idx;
  uint32_t grid_enabled;
  uint32_t scale_mode;
} save_state_header_t;

#define EMU_CONFIG_MAGIC 0x43424750u /* "PGBC" */

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t scale_mode;
  uint32_t grid_enabled;
  uint32_t display_fps;
  uint32_t palette_mode;     /* 0 = GBC Auto, 1..14 = manual 0..13 */
  uint32_t volume_level;     /* 1..5, default 3 */
  uint32_t brightness_level; /* 1..5, default 5 */
  uint32_t turbo_mode;       /* 0 = Normal 1x, 1 = Turbo 2x */
  uint32_t save_slot;        /* 1..4, default 1 */
  uint32_t load_slot;        /* 1..4, default 1 */
  uint32_t interleave;
} emu_config_t;

static ili9341_scale_mode_t persistent_scale_mode = SCALE_FIT;
static bool persistent_grid_enabled = false;
static bool persistent_shimmerless_enabled = true;
static bool persistent_interleave = false;
static uint8_t persistent_volume_level = 3;     /* 1..5 */
static uint8_t persistent_turbo_mode = 0;       /* 0 = 1x, 1 = 2x */
static uint8_t persistent_save_slot = 1;       /* 1..4 */
static uint8_t persistent_load_slot = 1;       /* 1..4 */
static char current_loaded_game_name[64] = "";

static void set_sound_volume_level(uint8_t level) {
  if (level > 5) level = 5;
  persistent_volume_level = level;
#if ENABLE_SOUND
  i2s_volume(&i2s_config, level);
#endif
}

static const char *palette_mode_names[] = {
  "< GBC Auto > ",
  "< GBC Brown >",
  "< GBC Pastel>",
  "< GBC Orange>",
  "< GBC Yellow>",
  "< Pocket B&W>",
  "< GBC Blue > ",
  "< GBC DkBlue>",
  "< GBC DkGrn >",
  "< GBC Red >  ",
  "< GBC DkBrwn>",
  "< ClassicGrn>",
  "< Inverted > ",
  "< DMG Green >",
  "< GB Light > "
};

static void apply_palette_mode(struct gb_s *gb_ptr) {
  if (persistent_palette_mode == 0) {
    char rom_title[17];  /* 16-char DMG title + NUL */
    memset(rom_title, 0, sizeof(rom_title));
    if (gb_ptr) {
      gb_get_rom_name(gb_ptr, rom_title);
      auto_assign_palette(palette, gb_colour_hash(gb_ptr), rom_title);
      printf("I Palette: GBC Auto for '%s'\n", rom_title);
    } else {
      get_colour_palette(palette, 0x05, 0x00);
    }
  } else if (persistent_palette_mode <= NUMBER_OF_MANUAL_PALETTES) {
    manual_palette_selected = (uint8_t)(persistent_palette_mode - 1);
    manual_assign_palette(palette, manual_palette_selected);
    printf("I Palette: %s (%u)\n", palette_mode_names[persistent_palette_mode],
           manual_palette_selected);
  }
}

static void set_current_game_name_from_path(const char *path) {
  if (path == NULL) {
    current_loaded_game_name[0] = '\0';
    return;
  }
  const char *base = strrchr(path, '/');
  if (!base) base = strrchr(path, '\\');
  base = base ? (base + 1) : path;

  strncpy(current_loaded_game_name, base, sizeof(current_loaded_game_name) - 1);
  current_loaded_game_name[sizeof(current_loaded_game_name) - 1] = '\0';

  char *dot = strrchr(current_loaded_game_name, '.');
  if (dot) {
    *dot = '\0';
  }
}

static dmg_emulator_desc_t s_gb_desc;
static void save_emulator_config(void) {
  g_dmg_config.scale_mode = (uint32_t)persistent_scale_mode;
  g_dmg_config.grid_effect = persistent_grid_enabled ? 1u : 0u;
  g_dmg_config.interleave = persistent_interleave ? 1u : 0u;
  g_dmg_config.turbo_mode = persistent_turbo_mode;
  g_dmg_config.volume_level = persistent_volume_level;
  g_dmg_config.brightness_level = persistent_brightness_level;
  g_dmg_config.save_slot = persistent_save_slot;
  g_dmg_config.load_slot = persistent_load_slot;
  g_dmg_config.extra_opt1 = persistent_palette_mode;
  g_dmg_config.extra_opt2 = persistent_shimmerless_enabled ? 0u : 1u;
  dmg_config_save(&s_gb_desc);
}

static void load_emulator_config(void) {
  dmg_config_load(&s_gb_desc);
  persistent_scale_mode = (ili9341_scale_mode_t)g_dmg_config.scale_mode;
  persistent_grid_enabled = (g_dmg_config.grid_effect != 0u);
  persistent_interleave = (g_dmg_config.interleave != 0u);
  persistent_turbo_mode = (uint8_t)g_dmg_config.turbo_mode;
  persistent_volume_level = (uint8_t)g_dmg_config.volume_level;
  persistent_brightness_level = (uint8_t)g_dmg_config.brightness_level;
  persistent_save_slot = (uint8_t)g_dmg_config.save_slot;
  persistent_load_slot = (uint8_t)g_dmg_config.load_slot;
  if (g_dmg_config.extra_opt1 < 15) {
    persistent_palette_mode = (uint8_t)g_dmg_config.extra_opt1;
  }
  persistent_shimmerless_enabled = (g_dmg_config.extra_opt2 == 0u);
  mk_ili9225_set_shimmerless_enabled(persistent_shimmerless_enabled);
}

typedef enum {
  INGAME_ACTION_RESUME = 0,
  INGAME_ACTION_RESET,
  INGAME_ACTION_EXIT
} ingame_action_e;

static void build_state_filename(struct gb_s *gb, char *filename,
                                 size_t filename_size, uint8_t slot) {
  char rom_name[17];   /* 16-char DMG title + NUL (gb_get_rom_name writes 17) */

  if (filename == NULL || filename_size == 0u) {
    return;
  }

  gb_get_rom_name(gb, rom_name);
  for (int i = 0; i < 16 && rom_name[i] != '\0'; i++) {
    if (rom_name[i] == '/' || rom_name[i] == '\\' || rom_name[i] == ':' ||
        rom_name[i] == '*' || rom_name[i] == '?' || rom_name[i] == '"' ||
        rom_name[i] == '<' || rom_name[i] == '>' || rom_name[i] == '|' ||
        rom_name[i] == ' ') {
      rom_name[i] = '_';
    }
  }
  if (slot == DMG_RESUME_SLOT) {
    snprintf(filename, filename_size, GB_SAVE_DIR_0 "/%s_R.ss", rom_name);
  } else {
    snprintf(filename, filename_size, GB_SAVE_DIR_0 "/%s_%u.ss", rom_name, (unsigned)slot);
  }
}

static bool save_state_file(struct gb_s *gb, uint8_t slot) {
  char filename[64];
  UINT bw;

  build_state_filename(gb, filename, sizeof(filename), slot);
  if (!ensure_sd_mounted()) {
    printf("E SD not mounted for save_state_file\n");
    return false;
  }

  f_mkdir("0:/saves");
  f_mkdir(GB_SAVE_DIR_0);
  f_mkdir("/saves");
  f_mkdir(GB_SAVE_DIR_ROOT);

  FIL fil;
  FRESULT fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
  if (fr != FR_OK) {
    printf("E f_open(%s) error: %d\n", filename, fr);
    return false;
  }

  save_state_header_t header;
  memcpy(header.magic, SAVE_STATE_MAGIC, 8);
  /* v3 = v2 + the APU block written after ram[] (see audio_state_save). */
  header.version = ENABLE_SOUND ? 3 : 2;
  header.gb_size = (uint32_t)sizeof(struct gb_s);
  header.ram_size = (uint32_t)sizeof(ram);
  header.palette_idx = (uint32_t)persistent_palette_mode;
  header.grid_enabled = mk_ili9225_get_grid_enabled() ? 1u : 0u;
  header.scale_mode = (uint32_t)mk_ili9225_get_scale_mode();

  fr = f_write(&fil, &header, sizeof(header), &bw);
  if (fr != FR_OK || bw != sizeof(header)) {
    f_close(&fil);
    return false;
  }

  fr = f_write(&fil, gb, sizeof(struct gb_s), &bw);
  if (fr != FR_OK || bw != sizeof(struct gb_s)) {
    f_close(&fil);
    return false;
  }

  fr = f_write(&fil, ram, sizeof(ram), &bw);
  if (fr != FR_OK || bw != sizeof(ram)) {
    f_close(&fil);
    return false;
  }

#if ENABLE_SOUND
  {
    size_t len = 0;
    void *part;
    for (unsigned int i = 0; (part = audio_state_part(i, &len)) != NULL; i++) {
      fr = f_write(&fil, part, (UINT)len, &bw);
      if (fr != FR_OK || bw != len) {
        f_close(&fil);
        return false;
      }
    }
  }
#endif

  f_close(&fil);
  printf("I save_state_file(%s) COMPLETE\n", filename);
  return true;
}

static bool load_state_file(struct gb_s *gb, uint8_t slot) {
  char filename[64];
  UINT br;

  build_state_filename(gb, filename, sizeof(filename), slot);
  if (!ensure_sd_mounted()) {
    printf("E SD not mounted for load_state_file\n");
    return false;
  }

  FIL fil;
  FRESULT fr = f_open(&fil, filename, FA_READ);
  if (fr != FR_OK && slot == 1) {
    /* Fallback to unnumbered legacy save state if slot 1 */
    char legacy_name[64];
    char rom_name[17];   /* 16-char DMG title + NUL (gb_get_rom_name writes 17) */
    gb_get_rom_name(gb, rom_name);
    for (int i = 0; i < 16 && rom_name[i] != '\0'; i++) {
      if (rom_name[i] == '/' || rom_name[i] == '\\' || rom_name[i] == ':' ||
          rom_name[i] == '*' || rom_name[i] == '?' || rom_name[i] == '"' ||
          rom_name[i] == '<' || rom_name[i] == '>' || rom_name[i] == '|' ||
          rom_name[i] == ' ') {
        rom_name[i] = '_';
      }
    }
    snprintf(legacy_name, sizeof(legacy_name), GB_SAVE_DIR_0 "/%s.ss", rom_name);
    fr = f_open(&fil, legacy_name, FA_READ);
  }
  if (fr != FR_OK) {
    printf("W state file slot %u does not exist: %d\n", (unsigned)slot, fr);
    return false;
  }

  save_state_header_t header;
  fr = f_read(&fil, &header, sizeof(header), &br);
  if (fr != FR_OK || br != sizeof(header)) {
    f_close(&fil);
    return false;
  }

  if ((memcmp(header.magic, SAVE_STATE_MAGIC, 8) != 0 &&
       memcmp(header.magic, "PBDMG_SS", 8) != 0) ||
      (header.version < 1 || header.version > 3) ||
      header.gb_size != sizeof(struct gb_s) || header.ram_size != sizeof(ram)) {
    printf("E Invalid state file header in %s\n", filename);
    f_close(&fil);
    return false;
  }

  /* Backup active runtime function pointers before reading serialized struct */
  uint8_t (*gb_rom_read_cb)(struct gb_s*, const uint_fast32_t) = gb->gb_rom_read;
  uint8_t (*gb_cart_ram_read_cb)(struct gb_s*, const uint_fast32_t) = gb->gb_cart_ram_read;
  void (*gb_cart_ram_write_cb)(struct gb_s*, const uint_fast32_t, const uint8_t) = gb->gb_cart_ram_write;
  void (*gb_error_cb)(struct gb_s*, const enum gb_error_e, const uint16_t) = gb->gb_error;
  void (*gb_serial_tx_cb)(struct gb_s*, const uint8_t) = gb->gb_serial_tx;
  enum gb_serial_rx_ret_e (*gb_serial_rx_cb)(struct gb_s*, uint8_t*) = gb->gb_serial_rx;
  uint8_t (*gb_bootrom_read_cb)(struct gb_s*, const uint_fast16_t) = gb->gb_bootrom_read;
  void (*lcd_draw_line_cb)(struct gb_s*, const uint8_t*, const uint_fast8_t) = gb->display.lcd_draw_line;
  void *priv_cb = gb->direct.priv;

  fr = f_read(&fil, gb, sizeof(struct gb_s), &br);
  if (fr != FR_OK || br != sizeof(struct gb_s)) {
    f_close(&fil);
    return false;
  }

  /* Restore active runtime function pointers */
  gb->gb_rom_read = gb_rom_read_cb;
  gb->gb_cart_ram_read = gb_cart_ram_read_cb;
  gb->gb_cart_ram_write = gb_cart_ram_write_cb;
  gb->gb_error = gb_error_cb;
  gb->gb_serial_tx = gb_serial_tx_cb;
  gb->gb_serial_rx = gb_serial_rx_cb;
  gb->gb_bootrom_read = gb_bootrom_read_cb;
  gb->display.lcd_draw_line = lcd_draw_line_cb;
  gb->direct.priv = priv_cb;

  fr = f_read(&fil, ram, sizeof(ram), &br);
  if (fr != FR_OK || br != sizeof(ram)) {
    f_close(&fil);
    return false;
  }

#if ENABLE_SOUND
  /* v3 carries the APU. Older states do not: the APU stays as it is (fine for
   * an in-session load, where the game already powered it). */
  if (header.version >= 3) {
    size_t len = 0;
    void *part;
    for (unsigned int i = 0; (part = audio_state_part(i, &len)) != NULL; i++) {
      if (f_read(&fil, part, (UINT)len, &br) != FR_OK || br != len) break;
    }
  }
#endif
  f_close(&fil);

  /* Restore palette, grid & scale setting */
  if (header.palette_idx < 15) {
    persistent_palette_mode = (uint8_t)header.palette_idx;
  }
  apply_palette_mode(gb);
  persistent_grid_enabled = (header.grid_enabled != 0u);
  mk_ili9225_set_grid_enabled(persistent_grid_enabled);

  if (header.version >= 2 && header.scale_mode <= (uint32_t)SCALE_INTEGER) {
    persistent_scale_mode = (ili9341_scale_mode_t)header.scale_mode;
    mk_ili9225_set_scale_mode(persistent_scale_mode);
  }
  mk_ili9225_set_shimmerless_enabled(persistent_shimmerless_enabled);
  save_emulator_config();

  printf("I load_state_file(slot %u) SUCCESS\n", (unsigned)slot);
  return true;
}

/* =========================================================================
 * Clean Light UI via shared_menu
 * ========================================================================= */
static char current_rom_dir[64] = GB_DEFAULT_ROM_DIR;

#if GBC_IS_DMG_BUILD
static const char *s_gb_candidate_dirs[] = {
    "0:/roms/gb",
    "/roms/gb",
    "roms/gb",
    "0:/ROMS/GB",
    "/ROMS/GB",
    "ROMS/GB",
    "0:/roms/gbc",
    "/roms/gbc",
    "roms/gbc",
    "0:/ROMS/GBC",
    "/ROMS/GBC",
    "ROMS/GBC",
    "0:/roms",
    "/roms",
    "roms",
    "0:/",
    "/",
    NULL
};
#else
static const char *s_gb_candidate_dirs[] = {
    "0:/roms/gbc",
    "/roms/gbc",
    "roms/gbc",
    "0:/ROMS/GBC",
    "/ROMS/GBC",
    "ROMS/GBC",
    "0:/roms/gb",
    "/roms/gb",
    "roms/gb",
    "0:/ROMS/GB",
    "/ROMS/GB",
    "ROMS/GB",
    "0:/roms",
    "/roms",
    "roms",
    "0:/",
    "/",
    NULL
};
#endif

static bool is_gameboy_rom(const char *fname) {
  if (fname == NULL || fname[0] == '\0' || fname[0] == '.') {
    return false;
  }
  const char *dot = strrchr(fname, '.');
  if (dot == NULL) {
    return false;
  }
  return (strcasecmp(dot, ".gb") == 0 ||
          strcasecmp(dot, ".gbc") == 0 ||
          strcasecmp(dot, ".dmg") == 0);
}

static void gbc_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w <= 0 || h <= 0) return;
  mk_ili9225_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

static void gbc_draw_text(const char *text, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg) {
  mk_ili9225_text((char *)text, x, y, fg, bg);
}

static void gbc_fill_screen(uint16_t color) {
  mk_ili9225_fill(color);
}

static struct gb_s *s_active_gb = NULL;

static bool gbc_save_state(const char *game_name, uint8_t slot) {
  (void)game_name;
  if (s_active_gb == NULL) return false;
  return save_state_file(s_active_gb, slot);
}

static bool gbc_load_state(const char *game_name, uint8_t slot) {
  (void)game_name;
  if (s_active_gb == NULL) return false;
  return load_state_file(s_active_gb, slot);
}

/* Slot probe for the shared menu's Load State row.
 *
 * It cannot use the generic <saves_dir>/<game>_<slot>.ss lookup, because the file
 * is named after the ROM header, not the cartridge file: the same game is
 * "Tetris (USA).gb" in the ROM list and TETRIS in the state file. It also has to
 * repeat the unnumbered legacy fallback that load_state_file() applies to slot 1,
 * or the row would grey out a slot that does load -- and there are real ones on
 * the cards, e.g. 0:/saves/gbc/DK_COUNTRY.ss. */
static bool gbc_state_exists(const char *game_name, uint8_t slot) {
  (void)game_name;
  if (s_active_gb == NULL) return false;
  if (slot < 1u || slot > 4u) return false;
  if (!ensure_sd_mounted()) return false;

  char filename[64];
  build_state_filename(s_active_gb, filename, sizeof(filename), slot);
  if (dmg_state_file_present(filename)) return true;

  if (slot == 1u) {
    char rom_name[17];   /* 16-char DMG title + NUL (gb_get_rom_name writes 17) */
    gb_get_rom_name(s_active_gb, rom_name);
    for (int i = 0; i < 16 && rom_name[i] != '\0'; i++) {
      if (rom_name[i] == '/' || rom_name[i] == '\\' || rom_name[i] == ':' ||
          rom_name[i] == '*' || rom_name[i] == '?' || rom_name[i] == '"' ||
          rom_name[i] == '<' || rom_name[i] == '>' || rom_name[i] == '|' ||
          rom_name[i] == ' ') {
        rom_name[i] = '_';
      }
    }
    char legacy_name[64];
    snprintf(legacy_name, sizeof(legacy_name), GB_SAVE_DIR_0 "/%s.ss", rom_name);
    if (dmg_state_file_present(legacy_name)) return true;
  }

  return false;
}

static void gbc_apply_scale(dmg_scale_mode_t scale_mode) {
  persistent_scale_mode = (ili9341_scale_mode_t)scale_mode;
  mk_ili9225_set_scale_mode(persistent_scale_mode);
}

static void gbc_apply_effect(bool effect_on) {
  persistent_grid_enabled = effect_on;
  mk_ili9225_set_grid_enabled(persistent_grid_enabled);
}

static void gbc_apply_interleave(bool interleave_on) {
  persistent_interleave = interleave_on;
  if (s_active_gb) s_active_gb->direct.interlace = interleave_on ? 1 : 0;
}

static void gbc_apply_speed(bool turbo_on) {
  persistent_turbo_mode = turbo_on ? 1u : 0u;
}

/* Combo hooks, defined next to the descriptor that stores them. */
static void gbc_save_config(void);
static bool gbc_get_fps(void);
static void gbc_apply_fps(bool on);

static void gbc_apply_volume(uint8_t volume) {
  set_sound_volume_level(volume);
}

static void gbc_apply_brightness(uint8_t brightness) {
  set_backlight_brightness(brightness);
}

static void draw_gbc_image_row(uint8_t idx, bool selected) {
  if (idx >= 5) return;
  uint16_t wx = dmg_get_window_x();
  uint16_t ww = dmg_get_window_w();
  uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)idx * DMG_UI_ITEM_HEIGHT);
  uint16_t bg = selected ? DMG_RGB565_SEL_COLOR : DMG_RGB565_BG_COLOR;

  mk_ili9225_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, bg);

  char buf[44];
  buf[0] = '\0';

  if (idx == 0) {
    const char *scale_names[] = { "< Fit >", "< Stretch >", "< Integer >" };
    snprintf(buf, sizeof(buf), "Scale:        %s", scale_names[persistent_scale_mode]);
  } else if (idx == 1) {
    snprintf(buf, sizeof(buf), "Effect:       %s", persistent_grid_enabled ? "< LCD >" : "< Normal >");
  } else if (idx == 2) {
    snprintf(buf, sizeof(buf), "Filter:       %s", persistent_shimmerless_enabled ? "< Shimmerless >" : "< Sharp >");
  } else if (idx == 3) {
    snprintf(buf, sizeof(buf), "Palette:      %s", palette_mode_names[persistent_palette_mode]);
  } else if (idx == 4) {
    snprintf(buf, sizeof(buf), "Screen:       %s", persistent_interleave ? "< Interleave >" : "< Progressive >");
  }

  mk_ili9225_text(buf, (uint16_t)(wx + 12u), (uint16_t)(y + 7u), selected ? DMG_RGB565_TEXT_SEL_COLOR : DMG_RGB565_TEXT_COLOR, bg);
}

static void gbc_image_submenu(const dmg_emulator_desc_t *desc, const char *game_name) {
  uint8_t cursor = 0; /* 0: Scale, 1: Effect, 2: Filter, 3: Palette, 4: Screen */
  bool prev_up = true, prev_down = true, prev_left = true, prev_right = true;
  bool prev_a = true, prev_b = true;
  uint16_t wx = dmg_get_window_x();
  uint16_t ww = dmg_get_window_w();

  dmg_wait_for_all_buttons_released();

  if (dmg_is_gb_mode()) {
    mk_ili9225_fill(0x0000);
    mk_ili9225_fill_rect(wx, 0, ww, 240, DMG_RGB565_BG_COLOR);
  } else {
    mk_ili9225_fill(DMG_RGB565_BG_COLOR);
  }
  dmg_draw_top_bar(desc, GB_APP_LABEL, NULL);
  dmg_draw_game_bar(desc, game_name);
  dmg_draw_bottom_bar(desc, "L/R / A: Adjust", "B: Back");

  for (uint8_t i = 0; i < 5; i++) {
    draw_gbc_image_row(i, i == cursor);
  }
  for (uint8_t i = 5; i < 8; i++) {
    uint16_t y = (uint16_t)(DMG_UI_INGAME_LIST_START_Y + (uint16_t)i * DMG_UI_ITEM_HEIGHT);
    mk_ili9225_fill_rect(wx, y, ww, DMG_UI_ITEM_HEIGHT, DMG_RGB565_BG_COLOR);
  }

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
      cursor = (cursor == 0) ? 4 : (cursor - 1);
      draw_gbc_image_row(prev, false);
      draw_gbc_image_row(cursor, true);
    } else if (pushed_down) {
      uint8_t prev = cursor;
      cursor = (cursor == 4) ? 0 : (cursor + 1);
      draw_gbc_image_row(prev, false);
      draw_gbc_image_row(cursor, true);
    }

    if (pushed_left || pushed_right || pushed_a) {
      if (cursor == 0) {
        if (pushed_right || pushed_a) {
          persistent_scale_mode = (ili9341_scale_mode_t)((persistent_scale_mode + 1) % 3);
        } else {
          persistent_scale_mode = (ili9341_scale_mode_t)((persistent_scale_mode == 0) ? 2 : (persistent_scale_mode - 1));
        }
        /* The driver call is deliberately NOT here.
         *
         * mk_ili9225_set_scale_mode() ends in mk_ili9225_fill(0x0000): it
         * repaints the whole panel black, which is correct when the game is
         * about to stream itself and wrong while a menu is drawn on top. Doing
         * it here blacked out every row except the one this submenu redrew --
         * the row under the cursor -- so the screen looked black with a single
         * visible line, and each cursor move brought one more row back.
         *
         * The new geometry is applied when the in-game menu closes instead; see
         * ingame_menu(). */
        save_emulator_config();
        draw_gbc_image_row(0, true);
      } else if (cursor == 1) {
        persistent_grid_enabled = !persistent_grid_enabled;
        mk_ili9225_set_grid_enabled(persistent_grid_enabled);
        save_emulator_config();
        draw_gbc_image_row(1, true);
      } else if (cursor == 2) {
        persistent_shimmerless_enabled = !persistent_shimmerless_enabled;
        mk_ili9225_set_shimmerless_enabled(persistent_shimmerless_enabled);
        save_emulator_config();
        draw_gbc_image_row(2, true);
      } else if (cursor == 3) {
        if (pushed_right || pushed_a) {
          persistent_palette_mode = (uint8_t)((persistent_palette_mode + 1) % 15);
        } else {
          persistent_palette_mode = (uint8_t)((persistent_palette_mode == 0) ? 14 : (persistent_palette_mode - 1));
        }
        if (s_active_gb) apply_palette_mode(s_active_gb);
        save_emulator_config();
        draw_gbc_image_row(3, true);
      } else if (cursor == 4) {
        persistent_interleave = !persistent_interleave;
        if (s_active_gb) s_active_gb->direct.interlace = persistent_interleave ? 1 : 0;
        save_emulator_config();
        draw_gbc_image_row(4, true);
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

#if ENABLE_SOUND
/* Resume intro chime (dmg_boot_intro.c, core 1). i2s_dma_write() does not
 * block: it drops whatever does not fit in the 2048-sample ring, so the intro
 * is told the real free space and pushes one sample at a time. Reporting a
 * fixed amount here (as before) lost most of the chime and changed its tone. */
static int gbc_intro_audio_free(void) {
  return i2s_config_ready ? (int)i2s_ring_free() : 0;
}
static void gbc_intro_audio_push(int16_t l, int16_t r) {
  if (i2s_config_ready) i2s_ring_push(&i2s_config, l, r);
}
/* After the intro: ring and DMA back to the state a normal game start has
 * (the game calls i2s_audio_start() again when it starts its own audio). */
static void gbc_audio_restore(void) {
  if (i2s_config_ready) i2s_audio_start(&i2s_config);
}
#endif

/* Battery save to the SD before a standby (dmg_power_off_with_resume). */
static void gbc_flush_saves(void) {
#if ENABLE_SDCARD
  /* write_cart_ram_file() checks the mount itself. */
  if (s_active_gb != NULL) write_cart_ram_file(s_active_gb);
#endif
}

static const dmg_emulator_desc_t s_gb_desc_storage = {
    .app_title            = GB_APP_LABEL,
    .default_rom_dir      = GB_DEFAULT_ROM_DIR,
    .candidate_dirs       = s_gb_candidate_dirs,
    .config_path          = GB_CONFIG_PATH_0,
    .config_fallback      = GB_CONFIG_PATH_ROOT,
    .saves_dir            = GB_SAVE_DIR_0,
    .config_magic         = EMU_CONFIG_MAGIC,
    .is_rgb555            = false,
    .display = {
        .fill_rect        = gbc_fill_rect,
        .draw_text        = gbc_draw_text,
        .flush_rows       = NULL,
        .fill_screen      = gbc_fill_screen,
        .flush            = NULL,
        .stop_blitter     = park_lcd_core1,
        .start_blitter    = unpark_lcd_core1,
        .write_rect_565   = mk_ili9225_write_rect_565
    },
    .is_rom_file          = is_gameboy_rom,
    .on_load_rom          = NULL,
    // On the console build the ROM goes to PSRAM, so the loading screen must not
    // claim to write a flash cache. s_use_psram is only known at boot (after
    // this struct is built), so pick the text here and let load_cart_rom_file()
    // fall back to the flash wording when no chip answered.
    .loading_status       = GB_USE_PSRAM ? "Writing PSRAM cache..." : NULL,
    .save_state           = gbc_save_state,
    .load_state           = gbc_load_state,
    .apply_scale          = gbc_apply_scale,
    .apply_effect         = gbc_apply_effect,
    .apply_interleave     = gbc_apply_interleave,
    .apply_speed          = gbc_apply_speed,
    .apply_volume         = gbc_apply_volume,
    .apply_brightness     = gbc_apply_brightness,
    .reset_game           = NULL,
    .exit_game            = NULL,
    .custom_submenu_title = "Palette >",
    .custom_submenu       = gbc_image_submenu,
    .save_config          = gbc_save_config,
    .get_fps              = gbc_get_fps,
    .apply_fps            = gbc_apply_fps,
    .state_exists         = gbc_state_exists,
#if ENABLE_SOUND
    .audio_free           = gbc_intro_audio_free,
    .audio_push           = gbc_intro_audio_push,
#endif
    .flush_saves          = gbc_flush_saves,
#if ENABLE_SOUND
    .audio_restore        = gbc_audio_restore
#endif
};

/* Combo hooks. The FPS overlay here is drawn by lcd_draw_line() straight from
 * the `display_fps` flag (rows 8..15, line 1046), not by a driver call, so
 * apply_fps only has to move the flag and persist it. */
static void gbc_save_config(void) {
  save_emulator_config();
}

static bool gbc_get_fps(void) {
  return display_fps;
}

static void gbc_apply_fps(bool on) {
  display_fps = on;
  save_emulator_config();
  printf("I FPS overlay: %s\n", display_fps ? "ON" : "OFF");
}

// The descriptor is mutable (unlike the storage above) because the loading
// footer has to reflect where the ROM is actually going: PSRAM on the console
// build, flash when no chip answered. s_gb_desc is a copy, so the menu still
// sees a valid descriptor from the first draw.
static dmg_emulator_desc_t s_gb_desc = s_gb_desc_storage;

static ingame_action_e ingame_menu(struct gb_s *gb) {
  s_active_gb = gb;
  ingame_action_e act = dmg_ingame_menu(&s_gb_desc, current_loaded_game_name);

  /* Apply the scale now that the menu is gone, not while it is up.
   *
   * The Scale row lives in the "Palette >" submenu, and changing it used to
   * call mk_ili9225_set_scale_mode() right there. That function ends in
   * mk_ili9225_fill(0x0000), so the panel went black except for the single row
   * the submenu had just redrawn -- the row under the cursor. Moving the cursor
   * repainted one more row, which is why the menu seemed to "repair" itself one
   * line at a time, and returning to the parent menu brought everything back
   * (that path does a full redraw).
   *
   * Deferring it here is safe on all three exits: Resume and Reset both do
   * mk_ili9225_fill(0x0000) and un-park the LCD core immediately afterwards, so
   * the game streams itself over a blank panel exactly as before. Exit leaves for
   * the ROM selector, which redraws the whole screen anyway.
   *
   * The setter returns early when the mode and the GB window are unchanged, so
   * opening the menu without touching Scale costs one comparison. */
  mk_ili9225_set_scale_mode(persistent_scale_mode);

  return act;
}

void load_cart_rom_file(char *filename) {
  set_current_game_name_from_path(filename);
  UINT br;
  static uint8_t block_buf_a[FLASH_BLOCK_SIZE];
  uint8_t *cur_buf = block_buf_a;
  bool mismatch = false;
#if !GB_USE_PSRAM
  /* On the PSRAM build the counter is file-static, because the ROM reader
   * function fills it from above this point. */
  uint32_t bytes_loaded = 0;
#endif
  const uint8_t *flash_rom = (const uint8_t *)(uintptr_t)FLASH_TARGET_OFFSET_ABS;
  char full_path[256];

  if (strchr(filename, '/') != NULL || current_rom_dir[0] == '\0') {
    snprintf(full_path, sizeof(full_path), "%s", filename);
  } else {
    size_t dlen = strlen(current_rom_dir);
    if (dlen > 0 && current_rom_dir[dlen - 1] == '/') {
      snprintf(full_path, sizeof(full_path), "%s%s", current_rom_dir, filename);
    } else {
      snprintf(full_path, sizeof(full_path), "%s/%s", current_rom_dir, filename);
    }
  }

  printf("[I] ROM loading starts: %s\n", full_path);

  /* Park core 1 gracefully so it does not touch SPI0 or flash */
  park_lcd_core1();

#if ENABLE_LCD
  /* Screen is drawn cleanly in draw_clean_light_loading_screen */
#endif

  if (!ensure_sd_mounted()) {
    printf("E SD not mounted for load_cart_rom_file\n");
    __atomic_store_n(&lcd_core_parked, false, __ATOMIC_SEQ_CST);
    return;
  }

  if (!last_rom_marker_loaded) {
    load_last_rom_marker();
  }

  FIL fil;
  FRESULT fr = f_open(&fil, full_path, FA_READ);
  if (fr == FR_OK) {
    uint32_t rom_file_size = (uint32_t)f_size(&fil);

#if GB_USE_PSRAM
    /* Console build: the ROM goes to PSRAM and the flash is never touched.
     * Falls through to the flash path below if no PSRAM answered at boot, so a
     * card that cannot map the chip still loads games instead of failing. */
    if (rom_file_size == 0) {
      printf("E ROM file is empty\n");
      mismatch = true;
    } else if (!s_use_psram) {
      printf("[I] PSRAM unavailable, using flash cache\n");
    } else {
      printf("[I] ROM file size: %u bytes\n", rom_file_size);
#if ENABLE_LCD
      dmg_draw_loading_screen(&s_gb_desc, "CARREGANDO ROM", filename, rom_file_size);
#endif
      if (gb_load_rom_to_psram(&fil, rom_file_size, &s_gb_desc)) {
#if ENABLE_LCD
        dmg_update_loading_progress(&s_gb_desc, bytes_loaded, rom_file_size);
        // Brief pause so the completed bar is visible before the game starts.
        sleep_ms(150);
#endif
        s_psram_load_done = true;
        /* No flash marker is stored: the flash cache is not used on this board. */
      } else {
        mismatch = true;
      }
    }
#endif

    /* Flash cache path. On the PSRAM build this is only reached when no chip
     * answered at boot; otherwise it is the only path. */
    if (!mismatch && !s_psram_load_done) {
      if (rom_file_size > ROM_FLASH_MAX_SIZE) {
        printf("E ROM too large: %u bytes > %u reserved\n", rom_file_size,
               (uint32_t)ROM_FLASH_MAX_SIZE);
        mismatch = true;
      } else {
        printf("[I] ROM file size: %u bytes\n", rom_file_size);
      }

    /* Check if this exact ROM is already flashed: cheaply verify first 4KB */
    bool already_flashed = (!mismatch && last_flashed_rom_size == rom_file_size && rom_file_size > 0);
    if (already_flashed) {
      uint8_t chk[FLASH_SECTOR_SIZE];
      UINT cbr = 0;
      fr = f_read(&fil, chk, sizeof(chk), &cbr);
      if (fr == FR_OK && cbr > 0) {
        for (uint32_t i = 0; i < cbr; i++) {
          if (chk[i] != flash_rom[i]) {
            already_flashed = false;
            break;
          }
        }
      } else {
        already_flashed = false;
      }
      f_lseek(&fil, 0);
    }

    if (already_flashed) {
      printf("[I] ROM already in flash, skipping write\n");
#if ENABLE_LCD
      dmg_draw_loading_screen(&s_gb_desc, "CARREGANDO ROM", filename, rom_file_size);
      sleep_ms(150);
#endif
      rom = flash_rom;
      rom_size = rom_file_size;
      bytes_loaded = rom_file_size;
    } else if (!mismatch) {
      printf("[I] ROM flashing needed, writing to flash\n");
#if ENABLE_LCD
      dmg_draw_loading_screen(&s_gb_desc, "CARREGANDO ROM", filename, rom_file_size);
#endif
      gpio_init(25); gpio_set_dir(25, GPIO_OUT); gpio_put(25, 1);
      dmg_boot_intro_flash_begin();

      uint32_t flash_target_offset = FLASH_TARGET_OFFSET;
      uint32_t block_start = 0;

      while (block_start < rom_file_size && !mismatch) {
        memset(cur_buf, 0xFF, FLASH_BLOCK_SIZE);
        fr = f_read(&fil, cur_buf, FLASH_BLOCK_SIZE, &br);
        if (fr != FR_OK) {
          printf("E f_read error: %s (%d) at byte %lu\n", FRESULT_str(fr), fr,
                 (unsigned long)block_start);
          mismatch = true;
          break;
        }
        if (br == 0u) {
          break; /* EOF */
        }

        /* 64 KB block erase with closed interrupts */
        uint32_t flash_ints = save_and_disable_interrupts();
        flash_range_erase(flash_target_offset, FLASH_BLOCK_SIZE);
        restore_interrupts(flash_ints);

        /* Program in 4 KB sectors */
        const uint32_t prog_len = br & ~(FLASH_PAGE_SIZE - 1u);
        for (uint32_t off = 0; off < prog_len; off += FLASH_SECTOR_SIZE) {
          flash_ints = save_and_disable_interrupts();
          flash_range_program(flash_target_offset + off, cur_buf + off,
                              FLASH_SECTOR_SIZE);
          restore_interrupts(flash_ints);

          /* Read back target region and check programming */
          for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
            if (flash_rom[(flash_target_offset + off - FLASH_TARGET_OFFSET) + i] !=
                cur_buf[off + i]) {
              mismatch = true;
            }
          }
        }

        block_start += br;
        bytes_loaded = block_start;
        flash_target_offset += FLASH_BLOCK_SIZE;

#if ENABLE_LCD
        if (rom_file_size > 0) {
          dmg_update_loading_progress(&s_gb_desc, bytes_loaded, rom_file_size);
          gpio_put(25, (bytes_loaded >> 16) & 1);
        }
#endif
      }

      if (!mismatch && bytes_loaded > 0) {
#if ENABLE_LCD
        dmg_update_loading_progress(&s_gb_desc, rom_file_size, rom_file_size);
#endif
        rom = flash_rom;
        rom_size = bytes_loaded;
        last_flashed_rom_size = rom_file_size;
        store_last_rom_marker(rom_file_size);
        gpio_put(25, 0);
        printf("[I] Programming successful!\n");
      } else {
        printf("E Programming failed!\n");
      }
    }
    }   // end: flash path (only entered when PSRAM did not take the ROM)
    f_close(&fil);
  } else {
    printf("E f_open(%s) error: %s (%d)\n", full_path, FRESULT_str(fr), fr);
  }

  /* Unpark core 1 */
  __atomic_store_n(&lcd_core_parked, false, __ATOMIC_SEQ_CST);

  printf("I load_cart_rom_file(%s) COMPLETE (%lu bytes)\n", full_path,
         (unsigned long)bytes_loaded);
}


static char s_selected_rom_buf[256];

void rom_file_selector(const char *auto_rom_filename) {
  if (auto_rom_filename && auto_rom_filename[0] != '\0') {
    load_cart_rom_file((char *)auto_rom_filename);
    return;
  }
  if (dmg_rom_selector(&s_gb_desc, s_selected_rom_buf, sizeof(s_selected_rom_buf))) {
    load_cart_rom_file(s_selected_rom_buf);
  }
}

#endif

int main(void) {
  static struct gb_s gb;
  enum gb_init_error_e ret;
#if ENABLE_SDCARD
  bool sd_ready = false;
#endif
#if ENABLE_NFC
  char nfc_rom_filename[256];
  bool nfc_boot_pending = false;
#endif

  /* Initialise USB serial connection for debugging. */
  stdio_init_all();
  console_startup_sync();
  time_init();
  putstdio("INIT: ");
  check_bootsel_combo();

#if ENABLE_NFC
  nfc_boot_pending =
      nfc_try_select_rom(nfc_rom_filename, sizeof(nfc_rom_filename));
#endif

  /* Overclock. RP2350 has a separate DVS-style VREG (POWMAN) and needs a
   * higher rail voltage for overclocking than the RP2040.
   * Clock defaults come from CMake (RP2350: 302.4 MHz at 1.30 V). */
  {
    const unsigned vco = RP2040_GB_SYS_CLOCK_VCO;
    const unsigned div1 = RP2040_GB_SYS_CLOCK_DIV1;
    const unsigned div2 = RP2040_GB_SYS_CLOCK_DIV2;

#if PICO_RP2350
    vreg_set_voltage(VREG_VOLTAGE_1_30);
#else
    vreg_set_voltage(VREG_VOLTAGE_1_15);
#endif
    sleep_ms(2);
    set_sys_clock_pll(vco, div1, div2);
    sleep_ms(2);
  }
  printf("Clock: sys=%lu Hz (vco=%u div1=%u div2=%u)\n",
         (unsigned long)clock_get_hz(clk_sys), RP2040_GB_SYS_CLOCK_VCO,
         RP2040_GB_SYS_CLOCK_DIV1, RP2040_GB_SYS_CLOCK_DIV2);

  /* Initialise GPIO pins. */
#if USE_DMG_BUTTON_MATRIX
  gpio_set_function(GPIO_P10, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P11, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P12, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P13, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P14, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_P15, GPIO_FUNC_SIO);
#else
  gpio_set_function(GPIO_UP, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_DOWN, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_LEFT, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_RIGHT, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_A, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_B, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_SELECT, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_START, GPIO_FUNC_SIO);
#endif
  gpio_set_function(GPIO_CS, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_CLK, GPIO_FUNC_SPI);
  gpio_set_function(GPIO_SDA, GPIO_FUNC_SPI);
  gpio_set_function(GPIO_RS, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_RST, GPIO_FUNC_SIO);
  gpio_set_function(GPIO_LED, GPIO_FUNC_SIO);

#if USE_DMG_BUTTON_MATRIX
  gpio_set_dir(GPIO_P10, false);
  gpio_set_dir(GPIO_P11, false);
  gpio_set_dir(GPIO_P12, false);
  gpio_set_dir(GPIO_P13, false);
  gpio_set_dir(GPIO_P14, true);
  gpio_set_dir(GPIO_P15, true);
#else
  gpio_set_dir(GPIO_UP, false);
  gpio_set_dir(GPIO_DOWN, false);
  gpio_set_dir(GPIO_LEFT, false);
  gpio_set_dir(GPIO_RIGHT, false);
  gpio_set_dir(GPIO_A, false);
  gpio_set_dir(GPIO_B, false);
  gpio_set_dir(GPIO_SELECT, false);
  gpio_set_dir(GPIO_START, false);
#endif
  gpio_set_dir(GPIO_CS, true);
  gpio_set_dir(GPIO_RS, true);
  gpio_set_dir(GPIO_RST, true);
  gpio_set_dir(GPIO_LED, true);
  gpio_set_slew_rate(GPIO_CLK, GPIO_SLEW_RATE_FAST);
  gpio_set_slew_rate(GPIO_SDA, GPIO_SLEW_RATE_FAST);

#if USE_DMG_BUTTON_MATRIX
  gpio_pull_up(GPIO_P10);
  gpio_pull_up(GPIO_P11);
  gpio_pull_up(GPIO_P12);
  gpio_pull_up(GPIO_P13);

  /* Deselect both matrix lines by default. */
  gpio_put(GPIO_P14, true);
  gpio_put(GPIO_P15, true);
#else
  gpio_pull_up(GPIO_UP);
  gpio_pull_up(GPIO_DOWN);
  gpio_pull_up(GPIO_LEFT);
  gpio_pull_up(GPIO_RIGHT);
  gpio_pull_up(GPIO_A);
  gpio_pull_up(GPIO_B);
  gpio_pull_up(GPIO_SELECT);
  gpio_pull_up(GPIO_START);
#endif

  /* Set SPI clock to use high frequency attached to clk_sys. */
  clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                  clock_get_hz(clk_sys), clock_get_hz(clk_sys));
#if defined(DISPLAY_ILI9341) || defined(DISPLAY_ST7789)
  /* Target 72.0 MHz for SPI display (with CPU clk_sys=288 MHz, divider 4 yields 72.0 MHz).
   * Stable intermediate frequency safely below 75.6 MHz artifacts to deliver rock-solid 60 FPS. */
  unsigned spibaud = spi_init(spi0, 72000000);
  spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  printf("LCD: SPI0 actual=%u Hz\n", spibaud);
#else
  spi_init(spi0, 30 * 1000 * 1000);
  spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
#endif

#if ENABLE_SDCARD
  /* FatFs SPI uses DMA_IRQ_0 (shared), ST7789/ILI9341 uses DMA_IRQ_1 (exclusive).
   * Do NOT redirect FatFs to DMA_IRQ_1 as that causes a fatal hard_assert collision! */
  sd_ready = sd_storage_ready();
  if (sd_ready) {
    dmg_init_boot_mode();
  }
#endif

#if ENABLE_LCD
  /* Initialise display with boot.txt settings active */
  mk_ili9225_init();
  mk_ili9225_fill(0x0000);
#endif

#if ENABLE_SOUND
  // Allocate memory for the stream buffer
  stream = malloc(AUDIO_BUFFER_SIZE_BYTES);
  assert(stream != NULL);
  memset(stream, 0, AUDIO_BUFFER_SIZE_BYTES); // Zero out the stream buffer

  // Initialize I2S sound driver
  i2s_config = i2s_get_default_config();
  i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
  i2s_config.dma_trans_count = AUDIO_SAMPLES;
  i2s_volume(&i2s_config, AUDIO_DEFAULT_VOLUME);
  i2s_init(&i2s_config);
  i2s_config_ready = true;
#endif

#if ENABLE_SDCARD
  if (sd_ready) {
    load_emulator_config();
    mk_ili9225_set_scale_mode(persistent_scale_mode);
    mk_ili9225_set_grid_enabled(persistent_grid_enabled);
  }
  /* Claims the rumble pin from /boot.txt RUMBLE=, so it needs the card mounted.
   * Called outside the sd_ready branch because a card that fails to mount
   * reports no motor either way -- which is the correct answer. */
  gb_rumble_init();
#endif

#if GB_USE_PSRAM
  /* Mapped HERE, after the clock change and after the SD card is up -- not at
   * the top of main() as it first was. SetupPsram derives the QMI clkdiv and
   * rxdelay from clock_get_hz(clk_sys) to stay inside the APS6404 timing, so it
   * has to run against the final clock. Running it against the 150 MHz power-on
   * clock and then overclocking to 288 MHz left the QMI timings ~2x too slow,
   * and the first write to the ROM buffer hung the SD read with the progress bar
   * still at 0 -- the same ordering the PicoNES uses in Frens::initAll(), which
   * calls initPsram() after the clock and before initSDCard(). */
  s_psram_bytes = gb_psram_setup();
  if (s_psram_bytes == 0) {
    printf("E No PSRAM detected (CS=GPIO%d), using flash\n", GB_PSRAM_CS_PIN);
    s_use_psram = false;
    // Loading screen must say flash now, or it would promise a PSRAM write that
    // is not happening.
    s_gb_desc.loading_status = NULL;
  } else {
    printf("[I] PSRAM: %lu bytes usable\n", (unsigned long)s_psram_bytes);
    s_use_psram = true;
  }
#endif

#if ENABLE_SDCARD && ENABLE_LCD
  if (sd_ready) {
    /* Standby by save state: the power switch saves and sleeps instead of
     * idling awake, and a wake reboots back here. On a game wake the
     * bootloader skipped its intro; play it on core 1 while this core
     * loads the ROM, then restore the _R state before the first frame. */
    dmg_power_enable_resume(&s_gb_desc);
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(&s_gb_desc, &rec);
        if (rk == DMG_RESUME_GAME) {
            dmg_boot_intro_start(&s_gb_desc);
            snprintf(s_selected_rom_buf, sizeof(s_selected_rom_buf), "%s", rec.rom_file);
            s_resume_pending = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            /* Woke in the ROM selector: nothing to load, so the bootloader's
             * intro just plays out before the list is drawn. */
            if (dmg_boot_intro_start(&s_gb_desc)) dmg_boot_intro_finish();
        }
    }
    if (s_resume_pending) {
      snprintf(current_rom_dir, sizeof(current_rom_dir), "%s", dmg_get_current_rom_dir());
    }
  }
#endif

  while (true) {
    select_default_rom_source();

#if ENABLE_SDCARD
    if (sd_ready) {
#if ENABLE_LCD
#if ENABLE_NFC
        if (nfc_boot_pending) {
          printf("NFC: auto-starting %s via ROM selector path\n",
                 nfc_rom_filename);
          rom_file_selector(nfc_rom_filename);
          nfc_boot_pending = false;
        } else
#endif
        {
          /* A standby wake loads the saved game straight away. */
          rom_file_selector(s_resume_pending ? s_selected_rom_buf : NULL);
        }
#endif
    } else {
#if ENABLE_LCD
      mk_ili9225_fill(0x0000);
      rom_selector_text("Cartao MicroSD nao pronto!", 2, 0xF800, 0x0000);
      rom_selector_text("Verifique a conexao do SD", 4, 0xFFFF, 0x0000);
      if (is_launched_from_bootloader()) {
        rom_selector_text("[SELECT+START] Retornar Bootloader", 7, 0x07E0, 0x0000);
        wait_for_all_buttons_released();
        while (true) {
          bool up, down, left, right, a, b, select, start;
          read_button_states(&up, &down, &left, &right, &a, &b, &select, &start);
          if (!select && !start) {
            reboot_to_bootloader();
          }
          sleep_ms(30);
        }
      }
#endif
    }
#endif

    /* Initialise GB context. */
    refresh_rom_bank0();
    ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read, &gb_cart_ram_write,
                  &gb_error, NULL);
    putstdio("GB ");

    if (ret != GB_INIT_NO_ERROR) {
      printf("Error: %d\n", ret);
      goto out;
    }

    /* Apply Game Boy Color palette mode (GBC Auto by default or saved preference) */
    apply_palette_mode(&gb);

    /* Seed the cartridge clock before the save is loaded, so an existing .sav
     * (which carries the clock in its trailing bytes) overwrites the seed. */
    rtc_seed_if_needed(&gb);

#if ENABLE_LCD
    dmg_boot_intro_finish();   /* no-op unless a resume intro is playing */
#endif

#if ENABLE_LCD
    mk_ili9225_set_scale_mode(persistent_scale_mode);
    mk_ili9225_set_grid_enabled(persistent_grid_enabled);
    mk_ili9225_set_shimmerless_enabled(persistent_shimmerless_enabled);
    gb_init_lcd(&gb, &lcd_draw_line);
  gb.direct.interlace = persistent_interleave ? 1 : 0;
    printf("I default interlace = %u\n", (unsigned)gb.direct.interlace);

    /* Start Core1, which processes requests to the LCD. */
    ensure_lcd_core_running();
#endif

#if ENABLE_SOUND
    // Initialize audio emulation and start I2S DMA
    audio_init();
    i2s_audio_start(&i2s_config);

    putstdio("AUDIO ");
#endif

#if ENABLE_SDCARD
    /* Load Save File. */
    if (sd_ready) {
      read_cart_ram_file(&gb);
    }
#endif

    s_active_gb = &gb;
    dmg_power_set_game(s_selected_rom_buf, current_loaded_game_name);

    putstdio("\n> ");
    /* Drop any half-finished SELECT+START hold or closed edge latch left over
     * from the previous game. */
    dmg_combo_reset();
    uint_fast32_t frames = 0;
    uint64_t start_time = time_us_64();
    uint32_t fps_frames = 0;
    uint64_t fps_report_time = start_time;
    uint64_t last_fps_tick_us = start_time;
    while (1) {
      int input;
      uint64_t frame_start_time = time_us_64();

      if (persistent_turbo_mode) {
        gb.direct.frame_skip = (frames & 1);
      } else {
        gb.direct.frame_skip = 0;
      }

      gb_run_frame(&gb);

      /* Advance the cartridge clock by the time this frame actually
       * represented. gb_tick_rtc() would step a whole second per frame, which
       * runs the RTC 59.73x fast; the sub-second entry point carries the
       * remainder instead. Only cartridges with a clock are touched.
       *
       * Double speed is read back from the CGB KEY1 register because the game
       * can switch it mid-frame: a frame drawn at double speed covers twice the
       * cycles, and using the nominal figure there would leave the clock
       * behind by the amount the game spent in double speed. */
      if (gb_has_rtc(&gb)) {
#if PEANUT_FULL_GBC_SUPPORT
        uint32_t cycles = gb.cgb.doubleSpeed ? 140448u : 70224u;
#else
        uint32_t cycles = 70224u;
#endif
        gb_tick_rtc_subsecond(&gb, cycles);
      }

      /* Drive the motor from bit 3 of the RAM bank register, sampled once per
       * frame. That is the whole mechanism: the cartridge wires the motor to
       * that bit and nothing else, so following it is both correct and
       * sufficient. An earlier version also latched "rumble was requested at any
       * point this frame", on the theory that fast pulses would be missed --
       * that smeared the motor on by a frame and broke Pokemon Pinball's
       * duty-cycle texture, so it is gone. */
      if (gb.has_rumble) gb_rumble_set(gb.cart_rumble != 0);

      frames++;

      /* Standby wake: put the game back where the switch left it, now that one
       * frame has run, through the same steps the pause menu's Load State
       * takes (LCD core parked, state loaded, panel cleared, core resumed). */
      if (s_resume_pending) {
        s_resume_pending = false;
#if ENABLE_LCD
        park_lcd_core1();
#endif
        const bool resumed = gbc_load_state(current_loaded_game_name, DMG_RESUME_SLOT);
        printf("[resume] state %s\n", resumed ? "restored" : "missing, starting fresh");
#if ENABLE_LCD
        mk_ili9225_fill(0x0000);
        __atomic_store_n(&lcd_core_parked, false, __ATOMIC_SEQ_CST);
#endif
        last_fps_tick_us = time_us_64();
        dmg_resume_consume();
        dmg_power_arm_menu_loaded(1000);
      }
      fps_frames++;
#if ENABLE_LCD
      if (!gb.direct.frame_skip) {
        union core_cmd cmd;
        cmd.cmd = CORE_CMD_LCD_FRAME;
        cmd.slot = s_gb_write_idx;
        cmd.unused2 = 0u;
        cmd.data = display_fps ? (uint8_t)current_fps : 0u;
        multicore_fifo_push_blocking(cmd.full);
        s_gb_write_idx ^= 1u;
      }
#endif
#if ENABLE_SOUND
      if (!gb.direct.frame_skip) {
        audio_callback(NULL, stream, AUDIO_BUFFER_SIZE_BYTES);
        i2s_dma_write(&i2s_config, stream);
      }
#endif

      static bool s_last_hud_active = false;
      bool hud_act = dmg_volume_hud_is_active();
      if (!hud_act && s_last_hud_active) {
        if (persistent_scale_mode == SCALE_INTEGER) {
          int hx = dmg_is_gb_mode() ? 45 : 0;
          int hw = dmg_is_gb_mode() ? 266 : 320;
          mk_ili9225_fill_rect(hx, 192, hw, 8, 0x0000);
        }
      }
      s_last_hud_active = hud_act;

      /* Update buttons state */
      bool up, down, left, right, a, b, select, start;
      read_button_states(&up, &down, &left, &right, &a, &b, &select, &start);

      bool raw_up     = !up;
      bool raw_down   = !down;
      bool raw_left   = !left;
      bool raw_right  = !right;
      bool raw_a      = !a;
      bool raw_b      = !b;
      bool raw_select = !select;
      bool raw_start  = !start;

      static bool s_last_sel_right = false;
      static bool s_last_sel_left = false;
      static bool s_sta_b_edge = false;

      /* START + B: in-game pause menu.
       *
       * Handled ahead of the shared combo layer because the menu call below is
       * synchronous -- it parks the LCD core, runs the menu and resumes -- so
       * it cannot be expressed as a flag the shared layer returns. Everything
       * else goes through dmg_combo_frame(). */
      if (raw_start && raw_b) {
        if (!s_sta_b_edge) {
          s_sta_b_edge = true;
#if ENABLE_LCD
          park_lcd_core1();
#endif
          ingame_action_e act = ingame_menu(&gb);
          if (act == INGAME_ACTION_EXIT) {
            goto out;
          }
          if (act == INGAME_ACTION_RESET) {
            gb_init(&gb, &gb_rom_read, &gb_cart_ram_read, &gb_cart_ram_write,
                    &gb_error, NULL);
            /* Park the motor: a real reset returns the bank select register to
             * 0, which on a rumble cartridge is motor off. */
            gb_rumble_set(false);
            apply_palette_mode(&gb);
            gb_init_lcd(&gb, lcd_draw_line);
            gb.direct.interlace = persistent_interleave ? 1 : 0;
#if ENABLE_SOUND
            audio_init();
#endif
          }
#if ENABLE_LCD
          mk_ili9225_fill(0x0000);
          __atomic_store_n(&lcd_core_parked, false, __ATOMIC_SEQ_CST);
#endif
          wait_for_all_buttons_released();
          frame_start_time = time_us_64();
          last_fps_tick_us = frame_start_time;
          continue;
        }
      } else {
        s_sta_b_edge = false;
      }

      /* Shared combos (shared_menu/dmg_combo.c):
       *   START + A       FPS overlay
       *   SELECT + START  exit to the selector, held ~1 s
       *   SELECT + A      1x / 2x
       *   SELECT + UP/DOWN volume
       *
       * The pad is written back in pressed form and inverted again below, so the
       * shared layer sees the same polarity it sees in the other eight
       * emulators. `select` is checked afterwards to tell "the player is
       * holding SELECT" from "SELECT was consumed by a combo", which is what
       * keeps the palette shortcuts below from firing on SELECT + A. */
      {
        dmg_pad_t combo_pad;
        combo_pad.up = raw_up;
        combo_pad.down = raw_down;
        combo_pad.left = raw_left;
        combo_pad.right = raw_right;
        combo_pad.a = raw_a;
        combo_pad.b = raw_b;
        combo_pad.select = raw_select;
        combo_pad.start = raw_start;

        dmg_combo_events_t combo_ev;
        dmg_combo_frame(&combo_pad, &s_gb_desc, &combo_ev);

        raw_up = combo_pad.up;
        raw_down = combo_pad.down;
        raw_left = combo_pad.left;
        raw_right = combo_pad.right;
        raw_a = combo_pad.a;
        raw_b = combo_pad.b;
        raw_select = combo_pad.select;
        raw_start = combo_pad.start;

        /* Power switch OFF (or battery cutoff): save at this frame boundary
         * and sleep. Never returns -- the wake reboots into the resume path. */
        if (combo_ev.request_power_off) {
#if ENABLE_LCD
          park_lcd_core1();
#endif
          dmg_power_off_with_resume(&s_gb_desc, (dmg_power_reason_t)combo_ev.power_reason);
        }

        if (combo_ev.request_exit_game) {
#if ENABLE_SDCARD
          if (sd_ready) {
            write_cart_ram_file(&gb);
          }
#endif
#if ENABLE_LCD
          park_lcd_core1();
#endif
          goto out;
        }
      }

      /* Convert back to the core's active-low convention: 1 means released. */
      up = !raw_up;
      down = !raw_down;
      left = !raw_left;
      right = !raw_right;
      a = !raw_a;
      b = !raw_b;
      select = !raw_select;
      start = !raw_start;

      /* START + A: mask A and B while START is held so the game does not
       * register presses meant as shortcuts. */
      if (raw_start) {
        a = 1;
        b = 1;
      }

      /* Remaining SELECT shortcuts, only while SELECT survived the combo pass. */
      if (raw_select) {
        if (raw_right) {
          if (!s_last_sel_right) {
            persistent_palette_mode = (uint8_t)((persistent_palette_mode + 1) % 15);
            apply_palette_mode(&gb);
            save_emulator_config();
          }
          s_last_sel_right = true;
          right = 1;
        } else {
          s_last_sel_right = false;
        }

        if (raw_left) {
          if (!s_last_sel_left) {
            persistent_palette_mode = (uint8_t)((persistent_palette_mode == 0) ? 14 : (persistent_palette_mode - 1));
            apply_palette_mode(&gb);
            save_emulator_config();
          }
          s_last_sel_left = true;
          left = 1;
        } else {
          s_last_sel_left = false;
        }
      } else {
        s_last_sel_right = false;
        s_last_sel_left = false;
      }

      gb.direct.joypad_bits.up = up;
      gb.direct.joypad_bits.down = down;
      gb.direct.joypad_bits.left = left;
      gb.direct.joypad_bits.right = right;
      gb.direct.joypad_bits.a = a;
      gb.direct.joypad_bits.b = b;
      gb.direct.joypad_bits.select = select;
      gb.direct.joypad_bits.start = start;

      uint64_t frame_end_time = time_us_64();
      uint64_t frame_us = frame_end_time - frame_start_time;
      uint64_t elapsed_us = frame_end_time - fps_report_time;
      if (elapsed_us >= 1000000ULL) {
        uint64_t fps_x100 = ((uint64_t)fps_frames * 100000000ULL) / elapsed_us;
        printf("FPS: %llu.%02llu | frames=%lu elapsed_ms=%llu | "
               "last_frame_ms=%llu | frameskip=%u interlace=%u | turbo=%u\n",
               (unsigned long long)(fps_x100 / 100ULL),
               (unsigned long long)(fps_x100 % 100ULL),
               (unsigned long)fps_frames,
               (unsigned long long)(elapsed_us / 1000ULL),
               (unsigned long long)(frame_us / 1000ULL),
               (unsigned)gb.direct.frame_skip, (unsigned)gb.direct.interlace,
               (unsigned)persistent_turbo_mode);
        stdio_flush();
        fps_frames = 0;
        fps_report_time = frame_end_time;
      }

      /* Pace the emulator loop to 59.73 Hz (or ~120 Hz in Turbo 2x).
       * Keeping ~16.74 ms per frame prevents audio buffer underruns/overruns and stutter.
       * Uses busy_wait_us_32 to avoid SDK alarm scheduler jitter (~700-800us overhead). */
      {
        uint64_t now_us = time_us_64();
        uint64_t frame_target_us = persistent_turbo_mode ? (16743u / 2u) : 16743u;
        uint64_t next_us = frame_start_time + frame_target_us;
        if (now_us < next_us) {
          busy_wait_us_32((uint32_t)(next_us - now_us));
        }
      }

      /* Frame rate calculation (matching pico-infonesPlus) */
      if (display_fps) {
        uint64_t now_us = time_us_64();
        uint32_t tick_us = (uint32_t)(now_us - last_fps_tick_us);
        if (tick_us > 0 && tick_us < 500000u) {
          uint32_t calculated_fps = (1000000u - 1u) / tick_us + 1u;
          if (calculated_fps > 99u) calculated_fps = 99u;
          current_fps = calculated_fps;
        }
        last_fps_tick_us = now_us;
      } else {
        last_fps_tick_us = time_us_64();
      }

      /* Serial monitor commands */
      input = getchar_timeout_us(0);
      if (input == PICO_ERROR_TIMEOUT)
        continue;

      switch (input) {
#if 0
		static bool invert = false;
		static bool sleep = false;
		static uint8_t freq = 1;
		static ili9225_color_mode_e colour = ILI9225_COLOR_MODE_FULL;

		case 'i':
			invert = !invert;
			mk_ili9225_display_control(invert, colour);
			break;

		case 'f':
			freq++;
			freq &= 0x0F;
			mk_ili9225_set_drive_freq(freq);
			printf("Freq %u\n", freq);
			break;
#endif
      case 'c': {
        static ili9225_color_mode_e mode = ILI9225_COLOR_MODE_FULL;
        union core_cmd cmd;

        mode = !mode;
        cmd.cmd = CORE_CMD_IDLE_SET;
        cmd.data = mode;
        multicore_fifo_push_blocking(cmd.full);
        break;
      }

      case 'i':
        gb.direct.interlace = !gb.direct.interlace;
        break;

      case 'f':
        gb.direct.frame_skip = !gb.direct.frame_skip;
        break;

      case 'b': {
        uint64_t end_time;
        uint32_t diff;
        uint32_t fps;

        end_time = time_us_64();
        diff = end_time - start_time;
        fps = ((uint64_t)frames * 1000 * 1000) / diff;
        printf("Frames: %u\n"
               "Time: %lu us\n"
               "FPS: %lu\n",
               frames, diff, fps);
        stdio_flush();
        frames = 0;
        start_time = time_us_64();
        break;
      }

      case 'n':
      case ']': {
        /* Serial: next manual color palette (same as select + right). */
        persistent_palette_mode = (uint8_t)((persistent_palette_mode + 1) % 15);
        apply_palette_mode(&gb);
        save_emulator_config();
        printf("I palette mode = %u (%s)\n", persistent_palette_mode, palette_mode_names[persistent_palette_mode]);
        break;
      }

      case 'p':
      case '[': {
        /* Serial: previous manual color palette (same as select + left). */
        persistent_palette_mode = (uint8_t)((persistent_palette_mode == 0) ? 14 : (persistent_palette_mode - 1));
        apply_palette_mode(&gb);
        save_emulator_config();
        printf("I palette mode = %u (%s)\n", persistent_palette_mode, palette_mode_names[persistent_palette_mode]);
        break;
      }

      case 'g':
      case 'G': {
        /* Serial: force DMG-green manual palette. */
        persistent_palette_mode = 13;
        apply_palette_mode(&gb);
        save_emulator_config();
        printf("I palette mode = %u (%s)\n", persistent_palette_mode, palette_mode_names[persistent_palette_mode]);
        break;
      }

      case 'l':
      case 'L': {
        /* Serial: force Game Boy Light manual palette. */
        persistent_palette_mode = 14;
        apply_palette_mode(&gb);
        save_emulator_config();
        printf("I palette mode = %u (%s)\n", persistent_palette_mode, palette_mode_names[persistent_palette_mode]);
        break;
      }

      case 'a':
      case 'A': {
        /* Serial: restore automatic per-game palette. */
        persistent_palette_mode = 0;
        apply_palette_mode(&gb);
        save_emulator_config();
        printf("I palette mode = 0 (GBC Auto)\n");
        break;
      }

      case '?':
      case 'h':
      case 'H': {
        printf("Serial keys: n/] next palette, p/[ prev palette, g DMG green, "
               "a auto palette\n");
        break;
      }

      case '\n':
      case '\r': {
        gb.direct.joypad_bits.start = 0;
        break;
      }

      case '\b': {
        gb.direct.joypad_bits.select = 0;
        break;
      }

      case '8': {
        gb.direct.joypad_bits.up = 0;
        break;
      }

      case '2': {
        gb.direct.joypad_bits.down = 0;
        break;
      }

      case '4': {
        gb.direct.joypad_bits.left = 0;
        break;
      }

      case '6': {
        gb.direct.joypad_bits.right = 0;
        break;
      }

      case 'z':
      case 'w': {
        gb.direct.joypad_bits.a = 0;
        break;
      }

      case 'x': {
        gb.direct.joypad_bits.b = 0;
        break;
      }

      case 'q':
#if ENABLE_LCD
        park_lcd_core1();
#endif
        goto out;

      default:
        break;
      }
    }
out:
    puts("\nEmulation Ended");
    dmg_power_set_game(NULL, NULL);
#if ENABLE_LCD
    dmg_boot_intro_finish();   /* a resume that failed before the game started */
#endif
    if (s_resume_pending) {
      s_resume_pending = false;
      dmg_resume_consume();
    }
    /* Park the motor: a game that leaves the enable bit set -- a reset, a crash,
     * or a title that simply exits while the motor is running -- would keep
     * spinning with nothing driving it. */
    gb_rumble_shutdown();
#if ENABLE_SOUND
    i2s_audio_stop(&i2s_config);
#endif
    /* stop lcd task running on core 1 */
#if ENABLE_LCD
    park_lcd_core1();
#endif
    multicore_reset_core1();
    __atomic_store_n(&lcd_core_running, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&lcd_core_ready, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&lcd_core_parked, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&loading_anim_active, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&loading_anim_done, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&loading_anim_rom_done, false, __ATOMIC_SEQ_CST);
  }
}
