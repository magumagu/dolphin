// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.


// NOTE:
// These functions are primarily used by the interpreter versions of the LoadStore instructions.
// However, if a JITed instruction (for example lwz) wants to access a bad memory area that call
// may be redirected here (for example to Read_U32()).

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/MemArena.h"
#include "Common/MemoryUtil.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVDInterface.h"
#include "Core/HW/EXI.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/MemoryInterface.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/JitCommon/JitBase.h"

#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Memory
{

// =================================
// Init() declarations
// ----------------
// Store the MemArena here
u8* physical_base = nullptr;
u8* logical_base = nullptr;

// The MemArena class
static MemArena g_arena;
// ==============

// STATE_TO_SAVE
static bool m_IsInitialized = false; // Save the Init(), Shutdown() state
// END STATE_TO_SAVE

u8* m_pRAM;
u8* m_pL1Cache;
u8* m_pEXRAM;

// MMIO mapping object.
MMIO::Mapping* mmio_mapping;

static void InitMMIO(MMIO::Mapping* mmio)
{
	g_video_backend->RegisterCPMMIO(mmio, 0xCC000000);
	PixelEngine::RegisterMMIO(mmio, 0xCC001000);
	VideoInterface::RegisterMMIO(mmio, 0xCC002000);
	ProcessorInterface::RegisterMMIO(mmio, 0xCC003000);
	MemoryInterface::RegisterMMIO(mmio, 0xCC004000);
	DSP::RegisterMMIO(mmio, 0xCC005000);
	DVDInterface::RegisterMMIO(mmio, 0xCC006000);
	SerialInterface::RegisterMMIO(mmio, 0xCC006400);
	ExpansionInterface::RegisterMMIO(mmio, 0xCC006800);
	AudioInterface::RegisterMMIO(mmio, 0xCC006C00);
}

static void InitMMIOWii(MMIO::Mapping* mmio)
{
	InitMMIO(mmio);

	WII_IPCInterface::RegisterMMIO(mmio, 0xCD000000);
	DVDInterface::RegisterMMIO(mmio, 0xCD006000);
	SerialInterface::RegisterMMIO(mmio, 0xCD006400);
	ExpansionInterface::RegisterMMIO(mmio, 0xCD006800);
	AudioInterface::RegisterMMIO(mmio, 0xCD006C00);
}

bool IsInitialized()
{
	return m_IsInitialized;
}


// We don't declare the IO region in here since its handled by other means.
static MemoryView views[] =
{
	{&m_pRAM,      0x00000000, RAM_SIZE,      0},
	{nullptr,      0x280000000, RAM_SIZE,     MV_MIRROR_PREVIOUS},
	{nullptr,      0x2C0000000, RAM_SIZE,     MV_MIRROR_PREVIOUS},
	{&m_pL1Cache,  0xE0000000, L1_CACHE_SIZE, 0},
	{&m_pEXRAM,    0x10000000, EXRAM_SIZE,    MV_WII_ONLY},
	{nullptr,      0x290000000, EXRAM_SIZE,   MV_WII_ONLY | MV_MIRROR_PREVIOUS},
	{nullptr,      0x2D0000000, EXRAM_SIZE,   MV_WII_ONLY | MV_MIRROR_PREVIOUS},
};
static const int num_views = sizeof(views) / sizeof(MemoryView);

void Init()
{
	bool wii = SConfig::GetInstance().m_LocalCoreStartupParameter.bWii;

	u32 flags = 0;
	if (wii) flags |= MV_WII_ONLY;
	physical_base = MemoryMap_Setup(views, num_views, flags, &g_arena);
	logical_base = physical_base + 0x200000000;

	mmio_mapping = new MMIO::Mapping();

	if (wii)
		InitMMIOWii(mmio_mapping);
	else
		InitMMIO(mmio_mapping);

	INFO_LOG(MEMMAP, "Memory system initialized. RAM at %p", m_pRAM);
	m_IsInitialized = true;
}

void DoState(PointerWrap &p)
{
	bool wii = SConfig::GetInstance().m_LocalCoreStartupParameter.bWii;
	p.DoArray(m_pRAM, RAM_SIZE);
	p.DoArray(m_pL1Cache, L1_CACHE_SIZE);
	p.DoMarker("Memory RAM");
	if (wii)
		p.DoArray(m_pEXRAM, EXRAM_SIZE);
	p.DoMarker("Memory EXRAM");
}

void Shutdown()
{
	m_IsInitialized = false;
	u32 flags = 0;
	if (SConfig::GetInstance().m_LocalCoreStartupParameter.bWii) flags |= MV_WII_ONLY;
	MemoryMap_Shutdown(views, num_views, flags, &g_arena);
	g_arena.ReleaseSHMSegment();
	physical_base = nullptr;
	logical_base = nullptr;
	delete mmio_mapping;
	INFO_LOG(MEMMAP, "Memory system shut down.");
}

void Clear()
{
	if (m_pRAM)
		memset(m_pRAM, 0, RAM_SIZE);
	if (m_pL1Cache)
		memset(m_pL1Cache, 0, L1_CACHE_SIZE);
	if (SConfig::GetInstance().m_LocalCoreStartupParameter.bWii && m_pEXRAM)
		memset(m_pEXRAM, 0, EXRAM_SIZE);
}

bool AreMemoryBreakpointsActivated()
{
#ifndef ENABLE_MEM_CHECK
	return false;
#else
	return true;
#endif
}

u32 Debug_Read_Instruction(const u32 address)
{
	UGeckoInstruction inst = Debug_Read_U32(address);
	return inst.hex;
}

static inline bool Device_ValidCopyRange(u32 address, size_t size)
{
	return (Device_GetPointer(address) != nullptr &&
	        Device_GetPointer(address + u32(size)) != nullptr &&
	        size < EXRAM_SIZE); // Make sure we don't have a range spanning seperate 2 banks
}

void Device_CopyFromEmu(void* data, u32 address, size_t size)
{
	if (!Device_ValidCopyRange(address, size))
	{
		PanicAlert("Invalid range in CopyFromEmu. %lx bytes from 0x%08x", (unsigned long)size, address);
		return;
	}
	memcpy(data, Device_GetPointer(address), size);
}

void Device_CopyToEmu(u32 address, const void* data, size_t size)
{
	if (!Device_ValidCopyRange(address, size))
	{
		PanicAlert("Invalid range in CopyToEmu. %lx bytes to 0x%08x", (unsigned long)size, address);
		return;
	}
	memcpy(Device_GetPointer(address), data, size);
}

void Device_Memset(const u32 _Address, const u8 _iValue, const u32 _iLength)
{
	u8* ptr = Device_GetPointer(_Address);
	if (ptr != nullptr)
	{
		memset(ptr,_iValue,_iLength);
	}
}

void CPU_ClearCacheLine(const u32 address)
{
	// FIXME: does this do the right thing if dcbz is run on hardware memory, e.g.
	// the FIFO? Do games even do that? Probably not, but we should try to be correct...
	for (u32 i = 0; i < 32; i += 8)
		CPU_Write_U64(0, address + i);
}

void CPU_DMA_LCToMemory(const u32 memAddr, const u32 cacheAddr, const u32 numBlocks)
{
	const u8* src = m_pL1Cache + (cacheAddr & 0x3FFFF);
	u8* dst = Device_GetPointer(memAddr);

	if ((dst != nullptr) && (src != nullptr) && (memAddr & 3) == 0 && (cacheAddr & 3) == 0)
	{
		memcpy(dst, src, 32 * numBlocks);
	}
	else
	{
		for (u32 i = 0; i < 32 * numBlocks; i++)
		{
			u8 Temp = CPU_Read_U8(cacheAddr + i);
			CPU_Write_U8(Temp, memAddr + i);
		}
	}
}

void CPU_DMA_MemoryToLC(const u32 cacheAddr, const u32 memAddr, const u32 numBlocks)
{
	const u8* src = Device_GetPointer(memAddr);
	u8* dst = m_pL1Cache + (cacheAddr & 0x3FFFF);

	if ((dst != nullptr) && (src != nullptr) && (memAddr & 3) == 0 && (cacheAddr & 3) == 0)
	{
		memcpy(dst, src, 32 * numBlocks);
	}
	else
	{
		for (u32 i = 0; i < 32 * numBlocks; i++)
		{
			u8 Temp = CPU_Read_U8(memAddr + i);
			CPU_Write_U8(Temp, cacheAddr + i);
		}
	}
}

std::string Device_GetString(u32 em_address, size_t size)
{
	const char* ptr = reinterpret_cast<const char*>(Device_GetPointer(em_address));
	if (ptr == nullptr)
		return "";

	if (size == 0) // Null terminated string.
	{
		return std::string(ptr);
	}
	else // Fixed size string, potentially null terminated or null padded.
	{
		size_t length = strnlen(ptr, size);
		return std::string(ptr, length);
	}
}

u8* Device_GetPointer(u32 address)
{
	// TODO: Should we be masking off more bits here?  Can all devices access
	// EXRAM?
	address = address & 0x3FFFFFFF;
	if (address < REALRAM_SIZE)
		return m_pRAM + address;

	if (SConfig::GetInstance().m_LocalCoreStartupParameter.bWii)
	{
		if ((address >> 28) == 0x1 && (address & 0x0fffffff) < EXRAM_SIZE)
			return m_pEXRAM + (address & EXRAM_MASK);
	}

	PanicAlert("Unknown Pointer %#8x PC %#8x LR %#8x", address, PC, LR);
	ERROR_LOG(MEMMAP, "Unknown Pointer %#8x PC %#8x LR %#8x", address, PC, LR);

	return nullptr;
}

u8 Device_Read_U8(u32 address)
{
	return *Device_GetPointer(address);
}

u16 Device_Read_U16(u32 address)
{
	return Common::swap16(Device_GetPointer(address));
}

u32 Device_Read_U32(u32 address)
{
	return Common::swap32(Device_GetPointer(address));
}

u64 Device_Read_U64(u32 address)
{
	return Common::swap64(Device_GetPointer(address));
}

void Device_Write_U8(u8 value, u32 address)
{
	*Device_GetPointer(address) = value;
}

void Device_Write_U16(u16 value, u32 address)
{
	*(u16*)Device_GetPointer(address) = Common::swap16(value);
}

void Device_Write_U32(u32 value, u32 address)
{
	*(u32*)Device_GetPointer(address) = Common::swap32(value);
}

void Device_Write_U64(u64 value, u32 address)
{
	*(u64*)Device_GetPointer(address) = Common::swap64(value);
}

void Device_Write_U32_Swap(u32 value, u32 address)
{
	*(u32*)Device_GetPointer(address) = value;
}

void Device_Write_U64_Swap(u64 value, u32 address)
{
	*(u64*)Device_GetPointer(address) = value;
}

}  // namespace
