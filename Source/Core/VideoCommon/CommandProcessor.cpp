// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Common/Atomic.h"
#include "Common/ChunkFile.h"
#include "Common/Common.h"
#include "Common/MathUtil.h"
#include "Common/Thread.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SystemTimers.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace CommandProcessor
{

int et_UpdateInterrupts;

// TODO(ector): Warn on bbox read/write

// STATE_TO_SAVE
// Note that gpuFifo == &cpuFifo except when bDeterministicGPUSync is on.
SCPFifoStruct cpuFifo, _gpuFifo;
SCPFifoStruct *gpuFifo;
UCPStatusReg m_CPStatusReg;
UCPCtrlReg  m_CPCtrlReg;
UCPClearReg m_CPClearReg;

u16 m_bboxleft;
u16 m_bboxtop;
u16 m_bboxright;
u16 m_bboxbottom;
u16 m_tokenReg;

volatile bool interruptSet= false;
volatile bool interruptWaiting= false;
volatile bool interruptTokenWaiting = false;
volatile bool gpuRunning = false;
u32 interruptTokenData;
volatile bool interruptFinishWaiting = false;
bool deterministicGPUSync = false;

volatile u32 VITicks = CommandProcessor::m_cpClockOrigin;

bool IsOnThread()
{
	return SConfig::GetInstance().m_LocalCoreStartupParameter.bCPUThread;
}

void UpdateInterrupts_Wrapper(u64 userdata, int cyclesLate)
{
	UpdateInterrupts(userdata);
}

void DoState(PointerWrap &p)
{
	p.DoPOD(m_CPStatusReg);
	p.DoPOD(m_CPCtrlReg);
	p.DoPOD(m_CPClearReg);
	p.Do(m_bboxleft);
	p.Do(m_bboxtop);
	p.Do(m_bboxright);
	p.Do(m_bboxbottom);
	p.Do(m_tokenReg);
	p.Do(cpuFifo);

	p.Do(interruptSet);
	p.Do(interruptWaiting);
	p.Do(interruptTokenWaiting);
	p.Do(interruptFinishWaiting);
}

inline void WriteLow (volatile u32& _reg, u16 lowbits)  {Common::AtomicStore(_reg,(_reg & 0xFFFF0000) | lowbits);}
inline void WriteHigh(volatile u32& _reg, u16 highbits) {Common::AtomicStore(_reg,(_reg & 0x0000FFFF) | ((u32)highbits << 16));}

inline u16 ReadLow  (u32 _reg)  {return (u16)(_reg & 0xFFFF);}
inline u16 ReadHigh (u32 _reg)  {return (u16)(_reg >> 16);}

void Init()
{
	m_CPStatusReg.Hex = 0;
	m_CPStatusReg.CommandIdle = 1;
	m_CPStatusReg.ReadIdle = 1;

	m_CPCtrlReg.Hex = 0;

	m_CPClearReg.Hex = 0;

	m_bboxleft = 0;
	m_bboxtop  = 0;
	m_bboxright = 640;
	m_bboxbottom = 480;

	m_tokenReg = 0;

	memset(&cpuFifo,0,sizeof(cpuFifo));
	cpuFifo.bFF_Breakpoint = 0;
	cpuFifo.bFF_HiWatermark = 0;
	cpuFifo.bFF_HiWatermarkInt = 0;
	cpuFifo.bFF_LoWatermark = 0;
	cpuFifo.bFF_LoWatermarkInt = 0;

	deterministicGPUSync = false;
	gpuFifo = &cpuFifo;
	UpdateDeterministicGPUSync();

	interruptSet = false;
	interruptWaiting = false;
	interruptFinishWaiting = false;
	interruptTokenWaiting = false;

	et_UpdateInterrupts = CoreTiming::RegisterEvent("CPInterrupt", UpdateInterrupts_Wrapper);
}

bool GPUHasWork()
{
	// In bDeterministicGPUSync mode, this is safe to call from SyncGPU, because:
	// - gpuFifo->bFF_GPReadEnable/CPWritePointer/CPBreakpoint only change later in SyncGPU.
	// - interruptWaiting *never* becomes true.
	// - No work is done between setting the read pointer and comparing it
	//   against CPWritePointer/CPBreakpoint.
	return gpuRunning &&
	       gpuFifo->bFF_GPReadEnable &&
	       !interruptWaiting &&
	       Common::AtomicLoad(gpuFifo->CPReadPointer) != Common::AtomicLoad(gpuFifo->CPWritePointer) &&
		   !AtBreakpointGpu();
}

static void SyncGPU()
{
	if (IsOnThread())
	{
		while (GPUHasWork())
			Common::YieldCPU();
	}
	if (deterministicGPUSync)
	{
		// need a barrier here for ARM
		if (interruptTokenWaiting)
		{
			PixelEngine::SetToken_OnMainThread(interruptTokenData, 0);
			interruptTokenData = 0;
		}
		if (interruptFinishWaiting)
		{
			PixelEngine::SetFinish_OnMainThread(0, 0);
		}
		cpuFifo.CPReadPointer = _gpuFifo.CPReadPointer;
		cpuFifo.SafeCPReadPointer = _gpuFifo.SafeCPReadPointer;
		_gpuFifo = cpuFifo;
		// need another barrier here
		SetCpStatus(true);
	}
}

void SyncGPUIfDeterministic()
{
	if (deterministicGPUSync)
	{
		SyncGPU();
	}
}

void UpdateDeterministicGPUSync()
{
	// This can change when we start and stop recording.
	int setting = Core::g_CoreStartupParameter.iDeterministicGPUSync;
	bool on;
	if (setting == 2)
	{
		on = Core::WantDeterminism();
	}
	else
	{
		on = setting == 1;
	}
	on = on && IsOnThread() && SConfig::GetInstance().m_LocalCoreStartupParameter.bSkipIdle;
	if (on != deterministicGPUSync)
	{
		if (gpuFifo)
		{
			SyncGPU();
			if (on)
			{
				// Might have async requests still waiting.
				CoreTiming::ProcessFifoWaitEvents();
			}
		}
		if (on)
		{
			_gpuFifo = cpuFifo;
			gpuFifo = &_gpuFifo;
		}
		else
		{
			gpuFifo = &cpuFifo;
		}
		deterministicGPUSync = on;
	}
}


bool IsPossibleWaitingSetDrawDone()
{
	// This is called from Idle.
	if (deterministicGPUSync)
	{
		// Time to sync.
		SyncGPU();
		return false;
	}
	else
	{
		return GPUHasWork();
	}
}

static u32 GetReadWriteDistance()
{
	u32 writePointer = cpuFifo.CPWritePointer;
	u32 readPointer = Common::AtomicLoad(cpuFifo.SafeCPReadPointer);
	u32 result = writePointer - readPointer;
	if (writePointer < readPointer)
		result += cpuFifo.CPEnd - cpuFifo.CPBase;

	if (deterministicGPUSync)
	{
		// Pretend we've advanced further.
		result = std::min(result, (u32) 32);
	}

	return result;
}

static u32 GetReadPointer()
{
	if (deterministicGPUSync)
	{
		u32 result = cpuFifo.CPWritePointer - GetReadWriteDistance();
		if (result < cpuFifo.CPBase)
			result += cpuFifo.CPEnd - cpuFifo.CPBase;
		return result;
	}
	else
	{
		return Common::AtomicLoad(cpuFifo.SafeCPReadPointer);
	}
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
	struct {
		u32 addr;
		u16* ptr;
		bool readonly;
		bool writes_align_to_32_bytes;
	} directly_mapped_vars[] = {
		{ FIFO_TOKEN_REGISTER, &m_tokenReg },

		// Bounding box registers are read only.
		{ FIFO_BOUNDING_BOX_LEFT, &m_bboxleft, true },
		{ FIFO_BOUNDING_BOX_RIGHT, &m_bboxright, true },
		{ FIFO_BOUNDING_BOX_TOP, &m_bboxtop, true },
		{ FIFO_BOUNDING_BOX_BOTTOM, &m_bboxbottom, true },

		// Some FIFO addresses need to be aligned on 32 bytes on write - only
		// the high part can be written directly without a mask.
		// FIFO_BASE can sync
		// FIFO_END can sync
		{ FIFO_HI_WATERMARK_LO, MMIO::Utils::LowPart(&cpuFifo.CPHiWatermark) },
		{ FIFO_HI_WATERMARK_HI, MMIO::Utils::HighPart(&cpuFifo.CPHiWatermark) },
		{ FIFO_LO_WATERMARK_LO, MMIO::Utils::LowPart(&cpuFifo.CPLoWatermark) },
		{ FIFO_LO_WATERMARK_HI, MMIO::Utils::HighPart(&cpuFifo.CPLoWatermark) },
		// FIFO_RW_DISTANCE has some complex read code different for
		// single/dual core.
		// FIFI_WRITE_POINTER can sync.
		// FIFO_READ_POINTER has different code for single/dual core.
		// FIFO_BP can sync.
	};
	for (auto& mapped_var : directly_mapped_vars)
	{
		u16 wmask = mapped_var.writes_align_to_32_bytes ? 0xFFE0 : 0xFFFF;
		mmio->Register(base | mapped_var.addr,
			MMIO::DirectRead<u16>(mapped_var.ptr),
			mapped_var.readonly
				? MMIO::InvalidWrite<u16>()
				: MMIO::DirectWrite<u16>(mapped_var.ptr, wmask)
		);
	}

	// Timing and metrics MMIOs are stubbed with fixed values.
	struct {
		u32 addr;
		u16 value;
	} metrics_mmios[] = {
		{ XF_RASBUSY_L, 0 },
		{ XF_RASBUSY_H, 0 },
		{ XF_CLKS_L, 0 },
		{ XF_CLKS_H, 0 },
		{ XF_WAIT_IN_L, 0 },
		{ XF_WAIT_IN_H, 0 },
		{ XF_WAIT_OUT_L, 0 },
		{ XF_WAIT_OUT_H, 0 },
		{ VCACHE_METRIC_CHECK_L, 0 },
		{ VCACHE_METRIC_CHECK_H, 0 },
		{ VCACHE_METRIC_MISS_L, 0 },
		{ VCACHE_METRIC_MISS_H, 0 },
		{ VCACHE_METRIC_STALL_L, 0 },
		{ VCACHE_METRIC_STALL_H, 0 },
		{ CLKS_PER_VTX_OUT, 4 },
	};
	for (auto& metrics_mmio : metrics_mmios)
	{
		mmio->Register(base | metrics_mmio.addr,
			MMIO::Constant<u16>(metrics_mmio.value),
			MMIO::InvalidWrite<u16>()
		);
	}

	mmio->Register(base | STATUS_REGISTER,
		MMIO::ComplexRead<u16>([](u32) {
			SetCpStatusRegister();
			return m_CPStatusReg.Hex;
		}),
		MMIO::InvalidWrite<u16>()
	);

	mmio->Register(base | CTRL_REGISTER,
		MMIO::DirectRead<u16>(&m_CPCtrlReg.Hex),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			UCPCtrlReg tmp(val);
			m_CPCtrlReg.Hex = tmp.Hex;
			SetCpControlRegister();
			if (!IsOnThread())
				RunGpu();
		})
	);

	mmio->Register(base | CLEAR_REGISTER,
		MMIO::DirectRead<u16>(&m_CPClearReg.Hex),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			UCPClearReg tmp(val);
			m_CPClearReg.Hex = tmp.Hex;
			SetCpClearRegister();
			if (!IsOnThread())
				RunGpu();
		})
	);

	mmio->Register(base | PERF_SELECT,
		MMIO::InvalidRead<u16>(),
		MMIO::Nop<u16>()
	);

	// Some MMIOs have different handlers for single core vs. dual core mode.
	mmio->Register(base | FIFO_RW_DISTANCE_LO,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadLow(GetReadWriteDistance());
		}),
		MMIO::DirectWrite<u16>(MMIO::Utils::LowPart(&cpuFifo.CPReadWriteDistance), 0xFFE0)
	);
	mmio->Register(base | FIFO_RW_DISTANCE_HI,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadHigh(GetReadWriteDistance());
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteHigh((u32 &)cpuFifo.CPReadWriteDistance, val);
			SyncGPU();
			if (val == 0)
			{
				GPFifo::ResetGatherPipe();
				ResetVideoBuffer();
			}
			else
			{
				ResetVideoBuffer();
			}
			if (!IsOnThread())
				RunGpu();
		})
	);
	mmio->Register(base | FIFO_READ_POINTER_LO,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadLow(GetReadPointer());
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			SyncGPUIfDeterministic();
			WriteLow((u32 &)cpuFifo.CPReadPointer, val & 0xFFE0);
			gpuFifo->CPReadPointer = cpuFifo.CPReadPointer;
		})
	);
	mmio->Register(base | FIFO_READ_POINTER_HI,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadHigh(GetReadPointer());
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			SyncGPUIfDeterministic();
			WriteHigh((u32 &)cpuFifo.CPReadPointer, val);
			cpuFifo.SafeCPReadPointer = cpuFifo.CPReadPointer;
			gpuFifo->CPReadPointer = cpuFifo.CPReadPointer;
			gpuFifo->SafeCPReadPointer = cpuFifo.SafeCPReadPointer;
		})
	);

	mmio->Register(base | FIFO_BASE_LO,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadLow(cpuFifo.CPBase);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteLow((u32 &)cpuFifo.CPBase, val & 0xFFE0);
			SyncGPUIfDeterministic();
		})
	);

	mmio->Register(base | FIFO_BASE_HI,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadHigh(cpuFifo.CPBase);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteHigh((u32 &)cpuFifo.CPBase, val);
			SyncGPUIfDeterministic();
		})
	);

	mmio->Register(base | FIFO_END_LO,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadLow(cpuFifo.CPEnd);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteLow((u32 &)cpuFifo.CPEnd, val & 0xFFE0);
			SyncGPUIfDeterministic();
		})
	);

	mmio->Register(base | FIFO_END_HI,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadHigh(cpuFifo.CPEnd);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteHigh((u32 &)cpuFifo.CPEnd, val);
			SyncGPUIfDeterministic();
		})
	);

	mmio->Register(base | FIFO_WRITE_POINTER_LO,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadLow(cpuFifo.CPWritePointer);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteLow((u32 &)cpuFifo.CPWritePointer, val & 0xFFE0);
			SyncGPUIfDeterministic();
		})
	);

	mmio->Register(base | FIFO_WRITE_POINTER_HI,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadHigh(cpuFifo.CPWritePointer);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteHigh((u32 &)cpuFifo.CPWritePointer, val);
			SyncGPUIfDeterministic();
		})
	);

	mmio->Register(base | FIFO_BP_LO,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadLow(cpuFifo.CPBreakpoint);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteLow((u32 &)cpuFifo.CPBreakpoint, val & 0xFFE0);
			SyncGPUIfDeterministic();
		})
	);

	mmio->Register(base | FIFO_BP_HI,
		MMIO::ComplexRead<u16>([](u32) {
			return ReadHigh(cpuFifo.CPBreakpoint);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			WriteHigh((u32 &)cpuFifo.CPBreakpoint, val);
			SyncGPUIfDeterministic();
		})
	);
}

