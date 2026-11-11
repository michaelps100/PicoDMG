#pragma GCC optimize("Ofast")
#pragma GCC section text=".time_critical"
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

	sound.c

//=========================================================================
//---------------------------------------------------------------------------

  History of changes:
  ===================

20 JUL 2002 - neopop_uk
=======================================
- Cleaned and tidied up for the source release

28 JUL 2002 - neopop_uk
=======================================
- Added clearer explanation of sound volume when debugging.

31 JUL 2002 - neopop_uk
=======================================
- Converted sound buffer to use unsigned data.

01 AUG 2002 - neopop_uk
=======================================
- Even with frameskipping, sound still gets choppy. I've removed
	the "optimisation" that I copied from MAME, 
	and didn't really understand.

03 AUG 2002 - neopop_uk
=======================================
- Add dac_update to fill the dac buffer.
- Removed the dac freq-dup as the O/S will mix the speeds.
- Improved mono mixing to match stereo volume.
- Converted DAC to mono, it never gets used in stereo.

03 AUG 2002 - neopop_uk
=======================================
- Increased the DAC buffer size, "Puyo Pop" was causing an overflow.
- DAC writes are ignored when mute is enabled.

09 AUG 2002 - neopop_uk
=======================================
- Mono chip mixing is now fake, the sound buffer is always stereo.
- Fixed & optimised dac_update if data buffer runs out during sound update.

16 AUG 2002 - neopop_uk
=======================================
- Fixed the sound chips so one is for noises, and the the other for tones.
	This sounds correct (verified from Sonic Adventure MP3 file I have).
- Removed stereo option.

//---------------------------------------------------------------------------
*/

/************************************************************************
 *                                                                      *
 *	Portions, but not all of this source file are based on MAME v0.60	*
 *	File "sn76496.c". All copyright goes to the original author.		*
 *	The remaining parts, including DAC processing, by neopop_uk			*
 *                                                                      *
 ************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include "neopop.h"
#include "mem.h"
#include "sound.h"

//=============================================================================

BOOL mute;

SoundChip toneChip;
SoundChip noiseChip;

//==== DAC
#define DAC_BUFFERSIZE		4096

int dacBufferRead, dacBufferWrite, dacBufferCount;
_u8 dacBufferL[DAC_BUFFERSIZE];

//=============================================================================

#define SOUNDCHIPCLOCK	(3072000)	//Unverified / sounds correct

#define MAX_OUTPUT 0x7fff
#define STEP 0x10000		//Fixed point adjuster

static _u32 VolTable[16];
static _u32 UpdateStep;	//Number of steps during one sample.

/* Formulas for noise generator */
/* bit0 = output */

/* noise feedback for white noise mode (verified on real SN76489 by John Kortink) */
#define FB_WNOISE 0x14002	/* (16bits) bit16 = bit0(out) ^ bit2 ^ bit15 */

/* noise feedback for periodic noise mode */
#define FB_PNOISE 0x08000	/* 15bit rotate */

/* noise generator start preset (for periodic noise) */
#define NG_PRESET 0x0f35

//=============================================================================

static inline int16_t sample_chip_tone(void)
{
	int i;
	int vol[3] = {0, 0, 0};
	int out = 0;

	for (i = 0; i < 3; i++)
	{
		if (toneChip.Output[i]) vol[i] += toneChip.Count[i];
		toneChip.Count[i] -= STEP;

		/* Period[i] is the half period of the square wave. */
		while (toneChip.Count[i] <= 0)
		{
			toneChip.Count[i] += toneChip.Period[i];
			if (toneChip.Count[i] > 0)
			{
				toneChip.Output[i] ^= 1;
				if (toneChip.Output[i]) vol[i] += toneChip.Period[i];
				break;
			}
			toneChip.Count[i] += toneChip.Period[i];
			vol[i] += toneChip.Period[i];
		}
		if (toneChip.Output[i]) vol[i] -= toneChip.Count[i];

		if (toneChip.Volume[i] > 0)
		{
			int centered = (vol[i] << 1) - STEP;
			out += (int)(((int64_t)centered * toneChip.Volume[i]) >> 16);
		}
	}

	if (out > 32767) out = 32767;
	else if (out < -32768) out = -32768;

	return (int16_t)out;
}

//=============================================================================

