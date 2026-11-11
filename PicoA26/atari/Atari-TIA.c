/*
 * File: Atari-TIA.c
 * Author: dgrubb
 * Date: 09/18/2017
 *
 * Implements the TIA memory map.
 */

#include <string.h>
#include <stdio.h>
#include "Atari-TIA.h"
#include "atari_audio.h"

atari_tia tia;

uint16_t tia_line_buffer[160];
uint16_t tia_color_map_rgb565[128];

/* See page 40 of docs/Stella Programmer's Guide.pdf */
uint16_t tia_player_size_map[] = {
    0b1000000000, /* 0: One copy */
    0b1010000000, /* 1: Two copies - close */
    0b1000100000, /* 2: Two copies - medium */
    0b1010100000, /* 3: Three copies - close */
    0b1000000010, /* 4: Two copies - wide */
    0b1100000000, /* 5: Double size player */
    0b1000100010, /* 6: Three copies medium */
    0b1111000000  /* 7: Quad-sized player */
};

/* Usage note:
 *
 * This table is mapped to allow for lookup from a value in one of the colour
 * registers. However, the colour reigsters don't use the least significant
 * bit so the table locations assume the value has been shifted by one bit
 * for easier alignment. See page 43 of docs/Stella Programmer's guide.pdf
 *
 * N.B: only the NTSC colour pallete is supported.
 */
