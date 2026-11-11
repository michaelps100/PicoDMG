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

	state.c

//=========================================================================
//---------------------------------------------------------------------------

  History of changes:
  ===================

20 JUL 2002 - neopop_uk
=======================================
- Cleaned and tidied up for the source release

28 JUL 2002 - neopop_uk
=======================================
- Converted the load and save functions to use the new state name.
- Move the version "0050" state loader into it's own function, this makes
	it much easier to add new loaders in the future.

15 AUG 2002 - neopop_uk
=======================================
- Removed storing the 'halt' state as the variable no longer exists.
- Changed 'int reserved1' into 'BOOL eepromStatusEnable'

//---------------------------------------------------------------------------
*/

#include "neopop.h"
#include "state.h"
#include "TLCS-900h/TLCS900h_registers.h"
#include "interrupt.h"
#include "dma.h"
#include "mem.h"
#include <stddef.h>

// Compile-time verification that streaming layout matches NEOPOPSTATE0050 exactly:
_Static_assert(sizeof(_u16) + 0xC000 + sizeof(_u16) + sizeof(NEOPOPSTATE_REGS) == sizeof(NEOPOPSTATE0050), "State layout match");
_Static_assert(offsetof(NEOPOPSTATE0050, pc) == (sizeof(_u16) + 0xC000 + sizeof(_u16)), "PC offset match");

//=============================================================================

static void read_state_0050(char* filename);

//-----------------------------------------------------------------------------
// state_restore()
//-----------------------------------------------------------------------------
void state_restore(char* filename)
{
	_u16 version = 0;

	if (!system_io_state_stream_open_read(filename))
		return;

	if (!system_io_state_stream_read((_u8*)&version, sizeof(_u16)))
	{
		system_io_state_stream_close();
		return;
	}

	switch(version)
	{
	case 0x0050:
		read_state_0050(filename);
		break;

	default:
		system_io_state_stream_close();
		system_message(system_get_string(IDS_BADSTATE));
		return;
	}

#ifdef NEOPOP_DEBUG
	system_debug_message("Restoring State ...");
	system_debug_refresh();
#endif
}

//=============================================================================

//-----------------------------------------------------------------------------
// state_store()
//-----------------------------------------------------------------------------
void state_store(char* filename)
{
	int i,j;
	_u16 valid_state_id = 0x0050;
	_u16 pad = 0;
	NEOPOPSTATE_REGS regs;

	if (!system_io_state_stream_open_write(filename))
		return;

	// 1. Write save state version ID (2 bytes)
	if (!system_io_state_stream_write((const _u8*)&valid_state_id, sizeof(_u16))) {
		system_io_state_stream_close();
		return;
	}

	// 2. Stream RAM (0xC000 = 48 KB) directly from memory
	if (!system_io_state_stream_write((const _u8*)ram, 0xC000)) {
		system_io_state_stream_close();
		return;
	}

	// 3. Write 2 bytes struct alignment padding
	if (!system_io_state_stream_write((const _u8*)&pad, sizeof(_u16))) {
		system_io_state_stream_close();
		return;
	}

	// 4. Fill metadata registers
	memset(&regs, 0, sizeof(NEOPOPSTATE_REGS));
	regs.pc = pc;
	regs.sr = sr;
	regs.f_dash = f_dash;

	for (i = 0; i < 4; i++)
	{
		regs.gpr[i] = gpr[i];
		for (j = 0; j < 4; j++)
			regs.gprBank[i][j] = gprBank[i][j];
	}

	regs.eepromStatusEnable = eepromStatusEnable;
	memcpy(&regs.Z80_regs, &Z80_regs, sizeof(Z80));
	memcpy(&regs.toneChip, &toneChip, sizeof(SoundChip));
	memcpy(&regs.noiseChip, &noiseChip, sizeof(SoundChip));

	regs.timer_hint = timer_hint;
	for (i = 0; i < 4; i++)
		regs.timer[i] = timer[i];

	regs.timer_clock0 = timer_clock0;
	regs.timer_clock1 = timer_clock1;
	regs.timer_clock2 = timer_clock2;
	regs.timer_clock3 = timer_clock3;

	for (i = 0; i < 4; i++)
	{
		regs.dmaS[i] = dmaS[i];
		regs.dmaD[i] = dmaD[i];
		regs.dmaC[i] = dmaC[i];
		regs.dmaM[i] = dmaM[i];
	}

	memcpy(&regs.header, rom_header, sizeof(RomHeader));

	// 5. Write metadata registers
	system_io_state_stream_write((const _u8*)&regs, sizeof(NEOPOPSTATE_REGS));
	system_io_state_stream_close();

#ifdef NEOPOP_DEBUG
	system_debug_message("Saving State ...");
#endif
}

//=============================================================================

static void read_state_0050(char* filename)
{
	int i,j;
	NEOPOPSTATE_REGS regs;

	// 1. Seek to metadata registers offset (0xC004) to verify header first
	if (!system_io_state_stream_seek(sizeof(_u16) + 0xC000 + sizeof(_u16)))
	{
		system_io_state_stream_close();
		return;
	}

	// 2. Read metadata registers
	if (!system_io_state_stream_read((_u8*)&regs, sizeof(NEOPOPSTATE_REGS)))
	{
		system_io_state_stream_close();
		return;
	}

	// 3. Verify correct rom BEFORE touching any emulator state
	if (memcmp(rom_header, &regs.header, sizeof(RomHeader)) != 0)
	{
		system_io_state_stream_close();
		system_message(system_get_string(IDS_WRONGROM));
		return;
	}

	// 4. Initialize emulator subsystems cleanly with reset()
	reset();

	// 5. Seek back to RAM offset (after 2-byte valid_state_id)
	if (!system_io_state_stream_seek(sizeof(_u16)))
	{
		system_io_state_stream_close();
		return;
	}

	// 6. Read saved RAM directly into ram[] (AFTER reset() so it is not erased!)
	if (!system_io_state_stream_read(ram, 0xC000))
	{
		system_io_state_stream_close();
		return;
	}

	system_io_state_stream_close();

	// 7. Apply restored registers
	eepromStatusEnable = regs.eepromStatusEnable;

	// TLCS-900h Registers
	pc = regs.pc;
	sr = regs.sr;
	f_dash = regs.f_dash;

	for (i = 0; i < 4; i++)
	{
		gpr[i] = regs.gpr[i];
		for (j = 0; j < 4; j++)
			gprBank[i][j] = regs.gprBank[i][j];
	}

	// Z80 Registers
	memcpy(&Z80_regs, &regs.Z80_regs, sizeof(Z80));

	// Sound Chips
	memcpy(&toneChip, &regs.toneChip, sizeof(SoundChip));
	memcpy(&noiseChip, &regs.noiseChip, sizeof(SoundChip));

	// Timers
	timer_hint = regs.timer_hint;

	for (i = 0; i < 4; i++)
		timer[i] = regs.timer[i];

	timer_clock0 = regs.timer_clock0;
	timer_clock1 = regs.timer_clock1;
	timer_clock2 = regs.timer_clock2;
	timer_clock3 = regs.timer_clock3;

	// DMA
	for (i = 0; i < 4; i++)
	{
		dmaS[i] = regs.dmaS[i];
		dmaD[i] = regs.dmaD[i];
		dmaC[i] = regs.dmaC[i];
		dmaM[i] = regs.dmaM[i];
	}

	changedSP();
}
