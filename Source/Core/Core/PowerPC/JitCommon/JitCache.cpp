// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

// Enable define below to enable oprofile integration. For this to work,
// it requires at least oprofile version 0.9.4, and changing the build
// system to link the Dolphin executable against libopagent.  Since the
// dependency is a little inconvenient and this is possibly a slight
// performance hit, it's not enabled by default, but it's useful for
// locating performance issues.

#include "disasm.h"

#include "Common/Common.h"
#include "Common/MemoryUtil.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/JitCommon/JitBase.h"

#ifdef _WIN32
#include <windows.h>
#endif

#if defined USE_OPROFILE && USE_OPROFILE
#include <opagent.h>

op_agent_t agent;
#endif

#if defined USE_VTUNE
#include <jitprofiling.h>
#pragma comment(lib, "libittnotify.lib")
#pragma comment(lib, "jitprofiling.lib")
#endif

using namespace Gen;

	bool JitBaseBlockCache::IsFull() const
	{
		return GetNumBlocks() >= MAX_NUM_BLOCKS - 1;
	}

	void JitBaseBlockCache::Init()
	{
#if defined USE_OPROFILE && USE_OPROFILE
		agent = op_open_agent();
#endif
		blocks = new JitBlock[MAX_NUM_BLOCKS];
		blockCodePointers = new const u8*[MAX_NUM_BLOCKS];
		m_phys_addrs = new u32*[1 << 18];
		memset(m_phys_addrs, 0, sizeof(u32*) * (1 << 18));
		Clear();
	}

	void JitBaseBlockCache::Shutdown()
	{
		delete[] blocks;
		delete[] blockCodePointers;
		blocks = nullptr;
		blockCodePointers = nullptr;
		num_blocks = 0;

		for (unsigned i = 0; i < (1 << 18); ++i)
		{
			if (m_phys_addrs[i])
			{
				delete[] m_phys_addrs[i];
			}
		}
		delete[] m_phys_addrs;
		m_phys_addrs = nullptr;

#if defined USE_OPROFILE && USE_OPROFILE
		op_close_agent(agent);
#endif

#ifdef USE_VTUNE
		iJIT_NotifyEvent(iJVM_EVENT_TYPE_SHUTDOWN, nullptr);
#endif
	}

	// This clears the JIT cache. It's called from JitCache.cpp when the JIT cache
	// is full and when saving and loading states.
	void JitBaseBlockCache::Clear()
	{
#if defined(_DEBUG) || defined(DEBUGFAST)
		if (IsFull())
			Core::DisplayMessage("Clearing block cache.", 3000);
		else
			Core::DisplayMessage("Clearing code cache.", 3000);
#endif

		for (int i = 0; i < num_blocks; i++)
		{
			DestroyBlock(i, false);
		}
		links_to.clear();
		block_map.clear();

		valid_block.clear();
		valid_block.resize(VALID_BLOCK_MASK_SIZE, false);

		num_blocks = 0;

		for (unsigned i = 0; i < (1 << 18); ++i)
		{
			if (m_phys_addrs[i])
			{
				delete[] m_phys_addrs[i];
				m_phys_addrs[i] = 0;
			}
		}
		memset(blockCodePointers, 0, sizeof(u8*)*MAX_NUM_BLOCKS);
	}

	void JitBaseBlockCache::Reset()
	{
		Shutdown();
		Init();
	}

	JitBlock *JitBaseBlockCache::GetBlock(int no)
	{
		return &blocks[no];
	}

	int JitBaseBlockCache::GetNumBlocks() const
	{
		return num_blocks;
	}

	bool JitBaseBlockCache::RangeIntersect(int s1, int e1, int s2, int e2) const
	{
		// check if any endpoint is inside the other range
		if ((s1 >= s2 && s1 <= e2) ||
			(e1 >= s2 && e1 <= e2) ||
			(s2 >= s1 && s2 <= e1) ||
			(e2 >= s1 && e2 <= e1))
			return true;
		else
			return false;
	}

	int JitBaseBlockCache::AllocateBlock(u32 em_address)
	{
		JitBlock &b = blocks[num_blocks];
		b.invalid = false;
		b.originalAddress = em_address;
		b.linkData.clear();
		num_blocks++; //commit the current block
		return num_blocks - 1;
	}

	void JitBaseBlockCache::FinalizeBlock(int block_num, bool block_link, const u8 *code_ptr)
	{
		blockCodePointers[block_num] = code_ptr;
		JitBlock &b = blocks[block_num];

		for (u32 i = 0; i < (b.originalSize + 7) / 8; ++i)
			valid_block[b.originalAddress / 32 + i] = true;

		block_map[std::make_pair(b.originalAddress + 4 * b.originalSize - 1, b.originalAddress)] = block_num;
		if (!m_phys_addrs[b.originalAddress >> 14])
		{
			m_phys_addrs[b.originalAddress >> 14] = new u32[1 << 12];
			memset(m_phys_addrs[b.originalAddress >> 14], 255, 4 * (1 << 12));
		}
		m_phys_addrs[b.originalAddress >> 14][(b.originalAddress >> 2) & ((1 << 12) - 1)] = block_num;

		if (block_link)
		{
			for (const auto& e : b.linkData)
			{
				links_to.insert(std::pair<u32, int>(e.exitAddress, block_num));
			}

			LinkBlock(block_num);
			LinkBlockExits(block_num);
		}

#if defined USE_OPROFILE && USE_OPROFILE
		char buf[100];
		sprintf(buf, "EmuCode%x", b.originalAddress);
		const u8* blockStart = blockCodePointers[block_num];
		op_write_native_code(agent, buf, (uint64_t)blockStart,
		                     blockStart, b.codeSize);
#endif

#ifdef USE_VTUNE
		sprintf(b.blockName, "EmuCode_0x%08x", b.originalAddress);

		iJIT_Method_Load jmethod = {0};
		jmethod.method_id = iJIT_GetNewMethodID();
		jmethod.class_file_name = "";
		jmethod.source_file_name = __FILE__;
		jmethod.method_load_address = (void*)blockCodePointers[block_num];
		jmethod.method_size = b.codeSize;
		jmethod.line_number_size = 0;
		jmethod.method_name = b.blockName;
		iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)&jmethod);
