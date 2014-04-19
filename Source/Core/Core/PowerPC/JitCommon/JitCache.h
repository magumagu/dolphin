// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <bitset>
#include <map>
#include <memory>
#include <vector>

#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCAnalyst.h"

// Define this in order to get VTune profile support for the Jit generated code.
// Add the VTune include/lib directories to the project directories to get this to build.
// #define USE_VTUNE

struct JitBlock
{
	const u8 *checkedEntry;
	const u8 *normalEntry;

	u32 originalAddress;
	u32 codeSize;
	u32 originalSize;
	int runCount;  // for profiling.

	bool invalid;

	struct LinkData
	{
		u8 *exitPtrs;    // to be able to rewrite the exit jum
		u32 exitAddress;
		bool linkStatus; // is it already linked?
	};
	std::vector<LinkData> linkData;

	// we don't really need to save start and stop
	// TODO (mb2): ticStart and ticStop -> "local var" mean "in block" ... low priority ;)
	u64 ticStart;   // for profiling - time.
	u64 ticStop;    // for profiling - time.
	u64 ticCounter; // for profiling - time.
};

typedef void (*CompiledCode)();

// This is essentially just an std::bitset, but Visual Studia 2013's
// implementation of std::bitset is slow.
class ValidBlockBitSet final
{
	enum
	{
		VALID_BLOCK_MASK_SIZE = 0x100000000 / 32,
		VALID_BLOCK_ALLOC_ELEMENTS = VALID_BLOCK_MASK_SIZE / 32
	};
	std::unique_ptr<u32[]> m_valid_block;

public:
	ValidBlockBitSet()
	{
		m_valid_block.reset(new u32[VALID_BLOCK_ALLOC_ELEMENTS]);
		ClearAll();
	}

	void Set(u32 bit)
	{
		m_valid_block[bit / 32] |= 1u << (bit % 32);
	}

	void Clear(u32 bit)
	{
		m_valid_block[bit / 32] &= ~(1u << (bit % 32));
	}

	void ClearAll()
	{
		memset(m_valid_block.get(), 0, sizeof(u32) * VALID_BLOCK_ALLOC_ELEMENTS);
	}

	bool Test(u32 bit)
	{
		return (m_valid_block[bit / 32] & (1u << (bit % 32))) != 0;
	}
};

class JitBaseBlockCache
{
	enum
	{
		MAX_NUM_BLOCKS = 65536 * 2,
	};
	u32** m_phys_addrs;

	std::array<const u8*, MAX_NUM_BLOCKS> blockCodePointers;
	std::array<JitBlock, MAX_NUM_BLOCKS> blocks;
	int num_blocks;
	std::multimap<u32, int> links_to;
	std::map<std::pair<u32,u32>, u32> block_map; // (end_addr, start_addr) -> number
	ValidBlockBitSet valid_block;

	bool m_initialized;

	bool RangeIntersect(int s1, int e1, int s2, int e2) const;
	void LinkBlockExits(int i);
	void LinkBlock(int i);
	void UnlinkBlock(int i);

	u32* GetICachePtr(u32 addr);
	void DestroyBlock(int block_num, bool invalidate);

	// Virtual for overloaded
	virtual void WriteLinkBlock(u8* location, const u8* address) = 0;
	virtual void WriteDestroyBlock(const u8* location, u32 address) = 0;

public:
	JitBaseBlockCache() : num_blocks(0), m_initialized(false)
	{
	}

	int AllocateBlock(u32 em_address);
	void FinalizeBlock(int block_num, bool block_link, const u8 *code_ptr);

	void Clear();
	void Init();
	void Shutdown();
	void Reset();

	bool IsFull() const;

	// Code Cache
	JitBlock *GetBlock(int block_num);
	int GetNumBlocks() const;
	const u8 **GetCodePointers();

	u32 **GetBlockNumberCache()
	{
		return m_phys_addrs;
	}

	// Fast way to get a block. Only works on the first ppc instruction of a block.
	int GetBlockNumberFromStartAddress(u32 em_address);

	CompiledCode GetCompiledCodeFromBlock(int block_num);

	// DOES NOT WORK CORRECTLY WITH INLINING
	void InvalidateICache(u32 address, const u32 length, bool forced);
};

// x86 BlockCache
class JitBlockCache : public JitBaseBlockCache
{
private:
	void WriteLinkBlock(u8* location, const u8* address) override;
	void WriteDestroyBlock(const u8* location, u32 address) override;
};
