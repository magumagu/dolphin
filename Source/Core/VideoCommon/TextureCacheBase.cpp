// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <algorithm>
#include <string>

#include "Common/FileUtil.h"
#include "Common/MemoryUtil.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"

#include "VideoCommon/Debugger.h"
#include "VideoCommon/FramebufferManagerBase.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"

static const u64 TEXHASH_INVALID = 0;
static const int TEXTURE_KILL_THRESHOLD = 200;
static const int TEXTURE_POOL_KILL_THRESHOLD = 3;
static const u64 FRAMECOUNT_INVALID = 0;

TextureCache *g_texture_cache;

GC_ALIGNED16(u8 *TextureCache::temp) = nullptr;
size_t TextureCache::temp_size;

TextureCache::TexCache TextureCache::textures;
TextureCache::TexPool TextureCache::texture_pool;

TextureCache::BackupConfig TextureCache::backup_config;

static bool invalidate_texture_cache_requested;

TextureCache::TCacheEntryBase::~TCacheEntryBase()
{
}

void TextureCache::CheckTempSize(size_t required_size)
{
	if (required_size <= temp_size)
		return;

	temp_size = required_size;
	FreeAlignedMemory(temp);
	temp = (u8*)AllocateAlignedMemory(temp_size, 16);
}

TextureCache::TextureCache()
{
	temp_size = 2048 * 2048 * 4;
	if (!temp)
		temp = (u8*)AllocateAlignedMemory(temp_size, 16);

	TexDecoder_SetTexFmtOverlayOptions(g_ActiveConfig.bTexFmtOverlayEnable, g_ActiveConfig.bTexFmtOverlayCenter);

	if (g_ActiveConfig.bHiresTextures && !g_ActiveConfig.bDumpTextures)
		HiresTexture::Init(SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID);

	SetHash64Function();

	invalidate_texture_cache_requested = false;
}

void TextureCache::RequestInvalidateTextureCache()
{
	invalidate_texture_cache_requested = true;
}

void TextureCache::Invalidate()
{
	for (auto& tex : textures)
	{
		delete tex.second;
	}
	textures.clear();

	for (auto& rt : texture_pool)
	{
		delete rt.second;
	}
	texture_pool.clear();
}

TextureCache::~TextureCache()
{
	Invalidate();
	FreeAlignedMemory(temp);
	temp = nullptr;
}

void TextureCache::OnConfigChanged(VideoConfig& config)
{
	if (g_texture_cache)
	{
		// TODO: Invalidating texcache is really stupid in some of these cases
		if (config.iSafeTextureCache_ColorSamples != backup_config.s_colorsamples ||
			config.bTexFmtOverlayEnable != backup_config.s_texfmt_overlay ||
			config.bTexFmtOverlayCenter != backup_config.s_texfmt_overlay_center ||
			config.bHiresTextures != backup_config.s_hires_textures ||
			invalidate_texture_cache_requested)
		{
			g_texture_cache->Invalidate();

			if (g_ActiveConfig.bHiresTextures)
				HiresTexture::Init(SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID);

			TexDecoder_SetTexFmtOverlayOptions(g_ActiveConfig.bTexFmtOverlayEnable, g_ActiveConfig.bTexFmtOverlayCenter);

			invalidate_texture_cache_requested = false;
		}

		if ((config.iStereoMode > 0) != backup_config.s_stereo_3d ||
			config.bStereoEFBMonoDepth != backup_config.s_efb_mono_depth)
		{
			g_texture_cache->DeleteShaders();
			g_texture_cache->CompileShaders();
		}
	}

	backup_config.s_colorsamples = config.iSafeTextureCache_ColorSamples;
	backup_config.s_texfmt_overlay = config.bTexFmtOverlayEnable;
	backup_config.s_texfmt_overlay_center = config.bTexFmtOverlayCenter;
	backup_config.s_hires_textures = config.bHiresTextures;
	backup_config.s_stereo_3d = config.iStereoMode > 0;
	backup_config.s_efb_mono_depth = config.bStereoEFBMonoDepth;
}

