// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Core/Core.h"
#include "Core/HW/Memmap.h"

#include "VideoBackends/Software/DebugUtil.h"
#include "VideoBackends/Software/EfbCopy.h"
#include "VideoBackends/Software/EfbInterface.h"
#include "VideoBackends/Software/HwRasterizer.h"
#include "VideoBackends/Software/SWCommandProcessor.h"
#include "VideoBackends/Software/SWRenderer.h"
#include "VideoBackends/Software/SWStatistics.h"
#include "VideoBackends/Software/SWVideoConfig.h"
#include "VideoBackends/Software/TextureEncoder.h"

namespace EfbCopy
{
	void CopyToXfb(u32 xfbAddr, u32 fbWidth, u32 fbHeight, const EFBRectangle& sourceRc, float Gamma)
	{
		GLInterface->Update(); // update the render window position and the backbuffer size

		if (!g_SWVideoConfig.bHwRasterizer)
		{
			INFO_LOG(VIDEO, "xfbaddr: %x, fbwidth: %i, fbheight: %i, source: (%i, %i, %i, %i), Gamma %f",
					 xfbAddr, fbWidth, fbHeight, sourceRc.top, sourceRc.left, sourceRc.bottom, sourceRc.right, Gamma);

			if (!g_SWVideoConfig.bBypassXFB)
			{
				EfbInterface::yuv422_packed* xfb_in_ram = (EfbInterface::yuv422_packed *) Memory::GetPointer(xfbAddr);

				EfbInterface::CopyToXFB(xfb_in_ram, fbWidth, fbHeight, sourceRc, Gamma);
			}
			else
			{
				// Ask SWRenderer for the next color texture
				u8 *colorTexture = SWRenderer::getColorTexture();

				EfbInterface::BypassXFB(colorTexture, fbWidth, fbHeight, sourceRc, Gamma);

				// Tell SWRenderer we are now finished with it.
				SWRenderer::swapColorTexture();

				// FifoPlayer is broken and never calls BeginFrame/EndFrame.
				// Hence, we manually force a swap now. This emulates the behavior
				// of hardware backends with XFB emulation disabled.
				// TODO: Fix FifoPlayer by making proper use of VideoInterface!
				//       This requires careful synchronization since GPU commands
				//       are processed on a different thread than VI commands.
				SWRenderer::Swap(fbWidth, fbHeight);
			}
		}
	}

	void CopyToRam()
	{
		if (!g_SWVideoConfig.bHwRasterizer)
		{
			u8 *dest_ptr = Memory::GetPointer(bpmem.copyTexDest << 5);

			TextureEncoder::Encode(dest_ptr);
		}
	}

	void ClearEfb()
	{
		u32 clearColor = (bpmem.clearcolorAR & 0xff) << 24 | bpmem.clearcolorGB << 8 | (bpmem.clearcolorAR & 0xff00) >> 8;

		int left   = bpmem.copyTexSrcXY.x;
		int top    = bpmem.copyTexSrcXY.y;
		int right  = left + bpmem.copyTexSrcWH.x;
		int bottom = top + bpmem.copyTexSrcWH.y;

		for (u16 y = top; y <= bottom; y++)
		{
			for (u16 x = left; x <= right; x++)
			{
				EfbInterface::SetColor(x, y, (u8*)(&clearColor));
				EfbInterface::SetDepth(x, y, bpmem.clearZValue);
			}
		}
	}
}
