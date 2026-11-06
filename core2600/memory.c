
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "platform.h"

#include "atari2600emulator.h"
#include "core_state.h"


Memory memory;
Cartridge cartridge;

void memory_init_random( void ) {
    for (int i = 0; i<128; i++) {
        memory[i] = rand();
    }
}

uint8_t __not_in_flash_func(memory_read_decoded)( uint16_t address ) {
    bool A12 = address>>12 & 1;
    bool A9 = address>>9 & 1;
    bool A7 = address>>7 & 1;

    if ( !A12 && !A7 ) {
        // TIA read. Collision latches need the pixels rendered so far.
        if ((address & 0x8) == 0) a26_video_sync();
        switch ( address & 0xF) {
            case 0: return video_CXM0P_read();
            case 1: return video_CXM1P_read();
            case 2: return video_CXP0FB_read();
            case 3: return video_CXP1FB_read();
            case 4: return video_CXM0FB_read();
            case 5: return video_CXM1FB_read();
            case 6: return video_CXBLPF_read();
            case 7: return video_CXPPMM_read();
            case 8: return input_INPT0_read();
            case 9: return input_INPT1_read();
            case 10: return input_INPT2_read();
            case 11: return input_INPT3_read();
            case 12: return input_INPT4_read();
            case 13: return input_INPT5_read();
        }
    }
    else if ( !A12 && !A9 && A7 ) {
        // RAM read
        return memory[address & 0x7F];
    }
    else if ( !A12 && A9 && A7 ) {
        // A3/A4 are mirrors on reads; A2 selects timer versus ports.
        a26_catch_up();
        switch (address & 7) {
            case 0: return input_SWCHA_read();
            case 1: return input_SWACNT_read();
            case 2: return input_SWCHB_read();
            case 3: return input_SWBCNT_read();
            case 4: case 6: return timer_INTIM_read();
            default: return timer_INSTAT_read();
        }
    }

    return cartridge_read(address);
}


void __not_in_flash_func(memory_write_decoded)( uint16_t address, uint8_t value ) {
    bool A12 = address>>12 & 1;
    bool A9 = address>>9 & 1;
    bool A7 = address>>7 & 1;

    if ( !A12 && !A7 ) {
        // TIA write. Render what the beam passed under the old state first.
        a26_video_sync();
        // AUDx: replay the deferred audio/timer edges before the sound changes.
        if ((unsigned)(address & 0x3F) - 0x15u <= 5u) a26_catch_up();
        switch ( address & 0x3F) {
            case 0x00: return video_VSYNC_write(value); 
            case 0x01: return write_after_delay( video_VBLANK_write, true, value, 1);
            case 0x02: return video_WSYNC_write(); 
            case 0x03: video_RSYNC_write(); a26_video_rebase(); return;
            case 0x04: return video_NUSIZ0_write(value); 
            case 0x05: return video_NUSIZ1_write(value); 
            case 0x06: return video_COLUP0_write(value); 
            case 0x07: return video_COLUP1_write(value); 
            case 0x08: return video_COLUPF_write(value); 
            case 0x09: return video_COLUBK_write(value); 
            case 0x0A: return video_CTRLPF_write(value); 
            case 0x0B: return write_after_delay( video_REFP0_write, true, value, 1);
            case 0x0C: return write_after_delay( video_REFP1_write, true, value, 1);
            case 0x0D: return write_after_delay( video_PF0_write, true, value, 2);
            case 0x0E: return write_after_delay( video_PF1_write, true, value, 2);
            case 0x0F: return write_after_delay( video_PF2_write, true, value, 2);
            case 0x10: return write_after_delay( video_RESP0_write, false, 0, 5); 
            case 0x11: return write_after_delay( video_RESP1_write, false, 0, 5);
            case 0x12: return write_after_delay( video_RESM0_write, false, 0, 4);
            case 0x13: return write_after_delay( video_RESM1_write, false, 0, 4);
            case 0x14: return write_after_delay( video_RESBL_write, false, 0, 4);
            case 0x15: return audio_AUDC0_write(value);
            case 0x16: return audio_AUDC1_write(value);
            case 0x17: return audio_AUDF0_write(value);
            case 0x18: return audio_AUDF1_write(value);
            case 0x19: return audio_AUDV0_write(value);
            case 0x1A: return audio_AUDV1_write(value);
            case 0x1B: return write_after_delay( video_GRP0_write, true, value, 1);
            case 0x1C: return write_after_delay( video_GRP1_write, true, value, 1);
            case 0x1D: return write_after_delay( video_ENAM0_write, true, value, 1);
            case 0x1E: return write_after_delay( video_ENAM1_write, true, value, 1);
            case 0x1F: return write_after_delay( video_ENABL_write, true, value, 1);
            case 0x20: return write_after_delay( video_HMP0_write, true, value, 2);
            case 0x21: return write_after_delay( video_HMP1_write, true, value, 2);
            case 0x22: return write_after_delay( video_HMM0_write, true, value, 2);
            case 0x23: return write_after_delay( video_HMM1_write, true, value, 2);
            case 0x24: return write_after_delay( video_HMBL_write, true, value, 2);
            case 0x25: return video_VDELP0_write(value); 
            case 0x26: return video_VDELP1_write(value); 
            case 0x27: return video_VDELBL_write(value); 
            case 0x28: return video_RESMP0_write(value); 
            case 0x29: return video_RESMP1_write(value); 
            case 0x2A:
                return write_after_delay( video_HMOVE_write, false, 0, 6);
            case 0x2B: return write_after_delay( video_HMCLR_write, false, 0, 2);
            case 0x2C: return video_CXCLR_write(); 
        }
    }
    else if ( !A12 && !A9 && A7 ) {
        // RAM write
        memory[address & 0x7F] = value;
    }
    else if ( !A12 && A9 && A7 ) {
        a26_catch_up();
        if (address & 4) {
            if (!(address & 0x10)) { timer_edge_control(address & 3); return; }
            switch (address & 3) {
                case 0: timer_TIM1T_write(value); break;
                case 1: timer_TIM8T_write(value); break;
                case 2: timer_TIM64T_write(value); break;
                case 3: timer_T1024T_write(value); break;
            }
        } else {
            switch (address & 3) {
                case 0: input_SWCHA_write(value); break;
                case 1: input_SWACNT_write(value); break;
                case 2: input_SWCHB_write(value); break;
                case 3: input_SWBCNT_write(value); break;
            }
        }
        return;
    }

    cartridge_write(address, value);
}


    


static void observe_bus(uint16_t address, uint8_t value) {
    if (cartridge.type == CARTRIDGE_FE) {
        if (cartridge.fe_pending) {
            cartridge.FE.current_bank = (value & 0x20) ? 0 : 1;
            cartridge.fe_pending = false;
        } else if ((address & 0x1fff) == 0x01fe) {
            cartridge.fe_pending = true;
        }
    }
    cartridge_bus = value;
}
uint8_t __not_in_flash_func(memory_read)(uint16_t address) {
    uint8_t value = memory_read_decoded(address);
    observe_bus(address, value);
    return value;
}
void __not_in_flash_func(memory_write)(uint16_t address, uint8_t value) {
    observe_bus(address, value);
    memory_write_decoded(address, value);
}