uint8_t tia_rgb_color_map[128];
tia_pixel_t tia_colour_map[] = {
    /* TIA:  >>1:  Index (dec): */
    { 0x00, 0x00, 0x00, 0xFF }, /* 0x00, 0x00, 0 */
    { 0x1A, 0x1A, 0x1A, 0xFF }, /* 0x02, 0x01, 1 */
    { 0x39, 0x39, 0x39, 0xFF }, /* 0x04, 0x02, 2 */
    { 0x58, 0x58, 0x58, 0xFF }, /* 0x06, 0x03, 3 */
    { 0x7E, 0x7E, 0x7E, 0xFF }, /* 0x08, 0x04, 4 */
    { 0xA2, 0xA2, 0xA2, 0xFF }, /* 0x0A, 0x05, 5 */
    { 0xC7, 0xC7, 0xC7, 0xFF }, /* 0x0C, 0x06, 6 */
    { 0xED, 0xED, 0xED, 0xFF }, /* 0x0E, 0x07, 7 */
    { 0x19, 0x02, 0x00, 0xFF }, /* 0x10, 0x08, 8 */
    { 0x3A, 0x1F, 0x00, 0xFF }, /* 0x12, 0x09, 9 */
    { 0x5D, 0x41, 0x00, 0xFF }, /* 0x14, 0x0A, 10 */
    { 0x82, 0x64, 0x00, 0xFF }, /* 0x16, 0x0B, 11 */
    { 0xA7, 0x88, 0x00, 0xFF }, /* 0x18, 0x0C, 12 */
    { 0xCC, 0xAD, 0x00, 0xFF }, /* 0x1A, 0x0D, 13 */
    { 0xF2, 0xD2, 0x19, 0xFF }, /* 0x1C, 0x0E, 14 */
    { 0xFE, 0xFA, 0x40, 0xFF }, /* 0x1E, 0x0F, 15 */
    { 0x37, 0x00, 0x00, 0xFF }, /* 0x20, 0x10, 16 */
    { 0x5E, 0x08, 0x00, 0xFF }, /* 0x22, 0x11, 17 */
    { 0x83, 0x27, 0x00, 0xFF }, /* 0x24, 0x12, 18 */
    { 0xA9, 0x49, 0x00, 0xFF }, /* 0x26, 0x13, 19 */
    { 0xCF, 0x6C, 0x00, 0xFF }, /* 0x28, 0x14, 20 */
    { 0xF5, 0x8F, 0x17, 0xFF }, /* 0x2A, 0x15, 21 */
    { 0xFE, 0xB4, 0x38, 0xFF }, /* 0x2C, 0x16, 22 */
    { 0xFE, 0xDF, 0x6F, 0xFF }, /* 0x2E, 0x17, 23 */
    { 0x47, 0x00, 0x00, 0xFF }, /* 0x30, 0x18, 24 */
    { 0x73, 0x00, 0x00, 0xFF }, /* 0x32, 0x19, 25 */
    { 0x98, 0x13, 0x00, 0xFF }, /* 0x34, 0x1A, 26 */
    { 0xBE, 0x32, 0x16, 0xFF }, /* 0x36, 0x1B, 27 */
    { 0xE4, 0x53, 0x35, 0xFF }, /* 0x38, 0x1C, 28 */
    { 0xFE, 0x76, 0x57, 0xFF }, /* 0x3A, 0x1D, 29 */
    { 0xFE, 0x9C, 0x81, 0xFF }, /* 0x3C, 0x1E, 30 */
    { 0xFE, 0xC6, 0xBB, 0xFF }, /* 0x3E, 0x1F, 31 */
    { 0x44, 0x00, 0x08, 0xFF }, /* 0x40, 0x20, 32 */
    { 0x6F, 0x00, 0x1F, 0xFF }, /* 0x42, 0x21, 33 */
    { 0x96, 0x06, 0x40, 0xFF }, /* 0x44, 0x22, 34 */
    { 0xBB, 0x24, 0x62, 0xFF }, /* 0x46, 0x23, 35 */
    { 0xE1, 0x45, 0x85, 0xFF }, /* 0x48, 0x24, 36 */
    { 0xFE, 0x67, 0xAA, 0xFF }, /* 0x4A, 0x25, 37 */
    { 0xFE, 0x8C, 0xD6, 0xFF }, /* 0x4C, 0x26, 38 */
    { 0xFE, 0xB7, 0xF6, 0xFF }, /* 0x4E, 0x27, 39 */
    { 0x2D, 0x00, 0x4A, 0xFF }, /* 0x50, 0x28, 40 */
    { 0x57, 0x00, 0x67, 0xFF }, /* 0x52, 0x29, 41 */
    { 0x7D, 0x05, 0x8C, 0xFF }, /* 0x54, 0x2A, 42 */
    { 0xA1, 0x22, 0xB1, 0xFF }, /* 0x56, 0x2B, 43 */
    { 0xC7, 0x43, 0xD7, 0xFF }, /* 0x58, 0x2C, 44 */
    { 0xED, 0x65, 0xFE, 0xFF }, /* 0x5A, 0x2D, 45 */
    { 0xFE, 0x8A, 0xF6, 0xFF }, /* 0x5C, 0x2E, 46 */
    { 0xFE, 0xB5, 0xF7, 0xFF }, /* 0x5E, 0x2F, 47 */
    { 0x0D, 0x00, 0x82, 0xFF }, /* 0x60, 0x30, 48 */
    { 0x33, 0x00, 0xA2, 0xFF }, /* 0x62, 0x31, 49 */
    { 0x55, 0x0F, 0xC9, 0xFF }, /* 0x64, 0x32, 50 */
    { 0x78, 0x2D, 0xF0, 0xFF }, /* 0x66, 0x33, 51 */
    { 0x9C, 0x4E, 0xFE, 0xFF }, /* 0x68, 0x34, 52 */
    { 0xC3, 0x72, 0xFE, 0xFF }, /* 0x6A, 0x35, 53 */
    { 0xEB, 0x98, 0xFE, 0xFF }, /* 0x6C, 0x36, 54 */
    { 0xFE, 0xC0, 0xF9, 0xFF }, /* 0x6E, 0x37, 55 */
    { 0x00, 0x00, 0x91, 0xFF }, /* 0x70, 0x38, 56 */
    { 0x0A, 0x05, 0xBD, 0xFF }, /* 0x72, 0x39, 57 */
    { 0x28, 0x22, 0xE4, 0xFF }, /* 0x74, 0x3A, 58 */
    { 0x48, 0x42, 0xFE, 0xFF }, /* 0x76, 0x3B, 59 */
    { 0x6B, 0x64, 0xFE, 0xFF }, /* 0x78, 0x3C, 50 */
    { 0x90, 0x8A, 0xFE, 0xFF }, /* 0x7A, 0x3D, 51 */
    { 0xB7, 0xB0, 0xFE, 0xFF }, /* 0x7C, 0x3E, 52 */
    { 0xDF, 0xD8, 0xFE, 0xFF }, /* 0x7E, 0x3F, 53 */
    { 0x00, 0x00, 0x72, 0xFF }, /* 0x80, 0x40, 54 */
    { 0x00, 0x1C, 0xAB, 0xFF }, /* 0x82, 0x41, 55 */
    { 0x03, 0x3C, 0xD6, 0xFF }, /* 0x84, 0x42, 56 */
    { 0x20, 0x5E, 0xFD, 0xFF }, /* 0x86, 0x43, 57 */
    { 0x40, 0x81, 0xFE, 0xFF }, /* 0x88, 0x44, 58 */
    { 0x64, 0xA6, 0xFE, 0xFF }, /* 0x8A, 0x45, 59 */
    { 0x89, 0xCE, 0xFE, 0xFF }, /* 0x8C, 0x46, 60 */
    { 0xB0, 0xF6, 0xFE, 0xFF }, /* 0x8E, 0x47, 61 */
    { 0x00, 0x10, 0x3A, 0xFF }, /* 0x90, 0x48, 62 */
    { 0x00, 0x31, 0x6E, 0xFF }, /* 0x92, 0x49, 63 */
    { 0x00, 0x55, 0xA2, 0xFF }, /* 0x94, 0x4A, 64 */
    { 0x05, 0x79, 0xC8, 0xFF }, /* 0x96, 0x4B, 65 */
    { 0x23, 0x9D, 0xEE, 0xFF }, /* 0x98, 0x4C, 66 */
    { 0x44, 0xC2, 0xFE, 0xFF }, /* 0x9A, 0x4D, 67 */
    { 0x68, 0xE9, 0xFE, 0xFF }, /* 0x9C, 0x4E, 68 */
    { 0x8F, 0xFE, 0xFE, 0xFF }, /* 0x9E, 0x4F, 69 */
    { 0x00, 0x1F, 0x02, 0xFF }, /* 0xA0, 0x50, 70 */
    { 0x00, 0x43, 0x26, 0xFF }, /* 0xA2, 0x51, 71 */
    { 0x00, 0x69, 0x57, 0xFF }, /* 0xA4, 0x52, 72 */
    { 0x00, 0x8D, 0x7A, 0xFF }, /* 0xA6, 0x53, 73 */
    { 0x1B, 0xB1, 0x9E, 0xFF }, /* 0xA8, 0x54, 74 */
    { 0x3B, 0xD7, 0xC3, 0xFF }, /* 0xAA, 0x55, 75 */
    { 0x5D, 0xFE, 0xE9, 0xFF }, /* 0xAC, 0x56, 76 */
    { 0x86, 0xFE, 0xFE, 0xFF }, /* 0xAE, 0x57, 77 */
    { 0x00, 0x24, 0x03, 0xFF }, /* 0xB0, 0x58, 78 */
    { 0x00, 0x4A, 0x05, 0xFF }, /* 0xB2, 0x59, 79 */
    { 0x00, 0x70, 0x0C, 0xFF }, /* 0xB4, 0x5A, 80 */
    { 0x09, 0x95, 0x2B, 0xFF }, /* 0xB6, 0x5B, 81 */
    { 0x28, 0xBA, 0x4C, 0xFF }, /* 0xB8, 0x5C, 82 */
    { 0x49, 0xE0, 0x6E, 0xFF }, /* 0xBA, 0x5D, 83 */
    { 0x6C, 0xFE, 0x92, 0xFF }, /* 0xBC, 0x5E, 84 */
    { 0x97, 0xFE, 0xB5, 0xFF }, /* 0xBE, 0x5F, 85 */
    { 0x00, 0x21, 0x02, 0xFF }, /* 0xC0, 0x60, 86 */
    { 0x00, 0x46, 0x04, 0xFF }, /* 0xC2, 0x61, 87 */
    { 0x08, 0x6B, 0x00, 0xFF }, /* 0xC4, 0x62, 88 */
    { 0x28, 0x90, 0x00, 0xFF }, /* 0xC6, 0x63, 89 */
    { 0x49, 0xB5, 0x09, 0xFF }, /* 0xC8, 0x64, 90 */
    { 0x6B, 0xDB, 0x28, 0xFF }, /* 0xCA, 0x65, 91 */
    { 0x8F, 0xFE, 0x49, 0xFF }, /* 0xCC, 0x66, 92 */
    { 0xBB, 0xFE, 0x69, 0xFF }, /* 0xCE, 0x67, 93 */
    { 0x00, 0x15, 0x01, 0xFF }, /* 0xD0, 0x68, 94 */
    { 0x10, 0x36, 0x00, 0xFF }, /* 0xD2, 0x69, 95 */
    { 0x30, 0x59, 0x00, 0xFF }, /* 0xD4, 0x6A, 96 */
    { 0x53, 0x7E, 0x00, 0xFF }, /* 0xD6, 0x6B, 97 */
    { 0x76, 0xA3, 0x00, 0xFF }, /* 0xD8, 0x6C, 98 */
    { 0x9A, 0xC8, 0x00, 0xFF }, /* 0xDA, 0x6D, 99 */
    { 0xBF, 0xEE, 0x1E, 0xFF }, /* 0xDC, 0x6E, 100 */
    { 0xE8, 0xFE, 0x3E, 0xFF }, /* 0xDE, 0x6F, 101 */
    { 0x1A, 0x02, 0x00, 0xFF }, /* 0xE0, 0x70, 102 */
    { 0x3B, 0x1F, 0x00, 0xFF }, /* 0xE2, 0x71, 103 */
    { 0x5E, 0x41, 0x00, 0xFF }, /* 0xE4, 0x72, 104 */
    { 0x83, 0x64, 0x00, 0xFF }, /* 0xE6, 0x73, 105 */
    { 0xA8, 0x88, 0x00, 0xFF }, /* 0xE8, 0x74, 106 */
    { 0xCE, 0xAD, 0x00, 0xFF }, /* 0xEA, 0x75, 107 */
    { 0xF4, 0xD2, 0x18, 0xFF }, /* 0xEC, 0x76, 108 */
    { 0xFE, 0xFA, 0x40, 0xFF }, /* 0xEE, 0x77, 109 */
    { 0x38, 0x00, 0x00, 0xFF }, /* 0xF0, 0x78, 110 */
    { 0x5F, 0x08, 0x00, 0xFF }, /* 0xF2, 0x79, 111 */
    { 0x84, 0x27, 0x00, 0xFF }, /* 0xF4, 0x7A, 112 */
    { 0xAA, 0x49, 0x00, 0xFF }, /* 0xF6, 0x7B, 113 */
    { 0xD0, 0x6B, 0x00, 0xFF }, /* 0xF8, 0x7C, 114 */
    { 0xF6, 0x8F, 0x18, 0xFF }, /* 0xFA, 0x7D, 115 */
    { 0xFE, 0xB4, 0x39, 0xFF }, /* 0xFC, 0x7E, 116 */
    { 0xFE, 0xDF, 0x70, 0xFF}  /* 0xFE, 0x7F, 117 */
};

