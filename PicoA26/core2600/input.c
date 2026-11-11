
#include <stdint.h>
#include <stdbool.h>

#include "atari2600emulator.h"
#include "core_state.h"
#include "platform.h"

/*
    0280    SWCHA   11111111  Port A; input or output  (read or write)
    0281    SWACNT  11111111  Port A DDR, 0= input, 1=output
    0282    SWCHB   11111111  Port B; console switches (read only)
    0283    SWBCNT  11111111  Port B DDR (hardwired as input)

    38      INPT0   1.......  read pot port
    39      INPT1   1.......  read pot port
    3A      INPT2   1.......  read pot port
    3B      INPT3   1.......  read pot port
    3C      INPT4   1.......  read input
    3D      INPT5   1.......  read input
*/

uint8_t swacnt = 0x00;
uint8_t swbcnt = 0x00;
uint8_t swcha = 0xFF;
bool reset_btn_pressed;
bool select_btn_pressed;
bool color_on = 1;
bool p0_expert_on;
bool p1_expert_on;
int input_mode; // joystick, paddle etc
bool vblank6, vblank7;
uint8_t inpt0=0x80, inpt1=0x80, inpt2=0x80, inpt3=0x80;
uint8_t inpt4=0x80, inpt5=0x80; // these are latched, so must be saved
Joystick joystick0, joystick1;
Paddle paddle0, paddle1, paddle2, paddle3;
Keypad keypad0, keypad1;

static uint8_t s_ora = 0x00;
static uint8_t s_orb = 0x00;
static uint8_t s_joy_in = 0xFF;
static uint8_t s_fire0_in = 0x80;
static uint8_t s_fire1_in = 0x80;
static uint8_t s_latch4 = 0x80;
static uint8_t s_latch5 = 0x80;

void A26_RAMFUNC(input_set_physical_controls) (uint8_t joy, bool fire0, bool fire1, bool select, bool reset) {
    s_joy_in = joy;
    s_fire0_in = fire0 ? 0x00 : 0x80;
    s_fire1_in = fire1 ? 0x00 : 0x80;
    if (vblank6 && fire0) s_latch4 = 0x00;
    if (vblank6 && fire1) s_latch5 = 0x00;
    select_btn_pressed = select;
    reset_btn_pressed = reset;
    swcha = (s_ora | (uint8_t)~swacnt) & s_joy_in;
    inpt4 = s_fire0_in;
    inpt5 = s_fire1_in;
    timer_sample_port();
}

void A26_RAMFUNC(input_reset_latches) (void) {
    s_latch4 = 0x80;
    s_latch5 = 0x80;
}

void input_reset_ports(void) {
    s_ora = s_orb = swacnt = swbcnt = 0;
    s_joy_in = swcha = 0xff;
    s_fire0_in = s_fire1_in = inpt0 = inpt1 = inpt2 = inpt3 = inpt4 = inpt5 = 0x80;
    vblank6 = vblank7 = false;
    reset_btn_pressed = select_btn_pressed = false;
    color_on = true;
    p0_expert_on = p1_expert_on = false;
    input_reset_latches();
}

uint8_t A26_RAMFUNC(input_INPT0_read) ( void ) {
    return inpt0;
}

uint8_t A26_RAMFUNC(input_INPT1_read) ( void ) {
    return inpt1;
}

uint8_t A26_RAMFUNC(input_INPT2_read) ( void ) {
    return inpt2;
}

uint8_t A26_RAMFUNC(input_INPT3_read) ( void ) {
    return inpt3;
}

uint8_t A26_RAMFUNC(input_INPT4_read) ( void ) {
    if (vblank6) {
        if (s_fire0_in == 0x00) s_latch4 = 0x00;
        return s_latch4;
    }
    return s_fire0_in;
}

uint8_t A26_RAMFUNC(input_INPT5_read) ( void ) {
    if (vblank6) {
        if (s_fire1_in == 0x00) s_latch5 = 0x00;
        return s_latch5;
    }
    return s_fire1_in;
}

uint8_t __not_in_flash_func(input_SWCHA_read) ( void ) {
    return (s_ora | (uint8_t)~swacnt) & s_joy_in;
}

uint8_t A26_RAMFUNC(input_SWACNT_read) ( void ) {
    return swacnt;
}

uint8_t A26_RAMFUNC(input_SWCHB_read) ( void ) {
    uint8_t val = (p1_expert_on ? 0x80 : 0)
                | (p0_expert_on ? 0x40 : 0)
                | 0x30 /* unused bits 4 and 5 high */
                | (color_on ? 0x08 : 0)
                | 0x04 /* bit 2 high */
                | (!select_btn_pressed ? 0x02 : 0)
                | (!reset_btn_pressed ? 0x01 : 0);
    return (s_orb & swbcnt) | (val & ~swbcnt);
}

uint8_t A26_RAMFUNC(input_SWBCNT_read) ( void ) {
    return swbcnt;
}

void A26_RAMFUNC(input_SWCHA_write) ( uint8_t value ) {
    s_ora = value;
    swcha = (s_ora | (uint8_t)~swacnt) & s_joy_in;
    timer_sample_port();
}

void A26_RAMFUNC(input_SWACNT_write) ( uint8_t value ) {
    swacnt = value;
    swcha = (s_ora | (uint8_t)~swacnt) & s_joy_in;
    timer_sample_port();
}

void A26_RAMFUNC(input_SWCHB_write) ( uint8_t value ) {
    s_orb = value;
}

void A26_RAMFUNC(input_SWBCNT_write) ( uint8_t value ) {
    swbcnt = value;
}





float input_paddle_load_amount(Paddle p) {
    float scanlines = (p.pod_angle + 135) / 270.0;
    return 100.0 / (scanlines*380*228 + 1);
}

void input_register_inputs(void) {
}



void input_export_state(A26InputState *s) {
    *s = (A26InputState){s_ora,s_orb,s_joy_in,s_fire0_in,s_fire1_in,s_latch4,s_latch5,
        swacnt,swbcnt,{inpt0,inpt1,inpt2,inpt3},reset_btn_pressed,select_btn_pressed,
        color_on,p0_expert_on,p1_expert_on,vblank6,vblank7};
}
void input_import_state(const A26InputState *s) {
    s_ora=s->ora; s_orb=s->orb; s_joy_in=s->joy;
    s_fire0_in=s->fire0; s_fire1_in=s->fire1; s_latch4=s->latch4; s_latch5=s->latch5;
    swacnt=s->ddra; swbcnt=s->ddrb;
    inpt0=s->pots[0]; inpt1=s->pots[1]; inpt2=s->pots[2]; inpt3=s->pots[3];
    reset_btn_pressed=s->reset; select_btn_pressed=s->select; color_on=s->color;
    p0_expert_on=s->expert0; p1_expert_on=s->expert1; vblank6=s->latch; vblank7=s->dump;
    swcha=input_SWCHA_read(); inpt4=s_fire0_in; inpt5=s_fire1_in;
}
