// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.


// ------------------------------------------
// Video backend must define these functions
// ------------------------------------------

#pragma once

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/VideoCommon.h"

namespace BPFunctions
{

void FlushPipeline();
void SetGenerationMode();
void SetScissor();
void SetLineWidth();
void SetDepthMode();
void SetBlendMode();
void SetDitherMode();
void SetLogicOpMode();
void SetColorMask();
void CopyEFB(u32 dstAddr, const EFBRectangle& srcRect,
             unsigned int dstFormat, PEControl::PixelFormat srcFormat,
             bool isIntensity, bool scaleByHalf);
void RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc,
                 u32 fbWidth, u32 fbHeight, float Gamma);
void ClearScreen(const EFBRectangle &rc);
void OnPixelFormatChange();
void SetInterlacingMode(const BPCmd &bp);
void SetViewportChanged();
void SetColorChanged(int type, int num);
void SetTexCoordChanged(u8 texmapid);
void SetZTextureBias();
void SetZTextureTypeChanged();
void SetAlpha();
void SetFogColorChanged();
void SetFogParamChanged();
void SetFogRangeAdjustChanged();
void SetDestAlpha();
void SetIndTexScaleChanged(bool high);
void SetIndMatrixChanged(int matrixidx);
};