/* Resets the TIA instance to default conditions with no state set.
 */
void TIA_init(void)
{
    for (int i = 0; i < 128; ++i) {
        uint16_t r = tia_colour_map[i].R >> 3;
        uint16_t g = tia_colour_map[i].G >> 2;
        uint16_t b = tia_colour_map[i].B >> 3;
        tia_color_map_rgb565[i] = (r << 11) | (g << 5) | b;
    }

    memset(tia.write_regs, 0, sizeof(tia.write_regs));
    memset(tia.read_regs, 0, sizeof(tia.read_regs));

    // Set all inputs (joystic fire buttons) to not pressed state
    tia.read_regs[TIA_READ_REG_INPT0] = 0x80; // 0x7f - firing
    tia.read_regs[TIA_READ_REG_INPT1] = 0x80;
    tia.read_regs[TIA_READ_REG_INPT2] = 0x80;    
    tia.read_regs[TIA_READ_REG_INPT3] = 0x80;
    tia.read_regs[TIA_READ_REG_INPT4] = 0x80;
    tia.read_regs[TIA_READ_REG_INPT5] = 0x80;

    tia.missiles[0] = (tia_missile_t){0};
    tia.missiles[1] = (tia_missile_t){0};
    tia.players[0] = (tia_player_t){0};
    tia.players[1] = (tia_player_t){0};
    tia.ball = (tia_ball_t){0};
}

