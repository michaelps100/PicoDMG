
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "atari2600emulator.h"

/*
    00      VSYNC   ......1.  vertical sync set-clear
    01      VBLANK  11....1.  vertical blank set-clear
    02      WSYNC   <strobe>  wait for leading edge of horizontal blank
    03      RSYNC   <strobe>  reset horizontal sync counter
    04      NUSIZ0  ..111111  number-size player-missile 0
    05      NUSIZ1  ..111111  number-size player-missile 1
    06      COLUP0  1111111.  color-lum player 0 and missile 0
    07      COLUP1  1111111.  color-lum player 1 and missile 1
    08      COLUPF  1111111.  color-lum playfield and ball
    09      COLUBK  1111111.  color-lum background
    0A      CTRLPF  ..11.111  control playfield ball size & collisions
    0B      REFP0   ....1...  reflect player 0
    0C      REFP1   ....1...  reflect player 1
    0D      PF0     1111....  playfield register byte 0
    0E      PF1     11111111  playfield register byte 1
    0F      PF2     11111111  playfield register byte 2
    10      RESP0   <strobe>  reset player 0
    11      RESP1   <strobe>  reset player 1
    12      RESM0   <strobe>  reset missile 0
    13      RESM1   <strobe>  reset missile 1
    14      RESBL   <strobe>  reset ball
    1B      GRP0    11111111  graphics player 0
    1C      GRP1    11111111  graphics player 1
    1D      ENAM0   ......1.  graphics (enable) missile 0
    1E      ENAM1   ......1.  graphics (enable) missile 1
    1F      ENABL   ......1.  graphics (enable) ball
    20      HMP0    1111....  horizontal motion player 0
    21      HMP1    1111....  horizontal motion player 1
    22      HMM0    1111....  horizontal motion missile 0
    23      HMM1    1111....  horizontal motion missile 1
    24      HMBL    1111....  horizontal motion ball
    25      VDELP0  .......1  vertical delay player 0
    26      VDELP1  .......1  vertical delay player 1
    27      VDELBL  .......1  vertical delay ball
    28      RESMP0  ......1.  reset missile 0 to player 0
    29      RESMP1  ......1.  reset missile 1 to player 1
    2A      HMOVE   <strobe>  apply horizontal motion
    2B      HMCLR   <strobe>  clear horizontal motion registers
    2C      CXCLR   <strobe>  clear collision latches

    30      CXM0P   11......  read collision M0-P1, M0-P0 (Bit 7,6)
    31      CXM1P   11......  read collision M1-P0, M1-P1
    32      CXP0FB  11......  read collision P0-PF, P0-BL
    33      CXP1FB  11......  read collision P1-PF, P1-BL
    34      CXM0FB  11......  read collision M0-PF, M0-BL
    35      CXM1FB  11......  read collision M1-PF, M1-BL
    36      CXBLPF  1.......  read collision BL-PF, unused
    37      CXPPMM  11......  read collision P0-P1, M0-M1
*/


Player0 player0;
Player1 player1;
Missile0 missile0;
Missile1 missile1;
Ball ball;
Playfield playfield;

int video_priority;
bool vsync;
bool vblank;
bool wsync;

uint8_t debug_nusiz0, debug_nusiz1;
static uint16_t s_collisions, s_collision_table[64];
static inline void video_latch_pixel(bool p0,bool p1,bool m0,bool m1,bool bl,bool pf) {
    unsigned mask=p0|(p1<<1)|(m0<<2)|(m1<<3)|(bl<<4)|(pf<<5);
    s_collisions |= s_collision_table[mask];
}
void video_sync_collision_flags(void) {
    player0.collides_with_player1=(s_collisions & (1u<<0))!=0;
    player0.collides_with_missile0=(s_collisions & (1u<<1))!=0;
    player0.collides_with_missile1=(s_collisions & (1u<<2))!=0;
    player0.collides_with_ball=(s_collisions & (1u<<3))!=0;
    player0.collides_with_playfield=(s_collisions & (1u<<4))!=0;
    player1.collides_with_missile0=(s_collisions & (1u<<5))!=0;
    player1.collides_with_missile1=(s_collisions & (1u<<6))!=0;
    player1.collides_with_ball=(s_collisions & (1u<<7))!=0;
    player1.collides_with_playfield=(s_collisions & (1u<<8))!=0;
    missile0.collides_with_missile1=(s_collisions & (1u<<9))!=0;
    missile0.collides_with_ball=(s_collisions & (1u<<10))!=0;
    missile0.collides_with_playfield=(s_collisions & (1u<<11))!=0;
    missile1.collides_with_ball=(s_collisions & (1u<<12))!=0;
    missile1.collides_with_playfield=(s_collisions & (1u<<13))!=0;
    ball.collides_with_playfield=(s_collisions & (1u<<14))!=0;
}
void video_restore_collision_bits(void) {
    s_collisions=0;
    if(player0.collides_with_player1) s_collisions |= 1u<<0;
    if(player0.collides_with_missile0) s_collisions |= 1u<<1;
    if(player0.collides_with_missile1) s_collisions |= 1u<<2;
    if(player0.collides_with_ball) s_collisions |= 1u<<3;
    if(player0.collides_with_playfield) s_collisions |= 1u<<4;
    if(player1.collides_with_missile0) s_collisions |= 1u<<5;
    if(player1.collides_with_missile1) s_collisions |= 1u<<6;
    if(player1.collides_with_ball) s_collisions |= 1u<<7;
    if(player1.collides_with_playfield) s_collisions |= 1u<<8;
    if(missile0.collides_with_missile1) s_collisions |= 1u<<9;
    if(missile0.collides_with_ball) s_collisions |= 1u<<10;
    if(missile0.collides_with_playfield) s_collisions |= 1u<<11;
    if(missile1.collides_with_ball) s_collisions |= 1u<<12;
    if(missile1.collides_with_playfield) s_collisions |= 1u<<13;
    if(ball.collides_with_playfield) s_collisions |= 1u<<14;
}

uint8_t video_color_regs[4];
void video_set_standard(int standard) {
    tv.standard=standard;
    video_COLUP0_write(video_color_regs[0]);
    video_COLUP1_write(video_color_regs[1]);
    video_COLUPF_write(video_color_regs[2]);
    video_COLUBK_write(video_color_regs[3]);
}