void TextureCache::Cleanup(int _frameCount)
{
	TexCache::iterator iter = textures.begin();
	TexCache::iterator tcend = textures.end();
	while (iter != tcend)
	{
		if(iter->second->frameCount == FRAMECOUNT_INVALID)
		{
			iter->second->frameCount = _frameCount;
		}
		if (_frameCount > TEXTURE_KILL_THRESHOLD + iter->second->frameCount &&
		    // EFB copies living on the host GPU are unrecoverable and thus shouldn't be deleted
		    !iter->second->IsEfbCopy())
		{
			FreeTexture(iter->second);
			iter = textures.erase(iter);
		}
		else
		{
			++iter;
		}
	}

	TexPool::iterator iter2 = texture_pool.begin();
	TexPool::iterator tcend2 = texture_pool.end();
	while (iter2 != tcend2)
	{
		if(iter2->second->frameCount == FRAMECOUNT_INVALID)
		{
			iter2->second->frameCount = _frameCount;
		}
		if (_frameCount > TEXTURE_POOL_KILL_THRESHOLD + iter2->second->frameCount)
		{
			delete iter2->second;
			iter2 = texture_pool.erase(iter2);
		}
		else
		{
			++iter2;
		}
	}
}

void TextureCache::InvalidateRange(u32 start_address, u32 size)
{
	TexCache::iterator
		iter = textures.begin(),
		tcend = textures.end();
	while (iter != tcend)
	{
		if (iter->second->OverlapsMemoryRange(start_address, size))
		{
			FreeTexture(iter->second);
			textures.erase(iter++);
		}
		else
		{
			++iter;
		}
	}
}

void TextureCache::MakeRangeDynamic(u32 start_address, u32 size)
{
	TexCache::iterator
		iter = textures.lower_bound(start_address),
		tcend = textures.upper_bound(start_address + size);

	if (iter != textures.begin())
		--iter;

	for (; iter != tcend; ++iter)
	{
		if (iter->second->OverlapsMemoryRange(start_address, size))
		{
			iter->second->SetHashes(TEXHASH_INVALID);
		}
	}
}

void TextureCache::WriteRangeToMemory(u32 start_address, u32 size)
{
	TexCache::iterator
		iter = textures.lower_bound(start_address),
		tcend = textures.upper_bound(start_address + size);

	if (iter != textures.begin())
		--iter;

	for (; iter != tcend; ++iter)
	{
		if (iter->second->OverlapsMemoryRange(start_address, size))
		{
			TCacheEntryBase*& entry = iter->second;
			if (entry->IsEfbCopy() && !entry->hash)
			{
				u8* dst = Memory::GetPointer(entry->addr);
				entry->EncodeToMemory(dst, entry->format, PEControl::RGB8_Z24, EFBRectangle(0, 0, entry->config.width, entry->config.height), false, false);
				entry->hash = GetHash64(dst, entry->size_in_bytes, g_ActiveConfig.iSafeTextureCache_ColorSamples);
			}
		}
	}
}

bool TextureCache::Find(u32 start_address, u64 hash)
{
	TexCache::iterator iter = textures.lower_bound(start_address);

	if (iter->second->hash == hash)
		return true;

	return false;
}

bool TextureCache::TCacheEntryBase::OverlapsMemoryRange(u32 range_address, u32 range_size) const
{
	if (addr + size_in_bytes <= range_address)
		return false;

	if (addr >= range_address + range_size)
		return false;

	return true;
}

void TextureCache::ClearRenderTargets()
{
	TexCache::iterator
		iter = textures.begin(),
		tcend = textures.end();

	while (iter != tcend)
	{
		if (iter->second->type == TCET_EC_VRAM)
		{
			FreeTexture(iter->second);
			textures.erase(iter++);
		}
		else
		{
			++iter;
		}
	}
}

void TextureCache::DumpTexture(TCacheEntryBase* entry, std::string basename, unsigned int level)
{
	std::string szDir = File::GetUserPath(D_DUMPTEXTURES_IDX) +
		SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID;

	// make sure that the directory exists
	if (!File::Exists(szDir) || !File::IsDirectory(szDir))
		File::CreateDir(szDir);

	if (level > 0)
	{
		basename += StringFromFormat("_mip%i", level);
	}
	std::string filename = szDir + "/" + basename + ".png";

	if (!File::Exists(filename))
		entry->Save(filename, level);
}