static inline int16_t sample_chip_noise(void)
{
	if (noiseChip.Volume[3] == 0)
	{
		noiseChip.Count[3] -= STEP;
		while (noiseChip.Count[3] <= 0 && noiseChip.Period[3] > 0)
			noiseChip.Count[3] += noiseChip.Period[3];
		return 0;
	}

	int vol3 = 0;
	int left = STEP;
	do
	{
		int nextevent;

		if (noiseChip.Count[3] < left) nextevent = noiseChip.Count[3];
		else nextevent = left;

		if (noiseChip.Output[3]) vol3 += noiseChip.Count[3];
		noiseChip.Count[3] -= nextevent;
		if (noiseChip.Count[3] <= 0)
		{
			if (noiseChip.RNG & 1) noiseChip.RNG ^= noiseChip.NoiseFB;
			noiseChip.RNG >>= 1;
			noiseChip.Output[3] = noiseChip.RNG & 1;
			noiseChip.Count[3] += noiseChip.Period[3];
			if (noiseChip.Output[3]) vol3 += noiseChip.Period[3];
		}
		if (noiseChip.Output[3]) vol3 -= noiseChip.Count[3];

		left -= nextevent;
	} while (left > 0);

	int centered = (vol3 << 1) - STEP;
	int out = (int)(((int64_t)centered * noiseChip.Volume[3]) >> 16);

	if (out > 32767) out = 32767;
	else if (out < -32768) out = -32768;

	return (int16_t)out;
}

//=============================================================================

void sound_update(_u16* chip_buffer, int length_bytes)
{
	int16_t *buf = (int16_t *)chip_buffer;
	while (length_bytes > 0)
	{
		int32_t mix = (int32_t)sample_chip_tone() + (int32_t)sample_chip_noise();
		if (mix > 32767) mix = 32767;
		else if (mix < -32768) mix = -32768;
		*(buf++) = (int16_t)mix;

		length_bytes -= 2;	// 2 bytes = 16 bits
	}
}


//=============================================================================

void WriteSoundChip(SoundChip* chip, _u8 data)
{
	//Command
	if (data & 0x80)
	{
		int r = (data & 0x70) >> 4;
		int c = r/2;

		chip->LastRegister = r;
		chip->Register[r] = (chip->Register[r] & 0x3f0) | (data & 0x0f);
	
		switch(r)
		{
		case 0:	/* tone 0 : frequency */
		case 2:	/* tone 1 : frequency */
		case 4:	/* tone 2 : frequency */
			chip->Period[c] = UpdateStep * chip->Register[r];
			if (chip->Period[c] == 0) chip->Period[c] = UpdateStep;
			if (r == 4)
			{
				/* update noise shift frequency */
				if ((chip->Register[6] & 0x03) == 0x03)
					chip->Period[3] = 2 * chip->Period[2];
			}
			break;

		case 1:	/* tone 0 : volume */
		case 3:	/* tone 1 : volume */
		case 5:	/* tone 2 : volume */
		case 7:	/* noise  : volume */
#ifdef NEOPOP_DEBUG
			if (filter_sound)
			{
				if (chip == &toneChip)
					system_debug_message("sound (T): Set Tone %d Volume to %d (0 = min, 15 = max)", c, 15 - (data & 0xF));
				else
					system_debug_message("sound (N): Set Tone %d Volume to %d (0 = min, 15 = max)", c, 15 - (data & 0xF));
			}
#endif
			chip->Volume[c] = VolTable[data & 0xF];
			break;

		case 6:	/* noise  : frequency, mode */
			{
				int n = chip->Register[6];
#ifdef NEOPOP_DEBUG
	if (filter_sound)
	{
		char *pm, *nm = "White";
		if ((n & 4)) nm = "Periodic";

		switch(n & 3)
		{
		case 0: pm = "N/512"; break;
		case 1: pm = "N/1024"; break;
		case 2: pm = "N/2048"; break;
		case 3: pm = "Tone#2"; break;
		}
			
		if (chip == &toneChip)
			system_debug_message("sound (T): Set Noise Mode to %s, Period = %s", nm, pm);
		else
			system_debug_message("sound (N): Set Noise Mode to %s, Period = %s", nm, pm);
	}
#endif
				chip->NoiseFB = (n & 4) ? FB_WNOISE : FB_PNOISE;
				n &= 3;
				/* N/512,N/1024,N/2048,Tone #2 output */
				chip->Period[3] = (n == 3) ? 2 * chip->Period[2] : (UpdateStep << (5+n));

				/* reset noise shifter */
				chip->RNG = NG_PRESET;
				chip->Output[3] = chip->RNG & 1;

			}
			break;
		}
	}
	else
	{
		int r = chip->LastRegister;
		int c = r/2;

		switch (r)
		{
			case 0:	/* tone 0 : frequency */
			case 2:	/* tone 1 : frequency */
			case 4:	/* tone 2 : frequency */
				chip->Register[r] = (chip->Register[r] & 0x0f) | ((data & 0x3f) << 4);
				chip->Period[c] = UpdateStep * chip->Register[r];
				if (chip->Period[c] == 0) chip->Period[c] = UpdateStep;
				if (r == 4)
				{
					/* update noise shift frequency */
					if ((chip->Register[6] & 0x03) == 0x03)
						chip->Period[3] = 2 * chip->Period[2];
				}
#ifdef NEOPOP_DEBUG
	if (filter_sound)
	{
		if (chip == &toneChip)
			system_debug_message("sound (T): Set Tone %d Frequency to %d", c, chip->Register[r]);
		else
			system_debug_message("sound (N): Set Tone %d Frequency to %d", c, chip->Register[r]);
	}
#endif
				break;
		}
	}
}

