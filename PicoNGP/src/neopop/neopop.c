#pragma GCC optimize("Ofast")
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

	neopop.c

//=========================================================================
//---------------------------------------------------------------------------

  History of changes:
  ===================

20 JUL 2002 - neopop_uk
=======================================
- Cleaned and tidied up for the source release

01 AUG 2002 - neopop_uk
=======================================
- Added support for frameskipping.

15 AUG 2002 - neopop_uk
=======================================
- Changed to the new timer functions. This makes graphical effects
	in "Memories Off Pure" stop flickering, and likely improves some others.

30 AUG 2002 - neopop_uk
=======================================
- Removed frameskipping.

09 SEP 2002 - neopop_uk
=======================================
- Unified timer calls now cycles are correct.

//---------------------------------------------------------------------------
*/

#include "neopop.h"
#include "TLCS-900h/TLCS900h_interpret.h"
#include "TLCS-900h/TLCS900h_registers.h"
#include "Z80_interface.h"
#include "interrupt.h"
#include "mem.h"

//=============================================================================

BOOL language_english;

COLOURMODE system_colour;

_u8 frameskip_count;

//=============================================================================

static BOOL debug_abort_instruction = FALSE;
void instruction_error(char* vaMessage,...)
{
	char message[1000];
	va_list vl;

	va_start(vl, vaMessage);
	vsprintf(message, vaMessage, vl);
	va_end(vl);

#ifdef NEOPOP_DEBUG
	system_debug_message(message);
	debug_abort_instruction = TRUE;
#else
	system_message("[PC %06X] %s", pc, message);
}

#endif
//=============================================================================

#include "pico.h"
#include <stdbool.h>

extern volatile bool s_frame_completed;

#ifndef NEOPOP_DEBUG

static int s_z80_debt = 0;

#include "ngp_prof.h"
#if NGP_PROFILE
volatile _u32 ngp_prof_blocks, ngp_prof_ins, ngp_prof_trun, ngp_prof_z80on;
volatile _u32 ngp_prof_z80, ngp_prof_gfx, ngp_prof_lines, ngp_prof_emu_us, ngp_prof_timers;
volatile _u32 ngp_prof_stall_us, ngp_prof_gfx_us, ngp_prof_z80_us;

void ngp_prof_reset(void)
{
	ngp_prof_stall_us = 0;
	ngp_prof_gfx_us = 0;
	ngp_prof_z80_us = 0;
	ngp_prof_blocks = 0;
	ngp_prof_ins = 0;
	ngp_prof_z80 = 0;
	ngp_prof_gfx = 0;
	ngp_prof_lines = 0;
	ngp_prof_timers = 0;
}
#endif

void __not_in_flash_func(emulate)(void)
{
	int z80_acc = 0;
	int n = 0;

	for (int i = 0; i < 64; i++)
	{
		_u8 c = TLCS900h_interpret();
		updateTimers(c);
		z80_acc += (c >> 1);
		n++;
		if (s_frame_completed) break;
	}

#if NGP_PROFILE
	ngp_prof_blocks++;
	ngp_prof_ins += n;
	ngp_prof_trun = ram[0x20] & 0x0F;
	ngp_prof_z80on = (ram[0xb9] == 0x55);
#endif

	if (ram[0xb9] == 0x55 && !(Z80_regs.IFF & IFF_HALT))
	{
		s_z80_debt += z80_acc;
		if (s_z80_debt > 0)
		{
#if NGP_PROFILE
			const _u32 prof_z0 = time_us_32();
#endif
			s_z80_debt = ExecZ80_batch(&Z80_regs, s_z80_debt);
#if NGP_PROFILE
			ngp_prof_z80_us += time_us_32() - prof_z0;
#endif
			if (Z80_regs.IFF & IFF_HALT)
				s_z80_debt = 0;
		}
	}
	else
	{
		s_z80_debt = 0;
	}
}


//=============================================================================

#endif
#ifdef NEOPOP_DEBUG

#include "TLCS900h_disassemble.h"

//Debug Message Filters
BOOL filter_sound;		
BOOL filter_bios;		
BOOL filter_comms;		
BOOL filter_dma;		
BOOL filter_mem;		

char* disassemble(void)
{
	if (pc >= 0x7000 && pc <= 0x7FFF)
	{
		char* s;
		_u16 p = (_u16)(pc & 0xFFF);
		s = Z80_disassemble(&p);
		pc = p + 0x7000;
		return s;
	}
	else
		return TLCS900h_disassemble();
}