static bool s_p0_dirty = true;
static bool s_p1_dirty = true;
static bool s_p0_active = false;
static bool s_p1_active = false;
static bool s_effective_p0[32];
static bool s_effective_p1[32];
/* Per-X occupancy of P0/P1/M0/M1/BL/PF. Rebuild only the object changed by a
 * register write, instead of evaluating six geometries at every colour clock. */
static uint32_t s_coverage_words[40];
static uint8_t s_coverage_dirty=63;
static uint32_t s_pf_dirty=0xfffff;
#define COVERAGE ((uint8_t *)s_coverage_words)
/* Which of the 40 coverage words each movable object (bits 1..16) has marked,
 * as two halves (words 0..31, 32..39), so a rebuild clears only those words
 * instead of all 40. */
static uint32_t s_cov_used_lo[5];
static uint8_t s_cov_used_hi[5];

static inline void video_calculate_effective_player_sprite( bool final_sprite[32], bool init_sprite[8], int size, bool is_mirrored ) {
    if (size == 8) {
        if (!is_mirrored) {
            for (int i = 0; i<8; i++) final_sprite[i] = init_sprite[i];
        } else {
            for (int i = 0; i<8; i++) final_sprite[i] = init_sprite[7-i];
        }
    } else if (size == 16) {
        if (!is_mirrored) {
            for (int i = 0; i<16; i++) final_sprite[i] = init_sprite[i >> 1];
        } else {
            for (int i = 0; i<16; i++) final_sprite[i] = init_sprite[7 - (i >> 1)];
        }
    } else { // size==32
        if (!is_mirrored) {
            for (int i = 0; i<32; i++) final_sprite[i] = init_sprite[i >> 2];
        } else {
            for (int i = 0; i<32; i++) final_sprite[i] = init_sprite[7 - (i >> 2)];
        }
    }
}

static inline void update_p0_sprite(void) {
    if (!s_p0_dirty) return;
    s_p0_dirty = false;
    video_calculate_effective_player_sprite(
        s_effective_p0,
        player0.is_vertically_delayed ? player0.vdel_sprite : player0.main_sprite,
        player0.size,
        player0.is_mirrored);
    s_p0_active = false;
    for (int i = 0; i < player0.size; i++) {
        if (s_effective_p0[i]) { s_p0_active = true; break; }
    }
}

static inline void update_p1_sprite(void) {
    if (!s_p1_dirty) return;
    s_p1_dirty = false;
    video_calculate_effective_player_sprite(
        s_effective_p1,
        player1.is_vertically_delayed ? player1.vdel_sprite : player1.main_sprite,
        player1.size,
        player1.is_mirrored);
    s_p1_active = false;
    for (int i = 0; i < player1.size; i++) {
        if (s_effective_p1[i]) { s_p1_active = true; break; }
    }
}

void video_invalidate_cache(void) { s_p0_dirty = s_p1_dirty = true; s_coverage_dirty=63; s_pf_dirty=0xfffff; }
void __not_in_flash_func(video_begin_scanline)(void) {
    if(player0.is_reset_this_scanline) s_coverage_dirty|=1;
    if(player1.is_reset_this_scanline) s_coverage_dirty|=2;
    if(missile0.is_reset_this_scanline) s_coverage_dirty|=4;
    if(missile1.is_reset_this_scanline) s_coverage_dirty|=8;
    if(ball.is_reset_this_scanline) s_coverage_dirty|=16;
    player0.is_reset_this_scanline=player1.is_reset_this_scanline=false;
    missile0.is_reset_this_scanline=missile1.is_reset_this_scanline=false;
    ball.is_reset_this_scanline=false;
}

void video_init_objects( void ) {
    memset(s_coverage_words,0,sizeof(s_coverage_words)); s_coverage_dirty=63;
    memset(s_cov_used_lo,0,sizeof(s_cov_used_lo)); memset(s_cov_used_hi,0,sizeof(s_cov_used_hi));
    s_pf_dirty=0xfffff;
    s_collisions=0;
    for(unsigned mask=0;mask<64;++mask) {
        uint16_t bits=0;
        if((mask & 3u)==3u) bits |= 1u<<0;
        if((mask & 5u)==5u) bits |= 1u<<1;
        if((mask & 9u)==9u) bits |= 1u<<2;
        if((mask & 17u)==17u) bits |= 1u<<3;
        if((mask & 33u)==33u) bits |= 1u<<4;
        if((mask & 6u)==6u) bits |= 1u<<5;
        if((mask & 10u)==10u) bits |= 1u<<6;
        if((mask & 18u)==18u) bits |= 1u<<7;
        if((mask & 34u)==34u) bits |= 1u<<8;
        if((mask & 12u)==12u) bits |= 1u<<9;
        if((mask & 20u)==20u) bits |= 1u<<10;
        if((mask & 36u)==36u) bits |= 1u<<11;
        if((mask & 24u)==24u) bits |= 1u<<12;
        if((mask & 40u)==40u) bits |= 1u<<13;
        if((mask & 48u)==48u) bits |= 1u<<14;
        s_collision_table[mask]=bits;
    }

    memset(video_color_regs,0,sizeof(video_color_regs));
    memset(&player0,0,sizeof(player0)); memset(&player1,0,sizeof(player1));
    memset(&missile0,0,sizeof(missile0)); memset(&missile1,0,sizeof(missile1));
    memset(&ball,0,sizeof(ball)); memset(&playfield,0,sizeof(playfield));
    memset(s_effective_p0,0,sizeof(s_effective_p0));
    memset(s_effective_p1,0,sizeof(s_effective_p1));
    s_p0_active = s_p1_active = false;
    video_priority = VIDEO_PRIORITY_NORMAL;
    debug_nusiz0 = debug_nusiz1 = 0;
    s_p0_dirty = true;
    s_p1_dirty = true;
    player0.color = 0x0000;
    player0.copies = 1;
    player0.size = 8;
    player0.distance_between_copies = 16;
    player0.x = 2;
    
    player0.color = 0x0000;
    player0.copies = 1;
    player0.size = 8;
    player0.distance_between_copies = 16;
    player0.x = 2;
    
    player1.color = 0x0000;
    player1.copies = 1;
    player1.size = 8;
    player1.distance_between_copies = 16;
    player1.x = 2;

    missile0.color = 0x0000;
    missile0.copies = 1;
    missile0.size = 1;
    missile0.distance_between_copies = 16;
    missile0.x = 1;

    missile1.color = 0x0000;
    missile1.copies = 1;
    missile1.size = 1;
    missile1.distance_between_copies = 16;
    missile1.x = 1;

    ball.color = 0x0000;
    ball.size = 1;
    ball.x = 1;

    playfield.color_background = 0x0000;
    playfield.color_left_half = 0x0000;
    playfield.color_right_half = 0x0000;
    
}

