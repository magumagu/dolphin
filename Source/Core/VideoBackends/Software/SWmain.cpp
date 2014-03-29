// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <string>

#include "Common/Atomic.h"
#include "Common/Common.h"
#include "Common/FileUtil.h"
#include "Common/LogManager.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"

#include "VideoBackends/OGL/GLExtensions/GLExtensions.h"
#include "VideoBackends/Software/Clipper.h"
#include "VideoBackends/Software/DebugUtil.h"
#include "VideoBackends/Software/EfbInterface.h"
#include "VideoBackends/Software/HwRasterizer.h"
#include "VideoBackends/Software/OpcodeDecoder.h"
#include "VideoBackends/Software/Rasterizer.h"
#include "VideoBackends/Software/SWCommandProcessor.h"
#include "VideoBackends/Software/SWRenderer.h"
#include "VideoBackends/Software/SWStatistics.h"
#include "VideoBackends/Software/SWVertexLoader.h"
#include "VideoBackends/Software/SWVideoConfig.h"
#include "VideoBackends/Software/VideoBackend.h"

#if defined(HAVE_WX) && HAVE_WX
#include "VideoBackends/Software/VideoConfigDialog.h"
#endif // HAVE_WX

#include "VideoCommon/BPStructs.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/XFMemory.h"

#define VSYNC_ENABLED 0

static volatile u32 s_swapRequested = false;

static volatile struct
{
	u32 xfbAddr;
	u32 fbWidth;
	u32 fbHeight;
} s_beginFieldArgs;

namespace SW
{

static volatile bool fifoStateRun = false;
static volatile bool emuRunningState = false;
static std::mutex m_csSWVidOccupied;

std::string VideoSoftware::GetName() const
{
	return _trans("Software Renderer");
}

void *DllDebugger(void *_hParent, bool Show)
{
	return nullptr;
}

void VideoSoftware::ShowConfig(void *_hParent)
{
#if defined(HAVE_WX) && HAVE_WX
	VideoConfigDialog diag((wxWindow*)_hParent, "Software", "gfx_software");
	diag.ShowModal();
#endif
}

class Renderer : public ::Renderer {
	virtual void SetColorMask()  {}
	virtual void SetBlendMode(bool forceUpdate)  {}
	virtual void SetScissorRect(const EFBRectangle& rc)  { Rasterizer::SetScissor(); }
	virtual void SetGenerationMode() {}
	virtual void SetDepthMode()  {}
	virtual void SetLogicOpMode()  {}
	virtual void SetDitherMode()  {}
	virtual void SetLineWidth()  {}
	virtual void SetSamplerState(int stage, int texindex) {}
	virtual void SetInterlacingMode() {}
	virtual void SetViewport() {}

	virtual void ApplyState(bool bUseDstAlpha) {}
	virtual void RestoreState() {}

	virtual void RenderText(const std::string& text, int left, int top, u32 color) {}

	virtual void ClearScreen(const EFBRectangle& rc, bool colorEnable, bool alphaEnable, bool zEnable, u32 color, u32 z) {}
	virtual void ReinterpretPixelData(unsigned int convtype) {}

	virtual u32 AccessEFB(EFBAccessType type, u32 x, u32 y, u32 poke_data) { return 0; }

	// What's the real difference between these? Too similar names.
	virtual void ResetAPIState() {}
	virtual void RestoreAPIState() {}

	// Finish up the current frame, print some stats
	virtual void SwapImpl(u32 xfbAddr, u32 fbWidth, u32 fbHeight, const EFBRectangle& rc, float Gamma = 1.0f) {}

