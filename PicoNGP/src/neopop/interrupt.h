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

	interrupt.h

//=========================================================================
//---------------------------------------------------------------------------

  History of changes:
  ===================

20 JUL 2002 - neopop_uk
=======================================
- Cleaned and tidied up for the source release

26 JUL 2002 - neopop_uk
=======================================
- Boiled the H/V problem down so that it hangs on one variable: gfx_hack.
	This isn't great, but it'll have to do for now...
 
01 AUG 2002 - neopop_uk
=======================================
- Added support for frameskipping.

16 AUG 2002 - neopop_uk
=======================================
- Added prototypes for split interrupt functions.

30 AUG 2002 - neopop_uk
=======================================
- Removed frameskipping.

09 SEP 2002 - neopop_uk
=======================================
- Unified timer update calls.
- Corrected timer speeds.

//---------------------------------------------------------------------------
*/

#ifndef __INTERRUPT__
#define __INTERRUPT__
//=============================================================================

void interrupt(_u8 index);

#define TIMER_HINT_RATE		515		//CPU Ticks between horizontal interrupts

#define TIMER_BASE_RATE		240		//ticks

#define TIMER_T1_RATE		(1 * TIMER_BASE_RATE)
#define TIMER_T4_RATE		(4 * TIMER_BASE_RATE)
#define TIMER_T16_RATE		(16 * TIMER_BASE_RATE)
#define TIMER_T256_RATE		(256 * TIMER_BASE_RATE)

void reset_timers(void);

void updateTimers_slow(_u8 cputicks);

//H-INT Timer
extern _u32 timer_hint;
extern _u8 timer[4];	//Up-counters
extern _u32 timer_clock0, timer_clock1, timer_clock2, timer_clock3;

// Set this value to fix problems with glitching extra lines.
extern BOOL gfx_hack;

// Fast path for the timer hardware.
//
// This used to read `timer_hint >= TIMER_HINT_RATE || (ram[0x20] & 0x0F)`,
// which looks like it rejects the common case but is exactly backwards: any
// game that enables a hardware timer -- which is most of them, for sound sync
// and vblank -- makes the second term permanently true, so the 1.7 KB
// updateTimers_slow() ran on EVERY instruction. With emulation measured at 97%
// of the frame, that call was the frame.
//
// The two jobs are now separated:
//   * the scanline/vblank block only matters once per TIMER_HINT_RATE, i.e.
//     199 times a frame, not once per instruction;
//   * the timer clock generators must accumulate every instruction, but they can
//     only change state when one of them reaches its rate, and that is checked
//     here first.
//
// The gate below is a NECESSARY condition for any timer to do work, so when it
// is false nothing is skipped. Timer 0 in h_int mode and timers 1/3 in chain
// mode are forced on because they can fire without their own clock reaching a
// rate; the chain flags themselves are safe to leave alone, because
// updateTimers_slow() clears timer0/timer2 before anything reads them.
static inline __attribute__((always_inline)) void updateTimers(_u8 cputicks)
{
	timer_hint += cputicks;

	if (__builtin_expect(timer_hint >= TIMER_HINT_RATE, 0))
	{
		updateTimers_slow(cputicks);
		return;
	}

	const _u8 trun = ram[0x20];
	if (__builtin_expect((trun & 0x0F) == 0, 1))
		return;

	timer_clock0 += cputicks;
	timer_clock1 += cputicks;
	timer_clock2 += cputicks;
	timer_clock3 += cputicks;

	// Timer 2: mode 0 does nothing; mode 1 is T1 rate / 2 (120 ticks for normal voice speed).
	if ((trun & 0x04) && (ram[0x28] & 0x03) != 0 && timer_clock2 >= (TIMER_T1_RATE / 2)) { updateTimers_slow(cputicks); return; }
	// Timer 1 chains off timer 0 and timer 3 chains off timer 2. A chain can
	// only ever fire if its source timer is enabled, so requiring the source
	// bit matters: with only timer 3 enabled there is no timer 2, the chain is
	// dead, and forcing the call here meant paying for it on every instruction.
	if ((trun & 0x02) && (ram[0x24] & 0x0C) == 0 && (trun & 0x01)) { updateTimers_slow(cputicks); return; }
	if ((trun & 0x08) && (ram[0x28] & 0x0C) == 0 && (trun & 0x04)) { updateTimers_slow(cputicks); return; }
	// Every remaining mode bottoms out at TIMER_T1_RATE.
	//
	// Each clock only counts if its own timer is enabled. All four clocks
	// accumulate whenever ANY timer runs, but updateTimers_slow() only ever
	// resets the clock of an enabled timer. Without the enable test below, a game
	// that runs just timer 3 (TRUN == 8, which is what Samurai Shodown and most
	// others do) lets clock0/clock1 climb past TIMER_T1_RATE once and stay there:
	// the gate is then true forever and the 2 KB slow path runs on EVERY
	// instruction. Skipping a disabled timer here loses nothing, because the slow
	// path does nothing for it either.
	//
	// The same reasoning applies one level down: a timer in h_int mode (timer 0)
	// or chained to another timer (1 and 3) never consults its own clock and
	// never resets it, so its clock must not be allowed to trip the gate either.
	// The chain cases that CAN fire are already forced through above.
	if (((trun & 0x01) && (ram[0x24] & 0x03) != 0 && timer_clock0 >= TIMER_T1_RATE) ||
	    ((trun & 0x02) && (ram[0x24] & 0x0C) != 0 && timer_clock1 >= TIMER_T1_RATE) ||
	    ((trun & 0x08) && (ram[0x28] & 0x0C) != 0 && timer_clock3 >= TIMER_T1_RATE)) { updateTimers_slow(cputicks); return; }
}

//=============================================================================
#endif