void __not_in_flash_func(video_VSYNC_write) ( uint8_t value ) {
    vsync = value>>1 & 1;
}



void __not_in_flash_func(video_VBLANK_write) ( uint8_t value ) {
    vblank = value>>1 & 1;
    vblank6 = value>>6 & 1;
    vblank7 = value>>7 & 1;
    if (!vblank6) {
        input_reset_latches();
    }
}



void __not_in_flash_func(video_WSYNC_write) ( void ) {
    wsync = true;
}



void __not_in_flash_func(video_RSYNC_write) ( void ) {
    s_coverage_dirty|=31;
    if (tv.x < 158) {
        player0.x = (player0.x + 157 - tv.x) % 160;
        player1.x = (player1.x + 157 - tv.x) % 160;
        missile0.x = (missile0.x + 157 - tv.x) % 160;
        missile1.x = (missile1.x + 157 - tv.x) % 160;
        ball.x = (ball.x + 157 - tv.x) % 160;
    }
    tv.x = 157;
    // if (tv.x < 156) {
    //     player0.x = (player0.x + 155 - tv.x) % 160;
    //     player1.x = (player1.x + 155 - tv.x) % 160;
    //     missile0.x = (missile0.x + 155 - tv.x) % 160;
    //     missile1.x = (missile1.x + 155 - tv.x) % 160;
    //     ball.x = (ball.x + 155 - tv.x) % 160;
    // }
    // tv.x = 155;
}



void __not_in_flash_func(video_NUSIZ0_write) ( uint8_t value ) {
    s_coverage_dirty|=5;
    debug_nusiz0 = value & 7;

    switch (value>>4 & 3) {
        case 0: missile0.size = 1; break;
        case 1: missile0.size = 2; break;
        case 2: missile0.size = 4; break;
        case 3: missile0.size = 8; break;
    }

    switch (value & 7) {
        case 0: {
            missile0.copies = player0.copies = 1;
            missile0.distance_between_copies = player0.distance_between_copies = 0;
            player0.size = 8;
            break;
        }
        case 1: {
            missile0.copies = player0.copies = 2;
            missile0.distance_between_copies = player0.distance_between_copies = 16;
            player0.size = 8;
            break;
        }
        case 2: {
            missile0.copies = player0.copies = 2;
            missile0.distance_between_copies = player0.distance_between_copies = 32;
            player0.size = 8;
            break;
        }
        case 3: {
            missile0.copies = player0.copies = 3;
            missile0.distance_between_copies = player0.distance_between_copies = 16;
            player0.size = 8;
            break;
        }
        case 4: {
            missile0.copies = player0.copies = 2;
            missile0.distance_between_copies = player0.distance_between_copies = 64;
            player0.size = 8;
            break;
        }
        case 5: {
            missile0.copies = player0.copies = 1;
            missile0.distance_between_copies = player0.distance_between_copies = 0;
            player0.size = 16;
            break;
        }
        case 6: {
            missile0.copies = player0.copies = 3;
            missile0.distance_between_copies = player0.distance_between_copies = 32;
            player0.size = 8;
            break;
        }
        case 7: {
            missile0.copies = player0.copies = 1;
            missile0.distance_between_copies = player0.distance_between_copies = 0;
            player0.size = 32;
            break;
        }
        
    }
    s_p0_dirty = true;
}



void __not_in_flash_func(video_NUSIZ1_write) ( uint8_t value ) {
    s_coverage_dirty|=10;
    debug_nusiz1 = value & 7;

    switch (value>>4 & 3) {
        case 0: missile1.size = 1; break;
        case 1: missile1.size = 2; break;
        case 2: missile1.size = 4; break;
        case 3: missile1.size = 8; break;
    }

    switch (value & 7) {
        case 0: {
            missile1.copies = player1.copies = 1;
            missile1.distance_between_copies = player1.distance_between_copies = 0;
            player1.size = 8;
            break;
        }
        case 1: {
            missile1.copies = player1.copies = 2;
            missile1.distance_between_copies = player1.distance_between_copies = 16;
            player1.size = 8;
            break;
        }
        case 2: {
            missile1.copies = player1.copies = 2;
            missile1.distance_between_copies = player1.distance_between_copies = 32;
            player1.size = 8;
            break;
        }
        case 3: {
            missile1.copies = player1.copies = 3;
            missile1.distance_between_copies = player1.distance_between_copies = 16;
            player1.size = 8;
            break;
        }
        case 4: {
            missile1.copies = player1.copies = 2;
            missile1.distance_between_copies = player1.distance_between_copies = 64;
            player1.size = 8;
            break;
        }
        case 5: {
            missile1.copies = player1.copies = 1;
            missile1.distance_between_copies = player1.distance_between_copies = 0;
            player1.size = 16;
            break;
        }
        case 6: {
            missile1.copies = player1.copies = 3;
            missile1.distance_between_copies = player1.distance_between_copies = 32;
            player1.size = 8;
            break;
        }
        case 7: {
            missile1.copies = player1.copies = 1;
            missile1.distance_between_copies = player1.distance_between_copies = 0;
            player1.size = 32;
            break;
        }
        
    }
    s_p1_dirty = true;
}



void __not_in_flash_func(video_COLUP0_write) ( uint8_t value ) {
    video_color_regs[0]=value;
    player0.color = video_decode_color(value, tv.standard);
    missile0.color = video_decode_color(value, tv.standard);
    if (video_priority == VIDEO_PRIORITY_SCORE) {
        playfield.color_left_half = video_decode_color(value, tv.standard);
    }
}



void __not_in_flash_func(video_COLUP1_write) ( uint8_t value ) {
    video_color_regs[1]=value;
    player1.color = video_decode_color(value, tv.standard);
    missile1.color = video_decode_color(value, tv.standard);
    if (video_priority == VIDEO_PRIORITY_SCORE) {
        playfield.color_right_half = video_decode_color(value, tv.standard);
    }
}



