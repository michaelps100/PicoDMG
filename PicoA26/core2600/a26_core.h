#ifndef _A26_CORE_H_
#define _A26_CORE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void a26_core_init(void);
uint8_t *a26_core_get_rom_buffer(size_t *max_size);
bool a26_core_load_rom(const uint8_t *rom_data, size_t size);
void a26_core_set_region_hint(const char *filename);
uint32_t a26_core_color_clock_hz(void);
void a26_core_reset(void);
void a26_core_set_controls(bool up, bool down, bool left, bool right, bool fire, bool select, bool reset);
typedef enum { A26_FRAME_VSYNC, A26_FRAME_LIMIT, A26_FRAME_JAM } a26_frame_reason_t;
typedef struct { uint32_t color_clocks, scanlines; a26_frame_reason_t reason; } a26_frame_result_t;
#define A26_NTSC_COLOR_HZ 3579545u
#define A26_PAL_COLOR_HZ 3546894u
#define A26_AUDIO_HZ 44100u
#define A26_FRAME_CLOCK_LIMIT (400u * 228u)
a26_frame_result_t a26_core_run_frame(void (*line_cb)(int line, const uint16_t *pixels), void (*audio_cb)(void));
const char *a26_core_last_error(void);
#define A26_STATE_MAX_SIZE 4096u

/* State save/load helpers */
size_t a26_core_save_state(uint8_t *dest, size_t max_size);
bool a26_core_load_state(const uint8_t *src, size_t size);

#ifdef __cplusplus
}
#endif

#endif