/* Retrieves a value in a specified register
 *
 * reg: register to read (e.g., VSYNC, VBLANK, etc ...).
 * *value: location to place retrieved value of register into.
 */
void TIA_read_register(uint8_t reg, uint8_t *value)
{
    reg &= 0x0F;
    if (reg < TIA_READ_REG_LEN) {
        *value = tia.read_regs[reg];
    } else {
        *value = 0x00;
    }
}

/* Writes a value into a register location
 *
 * reg: register to write to (e.g., CXM0P, CXM1P etc ...) value: value to place into register.
 */
void TIA_write_register(uint8_t reg, uint8_t value)
{
    reg &= 0x3F;
    if (reg >= TIA_WRITE_REG_LEN) {
        return;
    }

    /* Perform special state logic on strobing registers which influence
     * state regardless of value written. E.g., writing a 0 to WSYNC still
     * results in the processor clock suspending
     */
    switch (reg) {
        case TIA_WRITE_REG_WSYNC:
            tia.write_regs[TIA_WRITE_REG_WSYNC] = 1;
            break;
        case TIA_WRITE_REG_RSYNC:
            tia.colour_clock = 0;
            break;
        case TIA_WRITE_REG_RESP0:
            TIA_reset_player(0);
            break;
        case TIA_WRITE_REG_RESP1:
            TIA_reset_player(1);
            break;
        case TIA_WRITE_REG_RESM0:
            TIA_reset_missile(0);
            break;
        case TIA_WRITE_REG_RESM1:
            TIA_reset_missile(1);
            break;
        case TIA_WRITE_REG_RESBL:
            TIA_reset_ball();
            break;

        case TIA_WRITE_REG_RESMP0:
            tia.write_regs[TIA_WRITE_REG_RESMP0] = value;
            if (value & 0x02) {
                tia.missiles[0].position_clock = tia.players[0].position_clock;
            }
            break;
        case TIA_WRITE_REG_RESMP1:
            tia.write_regs[TIA_WRITE_REG_RESMP1] = value;
            if (value & 0x02) {
                tia.missiles[1].position_clock = tia.players[1].position_clock;
            }
            break;

        case TIA_WRITE_REG_VDELP0:
            tia.write_regs[reg] = value;
            if (!(value & 1)) {
                tia.players[0].grp = tia.write_regs[TIA_WRITE_REG_GRP0];
            }
            break;
        case TIA_WRITE_REG_VDELP1:
            tia.write_regs[reg] = value;
            if (!(value & 1)) {
                tia.players[1].grp = tia.write_regs[TIA_WRITE_REG_GRP1];
            }
            break;
        case TIA_WRITE_REG_VDELBL:
            tia.write_regs[reg] = value;
            if (!(value & 1)) {
                tia.ball.vertical_delay = tia.write_regs[TIA_WRITE_REG_ENABL];
            }
            break;

        case TIA_WRITE_REG_GRP0:
            if (tia.write_regs[TIA_WRITE_REG_VDELP0] & 1) {
                tia.players[0].grp_old = value; /* buffer */
            } else {
                tia.players[0].grp = value;     /* direct */
            }
            if (tia.write_regs[TIA_WRITE_REG_VDELP1] & 1) {
                tia.players[1].grp = tia.players[1].grp_old; /* transfer P1 buffer to active */
            }
            tia.write_regs[reg] = value;
            break;

        case TIA_WRITE_REG_GRP1:
            if (tia.write_regs[TIA_WRITE_REG_VDELP1] & 1) {
                tia.players[1].grp_old = value; /* buffer */
            } else {
                tia.players[1].grp = value;     /* direct */
            }
            if (tia.write_regs[TIA_WRITE_REG_VDELP0] & 1) {
                tia.players[0].grp = tia.players[0].grp_old; /* transfer P0 buffer to active */
            }
            if (tia.write_regs[TIA_WRITE_REG_VDELBL] & 1) {
                tia.ball.vertical_delay = tia.ball.enabled; /* transfer BL buffer to active */
            }
            tia.write_regs[reg] = value;
            break;

        case TIA_WRITE_REG_ENABL:
            if (tia.write_regs[TIA_WRITE_REG_VDELBL] & 1) {
                tia.ball.enabled = value; /* buffer */
            } else {
                tia.ball.vertical_delay = value; /* direct */
            }
            tia.write_regs[reg] = value;
            break;

        case TIA_WRITE_REG_HMOVE:
            tia.write_regs[TIA_WRITE_REG_HMOVE] = 1;
            TIA_apply_HMOVE();
            break;

        case TIA_WRITE_REG_HMP0:
            tia.write_regs[reg] = value;
            TIA_update_player_HMOVE(0);
            break;
        case TIA_WRITE_REG_HMP1:
            tia.write_regs[reg] = value;
            TIA_update_player_HMOVE(1);
            break;
        case TIA_WRITE_REG_HMM0:
            tia.write_regs[reg] = value;
            TIA_update_missile_HMOVE(0);
            break;
        case TIA_WRITE_REG_HMM1:
            tia.write_regs[reg] = value;
            TIA_update_missile_HMOVE(1);
            break;
        case TIA_WRITE_REG_HMBL:
            tia.write_regs[reg] = value;
            TIA_update_ball_HMOVE();
            break;

        case TIA_WRITE_REG_HMCLR:
            tia.write_regs[TIA_WRITE_REG_HMM0] = 0;
            tia.write_regs[TIA_WRITE_REG_HMM1] = 0;
            tia.write_regs[TIA_WRITE_REG_HMP0] = 0;
            tia.write_regs[TIA_WRITE_REG_HMP1] = 0;
            tia.write_regs[TIA_WRITE_REG_HMBL] = 0;
            tia.players[0].horizontal_offset = 0;
            tia.players[1].horizontal_offset = 0;
            tia.missiles[0].horizontal_offset = 0;
            tia.missiles[1].horizontal_offset = 0;
            tia.ball.horizontal_offset = 0;
            break;

        case TIA_WRITE_REG_CXCLR:
            /* Reset all collision latches */
            tia.read_regs[TIA_READ_REG_CXM0P] = 0;
            tia.read_regs[TIA_READ_REG_CXM1P] = 0;
            tia.read_regs[TIA_READ_REG_CXP0FB] = 0;
            tia.read_regs[TIA_READ_REG_CXP1FB] = 0;
            tia.read_regs[TIA_READ_REG_CXM0FB] = 0;
            tia.read_regs[TIA_READ_REG_CXM1FB] = 0;
            tia.read_regs[TIA_READ_REG_CXBLPF] = 0;
            tia.read_regs[TIA_READ_REG_CXPPMM] = 0;
            break;

        case TIA_WRITE_REG_AUDC0:
        case TIA_WRITE_REG_AUDC1:
        case TIA_WRITE_REG_AUDF0:
        case TIA_WRITE_REG_AUDF1:
        case TIA_WRITE_REG_AUDV0:
        case TIA_WRITE_REG_AUDV1:
            tia.write_regs[reg] = value;
            atari_audio_set_reg(reg, value);
            break;

        default:
            tia.write_regs[reg] = value;
            break;
    }
}

