/*
    Copyright 2014 StapleButter

    This file is part of blargSnes.

    blargSnes is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    blargSnes is distributed in the hope that it will be useful, but WITHOUT ANY 
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS 
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along 
    with blargSnes. If not, see http://www.gnu.org/licenses/.
*/

#include "snes.h"
#include "cpu.h"
#include "ppu.h"

#include <ctr/HID.h>


u8* ROM_Bank0;
u8* ROM_Bank0End;

u8 ROM_Region;

bool SNES_HiROM;
u8 SNES_SysRAM[0x20000] __attribute__((aligned(256)));
u32 SNES_SRAMMask;
u8* SNES_SRAM = NULL;
FILE* SNES_SRAMFile = NULL;

char SNES_SRAMPath[256] ;

// addressing: BBBBBBBB:AAAaaaaa:aaaaaaaa
// bit0-27: argument
// bit28: access speed (0 = 6 cycles, 1 = 8 cycles)
// bit29: special bit (0 = argument is a RAM pointer, 1 = other case)
// bit30: write permission (0 = can write, 1 = read-only)
// common cases:
// * b29=0, b30=0: system RAM, SRAM; arg = pointer to RAM
// * b29=1, b30=0: I/O, expansion RAM; arg = zero
// * b29=0, b30=1: cached ROM; arg = pointer to RAM
// * b29=1, b30=1: non-cached ROM; arg = file offset
//
// cheat: we place stuff before the start of the actual array-- those 
// can be accessed quickly by the CPU core since it keeps a pointer to
// this table in one of the CPU registers
//
// table[-1] -> SRAM dirty flag
// table[-2] -> HBlank/VBlank flags
u32 _Mem_PtrTable[(SNESSTATUS_SIZE >> 2) + 0x800];
u32* Mem_PtrTable ;
SNES_StatusData* SNES_Status;

u8 SNES_HVBJOY = 0x00;
u16 SNES_VMatch = 0;
u16 SNES_HMatchRaw = 0, SNES_HMatch = 0;
u16 SNES_HCheck = 0;

u8 SNES_MulA = 0;
u16 SNES_MulRes = 0;
u16 SNES_DivA = 0;
u16 SNES_DivRes = 0;

bool SNES_FastROM = false;

extern u8 DMA_HDMAFlag;



void reportBRK(u32 pc)
{
	bprintf("BRK @ %02X:%04X | %08X\n", pc>>16, pc&0xFFFF, MEM_PTR(pc>>16, pc&0xFFFF));
	for(;;);
}


bool SNES_LoadROM(char* path)
{
	SNES_Status = &_Mem_PtrTable[0];
	Mem_PtrTable = &_Mem_PtrTable[SNESSTATUS_SIZE >> 2];
	
	if (!ROM_LoadFile(path))
		return false;
		
	ROM_Bank0 = ROM_Buffer;
	ROM_Bank0End = ROM_Bank0 + (SNES_HiROM ? 0x10000:0x8000);

	u8 sramsize = ROM_Buffer[ROM_HeaderOffset + 0x18];
	u8 region = ROM_Buffer[ROM_HeaderOffset + 0x19];
	
	if (region <= 0x01 || (region >= 0x0D && region <= 0x10))
		ROM_Region = 0;
	else
		ROM_Region = 1;
	
	SNES_SRAMMask = sramsize ? ((1024 << sramsize) - 1) : 0;
	SNES_SRAMMask &= 0x000FFFFF;
	bprintf("SRAM size: %dKB\n", (SNES_SRAMMask+1) >> 10);
	
	if (SNES_SRAMMask)
	{
		// TODO
		/*strncpy(SNES_SRAMPath, path, strlen(path)-3);
		strncpy(SNES_SRAMPath + strlen(path)-3, "srm", 3);
		SNES_SRAMPath[strlen(path)] = '\0';
		FILE* sram = fopen(SNES_SRAMPath, "r+");
		if (!sram) sram = fopen(SNES_SRAMPath, "w+");
		if (sram) fclose(sram);*/
	}
	SNES_SRAMMask = 0; // REMOVEME
	
	return true;
}