#endif
	}

	const u8 **JitBaseBlockCache::GetCodePointers()
	{
		return blockCodePointers;
	}

	int JitBaseBlockCache::GetBlockNumberFromStartAddress_static(JitBaseBlockCache *cache, u32 addr)
	{
		u32* page = cache->m_phys_addrs[addr >> (12 + 2)];
		if (!page)
		{
			return -1;
		}
		return page[(addr >> 2) & ((1 << 12) - 1)];
	}

	CompiledCode JitBaseBlockCache::GetCompiledCodeFromBlock(int block_num)
	{
		return (CompiledCode)blockCodePointers[block_num];
	}

	//Block linker
	//Make sure to have as many blocks as possible compiled before calling this
	//It's O(N), so it's fast :)
	//Can be faster by doing a queue for blocks to link up, and only process those
	//Should probably be done

	void JitBaseBlockCache::LinkBlockExits(int i)
	{
		JitBlock &b = blocks[i];
		if (b.invalid)
		{
			// This block is dead. Don't relink it.
			return;
		}
		for (auto& e : b.linkData)
		{
			if (!e.linkStatus)
			{
				int destinationBlock = GetBlockNumberFromStartAddress(e.exitAddress);
				if (destinationBlock != -1)
				{
					WriteLinkBlock(e.exitPtrs, blocks[destinationBlock].checkedEntry);
					e.linkStatus = true;
				}
			}
		}
	}

	using namespace std;

	void JitBaseBlockCache::LinkBlock(int i)
	{
		LinkBlockExits(i);
		JitBlock &b = blocks[i];
		pair<multimap<u32, int>::iterator, multimap<u32, int>::iterator> ppp;
		// equal_range(b) returns pair<iterator,iterator> representing the range
		// of element with key b
		ppp = links_to.equal_range(b.originalAddress);
		if (ppp.first == ppp.second)
			return;
		for (multimap<u32, int>::iterator iter = ppp.first; iter != ppp.second; ++iter) {
			// PanicAlert("Linking block %i to block %i", iter->second, i);
			LinkBlockExits(iter->second);
		}
	}

	void JitBaseBlockCache::UnlinkBlock(int i)
	{
		JitBlock &b = blocks[i];
		pair<multimap<u32, int>::iterator, multimap<u32, int>::iterator> ppp;
		ppp = links_to.equal_range(b.originalAddress);
		if (ppp.first == ppp.second)
			return;
		for (multimap<u32, int>::iterator iter = ppp.first; iter != ppp.second; ++iter) {
			JitBlock &sourceBlock = blocks[iter->second];
			for (auto& e : sourceBlock.linkData)
			{
				if (e.exitAddress == b.originalAddress)
					e.linkStatus = false;
			}
		}
		links_to.erase(b.originalAddress);
	}

	void JitBaseBlockCache::DestroyBlock(int block_num, bool invalidate)
	{
		if (block_num < 0 || block_num >= num_blocks)
		{
			PanicAlert("DestroyBlock: Invalid block number %d", block_num);
			return;
		}
		JitBlock &b = blocks[block_num];
		if (b.invalid)
		{
			if (invalidate)
				PanicAlert("Invalidating invalid block %d", block_num);
			return;
		}
		b.invalid = true;

		UnlinkBlock(block_num);

		m_phys_addrs[b.originalAddress >> 14][(b.originalAddress >> 2) & ((1 << 12) - 1)] = block_num;

		// Send anyone who tries to run this block back to the dispatcher.
		// Not entirely ideal, but .. pretty good.
		// Spurious entrances from previously linked blocks can only come through checkedEntry
		WriteDestroyBlock(b.checkedEntry, b.originalAddress);
	}

	void JitBaseBlockCache::InvalidateICache(u32 address, const u32 length)
	{
		// Optimize the common case of length == 32 which is used by Interpreter::dcb*
		bool destroy_block = true;
		if (length == 32)
		{
			if (!valid_block[address / 32])
				destroy_block = false;
			else
				valid_block[address / 32] = false;
		}

		// destroy JIT blocks
		// !! this works correctly under assumption that any two overlapping blocks end at the same address
		if (destroy_block)
		{
			std::map<pair<u32, u32>, u32>::iterator it1 = block_map.lower_bound(std::make_pair(address, 0)), it2 = it1;
			while (it2 != block_map.end() && it2->first.second < address + length)
			{
				JitBlock &b = blocks[it2->second];
				DestroyBlock(it2->second, true);
				++it2;
			}
			if (it1 != it2)
			{
				block_map.erase(it1, it2);
			}
		}
	}
	void JitBlockCache::WriteLinkBlock(u8* location, const u8* address)
	{
		XEmitter emit(location);
		emit.JMP(address, true);
	}
	void JitBlockCache::WriteDestroyBlock(const u8* location, u32 address)
	{
		XEmitter emit((u8 *)location);
		emit.MOV(32, M(&PC), Imm32(address));
		emit.JMP(jit->GetAsmRoutines()->dispatcher, true);
	}
