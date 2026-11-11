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

	flash.c

//=========================================================================
//---------------------------------------------------------------------------

  History of changes:
  ===================

20 JUL 2002 - neopop_uk
=======================================
- Cleaned and tidied up for the source release

26 JUL 2002 - neopop_uk
=======================================
- Added flash_read(char* filename) - this is called after loading the rom
	by the common emulator code.
- Added defintions of flash data header and block.
- Added flash header reading to determine buffer size.
- Added flash header writing.
- Added flash block database, saving and loading of blocks - COMPLETE!

- Removed the EEPROM emulation as it is unused, and will likely never
	be completed.

- Renamed this file "flash.c"

10 AUG 2002 - neopop_uk
=======================================
- Added flash_commit function to do the writing.

11 AUG 2002 - neopop_uk
=======================================
- Optimised flash_write

15 AUG 2002 - neopop_uk
=======================================
- Removed the legacy 'eeprom' variables.

08 SEP 2002 - neopop_uk
=======================================
- Made flash writes more robust be eliminating a possible logic error.

//---------------------------------------------------------------------------
*/

#include "neopop.h"
#include "flash.h"
#include "mem.h"

//-----------------------------------------------------------------------------
// Local Definitions
//-----------------------------------------------------------------------------
//This value is used to verify flash data - it is set to the
//version number that the flash description was modified for.

#define FLASH_VALID_ID		0x0053

//Number of different flash blocks, this should be enough.

#define FLASH_MAX_BLOCKS	256

typedef struct
{
	//Flash Id
	_u16 valid_flash_id;		// = FLASH_VALID_ID
	
	_u16 block_count;			//Number of flash data blocks

	_u32 total_file_length;		// header + block[0 - block_count]

} FlashFileHeader;

typedef struct
{
	_u32 start_address;		// 24 bit address
	_u16 data_length;		// length of following data

	//Followed by data_length bytes of the actual data.

} FlashFileBlockHeader;

#include <string.h>

#define FLASH_OVERLAY_BLOCK_SIZE 256
#define FLASH_OVERLAY_MAX_BLOCKS 48

typedef struct {
    _u32 address;  // 256-byte aligned address
    _u8  data[FLASH_OVERLAY_BLOCK_SIZE];
    _u8  used;
} FlashOverlayBlock;

static FlashFileBlockHeader	blocks[256];
static _u16 block_count;

static FlashOverlayBlock s_flash_overlay[FLASH_OVERLAY_MAX_BLOCKS];
static int s_flash_overlay_count = 0;
static volatile BOOL s_flash_dirty = FALSE;

BOOL flash_is_dirty(void)
{
    return s_flash_dirty;
}

void flash_clear_dirty(void)
{
    s_flash_dirty = FALSE;
}

void flash_overlay_reset(void)
{
    memset(s_flash_overlay, 0, sizeof(s_flash_overlay));
    s_flash_overlay_count = 0;
    block_count = 0;
    s_flash_dirty = FALSE;
}

void* flash_overlay_get_read_ptr(_u32 address)
{
    if (__builtin_expect(s_flash_overlay_count == 0, 1))
        return NULL;
    _u32 block_addr = address & ~0xFFu;
    for (int i = 0; i < s_flash_overlay_count; i++)
    {
        if (s_flash_overlay[i].used && s_flash_overlay[i].address == block_addr)
        {
            return &s_flash_overlay[i].data[address & 0xFFu];
        }
    }
    return NULL;
}


void* flash_overlay_get_write_ptr(_u32 address)
{
    _u32 block_addr = address & ~0xFFu;
    for (int i = 0; i < s_flash_overlay_count; i++)
    {
        if (s_flash_overlay[i].used && s_flash_overlay[i].address == block_addr)
        {
            s_flash_dirty = TRUE;
            return &s_flash_overlay[i].data[address & 0xFFu];
        }
    }

    if (s_flash_overlay_count < FLASH_OVERLAY_MAX_BLOCKS)
    {
        int idx = s_flash_overlay_count++;
        s_flash_overlay[idx].address = block_addr;
        s_flash_overlay[idx].used = 1;

        // Initialize block from original ROM Flash data if available
        const _u8* rom_src = NULL;
        if (rom.data && block_addr >= ROM_START && block_addr <= ROM_END)
        {
            if (block_addr <= ROM_START + rom.length)
                rom_src = rom.data + (block_addr - ROM_START);
        }
        else if (rom.data && block_addr >= HIROM_START && block_addr <= HIROM_END)
        {
            if (block_addr <= HIROM_START + (rom.length - 0x200000))
                rom_src = rom.data + 0x200000 + (block_addr - HIROM_START);
        }

        if (rom_src)
            memcpy(s_flash_overlay[idx].data, rom_src, FLASH_OVERLAY_BLOCK_SIZE);
        else
            memset(s_flash_overlay[idx].data, 0xFF, FLASH_OVERLAY_BLOCK_SIZE);

        flash_write(block_addr, FLASH_OVERLAY_BLOCK_SIZE);
        s_flash_dirty = TRUE;
        return &s_flash_overlay[idx].data[address & 0xFFu];
    }

    return NULL;
}