static u32 CalculateLevelSize(u32 level_0_size, u32 level)
{
	return (level_0_size + ((1 << level) - 1)) >> level;
}

// Used by TextureCache::Load
static TextureCache::TCacheEntryBase* ReturnEntry(unsigned int stage, TextureCache::TCacheEntryBase* entry)
{
	entry->frameCount = FRAMECOUNT_INVALID;
	entry->Bind(stage);

	GFX_DEBUGGER_PAUSE_AT(NEXT_TEXTURE_CHANGE, true);

	return entry;
}

TextureCache::TCacheEntryBase* TextureCache::Load(const u32 stage)
{
	const FourTexUnits &tex = bpmem.tex[stage >> 2];
	const u32 id = stage & 3;
	const u32 address = (tex.texImage3[id].image_base/* & 0x1FFFFF*/) << 5;
	u32 width = tex.texImage0[id].width + 1;
	u32 height = tex.texImage0[id].height + 1;
	const int texformat = tex.texImage0[id].format;
	const u32 tlutaddr = tex.texTlut[id].tmem_offset << 9;
	const u32 tlutfmt = tex.texTlut[id].tlut_format;
	const bool use_mipmaps = (tex.texMode0[id].min_filter & 3) != 0;
	u32 tex_levels = (tex.texMode1[id].max_lod + 0xf) / 0x10 + 1;
	const bool from_tmem = tex.texImage1[id].image_type != 0;

	if (0 == address)
		return nullptr;

	// TexelSizeInNibbles(format) * width * height / 16;
	const unsigned int bsw = TexDecoder_GetBlockWidthInTexels(texformat) - 1;
	const unsigned int bsh = TexDecoder_GetBlockHeightInTexels(texformat) - 1;

	unsigned int expandedWidth  = (width  + bsw) & (~bsw);
	unsigned int expandedHeight = (height + bsh) & (~bsh);
	const unsigned int nativeW = width;
	const unsigned int nativeH = height;

	u32 texID = address;
	// Hash assigned to texcache entry (also used to generate filenames used for texture dumping and custom texture lookup)
	u64 tex_hash = TEXHASH_INVALID;
	u64 tlut_hash = TEXHASH_INVALID;

	u32 full_format = texformat;

	const bool isPaletteTexture = (texformat == GX_TF_C4 || texformat == GX_TF_C8 || texformat == GX_TF_C14X2);
	if (isPaletteTexture)
		full_format = texformat | (tlutfmt << 16);

	const u32 texture_size = TexDecoder_GetTextureSizeInBytes(expandedWidth, expandedHeight, texformat);

	const u8* src_data;
	if (from_tmem)
		src_data = &texMem[bpmem.tex[stage / 4].texImage1[stage % 4].tmem_even * TMEM_LINE_SIZE];
	else
		src_data = Memory::GetPointer(address);

	// TODO: This doesn't hash GB tiles for preloaded RGBA8 textures (instead, it's hashing more data from the low tmem bank than it should)
	tex_hash = GetHash64(src_data, texture_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);
	u32 palette_size = 0;
	if (isPaletteTexture)
	{
		palette_size = TexDecoder_GetPaletteSize(texformat);
		tlut_hash = GetHash64(&texMem[tlutaddr], palette_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);

		// NOTE: For non-paletted textures, texID is equal to the texture address.
		//       A paletted texture, however, may have multiple texIDs assigned though depending on the currently used tlut.
		//       This (changing texID depending on the tlut_hash) is a trick to get around
		//       an issue with Metroid Prime's fonts (it has multiple sets of fonts on each other
		//       stored in a single texture and uses the palette to make different characters
		//       visible or invisible. Thus, unless we want to recreate the textures for every drawn character,
		//       we must make sure that a paletted texture gets assigned multiple IDs for each tlut used.
		//
		// TODO: Because texID isn't always the same as the address now, CopyRenderTargetToTexture might be broken now
		u32 temp_texID = texID;
		texID ^= ((u32)tlut_hash) ^(u32)(tlut_hash >> 32);
		tex_hash ^= tlut_hash;

		// Don't change the texID depending on the tlut_hash for paletted textures that are efb copies and don't have
		// an entry in the cache for texID ^ tlut_hash. This makes those textures less broken when using efb to texture.
		// Examples are the mini map in Twilight Princess and objects on the targetting computer in Rogue Squadron 2(RS2).
		// TODO: Convert those textures using the right palette, so they display correctly
		auto iter = textures.find(temp_texID);
		if (iter != textures.end() && iter->second->IsEfbCopy() && textures.find(texID) == textures.end())
			texID = temp_texID;
	}

	// GPUs don't like when the specified mipmap count would require more than one 1x1-sized LOD in the mipmap chain
	// e.g. 64x64 with 7 LODs would have the mipmap chain 64x64,32x32,16x16,8x8,4x4,2x2,1x1,0x0, so we limit the mipmap count to 6 there
	tex_levels = std::min<u32>(IntLog2(std::max(width, height)) + 1, tex_levels);

	TCacheEntryBase*& entry = textures[texID];
	if (entry)
	{
		// 1. Calculate reference hash:
		// calculated from RAM texture data for normal textures. Hashes for paletted textures are modified by tlut_hash. 0 for virtual EFB copies.
		if (g_ActiveConfig.bCopyEFBToTexture && entry->IsEfbCopy())
			tex_hash = TEXHASH_INVALID;

		// 2. a) For EFB copies, only the hash and the texture address need to match
		if (entry->IsEfbCopy() && tex_hash == entry->hash && address == entry->addr)
		{
			entry->type = TCET_EC_VRAM;

			// TODO: Print a warning if the format changes! In this case,
			// we could reinterpret the internal texture object data to the new pixel format
			// (similar to what is already being done in Renderer::ReinterpretPixelFormat())
			return ReturnEntry(stage, entry);
		}

		// 2. b) For normal textures, all texture parameters need to match
		if (address == entry->addr && tex_hash == entry->hash && full_format == entry->format &&
			entry->native_levels >= tex_levels && entry->native_width == nativeW && entry->native_height == nativeH)
		{
			return ReturnEntry(stage, entry);
		}

		// pool this texture and make a new one later
		FreeTexture(entry);
	}

	std::unique_ptr<HiresTexture> hires_tex;
	if (g_ActiveConfig.bHiresTextures)
	{
		hires_tex.reset(HiresTexture::Search(
			src_data, texture_size,
			&texMem[tlutaddr], palette_size,
			width, height,
			texformat
		));

		if (hires_tex)
		{
			auto& l = hires_tex->m_levels[0];
			if (l.width != width || l.height != height)
			{
				width = l.width;
				height = l.height;
			}
			expandedWidth = l.width;
			expandedHeight = l.height;
			CheckTempSize(l.data_size);
			memcpy(temp, l.data, l.data_size);
		}
	}

	if (!hires_tex)
	{
		if (!(texformat == GX_TF_RGBA8 && from_tmem))
		{
			const u8* tlut = &texMem[tlutaddr];
			TexDecoder_Decode(temp, src_data, expandedWidth, expandedHeight, texformat, tlut, (TlutFormat) tlutfmt);
		}
		else
		{
			u8* src_data_gb = &texMem[bpmem.tex[stage/4].texImage2[stage%4].tmem_odd * TMEM_LINE_SIZE];
			TexDecoder_DecodeRGBA8FromTmem(temp, src_data, src_data_gb, expandedWidth, expandedHeight);
		}
	}

	u32 texLevels = use_mipmaps ? tex_levels : 1;
	const bool using_custom_lods = hires_tex && hires_tex->m_levels.size() >= texLevels;
	// Only load native mips if their dimensions fit to our virtual texture dimensions
	const bool use_native_mips = use_mipmaps && !using_custom_lods && (width == nativeW && height == nativeH);
	texLevels = (use_native_mips || using_custom_lods) ? texLevels : 1; // TODO: Should be forced to 1 for non-pow2 textures (e.g. efb copies with automatically adjusted IR)

	// create the entry/texture
	TCacheEntryConfig config;
	config.width = width;
	config.height = height;
	config.levels = texLevels;
	entry = AllocateTexture(config);
	entry->type = TCET_NORMAL;
	GFX_DEBUGGER_PAUSE_AT(NEXT_NEW_TEXTURE, true);

	entry->SetGeneralParameters(address, texture_size, full_format);
	entry->SetDimensions(nativeW, nativeH, tex_levels);
	entry->hash = tex_hash;

	// load texture
	entry->Load(width, height, expandedWidth, 0);

	std::string basename = "";
	if (g_ActiveConfig.bDumpTextures && !hires_tex)
	{
		basename = HiresTexture::GenBaseName(
			src_data, texture_size,
			&texMem[tlutaddr], palette_size,
			width, height,
			texformat
		);
		DumpTexture(entry, basename, 0);
	}

	u32 level = 1;
	// load mips - TODO: Loading mipmaps from tmem is untested!
	if (use_native_mips)
	{
		src_data += texture_size;

		const u8* ptr_even = nullptr;
		const u8* ptr_odd = nullptr;
		if (from_tmem)
		{
			ptr_even = &texMem[bpmem.tex[stage/4].texImage1[stage%4].tmem_even * TMEM_LINE_SIZE + texture_size];
			ptr_odd = &texMem[bpmem.tex[stage/4].texImage2[stage%4].tmem_odd * TMEM_LINE_SIZE];
		}

		for (; level != texLevels; ++level)
		{
			const u32 mip_width = CalculateLevelSize(width, level);
			const u32 mip_height = CalculateLevelSize(height, level);
			const u32 expanded_mip_width = (mip_width + bsw) & (~bsw);
			const u32 expanded_mip_height = (mip_height + bsh) & (~bsh);

			const u8*& mip_src_data = from_tmem
				? ((level % 2) ? ptr_odd : ptr_even)
				: src_data;
			const u8* tlut = &texMem[tlutaddr];
			TexDecoder_Decode(temp, mip_src_data, expanded_mip_width, expanded_mip_height, texformat, tlut, (TlutFormat) tlutfmt);
			mip_src_data += TexDecoder_GetTextureSizeInBytes(expanded_mip_width, expanded_mip_height, texformat);

			entry->Load(mip_width, mip_height, expanded_mip_width, level);

			if (g_ActiveConfig.bDumpTextures)
				DumpTexture(entry, basename, level);
		}
	}
	else if (using_custom_lods)
	{
		for (; level != texLevels; ++level)
		{
			auto& l = hires_tex->m_levels[level];
			CheckTempSize(l.data_size);
			memcpy(temp, l.data, l.data_size);
			entry->Load(l.width, l.height, l.width, level);
		}
	}

	INCSTAT(stats.numTexturesUploaded);
	SETSTAT(stats.numTexturesAlive, textures.size());

	return ReturnEntry(stage, entry);
}

