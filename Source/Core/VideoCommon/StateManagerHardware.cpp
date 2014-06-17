// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Common/Common.h"

#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"

#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/StateManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"

StateManager *g_state_manager;

void InitHWStateManager()
{
	g_state_manager = new StateManagerHardware;
}

void StateManagerHardware::FlushPipeline()
{
	VertexManager::Flush();
}

void StateManagerHardware::SetGenerationMode()
{
	g_renderer->SetGenerationMode();
}

void StateManagerHardware::SetScissor()
{
	/* NOTE: the minimum value here for the scissor rect and offset is -342.
	 * GX internally adds on an offset of 342 to both the offset and scissor
	 * coords to ensure that the register was always unsigned.
	 *
	 * The code that was here before tried to "undo" this offset, but
	 * since we always take the difference, the +342 added to both
	 * sides cancels out. */

	/* The scissor offset is always even, so to save space, the scissor offset
	 * register is scaled down by 2. So, if somebody calls
	 * GX_SetScissorBoxOffset(20, 20); the registers will be set to 10, 10. */
	const int xoff = bpmem.scissorOffset.x * 2;
	const int yoff = bpmem.scissorOffset.y * 2;

	EFBRectangle rc (bpmem.scissorTL.x - xoff,     bpmem.scissorTL.y - yoff,
	                 bpmem.scissorBR.x - xoff + 1, bpmem.scissorBR.y - yoff + 1);

	if (rc.left < 0) rc.left = 0;
	if (rc.top < 0) rc.top = 0;
	if (rc.right > EFB_WIDTH) rc.right = EFB_WIDTH;
	if (rc.bottom > EFB_HEIGHT) rc.bottom = EFB_HEIGHT;

	if (rc.left > rc.right) rc.right = rc.left;
	if (rc.top > rc.bottom) rc.bottom = rc.top;

	g_renderer->SetScissorRect(rc);
}

void StateManagerHardware::SetLineWidth()
{
	g_renderer->SetLineWidth();
}

void StateManagerHardware::SetDepthMode()
{
	g_renderer->SetDepthMode();
}

void StateManagerHardware::SetBlendMode()
{
	g_renderer->SetBlendMode(false);
}

void StateManagerHardware::SetDitherMode()
{
	g_renderer->SetDitherMode();
}

void StateManagerHardware::SetLogicOpMode()
{
	g_renderer->SetLogicOpMode();
}

void StateManagerHardware::SetColorMask()
{
	g_renderer->SetColorMask();
}

void StateManagerHardware::CopyEFB(u32 dstAddr, const EFBRectangle& srcRect,
	                               unsigned int dstFormat,
                                   PEControl::PixelFormat srcFormat,
	                               bool isIntensity, bool scaleByHalf)
{
	if (g_ActiveConfig.bShowEFBCopyRegions)
		stats.efb_regions.push_back(srcRect);

	// bpmem.zcontrol.pixel_format to PEControl::Z24 is when the game wants to copy from ZBuffer (Zbuffer uses 24-bit Format)
	if (g_ActiveConfig.bEFBCopyEnable)
	{
		TextureCache::CopyRenderTargetToTexture(dstAddr, dstFormat, srcFormat,
			srcRect, isIntensity, scaleByHalf);
	}
}

void StateManagerHardware::RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc,
	                                   u32 fbWidth, u32 fbHeight, float Gamma)
{
	Renderer::RenderToXFB(xfbAddr, sourceRc, fbWidth, fbHeight, Gamma);
}

/* Explanation of the magic behind ClearScreen:
	There's numerous possible formats for the pixel data in the EFB.
	However, in the HW accelerated backends we're always using RGBA8
	for the EFB format, which causes some problems:
	- We're using an alpha channel although the game doesn't
	- If the actual EFB format is RGBA6_Z24 or R5G6B5_Z16, we are using more bits per channel than the native HW

	To properly emulate the above points, we're doing the following:
	(1)
		- disable alpha channel writing of any kind of rendering if the actual EFB format doesn't use an alpha channel
		- NOTE: Always make sure that the EFB has been cleared to an alpha value of 0xFF in this case!
		- Same for color channels, these need to be cleared to 0x00 though.
	(2)
		- convert the RGBA8 color to RGBA6/RGB8/RGB565 and convert it to RGBA8 again
		- convert the Z24 depth value to Z16 and back to Z24
*/
void StateManagerHardware::ClearScreen(const EFBRectangle &rc)
{
	bool colorEnable = bpmem.blendmode.colorupdate;
	bool alphaEnable = bpmem.blendmode.alphaupdate;
	bool zEnable = bpmem.zmode.updateenable;
	auto pixel_format = bpmem.zcontrol.pixel_format;

	// (1): Disable unused color channels
	if (pixel_format == PEControl::RGB8_Z24 ||
		pixel_format == PEControl::RGB565_Z16 ||
		pixel_format == PEControl::Z24)
	{
		alphaEnable = false;
	}

	if (colorEnable || alphaEnable || zEnable)
	{
		u32 color = (bpmem.clearcolorAR << 16) | bpmem.clearcolorGB;
		u32 z = bpmem.clearZValue;

		// (2) drop additional accuracy
		if (pixel_format == PEControl::RGBA6_Z24)
		{
			color = RGBA8ToRGBA6ToRGBA8(color);
		}
		else if (pixel_format == PEControl::RGB565_Z16)
		{
			color = RGBA8ToRGB565ToRGBA8(color);
			z = Z24ToZ16ToZ24(z);
		}
		g_renderer->ClearScreen(rc, colorEnable, alphaEnable, zEnable, color, z);
	}
}

