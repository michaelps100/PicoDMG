//////////////////////////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////////////////////////
//
//
//
//
//
//
//////////////////////////////////////////////////////////////////////////////

#ifndef __ROM_H__
#define __ROM_H__

#include "types.h"

#define WS_SYSTEM_MONO			0
#define WS_SYSTEM_COLOR			1

#define WS_ROM_SIZE_2MBIT		1
#define WS_ROM_SIZE_4MBIT		2
#define WS_ROM_SIZE_8MBIT		3
#define WS_ROM_SIZE_16MBIT		4
#define WS_ROM_SIZE_24MBIT		5
#define WS_ROM_SIZE_32MBIT		6
#define WS_ROM_SIZE_48MBIT		7
#define WS_ROM_SIZE_64MBIT		8
#define WS_ROM_SIZE_128MBIT		9

#define WS_EEPROM_SIZE_NONE		0x00
#define WS_SRAM_SIZE_NONE		0x00

/* SRAM sizes in save byte (low nibble 0x01..0x05) */
#define WS_SRAM_SIZE_64KBIT		0x01	/* 8 KB (0x2000) */
#define WS_SRAM_SIZE_256KBIT	0x02	/* 32 KB (0x8000) */
#define WS_SRAM_SIZE_1MBIT		0x03	/* 128 KB (0x20000) */
#define WS_SRAM_SIZE_2MBIT		0x04	/* 256 KB (0x40000) */
#define WS_SRAM_SIZE_4MBIT		0x05	/* 512 KB (0x80000) */

/* EEPROM sizes in save byte (high nibble 0x10, 0x20, 0x50) */
#define WS_EEPROM_SIZE_1KBIT	0x10	/* 1 Kbit = 128 bytes (64x16) */
#define WS_EEPROM_SIZE_16KBIT	0x20	/* 16 Kbit = 2048 bytes (1024x16) */
#define WS_EEPROM_SIZE_8KBIT	0x50	/* 8 Kbit = 1024 bytes (512x16) */


typedef struct __attribute__((packed)) ws_romHeaderStruct
{
	uint8	developerId;            /* 0xFFF6 (offset -10) */
	uint8	minimumSupportSystem;   /* 0xFFF7 (offset -9): 0=Mono, 1=Color */
	uint8	cartId;                 /* 0xFFF8 (offset -8) */
	uint8	gameVersion;            /* 0xFFF9 (offset -7) */
	uint8	romSize;                /* 0xFFFA (offset -6) */
	uint8	saveType;               /* 0xFFFB (offset -5): 0=None, 1..5=SRAM, 0x10,0x20,0x50=EEPROM */
	uint8	flags;                  /* 0xFFFC (offset -4): bit 0: 0=Horizontal, 1=Vertical */
	uint8	realtimeClock;          /* 0xFFFD (offset -3): 0=None, 1=RTC */
	uint16	checksum;               /* 0xFFFE,0xFFFF (offset -2,-1) */
} ws_romHeaderStruct;


uint8	*ws_rom_load(uint8 *rom, uint32 *romSize);
void				ws_rom_dumpInfo(uint8 *wsrom, uint32 wsromSize);
ws_romHeaderStruct	*ws_rom_getHeader(uint8 *wsrom, uint32 wsromSize);
uint32				ws_rom_sramSize(uint8 *wsrom, uint32 wsromSize);
uint32				ws_rom_eepromSize(uint8 *wsrom, uint32 wsromSize);


#endif
