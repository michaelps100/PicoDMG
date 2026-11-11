//---------------------------------------------------------------------------
// NEOPOP : Emulator as in Dreamland
//
// Copyright (c) 2001-2002 by neopop_uk
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation; either version 2 of the License, or
//	(at your option) any later version. See also the license.txt file for
//	additional informations.
//---------------------------------------------------------------------------

/*
//---------------------------------------------------------------------------
//=========================================================================

	mem.h

//=========================================================================
//---------------------------------------------------------------------------

  History of changes:
  ===================

20 JUL 2002 - neopop_uk
=======================================
- Cleaned and tidied up for the source release

15 AUG 2002 - neopop_uk
=======================================
- Removed the legacy 'eeprom' variables.

18 AUG 2002 - neopop_uk
=======================================
- Moved RAM_START/RAM_END definition and ram[] declaration to NeoPop.h

//---------------------------------------------------------------------------
*/

#ifndef __MEM__
#define __MEM__
//=============================================================================

#define ROM_START	0x200000
#define ROM_END		0x3FFFFF

#define HIROM_START	0x800000
#define HIROM_END	0x9FFFFF

#define BIOS_START	0xFF0000
#define BIOS_END	0xFFFFFF

void reset_memory(void);

//static inline void* translate_address_read(_u32 address);
//static inline void* translate_address_write(_u32 address);

void dump_memory(_u32 start, _u32 length);

extern BOOL debug_abort_memory;
extern BOOL debug_mask_memory_error_messages;

extern BOOL memory_unlock_flash_write;
extern BOOL memory_flash_error;
extern BOOL memory_flash_command;

extern BOOL eepromStatusEnable;

/* Slow paths: hardware registers, BIOS, hi-ROM and the flash save overlay.
 * Reached only when the inline fast paths in mem.h decline an address. */
_u8  mem_slow_loadB(_u32 address);
_u16 mem_slow_loadW(_u32 address);
_u32 mem_slow_loadL(_u32 address);

void mem_slow_storeB(_u32 address, _u8 data);
void mem_slow_storeW(_u32 address, _u16 data);
void mem_slow_storeL(_u32 address, _u32 data);

//=============================================================================

/* ---------------------------------------------------------------------------
   Fast paths for the memory accessors.

   These six functions used to be out-of-line and, because mem.c is not part of
   the SRAM linker override, they were linked into the flash XIP window while
   the interpreter that calls them sits in SRAM. Every single load and store
   therefore paid a long-branch veneer plus a flash fetch, on top of the call
   itself -- in a CPU interpreter that is millions of times per frame. The
   PicoGBC hit exactly this bug and fixed it the same way.

   Virtually every access the emulated NGP makes is either work RAM or the
   cartridge ROM, and the ROM is where the instruction fetch lives, so both are
   resolved inline here. Anything else (the hardware register file below 0x100,
   BIOS, hi-ROM, the flash save overlay) falls through to the untouched slow
   path.
   --------------------------------------------------------------------------- */

#define MEM_RAM_LO 0x100u	/* below this is the hardware register file */
#define MEM_RAS_H  0x8008u	/* ordinary RAM, but reading it has a side effect */

static inline _u8* mem_fast_load_ptr(_u32 address)
{
	/* Single unsigned compare covers the whole work-RAM range. */
	if (__builtin_expect((address - MEM_RAM_LO) <= (RAM_END - MEM_RAM_LO), 1)) {
		/* 0x8008 feeds the simulated raster position, so the slow path has to
		 * see it even though it is plain RAM. */
		return (address == MEM_RAS_H) ? NULL : (ram + address);
	}

	if (__builtin_expect(rom.data != NULL, 1)) {
		const _u32 off = address - ROM_START;
		if (__builtin_expect(off <= (_u32)(ROM_END - ROM_START) && off <= rom.length, 1))
			return rom.data + off;
	}

	return NULL;
}

/* Stores deliberately do NOT take the ROM shortcut. translate_address_write()
 * gates cartridge writes behind memory_unlock_flash_write and drives the
 * EEPROM/flash-save command protocol there; bypassing it would let a game poke
 * straight into the ROM image. ROM writes are rare, the flash path stays. */
static inline _u8* mem_fast_store_ptr(_u32 address)
{
	if (__builtin_expect((address - MEM_RAM_LO) <= (RAM_END - MEM_RAM_LO), 1))
		return ram + address;
	return NULL;
}

static inline _u8 loadB(_u32 address)
{
	_u8* p = mem_fast_load_ptr(address);
	/* post_write() is not consulted for reads and mem_fast_load_ptr already
	 * excluded 0x8008, so a plain dereference is the whole story here. */
	if (__builtin_expect(p != NULL, 1)) return *p;
	return mem_slow_loadB(address);
}

static inline _u16 loadW(_u32 address)
{
	_u8* p = mem_fast_load_ptr(address);
	if (__builtin_expect(p != NULL, 1)) {
		/* memcpy of a fixed size compiles to one LDRH; the original built the
		 * word a byte at a time. Unaligned is fine, RAM is Normal memory. */
		_u16 v; memcpy(&v, p, 2); return v;
	}
	return mem_slow_loadW(address);
}

static inline _u32 loadL(_u32 address)
{
	_u8* p = mem_fast_load_ptr(address);
	if (__builtin_expect(p != NULL, 1)) {
		_u32 v; memcpy(&v, p, 4); return v;
	}
	return mem_slow_loadL(address);
}

static inline void storeB(_u32 address, _u8 data)
{
	_u8* p = mem_fast_store_ptr(address);
	if (__builtin_expect(p != NULL, 1)) {
		/* post_write() reacts only to 0x20, 0xA0, 0xA1, 0xA2 and 0xBA -- all
		 * below 0x100 -- and the fast path accepts only 0x100 and above, so
		 * skipping it is provably behaviour-preserving. */
		*p = data;
		return;
	}
	mem_slow_storeB(address, data);
}

static inline void storeW(_u32 address, _u16 data)
{
	_u8* p = mem_fast_store_ptr(address);
	if (__builtin_expect(p != NULL, 1)) {
		memcpy(p, &data, 2);
		return;
	}
	mem_slow_storeW(address, data);
}

static inline void storeL(_u32 address, _u32 data)
{
	_u8* p = mem_fast_store_ptr(address);
	if (__builtin_expect(p != NULL, 1)) {
		memcpy(p, &data, 4);
		return;
	}
	mem_slow_storeL(address, data);
}

//=============================================================================
#endif
