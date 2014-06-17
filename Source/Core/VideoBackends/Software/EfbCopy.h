// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include "Common/Common.h"

#include "VideoCommon/VideoCommon.h"

namespace EfbCopy
{
	void CopyToXfb(u32 xfbAddr, u32 fbWidth, u32 fbHeight, const EFBRectangle& sourceRc, float Gamma);
	void CopyToRam();
	void ClearEfb();
}