void STACKALIGN GatherPipeBursted()
{
	ProcessFifoEvents();
	// if we aren't linked, we don't care about gather pipe data
	if (!m_CPCtrlReg.GPLinkEnable)
	{
		if (!IsOnThread())
		{
			RunGpu();
		}
		else
		{
			// In multibuffer mode is not allowed write in the same FIFO attached to the GPU.
			// Fix Pokemon XD in DC mode.
			if (ProcessorInterface::Fifo_CPUEnd == cpuFifo.CPEnd && ProcessorInterface::Fifo_CPUBase == cpuFifo.CPBase)
			{
				SyncGPU();
			}
		}
		return;
	}

	u32 newPointer = cpuFifo.CPWritePointer;

	// update the fifo pointer
	if (newPointer >= cpuFifo.CPEnd)
		newPointer = cpuFifo.CPBase;
	else
		newPointer += GATHER_PIPE_SIZE;

	if (newPointer == cpuFifo.CPReadPointer)
	{
		if (deterministicGPUSync)
		{
			SyncGPU();
		}
		else
		{
			_assert_msg_(COMMANDPROCESSOR, false, "FIFO overflow");
		}
	}

	Common::AtomicStore(gpuFifo->CPWritePointer, newPointer);
	Common::AtomicStore(cpuFifo.CPWritePointer, newPointer);

	if (!IsOnThread())
		RunGpu();

	if (!deterministicGPUSync)
	{
		SetCpStatus(true);
	}

	// check if we are in sync
	_assert_msg_(COMMANDPROCESSOR, cpuFifo.CPWritePointer == ProcessorInterface::Fifo_CPUWritePointer, "FIFOs linked but out of sync");
	_assert_msg_(COMMANDPROCESSOR, cpuFifo.CPBase         == ProcessorInterface::Fifo_CPUBase, "FIFOs linked but out of sync");
	_assert_msg_(COMMANDPROCESSOR, cpuFifo.CPEnd          == ProcessorInterface::Fifo_CPUEnd, "FIFOs linked but out of sync");
}

