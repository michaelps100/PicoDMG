#pragma GCC optimize("Ofast")
////////////////////////////////////////////////////////////////////////////////
// I/O ports
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//////////////////////////////////////////////////////////////////////////////


#include <string.h>
#include "pico.h"
#include <io.h>
#include "./nec/nec.h"
#include "initialio.h"
#include "ieeprom.h"
#include "gpu.h"
#include "memory.h"
#include <stdbool.h>
#include "ws_audio.h"

extern uint32 externalEepromAddressMask;
extern uint32 romAddressMask;
extern bool ili9341_get_ws_vertical(void);

__aligned(4) uint8 ws_ioRam[0x100];

uint8 ws_key_start;
uint8 ws_key_left;
uint8 ws_key_right;
uint8 ws_key_up;
uint8 ws_key_down;
uint8 ws_key_button_1;
uint8 ws_key_button_2;
uint8 ws_key_flipped;

int rtcDataRegisterReadCount = 0;

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_io_reset(void) {
    ws_key_start = 0;
    ws_key_left = 0;
    ws_key_right = 0;
    ws_key_up = 0;
    ws_key_down = 0;
    ws_key_button_1 = 0;
    ws_key_button_2 = 0;

    memcpy(ws_ioRam, initialIoValue, 0x100);

    for (int i = 0; i < 0xc9; i++)
        cpu_writeport(i, initialIoValue[i]);

    rtcDataRegisterReadCount = 0;
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_io_init(void) {
    ws_io_reset();
    ws_key_flipped = 0;
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_io_flipControls(void) {
    ws_key_flipped = (!ws_key_flipped);
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
void ws_io_done(void) {
#if 0
    if (ws_ioRam==NULL)
        free(ws_ioRam);
#else
    memset(ws_ioRam, 0x00, sizeof(ws_ioRam));
#endif
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
uint8_t __not_in_flash_func(cpu_readport)(uint8_t port) {
    int w1, w2;

    switch (port) {
        case 0x4e:
        case 0x4f:
        case 0x50:
        case 0x51:
        case 0x6a:
        case 0x6b:
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8a:
        case 0x8b:
        case 0x8c:
        case 0x8d:
        case 0x8e:
        case 0x8f:
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
            return ws_audio_port_read(port);
        case 0xb3:    // ???
            if (ws_ioRam[0xb3] < 0x80)
                return 0;

            if (ws_ioRam[0xb3] < 0xc0)
                return 0x84;

            return 0xc4;
        case 0xb5:
            w1 = ws_ioRam[0xb5];
            w2 = 0;
            if (w1 & 0x40) {
                /* Row 0x40: START, Button A, Button B */
                if (ws_key_start)    w2 |= (1 << 1);
                if (ws_key_button_1) w2 |= (1 << 2);
                if (ws_key_button_2) w2 |= (1 << 3);
            }
            if (w1 & 0x20) {
                /* Row 0x20: X-pad */
                if (ili9341_get_ws_vertical()) {
                    /* In Tate mode: physical A/B map to X-pad for actions / rotation.
                     * X1 (bit 0) = Jump (Button B).
                     * X2 (bit 1) = Action / Wind Ring (Button A).
                     * Never send opposing bits simultaneously (X1|X3 or X2|X4) as games cancel them.
                     */
                    if (ws_key_button_1) w2 |= (1 << 1); /* X2 = Button A */
                    if (ws_key_button_2) w2 |= (1 << 0); /* X1 = Button B */
                } else {
                    if (ws_key_up)    w2 |= (1 << 0);
                    if (ws_key_right) w2 |= (1 << 1);
                    if (ws_key_down)  w2 |= (1 << 2);
                    if (ws_key_left)  w2 |= (1 << 3);
                }
            }
            if (w1 & 0x10) {
                /* Row 0x10: Y-pad */
                if (ili9341_get_ws_vertical()) {
                    /* In Tate mode: physical D-pad maps to Y-pad for directional movement */
                    if (ws_key_left)  w2 |= (1 << 0); /* Y1 = Left */
                    if (ws_key_up)    w2 |= (1 << 1); /* Y2 = Up */
                    if (ws_key_right) w2 |= (1 << 2); /* Y3 = Right */
                    if (ws_key_down)  w2 |= (1 << 3); /* Y4 = Down */
                } else {
                    if (ws_key_up)    w2 |= (1 << 0);
                    if (ws_key_right) w2 |= (1 << 1);
                    if (ws_key_down)  w2 |= (1 << 2);
                    if (ws_key_left)  w2 |= (1 << 3);
                }
            }
            return (uint8)((w1 & 0xf0) | (w2 & 0x0f));
        case 0xb6:
            return ws_ioRam[0xb6];
        case 0xbe:    // internal eeprom status/command register
            // ack eeprom write
            if (ws_ioRam[0xbe] & 0x20)
                return ws_ioRam[0xbe] | 2;

            // ack eeprom read
            if (ws_ioRam[0xbe] & 0x10)
                return ws_ioRam[0xbe] | 1;

            // else ack both
            return ws_ioRam[0xbe] | 3;
        case 0xba:    // eeprom even uint8_t read
            w1 = ((((uint16) ws_ioRam[0xbd]) << 8) | ((uint16) ws_ioRam[0xbc]));
            w1 = (w1 << 1) & 0x3ff;
            return internalEeprom[w1];
        case 0xbb:    // eeprom odd uint8_t read
            w1 = ((((uint16) ws_ioRam[0xbd]) << 8) | ((uint16) ws_ioRam[0xbc]));
            w1 = ((w1 << 1) + 1) & 0x3ff;
            return internalEeprom[w1];
        case 0xc0 : // ???
            return ((ws_ioRam[0xc0] & 0xf) | 0x20);

        case 0xc4:    // external eeprom even uint8_t read
            w1 = (((((uint16_t) ws_ioRam[0xc7]) << 8) | ((uint16_t) ws_ioRam[0xc6])) << 1) &
                 (externalEepromAddressMask);
            return externalEeprom[w1 & 0x1fff];

        case 0xc5:    // external eeprom odd uint8_t read
            w1 = (((((uint16_t) ws_ioRam[0xc7]) << 8) | ((uint16_t) ws_ioRam[0xc6])) << 1) &
                 (externalEepromAddressMask);
            return externalEeprom[(w1 + 1) & 0x1fff];

        case 0xc8:    // external eeprom status/command register
            // ack eeprom write
            if (ws_ioRam[0xc8] & 0x20)
                return ws_ioRam[0xc8] | 2;

            // ack eeprom read
            if (ws_ioRam[0xc8] & 0x10)
                return ws_ioRam[0xc8] | 1;

            // else ack both
            return ws_ioRam[0xc8] | 3;

        case 0xca : // RTC Command and status register
            // set ack to always 1
            return (ws_ioRam[0xca] | 0x80);
        case 0xcb : // RTC data register
#if 0
            if(ws_ioRam[0xca]==0x15)	// get time command
            {
                struct tm *newtime;
                time_t long_time;
                time( &long_time );
                newtime = localtime( &long_time );

#define  BCD(value) ((value/10)<<4)|(value%10)
                switch(rtcDataRegisterReadCount)
                {
                  case 0:	rtcDataRegisterReadCount++;
                            return BCD(newtime->tm_year-100);
                  case 1:	rtcDataRegisterReadCount++;
                            return BCD(newtime->tm_mon);
                  case 2:	rtcDataRegisterReadCount++;
                            return BCD(newtime->tm_mday);
                  case 3:	rtcDataRegisterReadCount++;
                            return BCD(newtime->tm_wday);
                  case 4:	rtcDataRegisterReadCount++;
                            return BCD(newtime->tm_hour);
                  case 5:	rtcDataRegisterReadCount++;
                            return BCD(newtime->tm_min);
                  case 6:	rtcDataRegisterReadCount=0;
                            return BCD(newtime->tm_sec);
                }
                return 0;
            }
            else
            {
                // set ack
                return (ws_ioRam[0xcb]|0x80);
            }
#else
            break;
#endif
    }
    return (ws_gpu_port_read(port));
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//
////////////////////////////////////////////////////////////////////////////////
extern volatile uint32_t ws_sram_epoch;
void __not_in_flash_func(cpu_writeport)(uint32_t port, uint8_t value) {
    int w1;
    /* OUT DX,AL / OUT DX,AX hand over a 16-bit port number. Final Fantasy loads only DL
     * (mov dl,8Eh) and leaves whatever was in DH, so it really does issue OUT to values
     * like 0x948E and relies on the hardware decoding just the low 8 bits. ws_ioRam[] has
     * 256 entries: indexing it with the full number wrote ~38 KB past its end (silent
     * while RAM sat behind it, a bus fault once the array moved to the top of SRAM). */
    port &= 0xFFu;
    ws_mc++; /* any port write counts as a state change for idle detection */

    if (port == 0xb6) {
        ws_ioRam[0xb6] &= ~value;
        ws_check_interrupts();
        return;
    }

    /* EEPROM data/command ports (0xBA..0xBE, 0xC4..0xC8) are never skipped: writing the same
     * word to two addresses, or issuing the same command twice, is a real second access. */
    if (ws_ioRam[port] == value) {
        if (port != 0x52 && port != 0x48 && (port < 0x80 || port > 0x95) &&
            (port < 0xba || port > 0xbe) && (port < 0xc4 || port > 0xc8))
            return;
    }

    ws_ioRam[port] = value;

    /* ROM bank registers move the pages the CPU fast path points at */
    if (port >= 0xc0 && port <= 0xc3) ws_memory_bank_changed();

    switch (port) {
        case 0xb2:
            ws_check_interrupts();
            break;
        case 0x4e:
        case 0x4f:
        case 0x50:
        case 0x51:
            break;
        case 0x52:
            ws_audio_port_write(port, value);
            break;
        case 0x6a:
        case 0x6b:
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8a:
        case 0x8b:
        case 0x8c:
        case 0x8d:
        case 0x8e:
        case 0x8f:
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
            ws_audio_port_write(port, value);
            break;
        case 0x48:    // DMA

            // bit 7 set to start dma transfer
            if (value & 0x80) {
                int dma_start = (((uint32_t) ws_ioRam[0x41]) << 8) | (((uint32_t) ws_ioRam[0x40])) |
                                (((uint32_t) ws_ioRam[0x42]) << 16);
                int dma_end = (((uint32_t) ws_ioRam[0x45]) << 8) | (((uint32_t) ws_ioRam[0x44])) |
                              (((uint32_t) ws_ioRam[0x43]) << 16);
                int dma_size = (((uint32_t) ws_ioRam[0x47]) << 8) | (((uint32_t) ws_ioRam[0x46]));

                for (int ix = 0; ix < dma_size; ix++)
                    cpu_writemem20(dma_end++, cpu_readmem20(dma_start++));

                ws_ioRam[0x47] = 0;
                ws_ioRam[0x46] = 0;
                ws_ioRam[0x41] = (uint8_t) (dma_start >> 8);
                ws_ioRam[0x40] = (uint8_t) (dma_start & 0xff);
                ws_ioRam[0x45] = (uint8_t) (dma_end >> 8);
                ws_ioRam[0x44] = (uint8_t) (dma_end & 0xff);
                ws_ioRam[0x48] = 0;
            }
            break;
        case 0xba:
            w1 = (((((uint16_t) ws_ioRam[0xbd]) << 8) | ((uint16_t) ws_ioRam[0xbc])));
            w1 = (w1 << 1) & 0x3ff;
            internalEeprom[w1] = value;
            return;

        case 0xbb:
            w1 = (((((uint16_t) ws_ioRam[0xbd]) << 8) | ((uint16_t) ws_ioRam[0xbc])));
            w1 = ((w1 << 1) + 1) & 0x3ff;
            internalEeprom[w1] = value;
            return;

        case 0xc4:    /* external EEPROM data latch; stored by the write command */
        case 0xc5:
            return;

        case 0xc8:    /* external EEPROM command: bit 5 = write the latched word */
            if ((value & 0x20) && externalEepromAddressMask) {
                w1 = (((((uint16_t) ws_ioRam[0xc7]) << 8) | ((uint16_t) ws_ioRam[0xc6])) << 1) & externalEepromAddressMask;
                uint8 *dst = &externalEeprom[w1 & 0x1ffe];
                if (dst[0] != ws_ioRam[0xc4] || dst[1] != ws_ioRam[0xc5]) {
                    dst[0] = ws_ioRam[0xc4];
                    dst[1] = ws_ioRam[0xc5];
                    ws_sram_epoch++;
                }
            }
            return;

        case 0xca:
            if (value == 0x15)
                rtcDataRegisterReadCount = 0;
            break;
    }
    ws_gpu_port_write(port, value);
//	if ((ws_gpu_unknownPort)&&(unknown_io_port))
//	{
//		fprintf(log_get(),"io: writing 0x%.2x to unknown port 0x%.2x\n",value,port); 
//	}
}
