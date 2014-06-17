// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.


// ------------------------------------------
// Video backend must define these functions
// ------------------------------------------

#pragma once

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/VideoCommon.h"

class StateManager
{
public:
	virtual void FlushPipeline() {}
	virtual void SetGenerationMode() {}
	virtual void SetScissor() {}
	virtual void SetLineWidth() {}
	virtual void SetDepthMode() {}
	virtual void SetBlendMode() {}
	virtual void SetDitherMode() {}
	virtual void SetLogicOpMode() {}
	virtual void SetColorMask() {}
	virtual void CopyEFB(u32 dstAddr, const EFBRectangle& srcRect,
	                     unsigned int dstFormat, PEControl::PixelFormat srcFormat,
	                     bool isIntensity, bool scaleByHalf) {}
	virtual void RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc,
	                         u32 fbWidth, u32 fbHeight, float Gamma) {}
	virtual void ClearScreen(const EFBRectangle &rc) {}
	virtual void OnPixelFormatChange() {}
	virtual void SetViewportChanged() {}
	virtual void SetColorChanged(int num, bool ra) {}
	virtual void SetTexCoordChanged(u8 texmapid) {}
	virtual void SetZTextureBias() {}
	virtual void SetZTextureTypeChanged() {}
	virtual void SetAlpha() {}
	virtual void SetFogColorChanged() {}
	virtual void SetFogParamChanged() {}
	virtual void SetFogRangeAdjustChanged() {}
	virtual void SetDestAlpha() {}
	virtual void SetIndTexScaleChanged(bool high) {}
	virtual void SetIndMatrixChanged(int matrixidx) {}
	virtual void ClearPixelPerf() {}
};

extern StateManager *g_state_manager;

class StateManagerHardware : public StateManager
{
public:
	virtual void FlushPipeline() override;
	virtual void SetGenerationMode() override;
	virtual void SetScissor() override;
	virtual void SetLineWidth() override;
	virtual void SetDepthMode() override;
	virtual void SetBlendMode() override;
	virtual void SetDitherMode() override;
	virtual void SetLogicOpMode() override;
	virtual void SetColorMask() override;
	virtual void CopyEFB(u32 dstAddr, const EFBRectangle& srcRect,
	                     unsigned int dstFormat, PEControl::PixelFormat srcFormat,
	                     bool isIntensity, bool scaleByHalf) override;
	virtual void RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc,
	                         u32 fbWidth, u32 fbHeight, float Gamma) override;
	virtual void ClearScreen(const EFBRectangle &rc) override;
	virtual void OnPixelFormatChange() override;
	virtual void SetViewportChanged() override;
	virtual void SetColorChanged(int num, bool ra) override;
	virtual void SetTexCoordChanged(u8 texmapid) override;
	virtual void SetZTextureBias() override;
	virtual void SetZTextureTypeChanged() override;
	virtual void SetAlpha() override;
	virtual void SetFogColorChanged() override;
	virtual void SetFogParamChanged() override;
	virtual void SetFogRangeAdjustChanged() override;
	virtual void SetDestAlpha() override;
	virtual void SetIndTexScaleChanged(bool high) override;
	virtual void SetIndMatrixChanged(int matrixidx) override;
	virtual void ClearPixelPerf() override;
};

void InitHWStateManager();