void TIA_reset_player(uint8_t player)
{
    tia.players[player].scanline_reset = 1;

    int pos = (int)tia.colour_clock - TIA_COLOUR_CLOCK_HSYNC;
    while (pos < 0) pos += 160;
    tia.players[player].position_clock = pos % 160;
}

void TIA_reset_missile(uint8_t missile)
{
    tia.missiles[missile].scanline_reset = 1;

    int pos = (int)tia.colour_clock - TIA_COLOUR_CLOCK_HSYNC;
    while (pos < 0) pos += 160;
    tia.missiles[missile].position_clock = pos % 160;
}

void TIA_reset_ball(void)
{
    tia.ball.scanline_reset = 1;

    int pos = (int)tia.colour_clock - TIA_COLOUR_CLOCK_HSYNC;
    while (pos < 0) pos += 160;
    tia.ball.position_clock = pos % 160;
}

void TIA_apply_HMOVE(void)
{
    int p0 = (int)tia.players[0].position_clock - tia.players[0].horizontal_offset;
    while (p0 < 0) p0 += 160;
    tia.players[0].position_clock = p0 % 160;

    int p1 = (int)tia.players[1].position_clock - tia.players[1].horizontal_offset;
    while (p1 < 0) p1 += 160;
    tia.players[1].position_clock = p1 % 160;

    int m0 = (int)tia.missiles[0].position_clock - tia.missiles[0].horizontal_offset;
    while (m0 < 0) m0 += 160;
    tia.missiles[0].position_clock = m0 % 160;

    int m1 = (int)tia.missiles[1].position_clock - tia.missiles[1].horizontal_offset;
    while (m1 < 0) m1 += 160;
    tia.missiles[1].position_clock = m1 % 160;

    int bl = (int)tia.ball.position_clock - tia.ball.horizontal_offset;
    while (bl < 0) bl += 160;
    tia.ball.position_clock = bl % 160;
}

void TIA_update_player_HMOVE(uint8_t player)
{
    uint8_t offset = tia.write_regs[player ? TIA_WRITE_REG_HMP1 : TIA_WRITE_REG_HMP0];
    tia.players[player].horizontal_offset = (offset > 127 ? -16 : 0) + (offset >> 4);
}

