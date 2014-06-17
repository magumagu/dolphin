// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Core/Core.h"
#include "Core/HW/Memmap.h"

#include "VideoBackends/Software/StateManagerSoftware.h"
#include "VideoBackends/Software/EfbCopy.h"
#include "VideoBackends/Software/EfbInterface.h"
#include "VideoBackends/Software/HwRasterizer.h"
#include "VideoBackends/Software/Rasterizer.h"
#include "VideoBackends/Software/SWVideoConfig.h"
#include "VideoBackends/Software/Tev.h"

#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VideoCommon.h"

void InitSWStateManager()
{
	g_state_manager = new StateManagerSoftware;
}

void StateManagerSoftware::SetScissor()
{
	Rasterizer::SetScissor();
}

void StateManagerSoftware::CopyEFB(u32 dstAddr, const EFBRectangle& srcRect,
	unsigned int dstFormat, PEControl::PixelFormat srcFormat,
	bool isIntensity, bool scaleByHalf)
{
	// FIXME: use srcrect
	EfbCopy::CopyToRam();
}

void StateManagerSoftware::RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc,
	u32 fbWidth, u32 fbHeight, float Gamma)
{
	EfbCopy::CopyToXfb(xfbAddr, fbWidth, fbHeight, sourceRc, Gamma);
}

void StateManagerSoftware::ClearScreen(const EFBRectangle &rc)
{
	// FIXME: use rc
	if (g_SWVideoConfig.bHwRasterizer)
		HwRasterizer::Clear();
	else
		EfbCopy::ClearEfb();
}

void StateManagerSoftware::ClearPixelPerf()
{
	memset(EfbInterface::perf_values, 0, sizeof(EfbInterface::perf_values));
}

void StateManagerSoftware::SetColorChanged(int num, bool ra)
{
	TevReg& reg = bpmem.tevregs[num];
	if (ra)
	{
		bool konst = reg.type_ra;
		Rasterizer::SetTevReg(num, Tev::ALP_C, konst, (s16)reg.alpha);
		Rasterizer::SetTevReg(num, Tev::RED_C, konst, (s16)reg.red);
	}
	else
	{
		bool konst = reg.type_bg;
		Rasterizer::SetTevReg(num, Tev::GRN_C, konst, (s16)reg.green);
		Rasterizer::SetTevReg(num, Tev::BLU_C, konst, (s16)reg.blue);
	}
}