void SNES_Reset()
{
	u32 i, a, b;

	for (i = 0; i < (128 * 1024); i += 4)
		*(u32*)&SNES_SysRAM[i] = 0x55555555; // idk about this
		
	SNES_FastROM = false;
	
	DMA_HDMAFlag = 0;

	/*if (SNES_SRAM) 
	{
		free(SNES_SRAM);
		SNES_SRAM = NULL;
	}
	if (SNES_SRAMMask)
	{
		SNES_SRAM = malloc(SNES_SRAMMask + 1);
		for (i = 0; i <= SNES_SRAMMask; i += 4)
			*(u32*)&SNES_SRAM[i] = 0;
		
		SNES_SRAMFile = fopen(SNES_SRAMPath, "r");
		if (SNES_SRAMFile)
		{
			fread(SNES_SRAM, SNES_SRAMMask+1, 1, SNES_SRAMFile);
			fclose(SNES_SRAMFile);
			SNES_SRAMFile = NULL;
		}
	}*/
	
	SNES_Status->SRAMDirty = 0;
	SNES_Status->HVBFlags = 0x00;
	SNES_Status->SRAMMask = SNES_SRAMMask;
	SNES_Status->IRQCond = 0;
	
	for (b = 0; b < 0x40; b++)
	{
		MEM_PTR(b, 0x0000) = MEM_PTR(0x80 + b, 0x0000) = MPTR_SLOW | (u32)&SNES_SysRAM[0];
		MEM_PTR(b, 0x2000) = MEM_PTR(0x80 + b, 0x2000) = MPTR_SPECIAL;
		MEM_PTR(b, 0x4000) = MEM_PTR(0x80 + b, 0x4000) = MPTR_SPECIAL;
		
		/*if ((b >= 0x30) && SNES_HiROM && SNES_SRAMMask)
			MEM_PTR(b, 0x6000) = MEM_PTR(0x80 + b, 0x6000) = MPTR_SLOW | MPTR_SRAM | (u32)&SNES_SRAM[(b << 13) & SNES_SRAMMask];
		else*/
			MEM_PTR(b, 0x6000) = MEM_PTR(0x80 + b, 0x6000) = MPTR_SLOW | MPTR_SPECIAL;
	}

	if (SNES_HiROM)
	{
		for (b = 0; b < 0x02; b++)
			for (a = 0; a < 0x10000; a += 0x2000)
				MEM_PTR(0x7E + b, a) = MPTR_SLOW | (u32)&SNES_SysRAM[(b << 16) + a];
	}
	else
	{
		/*if (SNES_SRAMMask)
		{
			for (b = 0; b < 0x0E; b++)
				for (a = 0; a < 0x8000; a += 0x2000)
					MEM_PTR(0x70 + b, a) = MEM_PTR(0xF0 + b, a) = MPTR_SLOW | MPTR_SRAM | (u32)&SNES_SRAM[((b << 15) + a) & SNES_SRAMMask];
			for (a = 0; a < 0x8000; a += 0x2000)
			{
				MEM_PTR(0xFE + b, a) = MPTR_SLOW | MPTR_SRAM | (u32)&SNES_SRAM[((0xE << 15) + a) & SNES_SRAMMask];
				MEM_PTR(0xFF + b, a) = MPTR_SLOW | MPTR_SRAM | (u32)&SNES_SRAM[((0xF << 15) + a) & SNES_SRAMMask];
			}
		}
		else*/
		{
			for (b = 0; b < 0x0E; b++)
				for (a = 0; a < 0x8000; a += 0x2000)
					MEM_PTR(0x70 + b, a) = MEM_PTR(0xF0 + b, a) = MPTR_SLOW | MPTR_SPECIAL;
			for (a = 0; a < 0x8000; a += 0x2000)
			{
				MEM_PTR(0xFE + b, a) = MPTR_SLOW | MPTR_SPECIAL;
				MEM_PTR(0xFF + b, a) = MPTR_SLOW | MPTR_SPECIAL;
			}
		}

		for (b = 0; b < 0x02; b++)
			for (a = 0; a < 0x10000; a += 0x2000)
				MEM_PTR(0x7E + b, a) = MEM_PTR(0xFE + b, a) = MPTR_SLOW | (u32)&SNES_SysRAM[(b << 16) + a];
	}
	
	bprintf("sysram = %08X\n", &SNES_SysRAM[0]);
	
	SNES_HVBJOY = 0x00;
	
	SNES_MulA = 0;
	SNES_MulRes = 0;
	SNES_DivA = 0;
	SNES_DivRes = 0;
	
	PPU_Reset();
}


void SNES_SaveSRAM()
{
	/*if (!SNES_SRAMMask) 
		return;
	
	if (!SNES_Status->SRAMDirty)
		return;
	
	SNES_SRAMFile = fopen(SNES_SRAMPath, "r+");
	if (SNES_SRAMFile)
	{
		bprintf("SRAM save\n");
		SNES_Status->SRAMDirty = 0;
		fseek(SNES_SRAMFile, 0, SEEK_SET);
		fwrite(SNES_SRAM, SNES_SRAMMask+1, 1, SNES_SRAMFile);
		fclose(SNES_SRAMFile);
		SNES_SRAMFile = NULL;
	}*/
}


void report_unk_lol(u32 op, u32 pc)
{
	if (op == 0xDB) 
	{
		asm("stmdb sp!, {r12}");
		printf("STOP %06X\n", pc);
		asm("ldmia sp!, {r12}");
		return; 
	}

	printf("OP_UNK %08X %02X\n", pc, op);
	for (;;);// swiWaitForVBlank();
}