void TIA_update_missile_HMOVE(uint8_t missile)
{
    uint8_t offset = tia.write_regs[missile ? TIA_WRITE_REG_HMM1 : TIA_WRITE_REG_HMM0];
    tia.missiles[missile].horizontal_offset = (offset > 127 ? -16 : 0) + (offset >> 4);
}

void TIA_update_ball_HMOVE(void)
{
    uint8_t offset = tia.write_regs[TIA_WRITE_REG_HMBL];
    tia.ball.horizontal_offset = (offset > 127 ? -16 : 0) + (offset >> 4);
}

/* On-the-fly pixel evaluators */
static inline uint8_t TIA_get_player_pixel(uint8_t player, int pixel_idx)
{
    uint8_t grp = tia.players[player].grp;
    if (!grp) return 0;

    int pos = tia.players[player].position_clock;
    int dx = pixel_idx - pos;
    if (dx < 0) dx += 160;

    uint8_t nusiz = tia.write_regs[player ? TIA_WRITE_REG_NUSIZ1 : TIA_WRITE_REG_NUSIZ0] & 0x07;
    int bit = -1;

    switch (nusiz) {
        case 0: /* One copy */
            if (dx < 8) bit = dx;
            break;
        case 1: /* Two copies - close (spacing 16) */
            if (dx < 8) bit = dx;
            else if (dx >= 16 && dx < 24) bit = dx - 16;
            break;
        case 2: /* Two copies - medium (spacing 32) */
            if (dx < 8) bit = dx;
            else if (dx >= 32 && dx < 40) bit = dx - 32;
            break;
        case 3: /* Three copies - close (spacing 16 and 32) */
            if (dx < 8) bit = dx;
            else if (dx >= 16 && dx < 24) bit = dx - 16;
            else if (dx >= 32 && dx < 40) bit = dx - 32;
            break;
        case 4: /* Two copies - wide (spacing 64) */
            if (dx < 8) bit = dx;
            else if (dx >= 64 && dx < 72) bit = dx - 64;
            break;
        case 5: /* Double size (16 pixels) */
            if (dx < 16) bit = dx >> 1;
            break;
        case 6: /* Three copies - medium (spacing 32 and 64) */
            if (dx < 8) bit = dx;
            else if (dx >= 32 && dx < 40) bit = dx - 32;
            else if (dx >= 64 && dx < 72) bit = dx - 64;
            break;
        case 7: /* Quad size (32 pixels) */
            if (dx < 32) bit = dx >> 2;
            break;
    }

    if (bit < 0) return 0;

    /* REFP: bit 3 (0x08) enables horizontal reflection */
    uint8_t refp = tia.write_regs[player ? TIA_WRITE_REG_REFP1 : TIA_WRITE_REG_REFP0] & 0x08;
    if (refp) {
        return (grp >> bit) & 1;
    } else {
        return (grp >> (7 - bit)) & 1;
    }
}

static inline uint8_t TIA_get_missile_pixel(uint8_t missile, int pixel_idx)
{
    /* ENAMx: bit 1 (0x02) enables missile */
    if (!(tia.write_regs[missile ? TIA_WRITE_REG_ENAM1 : TIA_WRITE_REG_ENAM0] & 0x02)) return 0;

    /* RESMPx: bit 1 (0x02) disables missile / locks to player */
    if (tia.write_regs[missile ? TIA_WRITE_REG_RESMP1 : TIA_WRITE_REG_RESMP0] & 0x02) return 0;

    int pos = tia.missiles[missile].position_clock;
    int dx = pixel_idx - pos;
    if (dx < 0) dx += 160;

    uint8_t nusiz = tia.write_regs[missile ? TIA_WRITE_REG_NUSIZ1 : TIA_WRITE_REG_NUSIZ0];
    int width = 1 << ((nusiz >> 4) & 0x03); /* 1, 2, 4, or 8 pixels */
    uint8_t copies = nusiz & 0x07;

    switch (copies) {
        case 0:
        case 5:
        case 7:
            return (dx < width) ? 1 : 0;
        case 1:
            return (dx < width || (dx >= 16 && dx < 16 + width)) ? 1 : 0;
        case 2:
            return (dx < width || (dx >= 32 && dx < 32 + width)) ? 1 : 0;
        case 3:
            return (dx < width || (dx >= 16 && dx < 16 + width) || (dx >= 32 && dx < 32 + width)) ? 1 : 0;
        case 4:
            return (dx < width || (dx >= 64 && dx < 64 + width)) ? 1 : 0;
        case 6:
            return (dx < width || (dx >= 32 && dx < 32 + width) || (dx >= 64 && dx < 64 + width)) ? 1 : 0;
    }
    return 0;
}

static inline uint8_t TIA_get_ball_pixel(int pixel_idx)
{
    if (!(tia.ball.vertical_delay & 0x02)) return 0;

    int pos = tia.ball.position_clock;
    int dx = pixel_idx - pos;
    if (dx < 0) dx += 160;

    int width = 1 << ((tia.write_regs[TIA_WRITE_REG_CTRLPF] >> 4) & 0x03);
    return (dx < width) ? 1 : 0;
}