//-----------------------------------------------------------------------------
// optimise_blocks()
//-----------------------------------------------------------------------------
static void optimise_blocks(void)
{
	int i, j;

	if (block_count < 2)
		return;

	// Bubble Sort by address
	for (i = 0; i < block_count - 1; i++)
	{
		for (j = i+1; j < block_count; j++)
		{
			//Swap?
			if (blocks[i].start_address > blocks[j].start_address)
			{
				_u32 temp32;
				_u16 temp16;

				temp32 = blocks[i].start_address;
				blocks[i].start_address = blocks[j].start_address;
				blocks[j].start_address = temp32;

				temp16 = blocks[i].data_length;
				blocks[i].data_length = blocks[j].data_length;
				blocks[j].data_length = temp16;
			}
		}
	}

	//Join contiguous or overlapping blocks
	for (i = 0; i < block_count - 1; /**/)
	{
		_u32 current_end = blocks[i].start_address + blocks[i].data_length;
		_u32 next_end = blocks[i+1].start_address + blocks[i+1].data_length;

		// Next block borders or overlaps this one, and merged length fits in 16 bits
		if (blocks[i+1].start_address <= current_end)
		{
			_u32 new_end = (next_end > current_end) ? next_end : current_end;
			if ((new_end - blocks[i].start_address) <= 65535)
			{
				blocks[i].data_length = (_u16)(new_end - blocks[i].start_address);

				// Remove the next one.
				for (j = i + 2; j < block_count; j++)
				{
					blocks[j-1].start_address = blocks[j].start_address;
					blocks[j-1].data_length = blocks[j].data_length;
				}
				block_count--;
				continue;
			}
		}
		i++; // Advance to compare next pair
	}
}

//=============================================================================

//-----------------------------------------------------------------------------
// flash_read()
//-----------------------------------------------------------------------------
void flash_read(void)
{
	FlashFileHeader header;
	_u8* flashdata, *fileptr;
	_u16 i;

	//Initialise the internal flash configuration
	flash_overlay_reset();

	//Read flash buffer header
	if (system_io_flash_read((_u8*)&header, sizeof(FlashFileHeader)) == FALSE)
		return;	//Silent failure - no flash data yet.

	//Verify correct flash id
	if (header.valid_flash_id != FLASH_VALID_ID)
		return;

	//Sanity check total file length
	if (header.total_file_length < sizeof(FlashFileHeader) || header.total_file_length > 65536)
		return;

	//Read the flash data
	flashdata = (_u8*)malloc(header.total_file_length);
	if (!flashdata)
		return;

	if (system_io_flash_read(flashdata, header.total_file_length) == FALSE)
	{
		free(flashdata);
		return;
	}

	_u16 num_blocks = header.block_count;
	if (num_blocks > FLASH_MAX_BLOCKS)
		num_blocks = FLASH_MAX_BLOCKS;

	fileptr = flashdata + sizeof(FlashFileHeader);
	_u8* file_end = flashdata + header.total_file_length;

	block_count = 0;

	for (i = 0; i < num_blocks; i++)
	{
		if (fileptr + sizeof(FlashFileBlockHeader) > file_end)
			break;

		FlashFileBlockHeader* current = (FlashFileBlockHeader*)fileptr;
		fileptr += sizeof(FlashFileBlockHeader);

		if (fileptr + current->data_length > file_end)
			break;

		_u32 addr = current->start_address;
		_u16 len = current->data_length;

		// Validate address: must be in valid save area (never Sector 0 boot header < 0x201000)
		if (!((addr >= 0x201000 && addr <= ROM_END) || (addr >= HIROM_START && addr <= HIROM_END)))
		{
			fileptr += len;
			continue;
		}

		blocks[block_count].start_address = addr;
		blocks[block_count].data_length = len;
		block_count++;

		// Place directly into flash overlay without storeB/flash_write side-effects
		_u32 offset = 0;
		while (offset < len && s_flash_overlay_count < FLASH_OVERLAY_MAX_BLOCKS)
		{
			_u32 cur_addr = addr + offset;
			_u32 block_addr = cur_addr & ~0xFFu;
			_u32 block_off = cur_addr & 0xFFu;
			_u32 chunk = 256 - block_off;
			if (chunk > (len - offset)) chunk = len - offset;

			// Find or create overlay block
			int found_idx = -1;
			for (int k = 0; k < s_flash_overlay_count; k++)
			{
				if (s_flash_overlay[k].used && s_flash_overlay[k].address == block_addr)
				{
					found_idx = k;
					break;
				}
			}
			if (found_idx < 0 && s_flash_overlay_count < FLASH_OVERLAY_MAX_BLOCKS)
			{
				found_idx = s_flash_overlay_count++;
				s_flash_overlay[found_idx].address = block_addr;
				s_flash_overlay[found_idx].used = 1;

				// Seed with original ROM Flash data if available
				const _u8* rom_src = NULL;
				if (rom.data && block_addr >= ROM_START && block_addr <= ROM_END)
				{
					if (block_addr <= ROM_START + rom.length)
						rom_src = rom.data + (block_addr - ROM_START);
				}
				else if (rom.data && block_addr >= HIROM_START && block_addr <= HIROM_END)
				{
					if (block_addr <= HIROM_START + (rom.length - 0x200000))
						rom_src = rom.data + 0x200000 + (block_addr - HIROM_START);
				}

				if (rom_src)
					memcpy(s_flash_overlay[found_idx].data, rom_src, FLASH_OVERLAY_BLOCK_SIZE);
				else
					memset(s_flash_overlay[found_idx].data, 0xFF, FLASH_OVERLAY_BLOCK_SIZE);
			}

			if (found_idx >= 0)
			{
				memcpy(&s_flash_overlay[found_idx].data[block_off], fileptr + offset, chunk);
			}

			offset += chunk;
		}

		fileptr += len;
	}

	free(flashdata);

	optimise_blocks();
	flash_clear_dirty();
}