inline u8 IO_ReadKeysLow()
{
	u32 keys = hidSharedMem[0x28>>2];
	u8 ret = 0;
	
	if (!(keys & PAD_A)) ret |= 0x80;
	if (!(keys & PAD_X)) ret |= 0x40;
	if (!(keys & PAD_L)) ret |= 0x20;
	if (!(keys & PAD_R)) ret |= 0x10;
	
	return ret;
}

inline u8 IO_ReadKeysHigh()
{
	u32 keys = hidSharedMem[0x28>>2];
	u8 ret = 0;
	
	if (!(keys & PAD_B)) 					ret |= 0x80;
	if (!(keys & PAD_Y)) 					ret |= 0x40;
	if (!(keys & PAD_SELECT)) 				ret |= 0x20;
	if (!(keys & PAD_START)) 				ret |= 0x10;
	if (!(keys & (PAD_UP|0x40000000))) 		ret |= 0x08;
	if (!(keys & (PAD_DOWN|0x80000000))) 	ret |= 0x04;
	if (!(keys & (PAD_LEFT|0x20000000))) 	ret |= 0x02;
	if (!(keys & (PAD_RIGHT|0x10000000))) 	ret |= 0x01;
	
	return ret;
}


u8 SNES_GIORead8(u32 addr)
{
	asm("stmdb sp!, {r12}");
	
	u8 ret = 0;
	switch (addr)
	{
		case 0x10:
			if (SNES_Status->HVBFlags & 0x20)
			{
				ret = 0x80;
				SNES_Status->HVBFlags &= 0xDF;
			}
			break;
			
		case 0x11:
			if (SNES_Status->HVBFlags & 0x10)
			{
				ret = 0x80;
				SNES_Status->HVBFlags &= 0xEF;
			}
			break;
			
		case 0x12:
			ret = SNES_Status->HVBFlags & 0xC0;
			break;
			
		case 0x14:
			ret = SNES_DivRes & 0xFF;
			break;
		case 0x15:
			ret = SNES_DivRes >> 8;
			break;
			
		case 0x16:
			ret = SNES_MulRes & 0xFF;
			break;
		case 0x17:
			ret = SNES_MulRes >> 8;
			break;
			
		case 0x18:
			ret = IO_ReadKeysLow();
			break;
		case 0x19:
			ret = IO_ReadKeysHigh();
			break;
	}
	
	asm("ldmia sp!, {r12}");
	return ret;
}

u16 SNES_GIORead16(u32 addr)
{
	asm("stmdb sp!, {r12}");
	
	u16 ret = 0;
	switch (addr)
	{
		case 0x14:
			ret = SNES_DivRes;
			break;
			
		case 0x16:
			ret = SNES_MulRes;
			break;
			
		case 0x18:
			ret = IO_ReadKeysLow() | (IO_ReadKeysHigh() << 8);
			break;
			
		default:
			ret = SNES_GIORead8(addr);
			ret |= (SNES_GIORead8(addr + 1) << 8);
			break;
	}
	
	asm("ldmia sp!, {r12}");
	return ret;
}

void SNES_GIOWrite8(u32 addr, u8 val)
{
	asm("stmdb sp!, {r12}");
	
	switch (addr)
	{
		case 0x00:
			// the NMI flag is handled in mem_io.s
			SNES_Status->IRQCond = (val & 0x30) >> 4;
			SNES_HCheck = (SNES_Status->IRQCond & 0x1) ? SNES_HMatch : 1364;
			break;
			
		case 0x02:
			SNES_MulA = val;
			break;
		case 0x03:
			SNES_MulRes = (u16)SNES_MulA * (u16)val;
			SNES_DivRes = (u16)val;
			break;
			
		case 0x04:
			SNES_DivA = (SNES_DivA & 0xFF00) | val;
			break;
		case 0x05:
			SNES_DivA = (SNES_DivA & 0x00FF) | (val << 8);
			break;
		case 0x06:
			// TODO: this is not nice, needs better method, but in the meantime it'll work
			{
				int rem, quo;
				derp_divmod(SNES_DivA, val, &rem, &quo);
				SNES_DivRes = (u16)rem;
				SNES_MulRes = (u16)quo;
			}
			break;
			
		case 0x07:
			SNES_HMatchRaw &= 0xFF00;
			SNES_HMatchRaw |= val;
			SNES_HMatch = 1364 - (SNES_HMatchRaw << 2);
			SNES_HCheck = (SNES_Status->IRQCond & 0x1) ? SNES_HMatch : 1364;
			break;
		case 0x08:
			SNES_HMatchRaw &= 0x00FF;
			SNES_HMatchRaw |= (val << 8);
			SNES_HMatch = 1364 - (SNES_HMatchRaw << 2);
			SNES_HCheck = (SNES_Status->IRQCond & 0x1) ? SNES_HMatch : 1364;
			break;
			
		case 0x09:
			SNES_VMatch &= 0xFF00;
			SNES_VMatch |= val;
			break;
		case 0x0A:
			SNES_VMatch &= 0x00FF;
			SNES_VMatch |= (val << 8);
			break;
			
		case 0x0B:
			DMA_Enable(val);
			break;
		case 0x0C:
			DMA_HDMAFlag = val;
			break;
			
		case 0x0D:
			{
				bool fast = (val & 0x01);
				if (fast ^ SNES_FastROM)
				{
					SNES_FastROM = fast;
					ROM_SpeedChanged();
				}
			}
			break;
	}
	
	asm("ldmia sp!, {r12}");
}