void UpdateInterrupts(u64 userdata)
{
	if (userdata)
	{
		interruptSet = true;
		INFO_LOG(COMMANDPROCESSOR,"Interrupt set");
		ProcessorInterface::SetInterrupt(INT_CAUSE_CP, true);
	}
	else
	{
		interruptSet = false;
		INFO_LOG(COMMANDPROCESSOR,"Interrupt cleared");
		ProcessorInterface::SetInterrupt(INT_CAUSE_CP, false);
	}
	interruptWaiting = false;
}

void UpdateInterruptsFromVideoBackend(u64 userdata)
{
	CoreTiming::ScheduleEvent_Threadsafe(0, et_UpdateInterrupts, userdata);
}

void SetCpStatus(bool isCPUThread)
{
	if (deterministicGPUSync)
	{
		// We don't care.
		cpuFifo.bFF_HiWatermark = 0;
		cpuFifo.bFF_LoWatermark = 0;
	}
	else
	{
		// overflow & underflow check
		u32 distance = GetReadWriteDistance();
		cpuFifo.bFF_HiWatermark = (distance > cpuFifo.CPHiWatermark);
		cpuFifo.bFF_LoWatermark = (distance < cpuFifo.CPLoWatermark);
	}

	// breakpoint
	if (cpuFifo.bFF_BPEnable && cpuFifo.CPBreakpoint == cpuFifo.CPReadPointer)
	{
		if (!cpuFifo.bFF_Breakpoint)
		{
			INFO_LOG(COMMANDPROCESSOR, "Hit breakpoint at %i", cpuFifo.CPReadPointer);
			cpuFifo.bFF_Breakpoint = true;
		}
	}
	else
	{
		if (cpuFifo.bFF_Breakpoint)
			INFO_LOG(COMMANDPROCESSOR, "Cleared breakpoint at %i", cpuFifo.CPReadPointer);
		cpuFifo.bFF_Breakpoint = false;
	}

	bool bpInt = cpuFifo.bFF_Breakpoint && cpuFifo.bFF_BPInt;
	bool ovfInt = cpuFifo.bFF_HiWatermark && cpuFifo.bFF_HiWatermarkInt;
	bool undfInt = cpuFifo.bFF_LoWatermark && cpuFifo.bFF_LoWatermarkInt;

	bool interrupt = (bpInt || ovfInt || undfInt) && m_CPCtrlReg.GPReadEnable;

	if (interrupt != interruptSet && !interruptWaiting)
	{
		u64 userdata = interrupt ? 1 : 0;
		if (!isCPUThread)
		{
			// GPU thread:
			interruptWaiting = true;
			CommandProcessor::UpdateInterruptsFromVideoBackend(userdata);
		}
		else
		{
			// CPU thread:
			CommandProcessor::UpdateInterrupts(userdata);
		}
	}
}

