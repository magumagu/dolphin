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
// LOCAL SETTINGS
// ----------------

// Enable the Translation Lookaside Buffer functions.
bool bFakeVMEM = false;
static bool bMMU = false;
// ==============


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
u8* m_pFakeVMEM;

// MMIO mapping object.
MMIO::Mapping* mmio_mapping;

static void InitMMIO(MMIO::Mapping* mmio)
{
	g_video_backend->RegisterCPMMIO(mmio, 0x0C000000);
	PixelEngine::RegisterMMIO(mmio, 0x0C001000);
	VideoInterface::RegisterMMIO(mmio, 0x0C002000);
	ProcessorInterface::RegisterMMIO(mmio, 0x0C003000);
	MemoryInterface::RegisterMMIO(mmio, 0x0C004000);
	DSP::RegisterMMIO(mmio, 0x0C005000);
	DVDInterface::RegisterMMIO(mmio, 0x0C006000);
	SerialInterface::RegisterMMIO(mmio, 0x0C006400);
	ExpansionInterface::RegisterMMIO(mmio, 0x0C006800);
	AudioInterface::RegisterMMIO(mmio, 0x0C006C00);
}

static void InitMMIOWii(MMIO::Mapping* mmio)
{
	InitMMIO(mmio);

	WII_IPCInterface::RegisterMMIO(mmio, 0x0D000000);
	DVDInterface::RegisterMMIO(mmio, 0x0D006000);
	SerialInterface::RegisterMMIO(mmio, 0x0D006400);
	ExpansionInterface::RegisterMMIO(mmio, 0x0D006800);
	AudioInterface::RegisterMMIO(mmio, 0x0D006C00);
}

bool IsInitialized()
{
	return m_IsInitialized;
}

namespace
{
	enum
	{
		MV_FAKE_VMEM = 1,
		MV_WII_ONLY = 2,
	};

	struct PhysicalMemoryRegion
	{
		u8** out_pointer;
		u32 physical_address;
		u32 size;
		u32 flags;
		u32 shm_position;
	};

	struct LogicalMemoryView
	{
		void* mapped_pointer;
		u32 mapped_size;
	};
}

// Dolphin allocates memory to represent four regions:
// - 32MB RAM (actually 24MB on hardware), available on Gamecube and Wii
// - 64MB "EXRAM", RAM only available on Wii
// - 32MB FakeVMem, allocated when MMU support is turned off. This is used
//   to approximate the behavior of a common library which pages memory to
//   and from the DSP's dedicated RAM, which isn't directly addressable on
//   GameCube.
// - 256KB Locked L1, to represent cache lines allocated out of the L1 data
//   cache in Locked L1 mode.  Dolphin does not emulate this hardware feature
//   accurately; it just pretends there is extra memory at 0xE0000000.
//
// The 4GB starting at physical_base represents access from the CPU
// with address translation turned off. (This is only used by the CPU;
// other devices, like the GPU, use other rules, approximated by
// Memory::GetPointer.) This memory is laid out as follows:
// [0x00000000, 0x01800000) - 24MB RAM
// [0x08000000, 0x0C000000) - EFB "mapping" (not handled here)
// [0x0C000000, 0x0E000000) - MMIO etc. (not handled here)
// [0x10000000, 0x14000000) - 64MB RAM (Wii-only; slightly slower)
// [0x7E000000, 0x80000000) - FakeVMEM
// [0xE0000000, 0xE0040000) - 256KB locked L1
//
// The 4GB starting at logical_base represents access from the CPU
// with address translation turned on.  This mapping is computed based
// on the BAT registers.
//
// Each of these 4GB regions is followed by 4GB of empty space so overflows
// in address computation in the JIT don't access the wrong memory.
//
// Dolphin doesn't emulate the difference between cached and uncached access.
static PhysicalMemoryRegion physical_regions[] =
{
	{&m_pRAM,      0x00000000, REALRAM_SIZE,  0},
	{&m_pL1Cache,  0xE0000000, L1_CACHE_SIZE, 0},
	{&m_pFakeVMEM, 0x7E000000, FAKEVMEM_SIZE, MV_FAKE_VMEM},
	{&m_pEXRAM,    0x10000000, EXRAM_SIZE,    MV_WII_ONLY},
};

static std::vector<LogicalMemoryView> logical_mapped_entries;