static inline uint8_t TIA_get_playfield_pixel(int pixel_idx)
{
    int bit_idx;
    if (pixel_idx < 80) {
        bit_idx = pixel_idx >> 2;
    } else {
        if (tia.write_regs[TIA_WRITE_REG_CTRLPF] & 0x01) {
            /* Mirror mode */
            bit_idx = (159 - pixel_idx) >> 2;
        } else {
            /* Repeat mode */
            bit_idx = (pixel_idx - 80) >> 2;
        }
    }

    if (bit_idx < 4) {
        return (tia.write_regs[TIA_WRITE_REG_PF0] >> (4 + bit_idx)) & 1;
    } else if (bit_idx < 12) {
        return (tia.write_regs[TIA_WRITE_REG_PF1] >> (7 - (bit_idx - 4))) & 1;
    } else {
        return (tia.write_regs[TIA_WRITE_REG_PF2] >> (bit_idx - 12)) & 1;
    }
}

/* Stubs for header compatibility */
void TIA_update_player_buffer(uint8_t player) { (void)player; }
void TIA_get_player_registers(uint8_t player, tia_writable_register_t *reflect,
        tia_writable_register_t *graphics, tia_writable_register_t *offset,
        tia_writable_register_t *vertical, tia_writable_register_t *size)
{
    player ? (*reflect = TIA_WRITE_REG_REFP1)   : (*reflect = TIA_WRITE_REG_REFP0);
    player ? (*graphics = TIA_WRITE_REG_GRP1)   : (*graphics = TIA_WRITE_REG_GRP0);
    player ? (*offset = TIA_WRITE_REG_HMP1)     : (*offset = TIA_WRITE_REG_HMP0);
    player ? (*vertical = TIA_WRITE_REG_VDELP1) : (*vertical = TIA_WRITE_REG_VDELP0);
    player ? (*size = TIA_WRITE_REG_NUSIZ1)     : (*size = TIA_WRITE_REG_NUSIZ0);
}
void TIA_get_missile_registers(uint8_t missile, tia_writable_register_t *enable,
        tia_writable_register_t *size, tia_writable_register_t *offset)
{
    missile ? (*enable = TIA_WRITE_REG_ENAM1) : (*enable = TIA_WRITE_REG_ENAM0);
    missile ? (*size = TIA_WRITE_REG_NUSIZ1)  : (*size = TIA_WRITE_REG_NUSIZ0);
    missile ? (*offset = TIA_WRITE_REG_HMM1)  : (*offset = TIA_WRITE_REG_HMM0);
}
void TIA_update_missile_buffer(uint8_t missile) { (void)missile; }
void TIA_update_ball_buffer(void) {}
void TIA_update_playfield(void) {}

int TIA_test_ball_bit(void)
{
    if (tia.colour_clock < TIA_COLOUR_CLOCK_HSYNC) return 0;
    return TIA_get_ball_pixel((int)tia.colour_clock - TIA_COLOUR_CLOCK_HSYNC);
}

int TIA_test_playfield_bit(void)
{
    if (tia.colour_clock < TIA_COLOUR_CLOCK_HSYNC) return 0;
    return TIA_get_playfield_pixel((int)tia.colour_clock - TIA_COLOUR_CLOCK_HSYNC);
}

int TIA_test_missile_bit(uint8_t missile)
{
    if (tia.colour_clock < TIA_COLOUR_CLOCK_HSYNC) return 0;
    return TIA_get_missile_pixel(missile, (int)tia.colour_clock - TIA_COLOUR_CLOCK_HSYNC);
}

int TIA_test_player_bit(uint8_t player)
{
    if (tia.colour_clock < TIA_COLOUR_CLOCK_HSYNC) return 0;
    return TIA_get_player_pixel(player, (int)tia.colour_clock - TIA_COLOUR_CLOCK_HSYNC);
}

uint8_t TIA_reverse_byte(uint8_t byte)
{
    byte = (byte & 0xF0) >> 4 | (byte & 0x0F) << 4;
    byte = (byte & 0xCC) >> 2 | (byte & 0x33) << 2;
    byte = (byte & 0xAA) >> 1 | (byte & 0x55) << 1;
    return byte;
}