//=============================================================================

void dac_write(void)
{
	if (mute) return;	// Ignore	

	_u8 dval = ram[0xA2];
	if ((dval == 0x80 || dval == 0) && (ram[0xA3] != 0 && ram[0xA3] != 0x80))
		dval = ram[0xA3];

	/* Pretend that conv=5.5 (44100/8000) conversion factor to match 44.1 kHz output.
	 * Alternating 5 and 6 yields exactly 5.5x (8000 Hz * 5.5 = 44000 Hz). */
	static int conv = 5;
	conv = (conv == 5) ? 6 : 5;

	for (int i = 0; i < conv; i++)
	{
		dacBufferL[dacBufferWrite] = dval;
		dacBufferWrite++;
		if (dacBufferWrite == DAC_BUFFERSIZE)
			dacBufferWrite = 0;

		if (dacBufferCount < DAC_BUFFERSIZE)
		{
			dacBufferCount++;
		}
		else
		{
			/* Ring buffer full: advance read pointer to discard oldest sample */
			dacBufferRead++;
			if (dacBufferRead == DAC_BUFFERSIZE)
				dacBufferRead = 0;
		}
	}
}
	
void dac_update(_u8* dac_buffer, int length_bytes)
{
	while (length_bytes > 0)
	{
		if (dacBufferCount > 0)
		{
			*(dac_buffer++) = dacBufferL[dacBufferRead];
			dacBufferCount--;

			dacBufferRead++;
			if (dacBufferRead == DAC_BUFFERSIZE)
				dacBufferRead = 0;
		}
		else
		{
			*(dac_buffer++) = 0x80;
		}
		length_bytes--;
	}
}

//=============================================================================

//Resets the sound chips, also used whenever sound options are changed
void sound_init(int SampleRate)
{
	int i;
	double out;

	/* the base clock for the tone generators is the chip clock divided by 16; */
	/* for the noise generator, it is clock / 256. */
	/* Here we calculate the number of steps which happen during one sample */
	/* at the given sample rate. No. of events = sample rate / (clock/16). */
	/* STEP is a multiplier used to turn the fraction into a fixed point */
	/* number. */
	UpdateStep = (_u32)(((double)STEP * SampleRate * 16) / SOUNDCHIPCLOCK);

	//Initialise Left Chip
	memset(&toneChip, 0, sizeof(SoundChip));

	//Initialise Right Chip
	memset(&noiseChip, 0, sizeof(SoundChip));

	//Default register settings
	for (i = 0;i < 8;i+=2)
	{
		toneChip.Register[i] = 0;
		toneChip.Register[i + 1] = 0x0f;	/* volume = 0 */
		noiseChip.Register[i] = 0;
		noiseChip.Register[i + 1] = 0x0f;	/* volume = 0 */
	}

	for (i = 0;i < 4;i++)
	{
		toneChip.Output[i] = 0;
		toneChip.Period[i] = toneChip.Count[i] = UpdateStep;
		noiseChip.Output[i] = 0;
		noiseChip.Period[i] = noiseChip.Count[i] = UpdateStep;
	}

	//Build the volume table
	out = MAX_OUTPUT / 3;

	/* build volume table (2dB per step) */
	for (i = 0;i < 15;i++)
	{
		VolTable[i] = (_u32)out;
		out /= 1.258925412;	/* = 10 ^ (2/20) = 2dB */
	}
	VolTable[15] = 0;


	//Clear the DAC buffer
	for (i = 0; i < DAC_BUFFERSIZE; i++)
		dacBufferL[i] = 0x80;

	dacBufferCount = 0;
	dacBufferRead = 0; 
	dacBufferWrite = 0;
}

//=============================================================================