void emulate_debug(BOOL dis_TLCS900h, BOOL dis_Z80)
{
	_u32 storePC = pc;

	debug_abort_memory = FALSE;
	debug_abort_instruction = FALSE;

	system_debug_history_add();		//For the debugger

	if (dis_TLCS900h)
	{
		char* s;

		//Disassemble TLCS-900h
		_u32 oldpc = pc;
		s = disassemble();
		system_debug_message(s);
		system_debug_message_associate_address(oldpc);
		free(s);
		pc = oldpc;
	}

	if (dis_Z80)
	{
		//Disassemble Z80
		if (Z80ACTIVE)
		{
			char* s;
			_u16 pc = Z80_getReg(Z80_REG_PC);
			_u16 store_pc = pc;

			//Disassemble
			s = Z80_disassemble(&pc);
			system_debug_message(s);
			system_debug_message_associate_address(store_pc + 0x7000);
			free(s);
		}
	}

	debug_abort_memory = FALSE;
	debug_abort_instruction = FALSE;

	//==================
	// EMULATE
	//==================
	{
		//TLCS900h instruction
		updateTimers(TLCS900h_interpret());

		//Z80 Instruction
		if (Z80ACTIVE) Z80EMULATE;
	}
	
	//Check register code error
	if (rErr != RERR_VALUE)
		instruction_error("Invalid register code used.");

	//Memory Exception
	if (debug_abort_memory && filter_mem)
	{
		_u32 oldpc = pc;
		char* s;

		debug_abort_memory = FALSE;

		//Try to disassemble the erroneous instruction
		pc = storePC;
		debug_mask_memory_error_messages = TRUE;
		s = disassemble();
		debug_mask_memory_error_messages = FALSE;

		if (debug_abort_memory == FALSE)
		{
			system_debug_message("Stopped due to memory exception caused by");
			system_debug_message("     %s", s);
			system_debug_message_associate_address(storePC);
			system_debug_message("\n");
		}
		else
		{
			system_debug_message("Stopped due to memory exception caused at %06X", storePC);
			system_debug_message_associate_address(storePC);
		}
		free(s);
		pc = oldpc;
		
		system_debug_stop();
		system_debug_refresh();
		return;
	}

	//Unimplemented Instruction
	if (debug_abort_instruction)
	{
		_u32 oldpc = pc;
		char* s;

		debug_abort_memory = FALSE;

		//Try to disassemble the erroneous instruction
		pc = storePC;
		debug_mask_memory_error_messages = TRUE;
		s = disassemble();
		debug_mask_memory_error_messages = FALSE;

		if (debug_abort_memory == FALSE)
		{
			system_debug_message("Stopped due to instruction");
			system_debug_message("     %s", s);
			system_debug_message_associate_address(storePC);
			system_debug_message("\n");
		}
		else
		{
			system_debug_message("Stopped due to instruction at %06X", storePC);
			system_debug_message_associate_address(storePC);
		}
		free(s);
		pc = oldpc;
		
		system_debug_stop();
		system_debug_refresh();
		return;
	}
}


#endif

//=============================================================================

#ifdef NEOPOP_DEBUG

#include "TLCS900h_disassemble.h"

//Debug Message Filters
BOOL filter_sound;		
BOOL filter_bios;		
BOOL filter_comms;		
BOOL filter_dma;		
BOOL filter_mem;		

char* disassemble(void)
{
	if (pc >= 0x7000 && pc <= 0x7FFF)
	{
		char* s;
		_u16 p = (_u16)(pc & 0xFFF);
		s = Z80_disassemble(&p);
		pc = p + 0x7000;
		return s;
	}
	else
		return TLCS900h_disassemble();
}

void emulate_debug(BOOL dis_TLCS900h, BOOL dis_Z80)
{
	_u32 storePC = pc;

	debug_abort_memory = FALSE;
	debug_abort_instruction = FALSE;

	system_debug_history_add();		//For the debugger

	if (dis_TLCS900h)
	{
		char* s;

		//Disassemble TLCS-900h
		_u32 oldpc = pc;
		s = disassemble();
		system_debug_message(s);
		system_debug_message_associate_address(oldpc);
		free(s);
		pc = oldpc;
	}

	if (dis_Z80)
	{
		//Disassemble Z80
		if (Z80ACTIVE)
		{
			char* s;
			_u16 pc = Z80_getReg(Z80_REG_PC);
			_u16 store_pc = pc;

			//Disassemble
			s = Z80_disassemble(&pc);
			system_debug_message(s);
			system_debug_message_associate_address(store_pc + 0x7000);
			free(s);
		}
	}

	debug_abort_memory = FALSE;
	debug_abort_instruction = FALSE;

	//==================
	// EMULATE
	//==================
	{
		//TLCS900h instruction
		updateTimers(TLCS900h_interpret());

		//Z80 Instruction
		if (Z80ACTIVE) Z80EMULATE;
	}
	
	//Check register code error
	if (rErr != RERR_VALUE)
		instruction_error("Invalid register code used.");

	//Memory Exception
	if (debug_abort_memory && filter_mem)
	{
		_u32 oldpc = pc;
		char* s;

		debug_abort_memory = FALSE;

		//Try to disassemble the erroneous instruction
		pc = storePC;
		debug_mask_memory_error_messages = TRUE;
		s = disassemble();
		debug_mask_memory_error_messages = FALSE;

		if (debug_abort_memory == FALSE)
		{
			system_debug_message("Stopped due to memory exception caused by");
			system_debug_message("     %s", s);
			system_debug_message_associate_address(storePC);
			system_debug_message("\n");
		}
		else
		{
			system_debug_message("Stopped due to memory exception caused at %06X", storePC);
			system_debug_message_associate_address(storePC);
		}
		free(s);
		pc = oldpc;
		
		system_debug_stop();
		system_debug_refresh();
		return;
	}

	//Unimplemented Instruction
	if (debug_abort_instruction)
	{
		_u32 oldpc = pc;
		char* s;

		debug_abort_memory = FALSE;

		//Try to disassemble the erroneous instruction
		pc = storePC;
		debug_mask_memory_error_messages = TRUE;
		s = disassemble();
		debug_mask_memory_error_messages = FALSE;

		if (debug_abort_memory == FALSE)
		{
			system_debug_message("Stopped due to instruction");
			system_debug_message("     %s", s);
			system_debug_message_associate_address(storePC);
			system_debug_message("\n");
		}
		else
		{
			system_debug_message("Stopped due to instruction at %06X", storePC);
			system_debug_message_associate_address(storePC);
		}
		free(s);
		pc = oldpc;
		
		system_debug_stop();
		system_debug_refresh();
		return;
	}
}


//=============================================================================

#endif