void Init()
{
	bool wii = SConfig::GetInstance().m_LocalCoreStartupParameter.bWii;
	bMMU = SConfig::GetInstance().m_LocalCoreStartupParameter.bMMU;
#ifndef _ARCH_32
	// The fake VMEM hack's address space is above the memory space that we allocate on 32bit targets
	// Disable it entirely on 32bit targets.
	bFakeVMEM = !bMMU;
#endif

	u32 flags = 0;
	if (wii) flags |= MV_WII_ONLY;
	if (bFakeVMEM) flags |= MV_FAKE_VMEM;
	u32 mem_size = 0;
	for (PhysicalMemoryRegion &region : physical_regions)
	{
		if ((flags & region.flags) != region.flags)
			continue;
		region.shm_position = mem_size;
		mem_size += region.size;
	}
	g_arena.GrabSHMSegment(mem_size);
	physical_base = MemArena::FindMemoryBase();

	for (PhysicalMemoryRegion &region : physical_regions)
	{
		if ((flags & region.flags) != region.flags)
			continue;

		u8* base = physical_base + region.physical_address;
		*region.out_pointer = (u8*)g_arena.CreateView(region.shm_position, region.size, base);

		if (!*region.out_pointer)
		{
			PanicAlert("MemoryMap_Setup: Failed finding a memory base.");
			exit(0);
		}

		mem_size += region.size;
	}

#ifndef _ARCH_32
	logical_base = physical_base + 0x200000000;
#endif

	mmio_mapping = new MMIO::Mapping();

	if (wii)
		InitMMIOWii(mmio_mapping);
	else
		InitMMIO(mmio_mapping);

	INFO_LOG(MEMMAP, "Memory system initialized. RAM at %p", m_pRAM);
	m_IsInitialized = true;
}

void UpdateLogicalMemory(u32* dbat_table)
{
	for (auto &entry : logical_mapped_entries)
	{
		g_arena.ReleaseView(entry.mapped_pointer, entry.mapped_size);
	}
	logical_mapped_entries.clear();
	for (unsigned i = 0; i < (1 << (32 - PowerPC::BAT_INDEX_SHIFT)); ++i)
	{
		if (dbat_table[i] & 1)
		{
			unsigned logical_address = i << PowerPC::BAT_INDEX_SHIFT;
			// TODO: Merge adjacent mappings to make this faster.
			unsigned logical_size = 1 << PowerPC::BAT_INDEX_SHIFT;
			unsigned translated_address = dbat_table[i] & ~1;
			for (PhysicalMemoryRegion &physical_region : physical_regions)
			{
				u32 mapping_address = physical_region.physical_address;
				u32 mapping_end = mapping_address + physical_region.size;
				u32 intersection_start = std::max(mapping_address, translated_address);
				u32 intersection_end = std::min(mapping_end, translated_address + logical_size);
				if (intersection_start < intersection_end)
				{
					// Found an overlapping region; map it.
					// We only worry about one overlapping region; in theory, a logical
					// region could translate to more than one physical region, but in
					// practice, that doesn't happen.
					u32 position = physical_region.shm_position;
					if (intersection_start > mapping_address)
						position += intersection_start - mapping_address;
					u8* base = logical_base + logical_address;
					if (intersection_start > translated_address)
						base += intersection_start - translated_address;
					u32 mapped_size = intersection_end - intersection_start;

					void* mapped_pointer = g_arena.CreateView(position, mapped_size, base);
					if (!mapped_pointer)
					{
						PanicAlert("MemoryMap_Setup: Failed finding a memory base.");
						exit(0);
					}
					logical_mapped_entries.push_back({ mapped_pointer, mapped_size });
					break;
				}
			}
		}
	}
}

void DoState(PointerWrap &p)
{
	bool wii = SConfig::GetInstance().m_LocalCoreStartupParameter.bWii;
	p.DoArray(m_pRAM, REALRAM_SIZE);
	p.DoArray(m_pL1Cache, L1_CACHE_SIZE);
	p.DoMarker("Memory RAM");
	if (bFakeVMEM)
		p.DoArray(m_pFakeVMEM, FAKEVMEM_SIZE);
	p.DoMarker("Memory FakeVMEM");
	if (wii)
		p.DoArray(m_pEXRAM, EXRAM_SIZE);
	p.DoMarker("Memory EXRAM");
}