void StateManagerHardware::OnPixelFormatChange()
{
	int convtype = -1;

	// TODO : Check for Z compression format change
	// When using 16bit Z, the game may enable a special compression format which we need to handle
	// If we don't, Z values will be completely screwed up, currently only Star Wars:RS2 uses that.

	/*
	 * When changing the EFB format, the pixel data won't get converted to the new format but stays the same.
	 * Since we are always using an RGBA8 buffer though, this causes issues in some games.
	 * Thus, we reinterpret the old EFB data with the new format here.
	 */
	if (!g_ActiveConfig.bEFBEmulateFormatChanges)
		return;

	auto old_format = Renderer::GetPrevPixelFormat();
	auto new_format = bpmem.zcontrol.pixel_format;

	// no need to reinterpret pixel data in these cases
	if (new_format == old_format || old_format == PEControl::INVALID_FMT)
		goto skip;

	// Check for pixel format changes
	switch (old_format)
	{
		case PEControl::RGB8_Z24:
		case PEControl::Z24:
			// Z24 and RGB8_Z24 are treated equal, so just return in this case
			if (new_format == PEControl::RGB8_Z24 || new_format == PEControl::Z24)
				goto skip;

			if (new_format == PEControl::RGBA6_Z24)
				convtype = 0;
			else if (new_format == PEControl::RGB565_Z16)
				convtype = 1;
			break;

		case PEControl::RGBA6_Z24:
			if (new_format == PEControl::RGB8_Z24 ||
				new_format == PEControl::Z24)
				convtype = 2;
			else if (new_format == PEControl::RGB565_Z16)
				convtype = 3;
			break;

		case PEControl::RGB565_Z16:
			if (new_format == PEControl::RGB8_Z24 ||
				new_format == PEControl::Z24)
				convtype = 4;
			else if (new_format == PEControl::RGBA6_Z24)
				convtype = 5;
			break;

		default:
			break;
	}

	if (convtype == -1)
	{
		ERROR_LOG(VIDEO, "Unhandled EFB format change: %d to %d\n", static_cast<int>(old_format), static_cast<int>(new_format));
		goto skip;
	}

	g_renderer->ReinterpretPixelData(convtype);

skip:
	DEBUG_LOG(VIDEO, "pixelfmt: pixel=%d, zc=%d", static_cast<int>(new_format), static_cast<int>(bpmem.zcontrol.zformat));

	Renderer::StorePixelFormat(new_format);
}

void StateManagerHardware::SetViewportChanged()
{
	VertexShaderManager::SetViewportChanged();
	PixelShaderManager::SetViewportChanged();
}

void StateManagerHardware::SetColorChanged(int num, bool ra)
{
	TevReg& reg = bpmem.tevregs[num];
	bool konst = ra ? (bool)reg.type_ra : (bool)reg.type_bg;
	PixelShaderManager::SetColorChanged(konst, num);
}

void StateManagerHardware::SetTexCoordChanged(u8 texmapid)
{
	PixelShaderManager::SetTexCoordChanged(texmapid);
}

void StateManagerHardware::SetZTextureBias()
{
	PixelShaderManager::SetZTextureBias();
}

void StateManagerHardware::SetZTextureTypeChanged()
{
	PixelShaderManager::SetZTextureTypeChanged();
}

void StateManagerHardware::SetAlpha()
{
	PixelShaderManager::SetAlpha();
}

void StateManagerHardware::SetFogColorChanged()
{
	PixelShaderManager::SetFogColorChanged();
}

void StateManagerHardware::SetFogParamChanged()
{
	PixelShaderManager::SetFogParamChanged();
}

void StateManagerHardware::SetFogRangeAdjustChanged()
{
	PixelShaderManager::SetFogRangeAdjustChanged();
}

void StateManagerHardware::SetDestAlpha()
{
	PixelShaderManager::SetDestAlpha();
}

void StateManagerHardware::SetIndTexScaleChanged(bool high)
{
	PixelShaderManager::SetIndTexScaleChanged(high);
}

void StateManagerHardware::SetIndMatrixChanged(int matrixidx)
{
	PixelShaderManager::SetIndMatrixChanged(matrixidx);
}

void StateManagerHardware::ClearPixelPerf()
{
	if (PerfQueryBase::ShouldEmulate())
		g_perf_query->ResetQuery();
}
