// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once


#include "Common/Common.h"
#include "VideoCommon/StateManager.h"

class StateManagerSoftware : public StateManager
{
public:
	virtual void SetScissor() override;
	virtual void CopyEFB(u32 dstAddr, const EFBRectangle& srcRect,
		unsigned int dstFormat, PEControl::PixelFormat srcFormat,
		bool isIntensity, bool scaleByHalf) override;
	virtual void RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc,
		u32 fbWidth, u32 fbHeight, float Gamma) override;
	virtual void ClearScreen(const EFBRectangle &rc) override;
	virtual void SetColorChanged(int num, bool ra) override;
	virtual void ClearPixelPerf() override;
};

void InitSWStateManager();