void TIA_generate_colour(void)
{
    int pixel_idx = (int)tia.colour_clock - TIA_COLOUR_CLOCK_HSYNC;
    if (pixel_idx < 0 || pixel_idx >= TIA_COLOUR_CLOCK_VISIBLE) return;

    /* Check VBLANK: Bit 1 (0x02) blanks video and disables collisions */
    if (tia.write_regs[TIA_WRITE_REG_VBLANK] & 0x02) {
        tia_line_buffer[pixel_idx] = 0;
        return;
    }

    /* HMOVE blanking: first 8 pixels are blanked if HMOVE was triggered */
    if (tia.write_regs[TIA_WRITE_REG_HMOVE] && (pixel_idx < 8)) {
        tia_line_buffer[pixel_idx] = 0;
        return;
    }

    uint8_t M0 = TIA_get_missile_pixel(0, pixel_idx);
    uint8_t M1 = TIA_get_missile_pixel(1, pixel_idx);
    uint8_t P0 = TIA_get_player_pixel(0, pixel_idx);
    uint8_t P1 = TIA_get_player_pixel(1, pixel_idx);
    uint8_t BL = TIA_get_ball_pixel(pixel_idx);
    uint8_t PF = TIA_get_playfield_pixel(pixel_idx);

    uint8_t tia_color = tia.write_regs[TIA_WRITE_REG_COLUBK];

    if (tia.write_regs[TIA_WRITE_REG_CTRLPF] & 0x04) {
        /* Highest: PF, BL; Second: P0, M0; Third: P1, M1; Lowest: BK */
        if (PF) {
            if (tia.write_regs[TIA_WRITE_REG_CTRLPF] & 0x02) {
                tia_color = (pixel_idx < TIA_COLOUR_CLOCK_VISIBLE_HALF) ?
                    tia.write_regs[TIA_WRITE_REG_COLUP0] : tia.write_regs[TIA_WRITE_REG_COLUP1];
            } else {
                tia_color = tia.write_regs[TIA_WRITE_REG_COLUPF];
            }
        } else if (BL) {
            tia_color = tia.write_regs[TIA_WRITE_REG_COLUPF];
        } else if (P0 || M0) {
            tia_color = tia.write_regs[TIA_WRITE_REG_COLUP0];
        } else if (P1 || M1) {
            tia_color = tia.write_regs[TIA_WRITE_REG_COLUP1];
        }
    } else {
        /* Highest: P0, M0; Second: P1, M1; Third: PF, BL; Lowest: BK */
        if (P0 || M0) {
            tia_color = tia.write_regs[TIA_WRITE_REG_COLUP0];
        } else if (P1 || M1) {
            tia_color = tia.write_regs[TIA_WRITE_REG_COLUP1];
        } else if (PF) {
            if (tia.write_regs[TIA_WRITE_REG_CTRLPF] & 0x02) {
                tia_color = (pixel_idx < TIA_COLOUR_CLOCK_VISIBLE_HALF) ?
                    tia.write_regs[TIA_WRITE_REG_COLUP0] : tia.write_regs[TIA_WRITE_REG_COLUP1];
            } else {
                tia_color = tia.write_regs[TIA_WRITE_REG_COLUPF];
            }
        } else if (BL) {
            tia_color = tia.write_regs[TIA_WRITE_REG_COLUPF];
        }
    }

    tia_line_buffer[pixel_idx] = tia_color_map_rgb565[tia_color >> 1];

    /* Sticky collision latches (active until cleared by CXCLR) */
    tia.read_regs[TIA_READ_REG_CXM0P]  |= ((M0 & P1) << 7) | ((M0 & P0) << 6);
    tia.read_regs[TIA_READ_REG_CXM1P]  |= ((M1 & P0) << 7) | ((M1 & P1) << 6);
    tia.read_regs[TIA_READ_REG_CXP0FB] |= ((P0 & PF) << 7) | ((P0 & BL) << 6);
    tia.read_regs[TIA_READ_REG_CXP1FB] |= ((P1 & PF) << 7) | ((P1 & BL) << 6);
    tia.read_regs[TIA_READ_REG_CXM0FB] |= ((M0 & PF) << 7) | ((M0 & BL) << 6);
    tia.read_regs[TIA_READ_REG_CXM1FB] |= ((M1 & PF) << 7) | ((M1 & BL) << 6);
    tia.read_regs[TIA_READ_REG_CXBLPF] |= ((BL & PF) << 7);
    tia.read_regs[TIA_READ_REG_CXPPMM] |= ((P0 & P1) << 7) | ((M0 & M1) << 6);
}

int TIA_clock_tick(void)
{
    /* Reset colour clock and prepare begin next line */
    if (tia.colour_clock >= TIA_COLOUR_CLOCK_TOTAL) {
        tia.colour_clock = 0;
        tia.write_regs[TIA_WRITE_REG_WSYNC] = 0;
        tia.missiles[0].scanline_reset = 0;
        tia.missiles[1].scanline_reset = 0;
        tia.ball.scanline_reset = 0;
        tia.write_regs[TIA_WRITE_REG_HMOVE] = 0;
    }

    if (tia.colour_clock >= TIA_COLOUR_CLOCK_HSYNC) {
        TIA_generate_colour();
    }

    tia.colour_clock++;
    return tia.colour_clock;
}

int TIA_get_WSYNC()
{
    return (tia.write_regs[TIA_WRITE_REG_WSYNC] ? 1 : 0);
}

int TIA_get_VSYNC()
{
    return (tia.write_regs[TIA_WRITE_REG_VSYNC] ? 1 : 0);
}

int TIA_get_VBLANK()
{
    return (tia.write_regs[TIA_WRITE_REG_VBLANK] ? 1 : 0);
}

void TIA_reset_line_buffer(uint8_t line_buffer[])
{
    memset(line_buffer, 0, TIA_COLOUR_CLOCK_VISIBLE);
}

void TIA_reset_buffer(void)
{
    memset(tia_line_buffer, 0, sizeof(tia_line_buffer));
}

void TIA_joy1_state(uint8_t state) {
    tia.read_regs[TIA_READ_REG_INPT4] = state ? 0x00 : 0x80;
}

void TIA_joy2_state(uint8_t state) {
    tia.read_regs[TIA_READ_REG_INPT5] = state ? 0x00 : 0x80;
}