void ProcessFifoEvents()
{
	if (IsOnThread() && !deterministicGPUSync &&
	    (interruptWaiting || interruptFinishWaiting || interruptTokenWaiting))
		CoreTiming::ProcessFifoWaitEvents();
}

void Shutdown()
{

}

void SetCpStatusRegister()
{
	// Here always there is one fifo attached to the GPU
	m_CPStatusReg.Breakpoint = cpuFifo.bFF_Breakpoint;
	m_CPStatusReg.ReadIdle = cpuFifo.CPReadPointer == cpuFifo.CPWritePointer || AtBreakpointCpu();
	m_CPStatusReg.CommandIdle = cpuFifo.CPReadPointer == cpuFifo.CPWritePointer || AtBreakpointCpu() || !cpuFifo.bFF_GPReadEnable;
	m_CPStatusReg.UnderflowLoWatermark = cpuFifo.bFF_LoWatermark;
	m_CPStatusReg.OverflowHiWatermark = cpuFifo.bFF_HiWatermark;

	INFO_LOG(COMMANDPROCESSOR,"\t Read from STATUS_REGISTER : %04x", m_CPStatusReg.Hex);
	DEBUG_LOG(COMMANDPROCESSOR, "(r) status: iBP %s | fReadIdle %s | fCmdIdle %s | iOvF %s | iUndF %s"
		, m_CPStatusReg.Breakpoint           ? "ON" : "OFF"
		, m_CPStatusReg.ReadIdle             ? "ON" : "OFF"
		, m_CPStatusReg.CommandIdle          ? "ON" : "OFF"
		, m_CPStatusReg.OverflowHiWatermark  ? "ON" : "OFF"
		, m_CPStatusReg.UnderflowLoWatermark ? "ON" : "OFF"
			);
}

