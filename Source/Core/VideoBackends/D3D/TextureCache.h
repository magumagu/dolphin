// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include "VideoBackends/D3D/D3DTexture.h"
#include "VideoCommon/TextureCacheBase.h"

namespace DX11
{

class TextureCache : public ::TextureCache
{
public:
	TextureCache();
	~TextureCache();

private:
	struct TCacheEntry : TCacheEntryBase
	{
		D3DTexture2D *const texture;

		D3D11_USAGE usage;

		TCacheEntry(const TCacheEntryConfig& config, D3DTexture2D *_tex) : TCacheEntryBase(config), texture(_tex) {}
		~TCacheEntry();

		void Load(unsigned int width, unsigned int height,
			unsigned int expanded_width, unsigned int levels) override;

		void FromRenderTarget(u32 dstAddr, unsigned int dstFormat,
			PEControl::PixelFormat srcFormat, const EFBRectangle& srcRect,
			bool isIntensity, bool scaleByHalf, unsigned int cbufid,
			const float *colmat) override;
		void EncodeToMemory(u8* dst, unsigned int dstFormat,
			PEControl::PixelFormat srcFormat, const EFBRectangle& srcRect,
			bool isIntensity, bool scaleByHalf) override;

		void Bind(unsigned int stage) override;
		bool Save(const std::string& filename, unsigned int level) override;
	};

	TCacheEntryBase* CreateTexture(const TCacheEntryConfig& config) override;

	void CompileShaders() override { }
	void DeleteShaders() override { }
};

}