//-----------------------------------------------------------------------------
// flash_write()
//-----------------------------------------------------------------------------
void flash_write(_u32 start_address, _u16 length)
{
	_u16 i;

	//Now we need a new flash command before the next flash write will work!
	memory_flash_command = FALSE;

//	system_debug_message("flash write: %06X, %d bytes", start_address, length);

	for (i = 0; i < block_count; i++)
	{
		//Got this block with enough bytes to cover it
		if (blocks[i].start_address == start_address &&
			blocks[i].data_length >= length)
		{
			return; //Nothing to do, block already registered.
		}

		//Got this block with but it's length is too short
		if (blocks[i].start_address == start_address &&
			blocks[i].data_length < length)
		{
			blocks[i].data_length = length;	//Enlarge block updating.
			return;
		}
	}

	// New block needs to be added
	if (block_count < FLASH_MAX_BLOCKS)
	{
		blocks[block_count].start_address = start_address;
		blocks[block_count].data_length = length;
		block_count++;
	}
}

//-----------------------------------------------------------------------------
// flash_commit()
//-----------------------------------------------------------------------------
void flash_commit(void)
{
	int i;
	FlashFileHeader header;
	_u8 *flashdata, *fileptr;

	//No flash data?
	if (block_count == 0)
		return;

	//Optimise before writing
	optimise_blocks();

	//Build a header;
	header.valid_flash_id = FLASH_VALID_ID;
	header.block_count = block_count;
	header.total_file_length = sizeof(FlashFileHeader);
	for (i = 0; i < block_count; i++)
	{
		header.total_file_length += sizeof(FlashFileBlockHeader);
		header.total_file_length += blocks[i].data_length;
	}

	//Write the flash data
	flashdata = (_u8*)malloc(header.total_file_length * sizeof(_u8));
	if (!flashdata) return;

	//Copy header
	memcpy(flashdata, &header, sizeof(FlashFileHeader));
	fileptr = flashdata + sizeof(FlashFileHeader);

	//Copy blocks
	for (i = 0; i < block_count; i++)
	{
		_u32 j;

		memcpy(fileptr, &blocks[i], sizeof(FlashFileBlockHeader));
		fileptr += sizeof(FlashFileBlockHeader);

		//Copy data
		for (j = 0; j < blocks[i].data_length; j++)
		{
			*fileptr = loadB(blocks[i].start_address + j);
			fileptr++;
		}
	}

	//Try to Write flash buffer
	system_io_flash_write(flashdata, header.total_file_length);

	free(flashdata);
	flash_clear_dirty();
}

//=============================================================================