void __not_in_flash_func(video_COLUPF_write) ( uint8_t value ) {
    video_color_regs[2]=value;
    ball.color = video_decode_color(value, tv.standard);
    if (video_priority != VIDEO_PRIORITY_SCORE) {
        playfield.color_left_half = video_decode_color(value, tv.standard);
        playfield.color_right_half = video_decode_color(value, tv.standard);
    }
}



void __not_in_flash_func(video_COLUBK_write) ( uint8_t value ) {
    video_color_regs[3]=value;
    playfield.color_background = video_decode_color(value, tv.standard);
}



void __not_in_flash_func(video_CTRLPF_write) ( uint8_t value ) {
    s_coverage_dirty|=48;
    s_pf_dirty=0xfffff;
    playfield.is_reflected = value & 1;
    if ( value>>2 & 1 ) video_priority = VIDEO_PRIORITY_HIGH;
    else if ( value>>1 & 1 ) video_priority = VIDEO_PRIORITY_SCORE;
    else video_priority = VIDEO_PRIORITY_NORMAL;

    if (video_priority == VIDEO_PRIORITY_SCORE) {
        playfield.color_left_half = player0.color;
        playfield.color_right_half = player1.color;
    }
    else {
        playfield.color_left_half = ball.color;
        playfield.color_right_half = ball.color;
    }

    switch (value>>4 & 3) {
        case 0: ball.size = 1; break;
        case 1: ball.size = 2; break;
        case 2: ball.size = 4; break;
        case 3: ball.size = 8; break;
    }
}



void __not_in_flash_func(video_REFP0_write) ( uint8_t value ) {
    s_coverage_dirty|=1;
    player0.is_mirrored = value & (1<<3);
    s_p0_dirty = true;
}



void __not_in_flash_func(video_REFP1_write) ( uint8_t value ) {
    s_coverage_dirty|=2;
    player1.is_mirrored = value & (1<<3);
    s_p1_dirty = true;
}



void __not_in_flash_func(video_PF0_write) ( uint8_t value ) {
    s_coverage_dirty|=32;
    s_pf_dirty|=0x0000f;
    playfield.sprite[0] = value & (1<<4);
    playfield.sprite[1] = value & (1<<5);
    playfield.sprite[2] = value & (1<<6);
    playfield.sprite[3] = value & (1<<7);
}



void __not_in_flash_func(video_PF1_write) ( uint8_t value ) {
    s_coverage_dirty|=32;
    s_pf_dirty|=0x00ff0;
    playfield.sprite[4] = value & (1<<7);
    playfield.sprite[5] = value & (1<<6);
    playfield.sprite[6] = value & (1<<5);
    playfield.sprite[7] = value & (1<<4);
    playfield.sprite[8] = value & (1<<3);
    playfield.sprite[9] = value & (1<<2);
    playfield.sprite[10] = value & (1<<1);
    playfield.sprite[11] = value & (1<<0);
}



void __not_in_flash_func(video_PF2_write) ( uint8_t value ) {
    s_coverage_dirty|=32;
    s_pf_dirty|=0xff000;
    playfield.sprite[12] = value & (1<<0);
    playfield.sprite[13] = value & (1<<1);
    playfield.sprite[14] = value & (1<<2);
    playfield.sprite[15] = value & (1<<3);
    playfield.sprite[16] = value & (1<<4);
    playfield.sprite[17] = value & (1<<5);
    playfield.sprite[18] = value & (1<<6);
    playfield.sprite[19] = value & (1<<7);
}



// https://www.youtube.com/watch?v=sJFnWZH5FXc see 12:47. mentions 4px, 5px, 6px
void __not_in_flash_func(video_RESP0_write) ( void ) {
    s_coverage_dirty|=5;
    /* The extra pixel of delay a double/quad-width player shows is a
     * drawing-time effect that follows the NUSIZ in force when the copy is
     * drawn, so it is applied in video_player0_effective_x_inline() and not
     * baked in here. Baking it at strobe time kept a stale +1 when a kernel
     * reset a wide player and then switched it to normal width (Enduro's
     * dashboard), which put P0 one pixel over the ball: a phantom P0/BL
     * collision every frame, so the game stayed in its skid state and
     * ignored the joystick. */
    player0.x = (tv.x<160) ? (tv.x)%160 : 3;
    player0.is_reset_this_scanline = true;
}



void __not_in_flash_func(video_RESP1_write) ( void ) {
    s_coverage_dirty|=10;
    /* The extra pixel of delay a double/quad-width player shows is a
     * drawing-time effect that follows the NUSIZ in force when the copy is
     * drawn, so it is applied in video_player1_effective_x_inline() and not
     * baked in here. Baking it at strobe time kept a stale +1 when a kernel
     * reset a wide player and then switched it to normal width (Enduro's
     * dashboard), which put P0 one pixel over the ball: a phantom P0/BL
     * collision every frame, so the game stayed in its skid state and
     * ignored the joystick. */
    player1.x = (tv.x<160) ? (tv.x)%160 : 3;
    player1.is_reset_this_scanline = true;
}



void __not_in_flash_func(video_RESM0_write) ( void ) {
    s_coverage_dirty|=4;
    missile0.x = (tv.x<160) ? (tv.x)%160 : 2;
    missile0.is_reset_this_scanline = true;
}



void __not_in_flash_func(video_RESM1_write) ( void ) {
    s_coverage_dirty|=8;
    missile1.x = (tv.x<160) ? (tv.x)%160 : 2;
    missile1.is_reset_this_scanline = true;
}



void __not_in_flash_func(video_RESBL_write) ( void ) {
    s_coverage_dirty|=16;
    ball.x = (tv.x<160) ? (tv.x)%160 : 2;
    ball.is_reset_this_scanline = true;
}



void __not_in_flash_func(video_GRP0_write) ( uint8_t value ) {
    s_coverage_dirty|=3;
    player0.main_sprite[0] = value & (1<<7);
    player0.main_sprite[1] = value & (1<<6);
    player0.main_sprite[2] = value & (1<<5);
    player0.main_sprite[3] = value & (1<<4);
    player0.main_sprite[4] = value & (1<<3);
    player0.main_sprite[5] = value & (1<<2);
    player0.main_sprite[6] = value & (1<<1);
    player0.main_sprite[7] = value & (1<<0);
    
    player1.vdel_sprite[0] = player1.main_sprite[0];
    player1.vdel_sprite[1] = player1.main_sprite[1];
    player1.vdel_sprite[2] = player1.main_sprite[2];
    player1.vdel_sprite[3] = player1.main_sprite[3];
    player1.vdel_sprite[4] = player1.main_sprite[4];
    player1.vdel_sprite[5] = player1.main_sprite[5];
    player1.vdel_sprite[6] = player1.main_sprite[6];
    player1.vdel_sprite[7] = player1.main_sprite[7];
    s_p0_dirty = true;
    s_p1_dirty = true;
}