void SNES_GIOWrite16(u32 addr, u16 val)
{
	asm("stmdb sp!, {r12}");
	
	switch (addr)
	{
		case 0x02:
			SNES_MulA = val & 0xFF;
			SNES_MulRes = (u16)SNES_MulA * (val >> 8);
			SNES_DivRes = (u16)val;
			break;
			
		case 0x04:
			SNES_DivA = val;
			break;
			
		case 0x07:
			SNES_HMatchRaw = val;
			SNES_HMatch = 1364 - (SNES_HMatchRaw << 2);
			SNES_HCheck = (SNES_Status->IRQCond & 0x1) ? SNES_HMatch : 1364;
			break;
			
		case 0x09:
			SNES_VMatch = val;
			break;
			
		case 0x0B:
			DMA_Enable(val & 0xFF);
			DMA_HDMAFlag = val >> 8;
			break;
			
		default:
			SNES_GIOWrite8(addr, val & 0xFF);
			SNES_GIOWrite8(addr + 1, val >> 8);
			break;
	}
	
	asm("ldmia sp!, {r12}");
}


u8 SNES_JoyRead8(u32 addr)
{
	asm("stmdb sp!, {r12}");
	
	u8 ret = 0;

	// this isn't proper or even nice
	// games that actually require manual joypad I/O will fuck up
	// but this seems to convince SMAS that there is a joystick plugged in
	if (addr == 0x16) ret = 0x01;
	
	asm("ldmia sp!, {r12}");
	return ret;
}

u16 SNES_JoyRead16(u32 addr)
{
	asm("stmdb sp!, {r12}");
	
	u16 ret = 0;
	
	//bprintf("joy read16 40%02X\n", addr);
	
	asm("ldmia sp!, {r12}");
	return ret;
}

void SNES_JoyWrite8(u32 addr, u8 val)
{
	asm("stmdb sp!, {r12}");
	
	asm("ldmia sp!, {r12}");
}

void SNES_JoyWrite16(u32 addr, u16 val)
{
	asm("stmdb sp!, {r12}");
	
	asm("ldmia sp!, {r12}");
}


// this used for DMA

u8 SNES_Read8(u32 addr)
{
	u32 ptr = Mem_PtrTable[addr >> 13];
	if (ptr & MPTR_SPECIAL)
	{
		return SNES_IORead8(addr);
	}
	else
	{
		u8* mptr = (u8*)(ptr & 0x0FFFFFFF);
		return mptr[addr & 0x1FFF];
	}
}

u16 SNES_Read16(u32 addr)
{
	u32 ptr = Mem_PtrTable[addr >> 13];
	if (ptr & MPTR_SPECIAL)
	{
		return SNES_IORead16(addr);
	}
	else
	{
		u8* mptr = (u8*)(ptr & 0x0FFFFFFF);
		addr &= 0x1FFF;
		return mptr[addr] | (mptr[addr + 1] << 8);
	}
}

void SNES_Write8(u32 addr, u8 val)
{
	u32 ptr = Mem_PtrTable[addr >> 13];
	if (ptr & MPTR_READONLY) return;
	if (ptr & MPTR_SPECIAL)
		SNES_IOWrite8(addr, val);
	else
	{
		u8* mptr = (u8*)(ptr & 0x0FFFFFFF);
		mptr[addr & 0x1FFF] = val;
	}
}

void SNES_Write16(u32 addr, u8 val)
{
	u32 ptr = Mem_PtrTable[addr >> 13];
	if (ptr & MPTR_READONLY) return;
	if (ptr & MPTR_SPECIAL)
		SNES_IOWrite16(addr, val);
	else
	{
		u8* mptr = (u8*)(ptr & 0x0FFFFFFF);
		addr &= 0x1FFF;
		mptr[addr] = val & 0xFF;
		mptr[addr + 1] = val >> 8;
	}
}



// TEMPORARY SHIT

u16 PPU_VCount;