void SetCpControlRegister()
{
	// If the new fifo is being attached, force an exception check
	// This fixes the hang while booting Eternal Darkness
	if (!cpuFifo.bFF_GPReadEnable && m_CPCtrlReg.GPReadEnable && !m_CPCtrlReg.BPEnable)
	{
		CoreTiming::ForceExceptionCheck(0);
	}

	cpuFifo.bFF_BPInt = m_CPCtrlReg.BPInt;
	cpuFifo.bFF_BPEnable = m_CPCtrlReg.BPEnable;
	cpuFifo.bFF_HiWatermarkInt = m_CPCtrlReg.FifoOverflowIntEnable;
	cpuFifo.bFF_LoWatermarkInt = m_CPCtrlReg.FifoUnderflowIntEnable;
	cpuFifo.bFF_GPLinkEnable = m_CPCtrlReg.GPLinkEnable;

	if (m_CPCtrlReg.GPReadEnable && m_CPCtrlReg.GPLinkEnable)
	{
		ProcessorInterface::Fifo_CPUWritePointer = cpuFifo.CPWritePointer;
		ProcessorInterface::Fifo_CPUBase = cpuFifo.CPBase;
		ProcessorInterface::Fifo_CPUEnd = cpuFifo.CPEnd;
	}

	cpuFifo.bFF_GPReadEnable = m_CPCtrlReg.GPReadEnable;
	SyncGPU();
	// Safe because nothing has been scheduled since the SyncGPU.
	SetCpStatus(true);

	DEBUG_LOG(COMMANDPROCESSOR, "\t GPREAD %s | BP %s | Int %s | OvF %s | UndF %s | LINK %s"
		, cpuFifo.bFF_GPReadEnable           ? "ON" : "OFF"
		, cpuFifo.bFF_BPEnable               ? "ON" : "OFF"
		, cpuFifo.bFF_BPInt                  ? "ON" : "OFF"
		, m_CPCtrlReg.FifoOverflowIntEnable  ? "ON" : "OFF"
		, m_CPCtrlReg.FifoUnderflowIntEnable ? "ON" : "OFF"
		, m_CPCtrlReg.GPLinkEnable           ? "ON" : "OFF"
		);

}

// NOTE: We intentionally don't emulate this function at the moment.
// We don't emulate proper GP timing anyway at the moment, so it would just slow down emulation.
void SetCpClearRegister()
{
}

void Update()
{
	// called only when bSyncGPU is true
	while (VITicks > m_cpClockOrigin && GPUHasWork() && IsOnThread())
		Common::YieldCPU();

	if (GPUHasWork())
		Common::AtomicAdd(VITicks, SystemTimers::GetTicksPerSecond() / 10000);
}
} // end of namespace CommandProcessor