void TextureCache::CopyRenderTargetToTexture(u32 dstAddr, unsigned int dstFormat, PEControl::PixelFormat srcFormat,
	const EFBRectangle& unclampedSrc, bool isIntensity, bool scaleByHalf)
{
	// Emulation methods:
	//
	// - EFB to RAM:
	//      Encodes the requested EFB data at its native resolution to the emulated RAM using shaders.
	//      Load() decodes the data from there again (using TextureDecoder) if the EFB copy is being used as a texture again.
	//      Advantage: CPU can read data from the EFB copy and we don't lose any important updates to the texture
	//      Disadvantage: Encoding+decoding steps often are redundant because only some games read or modify EFB copies before using them as textures.
	//
	// - EFB to texture:
	//      Copies the requested EFB data to a texture object in VRAM, performing any color conversion using shaders.
	//      Advantage: Works for many games, since in most cases EFB copies aren't read or modified at all before being used as a texture again.
	//                 Since we don't do any further encoding or decoding here, this method is much faster.
	//                 It also allows enhancing the visual quality by doing scaled EFB copies.
	//
	// - Hybrid EFB copies:
	//      1a) Whenever this function gets called, encode the requested EFB data to RAM (like EFB to RAM)
	//      1b) Set type to TCET_EC_DYNAMIC for all texture cache entries in the destination address range.
	//          If EFB copy caching is enabled, further checks will (try to) prevent redundant EFB copies.
	//      2) Check if a texture cache entry for the specified dstAddr already exists (i.e. if an EFB copy was triggered to that address before):
	//      2a) Entry doesn't exist:
	//          - Also copy the requested EFB data to a texture object in VRAM (like EFB to texture)
	//          - Create a texture cache entry for the target (type = TCET_EC_VRAM)
	//          - Store a hash of the encoded RAM data in the texcache entry.
	//      2b) Entry exists AND type is TCET_EC_VRAM:
	//          - Like case 2a, but reuse the old texcache entry instead of creating a new one.
	//      2c) Entry exists AND type is TCET_EC_DYNAMIC:
	//          - Only encode the texture to RAM (like EFB to RAM) and store a hash of the encoded data in the existing texcache entry.
	//          - Do NOT copy the requested EFB data to a VRAM object. Reason: the texture is dynamic, i.e. the CPU is modifying it. Storing a VRAM copy is useless, because we'd always end up deleting it and reloading the data from RAM anyway.
	//      3) If the EFB copy gets used as a texture, compare the source RAM hash with the hash you stored when encoding the EFB data to RAM.
	//      3a) If the two hashes match AND type is TCET_EC_VRAM, reuse the VRAM copy you created
	//      3b) If the two hashes differ AND type is TCET_EC_VRAM, screw your existing VRAM copy. Set type to TCET_EC_DYNAMIC.
	//          Redecode the source RAM data to a VRAM object. The entry basically behaves like a normal texture now.
	//      3c) If type is TCET_EC_DYNAMIC, treat the EFB copy like a normal texture.
	//      Advantage: Non-dynamic EFB copies can be visually enhanced like with EFB to texture.
	//                 Compatibility is as good as EFB to RAM.
	//      Disadvantage: Slower than EFB to texture and often even slower than EFB to RAM.
	//                    EFB copy cache depends on accurate texture hashing being enabled. However, with accurate hashing you end up being as slow as without a copy cache anyway.
	//
	// Disadvantage of all methods: Calling this function requires the GPU to perform a pipeline flush which stalls any further CPU processing.
	//
	// For historical reasons, Dolphin doesn't actually implement "pure" EFB to RAM emulation, but only EFB to texture and hybrid EFB copies.

	float colmat[28] = {0};
	float *const fConstAdd = colmat + 16;
	float *const ColorMask = colmat + 20;
	ColorMask[0] = ColorMask[1] = ColorMask[2] = ColorMask[3] = 255.0f;
	ColorMask[4] = ColorMask[5] = ColorMask[6] = ColorMask[7] = 1.0f / 255.0f;
	unsigned int cbufid = -1;
	bool efbHasAlpha = bpmem.zcontrol.pixel_format == PEControl::RGBA6_Z24;

	if (srcFormat == PEControl::Z24)
	{
		switch (dstFormat)
		{
		case 0: // Z4
			colmat[3] = colmat[7] = colmat[11] = colmat[15] = 1.0f;
			cbufid = 0;
			break;
		case 1: // Z8
		case 8: // Z8
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1.0f;
			cbufid = 1;
			break;

		case 3: // Z16
			colmat[1] = colmat[5] = colmat[9] = colmat[12] = 1.0f;
			cbufid = 2;
			break;

		case 11: // Z16 (reverse order)
			colmat[0] = colmat[4] = colmat[8] = colmat[13] = 1.0f;
			cbufid = 3;
			break;

		case 6: // Z24X8
			colmat[0] = colmat[5] = colmat[10] = 1.0f;
			cbufid = 4;
			break;

		case 9: // Z8M
			colmat[1] = colmat[5] = colmat[9] = colmat[13] = 1.0f;
			cbufid = 5;
			break;

		case 10: // Z8L
			colmat[2] = colmat[6] = colmat[10] = colmat[14] = 1.0f;
			cbufid = 6;
			break;

		case 12: // Z16L - copy lower 16 depth bits
			// expected to be used as an IA8 texture (upper 8 bits stored as intensity, lower 8 bits stored as alpha)
			// Used e.g. in Zelda: Skyward Sword
			colmat[1] = colmat[5] = colmat[9] = colmat[14] = 1.0f;
			cbufid = 7;
			break;

		default:
			ERROR_LOG(VIDEO, "Unknown copy zbuf format: 0x%x", dstFormat);
			colmat[2] = colmat[5] = colmat[8] = 1.0f;
			cbufid = 8;
			break;
		}
	}
	else if (isIntensity)
	{
		fConstAdd[0] = fConstAdd[1] = fConstAdd[2] = 16.0f/255.0f;
		switch (dstFormat)
		{
		case 0: // I4
		case 1: // I8
		case 2: // IA4
		case 3: // IA8
		case 8: // I8
			// TODO - verify these coefficients
			colmat[0] = 0.257f; colmat[1] = 0.504f; colmat[2] = 0.098f;
			colmat[4] = 0.257f; colmat[5] = 0.504f; colmat[6] = 0.098f;
			colmat[8] = 0.257f; colmat[9] = 0.504f; colmat[10] = 0.098f;

			if (dstFormat < 2 || dstFormat == 8)
			{
				colmat[12] = 0.257f; colmat[13] = 0.504f; colmat[14] = 0.098f;
				fConstAdd[3] = 16.0f/255.0f;
				if (dstFormat == 0)
				{
					ColorMask[0] = ColorMask[1] = ColorMask[2] = 15.0f;
					ColorMask[4] = ColorMask[5] = ColorMask[6] = 1.0f / 15.0f;
					cbufid = 9;
				}
				else
				{
					cbufid = 10;
				}
			}
			else// alpha
			{
				colmat[15] = 1;
				if (dstFormat == 2)
				{
					ColorMask[0] = ColorMask[1] = ColorMask[2] = ColorMask[3] = 15.0f;
					ColorMask[4] = ColorMask[5] = ColorMask[6] = ColorMask[7] = 1.0f / 15.0f;
					cbufid = 11;
				}
				else
				{
					cbufid = 12;
				}

			}
			break;

		default:
			ERROR_LOG(VIDEO, "Unknown copy intensity format: 0x%x", dstFormat);
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			cbufid = 13;
			break;
		}
	}
	else
	{
		switch (dstFormat)
		{
		case 0: // R4
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1;
			ColorMask[0] = 15.0f;
			ColorMask[4] = 1.0f / 15.0f;
			cbufid = 14;
			break;
		case 1: // R8
		case 8: // R8
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1;
			cbufid = 15;
			break;

		case 2: // RA4
			colmat[0] = colmat[4] = colmat[8] = colmat[15] = 1.0f;
			ColorMask[0] = ColorMask[3] = 15.0f;
			ColorMask[4] = ColorMask[7] = 1.0f / 15.0f;

			cbufid = 16;
			if (!efbHasAlpha)
			{
				ColorMask[3] = 0.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 17;
			}
			break;
		case 3: // RA8
			colmat[0] = colmat[4] = colmat[8] = colmat[15] = 1.0f;

			cbufid = 18;
			if (!efbHasAlpha)
			{
				ColorMask[3] = 0.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 19;
			}
			break;

		case 7: // A8
			colmat[3] = colmat[7] = colmat[11] = colmat[15] = 1.0f;

			cbufid = 20;
			if (!efbHasAlpha)
			{
				ColorMask[3] = 0.0f;
				fConstAdd[0] = 1.0f;
				fConstAdd[1] = 1.0f;
				fConstAdd[2] = 1.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 21;
			}
			break;

		case 9: // G8
			colmat[1] = colmat[5] = colmat[9] = colmat[13] = 1.0f;
			cbufid = 22;
			break;
		case 10: // B8
			colmat[2] = colmat[6] = colmat[10] = colmat[14] = 1.0f;
			cbufid = 23;
			break;

		case 11: // RG8
			colmat[0] = colmat[4] = colmat[8] = colmat[13] = 1.0f;
			cbufid = 24;
			break;

		case 12: // GB8
			colmat[1] = colmat[5] = colmat[9] = colmat[14] = 1.0f;
			cbufid = 25;
			break;

		case 4: // RGB565
			colmat[0] = colmat[5] = colmat[10] = 1.0f;
			ColorMask[0] = ColorMask[2] = 31.0f;
			ColorMask[4] = ColorMask[6] = 1.0f / 31.0f;
			ColorMask[1] = 63.0f;
			ColorMask[5] = 1.0f / 63.0f;
			fConstAdd[3] = 1.0f; // set alpha to 1
			cbufid = 26;
			break;

		case 5: // RGB5A3
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			ColorMask[0] = ColorMask[1] = ColorMask[2] = 31.0f;
			ColorMask[4] = ColorMask[5] = ColorMask[6] = 1.0f / 31.0f;
			ColorMask[3] = 7.0f;
			ColorMask[7] = 1.0f / 7.0f;

			cbufid = 27;
			if (!efbHasAlpha)
			{
				ColorMask[3] = 0.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 28;
			}
			break;
		case 6: // RGBA8
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;

			cbufid = 29;
			if (!efbHasAlpha)
			{
				ColorMask[3] = 0.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 30;
			}
			break;

		default:
			ERROR_LOG(VIDEO, "Unknown copy color format: 0x%x", dstFormat);
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			cbufid = 31;
			break;
		}
	}

	// Clamp srcRect to 640x528. BPS: The Strike tries to encode an 800x600
	// texture, which is invalid.
	EFBRectangle correctSrc = unclampedSrc;
	correctSrc.ClampUL(0, 0, EFB_WIDTH, EFB_HEIGHT);

	// Validate source rect size
	if (correctSrc.GetWidth() <= 0 || correctSrc.GetHeight() <= 0)
		return;

	const unsigned int tex_w = scaleByHalf ? correctSrc.GetWidth() / 2 : correctSrc.GetWidth();
	const unsigned int tex_h = scaleByHalf ? correctSrc.GetHeight() / 2 : correctSrc.GetHeight();

	unsigned int scaled_tex_w = g_ActiveConfig.bCopyEFBScaled ? Renderer::EFBToScaledX(tex_w) : tex_w;
	unsigned int scaled_tex_h = g_ActiveConfig.bCopyEFBScaled ? Renderer::EFBToScaledY(tex_h) : tex_h;

	TCacheEntryBase*& entry = textures[dstAddr];
	if (entry)
		FreeTexture(entry);

	// create the texture
	TCacheEntryConfig config;
	config.rendertarget = true;
	config.width = scaled_tex_w;
	config.height = scaled_tex_h;
	config.layers = FramebufferManagerBase::GetEFBLayers();

	entry = AllocateTexture(config);

	// TODO: Using the wrong dstFormat, dumb...
	entry->SetGeneralParameters(dstAddr, 0, dstFormat);
	entry->SetDimensions(tex_w, tex_h, 1);
	entry->SetHashes(TEXHASH_INVALID);
	entry->type = TCET_EC_VRAM;

	entry->frameCount = FRAMECOUNT_INVALID;

	entry->size_in_bytes = bpmem.copyMipMapStrideChannels * 32 * (tex_h / (dstFormat == 0 ? 8 : 4));

	g_renderer->ResetAPIState();

	entry->FromRenderTarget(dstAddr, dstFormat, srcFormat, correctSrc, isIntensity, scaleByHalf, cbufid, colmat);

	if (!g_ActiveConfig.bCopyEFBToTexture)
	{
		u8* dst = Memory::GetPointer(dstAddr);
		entry->EncodeToMemory(dst, dstFormat, srcFormat, correctSrc, isIntensity, scaleByHalf);
		u64 hash = GetHash64(dst, entry->size_in_bytes, g_ActiveConfig.iSafeTextureCache_ColorSamples);

		TextureCache::MakeRangeDynamic(dstAddr, (u32)entry->size_in_bytes);

		entry->hash = hash;
	}

	g_renderer->RestoreAPIState();
}

TextureCache::TCacheEntryBase* TextureCache::AllocateTexture(const TCacheEntryConfig& config)
{
	TexPool::iterator iter = texture_pool.find(config);
	if (iter != texture_pool.end())
	{
		TextureCache::TCacheEntryBase* entry = iter->second;
		texture_pool.erase(iter);
		return entry;
	}

	INCSTAT(stats.numTexturesCreated);
	return g_texture_cache->CreateTexture(config);
}

void TextureCache::FreeTexture(TCacheEntryBase* entry)
{
	entry->frameCount = FRAMECOUNT_INVALID;
	texture_pool.insert(TexPool::value_type(entry->config, entry));
}