	virtual bool SaveScreenshot(const std::string &filename, const TargetRectangle &rc) { return false; }
	virtual TargetRectangle ConvertEFBRectangle(const EFBRectangle& rc) { return TargetRectangle(); }
};

class VertexManager : public ::VertexManager {
	virtual ::NativeVertexFormat* CreateNativeVertexFormat() { return nullptr; }
	virtual void ResetBuffer(u32 stride) {}
	virtual void vFlush(bool useDstAlpha) {}
};

bool VideoSoftware::Initialize(void *&window_handle)
{
	g_SWVideoConfig.Load((File::GetUserPath(D_CONFIG_IDX) + "gfx_software.ini").c_str());

	InitInterface();
	GLInterface->SetMode(GLInterfaceMode::MODE_DETECT);
	if (!GLInterface->Create(window_handle))
	{
		INFO_LOG(VIDEO, "%s", "SWRenderer::Create failed\n");
		return false;
	}

	BPInit();
	// TODO: Equivalent to InitXFMemory();
	SWCommandProcessor::Init();
	PixelEngine::Init();
	OpcodeDecoder::Init();
	Clipper::Init();
	Rasterizer::Init();
	HwRasterizer::Init();
	SWRenderer::Init();
	DebugUtil::Init();
	g_renderer = new SW::Renderer;
	g_vertex_manager = new SW::VertexManager;

	return true;
}

void VideoSoftware::DoState(PointerWrap& p)
{
	bool software = true;
	p.Do(software);
	if (p.GetMode() == PointerWrap::MODE_READ && software == false)
		// change mode to abort load of incompatible save state.
		p.SetMode(PointerWrap::MODE_VERIFY);

	// TODO: incomplete?
	SWCommandProcessor::DoState(p);
	PixelEngine::DoState(p);
	EfbInterface::DoState(p);
	OpcodeDecoder::DoState(p);
	Clipper::DoState(p);
	p.Do(xfregs);
	p.Do(bpmem);
	p.DoPOD(swstats);

	// CP Memory
	p.DoArray(arraybases, 16);
	p.DoArray(arraystrides, 16);
	p.Do(MatrixIndexA);
	p.Do(MatrixIndexB);
	p.Do(g_VtxDesc.Hex);
	p.DoArray(g_VtxAttr, 8);
	p.DoMarker("CP Memory");

}

void VideoSoftware::CheckInvalidState()
{
	// there is no state to invalidate
}

void VideoSoftware::PauseAndLock(bool doLock, bool unpauseOnUnlock)
{
	if (doLock)
	{
		EmuStateChange(EMUSTATE_CHANGE_PAUSE);
		if (!Core::IsGPUThread())
			m_csSWVidOccupied.lock();
	}
	else
	{
		if (unpauseOnUnlock)
			EmuStateChange(EMUSTATE_CHANGE_PLAY);
		if (!Core::IsGPUThread())
			m_csSWVidOccupied.unlock();
	}
}

void VideoSoftware::RunLoop(bool enable)
{
	emuRunningState = enable;
}

void VideoSoftware::EmuStateChange(EMUSTATE_CHANGE newState)
{
	emuRunningState = (newState == EMUSTATE_CHANGE_PLAY) ? true : false;
}

void VideoSoftware::Shutdown()
{
	// TODO: should be in Video_Cleanup
	HwRasterizer::Shutdown();
	SWRenderer::Shutdown();

	// Do our OSD callbacks
	OSD::DoCallbacks(OSD::OSD_SHUTDOWN);

	GLInterface->Shutdown();
}

void VideoSoftware::Video_Cleanup()
{
	GLInterface->ClearCurrent();
}

// This is called after Video_Initialize() from the Core
void VideoSoftware::Video_Prepare()
{
	GLInterface->MakeCurrent();

	// Init extension support.
	if (!GLExtensions::Init())
	{
		ERROR_LOG(VIDEO, "GLExtensions::Init failed!Does your video card support OpenGL 2.0?");
		return;
	}

	// Handle VSync on/off
	GLInterface->SwapInterval(VSYNC_ENABLED);

	// Do our OSD callbacks
	OSD::DoCallbacks(OSD::OSD_INIT);

	HwRasterizer::Prepare();
	SWRenderer::Prepare();

	INFO_LOG(VIDEO, "Video backend initialized.");
}

// Run from the CPU thread (from VideoInterface.cpp)
void VideoSoftware::Video_BeginField(u32 xfbAddr, u32 fbWidth, u32 fbHeight)
{
	s_beginFieldArgs.xfbAddr = xfbAddr;
	s_beginFieldArgs.fbWidth = fbWidth;
	s_beginFieldArgs.fbHeight = fbHeight;
}

// Run from the CPU thread (from VideoInterface.cpp)
void VideoSoftware::Video_EndField()
{
	// Techincally the XFB is continually rendered out scanline by scanline between
	// BeginField and EndFeild, We could possibly get away with copying out the whole thing
	// at BeginField for less lag, but for the safest emulation we run it here.

	if (g_bSkipCurrentFrame || s_beginFieldArgs.xfbAddr == 0)
	{
		swstats.frameCount++;
		swstats.ResetFrame();
		Core::Callback_VideoCopiedToXFB(false);
		return;
	}
	if (!g_SWVideoConfig.bHwRasterizer)
	{
		if (!g_SWVideoConfig.bBypassXFB)
		{
			EfbInterface::yuv422_packed *xfb = (EfbInterface::yuv422_packed *) Memory::GetPointer(s_beginFieldArgs.xfbAddr);

			SWRenderer::UpdateColorTexture(xfb, s_beginFieldArgs.fbWidth, s_beginFieldArgs.fbHeight);
		}
	}

	// Ideally we would just move all the OpenGL context stuff to the CPU thread,
	// but this gets messy when the hardware rasterizer is enabled.
	// And neobrain loves his hardware rasterizer.

	// If BypassXFB has already done a swap (cf. EfbCopy::CopyToXfb), skip this.
	if (!g_SWVideoConfig.bBypassXFB)
	{
		// If we are in dual core mode, notify the GPU thread about the new color texture.
		if (SConfig::GetInstance().m_LocalCoreStartupParameter.bCPUThread)
			Common::AtomicStoreRelease(s_swapRequested, true);
		else
			SWRenderer::Swap(s_beginFieldArgs.fbWidth, s_beginFieldArgs.fbHeight);
	}
}

u32 VideoSoftware::Video_AccessEFB(EFBAccessType type, u32 x, u32 y, u32 InputData)
{
	u32 value = 0;

	switch (type)
	{
	case PEEK_Z:
		{
			value = EfbInterface::GetDepth(x, y);
			break;
		}

	case POKE_Z:
		break;

	case PEEK_COLOR:
		{
			u32 color = 0;
			EfbInterface::GetColor(x, y, (u8*)&color);

			// rgba to argb
			value = (color >> 8) | (color & 0xff) << 24;
			break;
		}

	case POKE_COLOR:
		break;
	}

	return value;
}

u32 VideoSoftware::Video_GetQueryResult(PerfQueryType type)
{
	return EfbInterface::perf_values[type];
}

bool VideoSoftware::Video_Screenshot(const std::string& filename)
{
	SWRenderer::SetScreenshot(filename.c_str());
	return true;
}

// Run from the graphics thread
static void VideoFifo_CheckSwapRequest()
{
	if (Common::AtomicLoadAcquire(s_swapRequested))
	{
		SWRenderer::Swap(s_beginFieldArgs.fbWidth, s_beginFieldArgs.fbHeight);
		Common::AtomicStoreRelease(s_swapRequested, false);
	}
}

// -------------------------------
// Enter and exit the video loop
// -------------------------------
void VideoSoftware::Video_EnterLoop()
{
	std::lock_guard<std::mutex> lk(m_csSWVidOccupied);
	fifoStateRun = true;

	while (fifoStateRun)
	{
		VideoFifo_CheckSwapRequest();
		g_video_backend->PeekMessages();

		if (!SWCommandProcessor::RunBuffer())
		{
			Common::YieldCPU();
		}

		while (!emuRunningState && fifoStateRun)
		{
			g_video_backend->PeekMessages();
			VideoFifo_CheckSwapRequest();
			m_csSWVidOccupied.unlock();
			Common::SleepCurrentThread(1);
			m_csSWVidOccupied.lock();
		}
	}
}

void VideoSoftware::Video_ExitLoop()
{
	fifoStateRun = false;
}

// TODO : could use the OSD class in video common, we would need to implement the Renderer class
//        however most of it is useless for the SW backend so we could as well move it to its own class
void VideoSoftware::Video_AddMessage(const std::string& msg, u32 milliseconds)
{
}
void VideoSoftware::Video_ClearMessages()
{
}

void VideoSoftware::Video_SetRendering(bool bEnabled)
{
	SWCommandProcessor::SetRendering(bEnabled);
}

void VideoSoftware::Video_GatherPipeBursted()
{
	SWCommandProcessor::GatherPipeBursted();
}

bool VideoSoftware::Video_IsPossibleWaitingSetDrawDone(void)
{
	return false;
}

bool VideoSoftware::Video_IsHiWatermarkActive(void)
{
	return false;
}


void VideoSoftware::Video_AbortFrame(void)
{
}

void VideoSoftware::RegisterCPMMIO(MMIO::Mapping* mmio, u32 base)
{
	SWCommandProcessor::RegisterMMIO(mmio, base);
}

// Draw messages on top of the screen
unsigned int VideoSoftware::PeekMessages()
{
	return GLInterface->PeekMessages();
}

// Show the current FPS
void VideoSoftware::UpdateFPSDisplay(const std::string& text)
{
	GLInterface->UpdateFPSDisplay(StringFromFormat("%s | Software | %s", scm_rev_str, text.c_str()));
}

}
