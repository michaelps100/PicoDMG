#ifndef A26_INTERNAL_STATE_H
#define A26_INTERNAL_STATE_H
#include "atari2600emulator.h"
#include <stddef.h>
extern uint8_t video_color_regs[4];
void video_set_standard(int standard);
void video_sync_collision_flags(void);
void video_restore_collision_bits(void);

typedef struct {
    uint8_t ora, orb, joy, fire0, fire1, latch4, latch5;
    uint8_t ddra, ddrb, pots[4];
    bool reset, select, color, expert0, expert1, latch, dump;
} A26InputState;
typedef struct { uint8_t reg, value, delay, active; } A26DelayedEntry;
typedef struct { A26DelayedEntry entries[32]; } A26DelayedState;
void input_export_state(A26InputState *state);
void input_import_state(const A26InputState *state);
void delayed_writer_reset(void);
void delayed_writer_export(A26DelayedState *state);
bool delayed_writer_validate(const A26DelayedState *state);
void delayed_writer_import(const A26DelayedState *state);
void video_invalidate_cache(void);
void video_begin_scanline(void);
/* Lazy rendering (a26_core.c): render the pixels the beam has passed. Must be
 * called before any change to TIA render state and before collision reads. */
void a26_video_sync(void);
void a26_video_rebase(void);
/* Replays deferred timer/audio phi2 edges (a26_core.c). Must be called before
 * any RIOT access and any AUDx write. */
void a26_catch_up(void);
void video_render_span(unsigned x0, unsigned x1, uint16_t *buf);
void timer_reset(void);
void timer_edge_control(uint8_t value);
void timer_sample_port(void);
extern bool timer_edge_positive, timer_pa7_last;
extern bool timer_wrapped_this_cycle;
extern size_t cartridge_size;
extern uint8_t cartridge_bus;
void cartridge_reset(void);
int cartridge_detect(const uint8_t *rom, size_t size);
const char *cartridge_name(int type);
uint8_t cartridge_read(uint16_t address);
void cartridge_write(uint16_t address, uint8_t value);
size_t cartridge_state_size(int type);
bool cartridge_validate_state(const Cartridge *cart);
#endif