void __not_in_flash_func(video_GRP1_write) ( uint8_t value ) {
    s_coverage_dirty|=19;
    player1.main_sprite[0] = value & (1<<7);
    player1.main_sprite[1] = value & (1<<6);
    player1.main_sprite[2] = value & (1<<5);
    player1.main_sprite[3] = value & (1<<4);
    player1.main_sprite[4] = value & (1<<3);
    player1.main_sprite[5] = value & (1<<2);
    player1.main_sprite[6] = value & (1<<1);
    player1.main_sprite[7] = value & (1<<0);
    
    player0.vdel_sprite[0] = player0.main_sprite[0];
    player0.vdel_sprite[1] = player0.main_sprite[1];
    player0.vdel_sprite[2] = player0.main_sprite[2];
    player0.vdel_sprite[3] = player0.main_sprite[3];
    player0.vdel_sprite[4] = player0.main_sprite[4];
    player0.vdel_sprite[5] = player0.main_sprite[5];
    player0.vdel_sprite[6] = player0.main_sprite[6];
    player0.vdel_sprite[7] = player0.main_sprite[7];

    ball.vdel_sprite[0] = ball.main_sprite[0];
    s_p0_dirty = true;
    s_p1_dirty = true;
}



void __not_in_flash_func(video_ENAM0_write) ( uint8_t value ) {
    s_coverage_dirty|=4;
    missile0.sprite[0] = value & (1<<1);
}



void __not_in_flash_func(video_ENAM1_write) ( uint8_t value ) {
    s_coverage_dirty|=8;
    missile1.sprite[0] = value & (1<<1);
}



void __not_in_flash_func(video_ENABL_write) ( uint8_t value ) {
    s_coverage_dirty|=16;
    ball.main_sprite[0] = value & (1<<1);
}



void __not_in_flash_func(video_HMP0_write) ( uint8_t value ) {
    player0.hm_value = (value >> 4) & 15;
}



void __not_in_flash_func(video_HMP1_write) ( uint8_t value ) {
    player1.hm_value = (value >> 4) & 15;
}



void __not_in_flash_func(video_HMM0_write) ( uint8_t value ) {
    missile0.hm_value = (value >> 4) & 15;
}



void __not_in_flash_func(video_HMM1_write) ( uint8_t value ) {
    missile1.hm_value = (value >> 4) & 15;
}



void __not_in_flash_func(video_HMBL_write) ( uint8_t value ) {
    ball.hm_value = (value >> 4) & 15;
}



void __not_in_flash_func(video_VDELP0_write) ( uint8_t value ) {
    s_coverage_dirty|=1;
    player0.is_vertically_delayed = value & 1;
    s_p0_dirty = true;
}



void __not_in_flash_func(video_VDELP1_write) ( uint8_t value ) {
    s_coverage_dirty|=2;
    player1.is_vertically_delayed = value & 1;
    s_p1_dirty = true;
}



void __not_in_flash_func(video_VDELBL_write) ( uint8_t value ) {
    s_coverage_dirty|=16;
    ball.is_vertically_delayed = value & 1;
}



void __not_in_flash_func(video_RESMP0_write) ( uint8_t value ) {
    s_coverage_dirty|=4;
    missile0.is_locked_on_player = (value>>1 & 1);
}



void __not_in_flash_func(video_RESMP1_write) ( uint8_t value ) {
    s_coverage_dirty|=8;
    missile1.is_locked_on_player = (value>>1 & 1);
}



void __not_in_flash_func(video_HMOVE_write) ( void ) {
    s_coverage_dirty|=31;
    // chatgpt claims repeated HMOVE calls don't accumulate dx, but it actually does
    // dont trust chatgpt, analyze working emulators instead

    // values here are obtained experimentally using Stella's debugger.
    // basically, HMOVE offsets depend on HMxx AND the tv.x variable!
    // same list can be found at https://www.biglist.com/lists/stella/archives/199804/msg00198.html
    static int offsets[16][76] = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -2, -3, -4, -5, -5, -6, -7, -8, -8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -3, -3, -4, -5, -6, -6, -7, -8, -9, -9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 1, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -2, -3, -4, -4, -5, -6, -7, -7, -8, -9, -10, -10, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -1, 0, 1, 1, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -2, -3, -4, -5, -5, -6, -7, -8, -8, -9, -10, -11, -11, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -2, -2, -1, 0, 1, 1, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -3, -3, -4, -5, -6, -6, -7, -8, -9, -9, -10, -11, -12, -12, -4, -4, -4, -4, -4, -4, -4, -4, -4, -3, -2, -2, -1, 0, 1, 1, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -2, -3, -4, -4, -5, -6, -7, -7, -8, -9, -10, -10, -11, -12, -13, -13, -5, -5, -5, -5, -5, -5, -5, -5, -4, -3, -2, -2, -1, 0, 1, 1, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -2, -3, -4, -5, -5, -6, -7, -8, -8, -9, -10, -11, -11, -12, -13, -14, -14, -6, -6, -6, -6, -6, -6, -5, -5, -4, -3, -2, -2, -1, 0, 1, 1, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -3, -3, -4, -5, -6, -6, -7, -8, -9, -9, -10, -11, -12, -12, -13, -14, -15, -15, -7, -7, -7, -7, -7, -6, -5, -5, -4, -3, -2, -2, -1, 0, 1, 1, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -2, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -3, -3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -2, -3, -4, -4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -2, -3, -4, -5, -5, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -3, -3, -4, -5, -6, -6, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 4, 4, 5, 6},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -2, -3, -4, -4, -5, -6, -7, -7, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 4, 5, 6}
    };

    // this if statement should never be true
    if (tv.x < 0 || tv.x >= 228) {
        printf("unreachable %d\n", tv.x );
        return;
    };

    player0.x =  (player0.x +  offsets[player0.hm_value][tv.x/3]  + 160) % 160;
    player1.x =  (player1.x +  offsets[player1.hm_value][tv.x/3]  + 160) % 160;
    missile0.x = (missile0.x + offsets[missile0.hm_value][tv.x/3] + 160) % 160;
    missile1.x = (missile1.x + offsets[missile1.hm_value][tv.x/3] + 160) % 160;
    ball.x =     (ball.x +     offsets[ball.hm_value][tv.x/3]     + 160) % 160;
}