void Shutdown()
{
	m_IsInitialized = false;
	u32 flags = 0;
	if (SConfig::GetInstance().m_LocalCoreStartupParameter.bWii) flags |= MV_WII_ONLY;
	if (bFakeVMEM) flags |= MV_FAKE_VMEM;
	for (PhysicalMemoryRegion &region : physical_regions)
	{
		if ((flags & region.flags) != region.flags)
			continue;
		g_arena.ReleaseView(*region.out_pointer, region.size);
		*region.out_pointer = 0;
	}
	for (auto &entry : logical_mapped_entries)
	{
		g_arena.ReleaseView(entry.mapped_pointer, entry.mapped_size);
	}
	logical_mapped_entries.clear();
	g_arena.ReleaseSHMSegment();
	physical_base = nullptr;
	logical_base = nullptr;
	delete mmio_mapping;
	INFO_LOG(MEMMAP, "Memory system shut down.");
}

void Clear()
{
	if (m_pRAM)
		memset(m_pRAM, 0, REALRAM_SIZE);
	if (m_pL1Cache)
		memset(m_pL1Cache, 0, L1_CACHE_SIZE);
	if (m_pFakeVMEM)
		memset(m_pFakeVMEM, 0, FAKEVMEM_SIZE);
	if (m_pEXRAM)
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

static inline bool ValidCopyRange(u32 address, size_t size)
{
	return (GetPointer(address) != nullptr &&
	        GetPointer(address + u32(size)) != nullptr &&
	        size < EXRAM_SIZE); // Make sure we don't have a range spanning seperate 2 banks
}

void CopyFromEmu(void* data, u32 address, size_t size)
{
	if (!ValidCopyRange(address, size))
	{
		PanicAlert("Invalid range in CopyFromEmu. %lx bytes from 0x%08x", (unsigned long)size, address);
		return;
	}
	memcpy(data, GetPointer(address), size);
}

void CopyToEmu(u32 address, const void* data, size_t size)
{
	if (!ValidCopyRange(address, size))
	{
		PanicAlert("Invalid range in CopyToEmu. %lx bytes to 0x%08x", (unsigned long)size, address);
		return;
	}
	memcpy(GetPointer(address), data, size);
}

void Memset(const u32 _Address, const u8 _iValue, const u32 _iLength)
{
	u8* ptr = GetPointer(_Address);
	if (ptr != nullptr)
	{
		memset(ptr,_iValue,_iLength);
	}
}

std::string GetString(u32 em_address, size_t size)
{
	const char* ptr = reinterpret_cast<const char*>(GetPointer(em_address));
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

u8* GetPointer(u32 address)
{
	// TODO: Should we be masking off more bits here?  Can all devices access
	// EXRAM?
	address &= 0x3FFFFFFF;
	if (address < REALRAM_SIZE)
		return m_pRAM + address;

	if (SConfig::GetInstance().m_LocalCoreStartupParameter.bWii)
	{
		if ((address >> 28) == 0x1 && (address & 0x0fffffff) < EXRAM_SIZE)
			return m_pEXRAM + (address & EXRAM_MASK);
	}

	PanicAlert("Unknown Pointer 0x%08x PC 0x%08x LR 0x%08x", address, PC, LR);

	return nullptr;
}

u8 Read_U8(u32 address)
{
	return *GetPointer(address);
}

u16 Read_U16(u32 address)
{
	return Common::swap16(GetPointer(address));
}

u32 Read_U32(u32 address)
{
	return Common::swap32(GetPointer(address));
}

u64 Read_U64(u32 address)
{
	return Common::swap64(GetPointer(address));
}

void Write_U8(u8 value, u32 address)
{
	*GetPointer(address) = value;
}

void Write_U16(u16 value, u32 address)
{
	*(u16*)GetPointer(address) = Common::swap16(value);
}

void Write_U32(u32 value, u32 address)
{
	*(u32*)GetPointer(address) = Common::swap32(value);
}

void Write_U64(u64 value, u32 address)
{
	*(u64*)GetPointer(address) = Common::swap64(value);
}

void Write_U32_Swap(u32 value, u32 address)
{
	*(u32*)GetPointer(address) = value;
}

void Write_U64_Swap(u64 value, u32 address)
{
	*(u64*)GetPointer(address) = value;
}

}  // namespace
