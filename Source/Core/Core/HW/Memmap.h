// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"

// Enable memory checks in the Debug/DebugFast builds, but NOT in release
#if defined(_DEBUG) || defined(DEBUGFAST)
	#define ENABLE_MEM_CHECK
#endif

// Global declarations
class PointerWrap;
namespace MMIO { class Mapping; }

namespace Memory
{
// Base is a pointer to the base of the memory map. Yes, some MMU tricks
// are used to set up a full GC or Wii memory map in process memory.  on
// 32-bit, you have to mask your offsets with 0x3FFFFFFF. This means that
// some things are mirrored too many times, but eh... it works.

// In 64-bit, this might point to "high memory" (above the 32-bit limit),
// so be sure to load it into a 64-bit register.
extern u8* physical_base;
extern u8* logical_base;

// These are guaranteed to point to "low memory" addresses (sub-32-bit).
extern u8* m_pRAM;
extern u8* m_pEXRAM;
extern u8* m_pL1Cache;

enum
{
	// RAM_SIZE is the amount allocated by the emulator, whereas REALRAM_SIZE is
	// what will be reported in lowmem, and thus used by emulated software.
	// Note: Writing to lowmem is done by IPL. If using retail IPL, it will
	// always be set to 24MB.
	REALRAM_SIZE  = 0x01800000,
	RAM_SIZE      = ROUND_UP_POW2(REALRAM_SIZE),
	RAM_MASK      = RAM_SIZE - 1,
	L1_CACHE_SIZE = 0x00040000,
	L1_CACHE_MASK = L1_CACHE_SIZE - 1,
	IO_SIZE       = 0x00010000,
	EXRAM_SIZE    = 0x04000000,
	EXRAM_MASK    = EXRAM_SIZE - 1,

	ADDR_MASK_HW_ACCESS = 0x0c000000,
	ADDR_MASK_MEM1      = 0x20000000,

#if _ARCH_32
	MEMVIEW32_MASK  = 0x3FFFFFFF,
#endif
};

// MMIO mapping object.
extern MMIO::Mapping* mmio_mapping;

// Init and Shutdown
bool IsInitialized();
void Init();
void Shutdown();
void DoState(PointerWrap &p);

void Clear();
bool AreMemoryBreakpointsActivated();

// Routines for debugger UI, cheats, etc. to access emulated memory from the
// perspective of the CPU.  Not for use by core emulation routines. 
// Use "Debug_" prefix.
u8 Debug_Read_U8(const u32 address);
u16 Debug_Read_U16(const u32 address);
u32 Debug_Read_U32(const u32 address);
u32 Debug_Read_Instruction(const u32 address);

void Debug_Write_U8(const u8 var, const u32 address);
void Debug_Write_U16(const u16 var, const u32 address);
void Debug_Write_U32(const u32 var, const u32 address);
void Debug_Write_U64(const u64 var, const u32 address);

// Returns whether a read or write to the given address will resolve to a RAM
// access given the current CPU state.
bool Debug_IsRAMAddress(const u32 address);

std::string Debug_GetString(u32 em_address, size_t size = 0);

// Routines for the CPU cores to access memory. Use "CPU_" prefix.

// Used by interpreter to read instructions, uses iCache
u32 CPU_Read_Opcode(const u32 address);
struct TryReadInstResult
{
	bool valid;
	bool from_bat;
	u32 hex;
};
TryReadInstResult CPU_TryReadInstruction(const u32 address);

u8  CPU_Read_U8(const u32 address);
u16 CPU_Read_U16(const u32 address);
u32 CPU_Read_U32(const u32 address);
u64 CPU_Read_U64(const u32 address);

// Useful helper functions, used by ARM JIT
float CPU_Read_F32(const u32 address);
double CPU_Read_F64(const u32 address);

// used by JIT. Return zero-extended 32bit values
u32 CPU_Read_U8_ZX(const u32 address);
u32 CPU_Read_U16_ZX(const u32 address);

void CPU_Write_U8(const u8 var, const u32 address);
void CPU_Write_U16(const u16 var, const u32 address);
void CPU_Write_U32(const u32 var, const u32 address);
void CPU_Write_U64(const u64 var, const u32 address);

void CPU_Write_U16_Swap(const u16 var, const u32 address);
void CPU_Write_U32_Swap(const u32 var, const u32 address);
void CPU_Write_U64_Swap(const u64 var, const u32 address);

// Useful helper functions, used by ARM JIT
void CPU_Write_F64(const double var, const u32 address);

void CPU_DMA_LCToMemory(const u32 memAddr, const u32 cacheAddr, const u32 numBlocks);
void CPU_DMA_MemoryToLC(const u32 cacheAddr, const u32 memAddr, const u32 numBlocks);
void CPU_ClearCacheLine(const u32 address); // Zeroes 32 bytes; address should be 32-byte-aligned

// TLB functions
void CPU_SDRUpdated();
void CPU_InvalidateTLBEntry(u32 address);

// Result changes based on the BAT registers and MSR.DR.  Returns whether
// it's safe to optimize a read or write to this address to an unguarded
// memory access.  Does not consider page tables.
bool CPU_IsRAMAddress(const u32 address);

// Routines to access physically addressed memory, designed for use by
// emulated hardware outside the CPU. Use "Device_" prefix.
std::string Device_GetString(u32 em_address, size_t size = 0);
u8* Device_GetPointer(const u32 address);
void Device_CopyFromEmu(void* data, u32 address, size_t size);
void Device_CopyToEmu(u32 address, const void* data, size_t size);
void Device_Memset(const u32 address, const u8 var, const u32 length);
u8  Device_Read_U8(const u32 address);
u16 Device_Read_U16(const u32 address);
u32 Device_Read_U32(const u32 address);
u64 Device_Read_U64(const u32 address);
void Device_Write_U8(const u8 var, const u32 address);
void Device_Write_U16(const u16 var, const u32 address);
void Device_Write_U32(const u32 var, const u32 address);
void Device_Write_U64(const u64 var, const u32 address);
void Device_Write_U32_Swap(const u32 var, const u32 address);
void Device_Write_U64_Swap(const u64 var, const u32 address);

}