void __not_in_flash_func(video_HMCLR_write) ( void ) {
    player0.hm_value = 0;
    player1.hm_value = 0;
    missile0.hm_value = 0;
    missile1.hm_value = 0;
    ball.hm_value = 0;
}



void __not_in_flash_func(video_CXCLR_write) ( void ) {
    s_collisions=0;
    player0.collides_with_player1 = false;
    player0.collides_with_missile0 = false;
    player0.collides_with_missile1 = false;
    player0.collides_with_ball = false;
    player0.collides_with_playfield = false;
    player1.collides_with_missile0 = false;
    player1.collides_with_missile1 = false;
    player1.collides_with_ball = false;
    player1.collides_with_playfield = false;
    missile0.collides_with_missile1 = false;
    missile0.collides_with_ball = false;
    missile0.collides_with_playfield = false;
    missile1.collides_with_ball = false;
    missile1.collides_with_playfield = false;
    ball.collides_with_playfield = false;
}



uint8_t __not_in_flash_func(video_CXM0P_read) ( void ) {
    return (((s_collisions>>5)&1)<<7) | (((s_collisions>>1)&1)<<6);
}



uint8_t __not_in_flash_func(video_CXM1P_read) ( void ) {
    return (((s_collisions>>2)&1)<<7) | (((s_collisions>>6)&1)<<6);
}



uint8_t __not_in_flash_func(video_CXP0FB_read) ( void ) {
    return (((s_collisions>>4)&1)<<7) | (((s_collisions>>3)&1)<<6);
}



uint8_t __not_in_flash_func(video_CXP1FB_read) ( void ) {
    return (((s_collisions>>8)&1)<<7) | (((s_collisions>>7)&1)<<6);
}



uint8_t __not_in_flash_func(video_CXM0FB_read) ( void ) {
    return (((s_collisions>>11)&1)<<7) | (((s_collisions>>10)&1)<<6);
}



uint8_t __not_in_flash_func(video_CXM1FB_read) ( void ) {
    return (((s_collisions>>13)&1)<<7) | (((s_collisions>>12)&1)<<6);
}



uint8_t __not_in_flash_func(video_CXBLPF_read) ( void ) {
    return (((s_collisions>>14)&1)<<7);
}



uint8_t __not_in_flash_func(video_CXPPMM_read) ( void ) {
    return (((s_collisions>>0)&1)<<7) | (((s_collisions>>9)&1)<<6);
}



static inline int video_player0_effective_x_inline(void) {
    return player0.size > 8 ? (player0.x + 1) % 160 : player0.x;
}

static inline int video_player1_effective_x_inline(void) {
    return player1.size > 8 ? (player1.x + 1) % 160 : player1.x;
}

static inline int video_missile0_effective_x_inline(void) {
    if (missile0.is_locked_on_player) {
        return (video_player0_effective_x_inline() + (player0.size - missile0.size) / 2) % 160;
    }
    return missile0.x;
}

static inline int video_missile1_effective_x_inline(void) {
    if (missile1.is_locked_on_player) {
        return (video_player1_effective_x_inline() + (player1.size - missile1.size) / 2) % 160;
    }
    return missile1.x;
}

static inline int video_ball_effective_x_inline(void) {
    return ball.x;
}

static inline bool video_playfield_occupies_x_inline(int x) {
    if ((unsigned)x >= 160) return false;
    if (x < 80) return playfield.sprite[x >> 2];
    if (!playfield.is_reflected) return playfield.sprite[(x - 80) >> 2];
    return playfield.sprite[19 - ((x - 80) >> 2)];
}

static inline bool video_ball_occupies_x_inline(int x) {
    if ((unsigned)x >= 160) return false;
    bool* effective_sprite = ball.is_vertically_delayed ? ball.vdel_sprite : ball.main_sprite;
    if (!effective_sprite[0]) return false;
    if (ball.is_reset_this_scanline) return false;
    int start = video_ball_effective_x_inline();
    int dx = x - start;
    if (dx < 0) dx += 160;
    return (dx < ball.size);
}

static inline bool video_missile0_occupies_x_inline(int x) {
    if ((unsigned)x >= 160) return false;
    if (!missile0.sprite[0]) return false;
    int start = video_missile0_effective_x_inline();
    int dx = x - start;
    if (dx < 0) dx += 160;

    if (missile0.copies <= 1) {
        if (missile0.is_reset_this_scanline) return false;
        return (dx < missile0.size);
    }

    int dist = missile0.distance_between_copies;
    if (dist <= 0) return false;
    int off = dx & (dist-1); /* NUSIZ distances are 16, 32 or 64. */
    if (dx < missile0.copies*dist && off < missile0.size) {
        if (missile0.is_reset_this_scanline && dx < dist) return false;
        return true;
    }
    return false;
}

static inline bool video_missile1_occupies_x_inline(int x) {
    if ((unsigned)x >= 160) return false;
    if (!missile1.sprite[0]) return false;
    int start = video_missile1_effective_x_inline();
    int dx = x - start;
    if (dx < 0) dx += 160;

    if (missile1.copies <= 1) {
        if (missile1.is_reset_this_scanline) return false;
        return (dx < missile1.size);
    }

    int dist = missile1.distance_between_copies;
    if (dist <= 0) return false;
    int off = dx & (dist-1);
    if (dx < missile1.copies*dist && off < missile1.size) {
        if (missile1.is_reset_this_scanline && dx < dist) return false;
        return true;
    }
    return false;
}

static inline bool video_player0_occupies_x_inline(int x) {
    if ((unsigned)x >= 160) return false;
    update_p0_sprite();
    if (!s_p0_active) return false;
    int start = video_player0_effective_x_inline();
    int dx = x - start;
    if (dx < 0) dx += 160;

    if (player0.copies <= 1) {
        if (player0.is_reset_this_scanline) return false;
        return (dx < player0.size) ? s_effective_p0[dx] : false;
    }

    int dist = player0.distance_between_copies;
    if (dist <= 0) return false;
    int off = dx & (dist-1);
    if (dx < player0.copies*dist && off < player0.size) {
        if (player0.is_reset_this_scanline && dx < dist) return false;
        return s_effective_p0[off];
    }
    return false;
}

