/*
 * File: mos6532.c
 * Author: dgrubb
 * Date: 06/26/2017
 *
 * Provides implementation of MOS 6532 RAM-I/O-Timer (RIOT) chip. This handles
 * both 128 bytes of RAM storage and digital I/O.
 */

#include <string.h>
#include "../mos6507/mos6507.h"
#include "mos6532.h"

static uint8_t memory[MEM_SIZE];
mos6532_timer_t timer;
uint8_t joy1_state = 0xFF;
uint8_t switches_state = 0b00001011;

void mos6532_init(void)
{
    timer = (mos6532_timer_t){0};
    timer.timer_set = MOS6532_TIMER_DIVISOR_NONE;
    timer.interval_timer = 0;
    timer.counter = 0;
    mos6532_clear_memory();
}

/* Checks that a requested memory address is within the 
 * RAM space.
 *
 * address: address to verify.
 *
 * Returns 0 on success, -1 on error.
 */
int mos6532_bounds_check(uint16_t address)
{
    if (address > MEM_SIZE) {
        return -1;
    }
    /* Flag an error */
    return 0;
}

/* Resets all RAM to zero
 */
void mos6532_clear_memory(void)
{
    memset(memory, 0, MEM_SIZE);
}

/* Loads a value from within RAM and places it into 
 * a variable given by pointer.
 *
 * Returns 0 on success, -1 on error.
 */
int mos6532_read(uint16_t address, uint8_t *data)
{
    if (address < MEM_SIZE) {
        *data = memory[address];
        return 0;
    }

    uint16_t reg = address & 0x02FF;
    switch (reg) {
        case SWCHA:
        case 0x288:
            *data = joy1_state;
            return 0;
        case SWACNT:
        case 0x289:
            *data = 0x00;
            return 0;
        case SWCHB:
        case 0x28A:
            *data = switches_state;
            return 0;
        case SWBCNT:
        case 0x28B:
            *data = 0x00;
            return 0;
        case MOS6532_MEMMAP_INTIM:
        case 0x28C:
            *data = timer.counter;
            return 0;
        case 0x285:
        case 0x28D:
            /* INSTAT: Bit 7 set if timer underflowed */
            *data = timer.fired ? 0x80 : 0x00;
            return 0;
    }

    if (address < 0x80) {
        *data = memory[address];
    } else {
        *data = 0x00;
    }
    return 0;
}

int mos6532_set_timer(mos6532_timer_divisor_t divisor, uint8_t data)
{
    timer.timer_set = divisor;
    timer.interval_timer = (uint16_t)divisor;
    timer.counter = data;
    timer.fired = 0;
    return 0;
}

/* Writes to a RAM address.
 *
 * Returns 0 on success, -1 on error.
 */
int mos6532_write(uint16_t address, uint8_t data)
{
    if (address < MEM_SIZE) {
        memory[address] = data;
        return 0;
    }

    uint16_t reg = address & 0x02FF;
    switch (reg) {
        case SWCHA:
        case 0x288:
            joy1_state = data;
            return 0;
        case SWCHB:
        case 0x28A:
            switches_state = data;
            return 0;
        case SWACNT:
        case 0x289:
        case SWBCNT:
        case 0x28B:
            return 0;
        case MOS6532_MEMMAP_TIM1T:
        case 0x29C:
            return mos6532_set_timer(MOS6532_TIMER_DIVISOR_T1, data);
        case MOS6532_MEMMAP_TIM8T:
        case 0x29D:
            return mos6532_set_timer(MOS6532_TIMER_DIVISOR_T8, data);
        case MOS6532_MEMMAP_TIM64T:
        case 0x29E:
            return mos6532_set_timer(MOS6532_TIMER_DIVISOR_T64, data);
        case MOS6532_MEMMAP_TIM1024T:
        case 0x29F:
            return mos6532_set_timer(MOS6532_TIMER_DIVISOR_T1024, data);
    }

    if (address < 0x80) {
        memory[address] = data;
    }
    return 0;
}

void mos6532_clock_tick(void)
{
    if (timer.timer_set == MOS6532_TIMER_DIVISOR_NONE) {
        return;
    }

    if (timer.fired) {
        /* Underflowed: counts down every 1 CPU cycle */
        timer.counter--;
    } else {
        if (timer.interval_timer > 0) {
            timer.interval_timer--;
        }
        if (timer.interval_timer == 0) {
            timer.interval_timer = (uint16_t)timer.timer_set;
            if (timer.counter == 0) {
                timer.fired = 1;
                timer.counter = 0xFF;
            } else {
                timer.counter--;
            }
        }
    }
}


void mos6532_get_interval(mos6532_timer_divisor_t *divisor)
{
    *divisor = timer.timer_set;
}

void mos6532_get_counter(uint8_t *counter)
{
    *counter = timer.counter;
}

char * mos6532_get_divisor_str(mos6532_timer_divisor_t divisor)
{
    switch (divisor) {
        case MOS6532_TIMER_DIVISOR_T1:    return "TIM1T - 1";
        case MOS6532_TIMER_DIVISOR_T8:    return "TIM8T - 8";
        case MOS6532_TIMER_DIVISOR_T64:   return "TIM64T - 64";
        case MOS6532_TIMER_DIVISOR_T1024: return "TIM1024T - 1024";
        default:             return "Unknown";
    }
}

void mos6532_map_mirrored_addresses(uint16_t *address)
{
    if (*address & 0x0100) {
        *address &= 0x00FF;
    }
}

uint8_t *mos6532_get_raw_memory(void) {
    return memory;
}

mos6532_timer_t *mos6532_get_timer(void) {
    return &timer;
}

uint8_t mos6532_get_joy1(void) {
    return joy1_state;
}

void mos6532_set_joy1(uint8_t val) {
    joy1_state = val;
}

uint8_t mos6532_get_switches(void) {
    return switches_state;
}

void mos6532_set_switches(uint8_t val) {
    switches_state = val;
}