static inline bool video_player1_occupies_x_inline(int x) {
    if ((unsigned)x >= 160) return false;
    update_p1_sprite();
    if (!s_p1_active) return false;
    int start = video_player1_effective_x_inline();
    int dx = x - start;
    if (dx < 0) dx += 160;

    if (player1.copies <= 1) {
        if (player1.is_reset_this_scanline) return false;
        return (dx < player1.size) ? s_effective_p1[dx] : false;
    }

    int dist = player1.distance_between_copies;
    if (dist <= 0) return false;
    int off = dx & (dist-1);
    if (dx < player1.copies*dist && off < player1.size) {
        if (player1.is_reset_this_scanline && dx < dist) return false;
        return s_effective_p1[off];
    }
    return false;
}

static inline void coverage_clear(unsigned bit) {
    uint32_t keep=~(0x01010101u*bit);
    unsigned obj=(unsigned)__builtin_ctz(bit);
    uint32_t lo=s_cov_used_lo[obj];
    while(lo) { s_coverage_words[__builtin_ctz(lo)]&=keep; lo&=lo-1; }
    uint32_t hi=s_cov_used_hi[obj];
    while(hi) { s_coverage_words[32+__builtin_ctz(hi)]&=keep; hi&=hi-1; }
    s_cov_used_lo[obj]=0; s_cov_used_hi[obj]=0;
}
static inline void coverage_sprite(unsigned bit,int start,int size,int copies,int distance,
                                   bool reset,const bool *pixels) {
    if((unsigned)start>=160 || size<1 || size>32 || copies<1 || copies>3 ||
       distance<0 || distance>64 || (copies>1 && !distance)) return;
    for(int copy=reset?1:0;copy<copies;++copy) {
        for(int off=0;off<size;++off) {
            if(pixels && !pixels[off]) continue;
            unsigned offset=copy*distance+off;
            if(offset>=160) continue;
            unsigned pos=start+offset;
            if(pos>=160) pos-=160;
            COVERAGE[pos]|=bit;
            unsigned w=pos>>2, obj=(unsigned)__builtin_ctz(bit);
            if(w<32) s_cov_used_lo[obj]|=1u<<w; else s_cov_used_hi[obj]|=(uint8_t)(1u<<(w-32));
        }
    }
}
static void __not_in_flash_func(video_refresh_coverage)(void) {
    unsigned dirty=s_coverage_dirty;
    s_coverage_dirty=0;
    if(dirty&1) {
        coverage_clear(1); update_p0_sprite();
        if(s_p0_active) coverage_sprite(1,video_player0_effective_x_inline(),player0.size,player0.copies,
            player0.distance_between_copies,player0.is_reset_this_scanline,s_effective_p0);
    }
    if(dirty&2) {
        coverage_clear(2); update_p1_sprite();
        if(s_p1_active) coverage_sprite(2,video_player1_effective_x_inline(),player1.size,player1.copies,
            player1.distance_between_copies,player1.is_reset_this_scanline,s_effective_p1);
    }
    if(dirty&4) {
        coverage_clear(4);
        if(missile0.sprite[0]) coverage_sprite(4,video_missile0_effective_x_inline(),
            missile0.size,missile0.copies,missile0.distance_between_copies,
            missile0.is_reset_this_scanline,NULL);
    }
    if(dirty&8) {
        coverage_clear(8);
        if(missile1.sprite[0]) coverage_sprite(8,video_missile1_effective_x_inline(),
            missile1.size,missile1.copies,missile1.distance_between_copies,
            missile1.is_reset_this_scanline,NULL);
    }
    if(dirty&16) {
        coverage_clear(16);
        if((ball.is_vertically_delayed?ball.vdel_sprite:ball.main_sprite)[0])
            coverage_sprite(16,ball.x,ball.size,1,0,ball.is_reset_this_scanline,NULL);
    }
    if(dirty&32) {
        uint32_t pending=s_pf_dirty;
        s_pf_dirty=0;
        while(pending) {
            unsigned cell=__builtin_ctz(pending);
            pending&=pending-1;
            unsigned right=playfield.is_reflected?39-cell:20+cell;
            uint32_t pixels=playfield.sprite[cell]?0x20202020u:0;
            s_coverage_words[cell]=(s_coverage_words[cell]&~0x20202020u)|pixels;
            s_coverage_words[right]=(s_coverage_words[right]&~0x20202020u)|pixels;
        }
    }
}

int __not_in_flash_func(video_render_pixel_and_collide)( int x ) {
    if ((unsigned)x >= 160 || vblank) return 0x0000;
    if(__builtin_expect(s_coverage_dirty!=0,0)) video_refresh_coverage();
    unsigned mask=COVERAGE[x];
    s_collisions|=s_collision_table[mask];

    if (video_priority == VIDEO_PRIORITY_NORMAL) {
        if (mask&5) return player0.color;
        if (mask&10) return player1.color;
        if (mask&48) return ball.color;
        return playfield.color_background;
    } else if (video_priority == VIDEO_PRIORITY_SCORE) {
        /* Score selects PF colour only; it does not put PF ahead of players. */
        if (mask&5) return player0.color;
        if (mask&10) return player1.color;
        if (mask&16) return ball.color;
        if (mask&32) return x<80?player0.color:player1.color;
        return playfield.color_background;
    } else { // VIDEO_PRIORITY_HIGH
        if (mask&48) return ball.color;
        if (mask&5) return player0.color;
        if (mask&10) return player1.color;
        return playfield.color_background;
    }
}

/* Renders pixels [x0, x1) of the current scanline into buf and latches their
 * collisions. Exactly equivalent to calling video_render_pixel_and_collide()
 * once per pixel, provided no TIA state changes inside the span -- which the
 * caller guarantees by flushing (a26_video_sync) before every TIA write,
 * delayed write, collision read and scanline wrap. */
/* Colour class shown for each coverage mask, per priority mode: 0 background,
 * 1 P0/M0, 2 P1/M1, 3 BL (and PF outside score mode), 4 PF in score mode. The
 * chains mirror video_render_pixel_and_collide() exactly. */
static uint8_t s_mask_class[3][64];
static bool s_mask_class_ready;
static void build_mask_class(void) {
    for (unsigned m = 0; m < 64; ++m) {
        s_mask_class[0][m] = (m & 5) ? 1 : (m & 10) ? 2 : (m & 48) ? 3 : 0;
        s_mask_class[1][m] = (m & 5) ? 1 : (m & 10) ? 2 : (m & 16) ? 3 : (m & 32) ? 4 : 0;
        s_mask_class[2][m] = (m & 48) ? 3 : (m & 5) ? 1 : (m & 10) ? 2 : 0;
    }
    s_mask_class_ready = true;
}

void A26_RAMFUNC(video_render_span)(unsigned x0, unsigned x1, uint16_t *buf) {
    if (x0 >= x1) return;
    if (s_coverage_dirty != 0) video_refresh_coverage();
    if (!s_mask_class_ready) build_mask_class();
    const uint8_t *cov = COVERAGE;
    uint16_t col = s_collisions;
    uint16_t colors[5];
    colors[0] = (uint16_t)playfield.color_background;
    colors[1] = (uint16_t)player0.color;
    colors[2] = (uint16_t)player1.color;
    colors[3] = (uint16_t)ball.color;
    colors[4] = colors[1];
    const uint8_t *cls = video_priority == VIDEO_PRIORITY_NORMAL ? s_mask_class[0]
                       : video_priority == VIDEO_PRIORITY_SCORE ? s_mask_class[1]
                       : s_mask_class[2];
    /* Score mode paints PF in P0's colour on the left half, P1's on the right. */
    unsigned split = x1 < 80u ? x1 : 80u;
    unsigned x = x0;
    for (; x < split; ++x) {
        unsigned m = cov[x];
        col |= s_collision_table[m];
        buf[x] = colors[cls[m]];
    }
    colors[4] = colors[2];
    for (; x < x1; ++x) {
        unsigned m = cov[x];
        col |= s_collision_table[m];
        buf[x] = colors[cls[m]];
    }
    s_collisions = col;
}

void video_calculate_collisions( int x ) {
    if (vblank || (unsigned)x >= 160) return;
    bool pf = video_playfield_occupies_x_inline(x);
    bool bl = video_ball_occupies_x_inline(x);
    bool m0 = video_missile0_occupies_x_inline(x);
    bool m1 = video_missile1_occupies_x_inline(x);
    bool p0 = video_player0_occupies_x_inline(x);
    bool p1 = video_player1_occupies_x_inline(x);
    video_latch_pixel(p0,p1,m0,m1,bl,pf);
}

int video_calculate_pixel( int x ) {
    return video_render_pixel_and_collide(x);
}

#include "regional_palettes.h"
int __not_in_flash_func(video_decode_color)( uint8_t color, int tv_standard ) {
    static const uint16_t ntsc_colors_rgb565[128] = {
        0x0000, 0x4208, 0x6B6D, 0x9492, 0xB596, 0xCE59, 0xDEFB, 0xEF7D,
        0x4220, 0x6322, 0x8424, 0xA506, 0xBDC8, 0xD68A, 0xEF4B, 0xFFED,
        0x7140, 0x8222, 0x9AE5, 0xABC7, 0xBC69, 0xCD0B, 0xDDAD, 0xEE4F,
        0x80C0, 0x99A3, 0xAA86, 0xC349, 0xD40B, 0xE4AE, 0xED50, 0xFDF2,
        0x8800, 0x9904, 0xB1E7, 0xC2CB, 0xD38E, 0xE451, 0xED14, 0xFDB6,
        0x780B, 0x890E, 0xA1F1, 0xB2D3, 0xC396, 0xD438, 0xDCFA, 0xED9C,
        0x480F, 0x6112, 0x79F4, 0x8AD7, 0xA399, 0xB43B, 0xC4FD, 0xD59F,
        0x1010, 0x3113, 0x49F5, 0x6AD8, 0x7B9A, 0x945C, 0xAD1D, 0xBDBF,
        0x0011, 0x1913, 0x3A16, 0x52F8, 0x6BBA, 0x7C7C, 0x953D, 0xA5DF,
        0x00CF, 0x19D2, 0x3AB5, 0x5397, 0x6C59, 0x7CFB, 0x95BD, 0xA65F,
        0x016B, 0x1A6F, 0x3B52, 0x5435, 0x6CF8, 0x7DBA, 0x967D, 0xA71F,
        0x01E5, 0x1AE9, 0x3BEC, 0x54F0, 0x6DB2, 0x7E95, 0x9738, 0xA7FA,
        0x01E0, 0x22E4, 0x43E8, 0x5CEB, 0x75AE, 0x8E91, 0xA734, 0xBFF7,
        0x11C0, 0x32E3, 0x53E7, 0x6CCA, 0x85AD, 0x9E6F, 0xB732, 0xCFF4,
        0x2980, 0x4A83, 0x6B86, 0x8469, 0x9D4C, 0xB60F, 0xCEB1, 0xE773,
        0x4140, 0x6243, 0x8346, 0xA428, 0xBCEB, 0xD5AD, 0xEE6F, 0xFF11
    };

    unsigned index=(color>>1)&127;
    if(tv_standard==TV_PAL) return pal_colors_rgb565[index];
    if(tv_standard==TV_SECAM) return secam_colors_rgb565[index];
    return ntsc_colors_rgb565[index];
}

/* Compatibility wrappers for external header callers if needed */
bool video_playfield_occupies_x( int x ) { return video_playfield_occupies_x_inline(x); }
bool video_ball_occupies_x( int x ) { return video_ball_occupies_x_inline(x); }
bool video_missile0_occupies_x( int x ) { return video_missile0_occupies_x_inline(x); }
bool video_missile1_occupies_x( int x ) { return video_missile1_occupies_x_inline(x); }
bool video_player0_occupies_x( int x ) { return video_player0_occupies_x_inline(x); }
bool video_player1_occupies_x( int x ) { return video_player1_occupies_x_inline(x); }
int video_player0_effective_x ( void ) { return video_player0_effective_x_inline(); }
int video_player1_effective_x ( void ) { return video_player1_effective_x_inline(); }
int video_missile0_effective_x ( void ) { return video_missile0_effective_x_inline(); }
int video_missile1_effective_x ( void ) { return video_missile1_effective_x_inline(); }
int video_ball_effective_x ( void ) { return video_ball_effective_x_inline(); }
