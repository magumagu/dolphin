// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official Git repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

/*
Here is a nice ascii overview of audio flow affected by this file:

(RAM)---->[AI FIFO]---->[SRC]---->[Mixer]---->[DAC]---->(Speakers)
                          ^
                          |
                      [L/R Volume]
                           \
(DVD)---->[Drive I/F]---->[SRC]---->[Counter]

Notes:
Output at "48KHz" is actually 48043Hz.
Sample counter counts streaming stereo samples after upsampling.
[DAC] causes [AI I/F] to read from RAM at rate selected by AIDFR.
Each [SRC] will upsample a 32KHz source, or pass through the 48KHz
  source. The [Mixer]/[DAC] only operate at 48KHz.

AIS == disc streaming == DTK(Disk Track Player) == streaming audio, etc.

Supposedly, the retail hardware only supports 48KHz streaming from
  [Drive I/F]. However it's more likely that the hardware supports
  32KHz streaming, and the upsampling is transparent to the user.
  TODO check if anything tries to stream at 32KHz.

The [Drive I/F] actually supports simultaneous requests for audio and
  normal data. For this reason, we can't really get rid of the crit section.

IMPORTANT:
This file mainly deals with the [Drive I/F], however [AIDFR] controls
  the rate at which the audio data is DMA'd from RAM into the [AI FIFO]
  (and the speed at which the FIFO is read by its SRC). Everything else
  relating to AID happens in DSP.cpp. It's kinda just bad hardware design.
  TODO maybe the files should be merged?
*/

#include "AudioCommon/AudioCommon.h"

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "Core/CoreTiming.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DVDInterface.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/StreamADPCM.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"

namespace AudioInterface
{

// Internal hardware addresses
enum
{
	AI_CONTROL_REGISTER = 0x6C00,
	AI_VOLUME_REGISTER  = 0x6C04,
	AI_SAMPLE_COUNTER   = 0x6C08,
	AI_INTERRUPT_TIMING = 0x6C0C,
};

enum
{
	AIS_32KHz = 0,
	AIS_48KHz = 1,

	AID_32KHz = 1,
	AID_48KHz = 0
};

// AI Control Register
union AICR
{
	AICR() { hex = 0;}
	AICR(u32 _hex) { hex = _hex;}
	struct
	{
		u32 PSTAT    : 1;  // sample counter/playback enable
		u32 AISFR    : 1;  // AIS Frequency (0=32khz 1=48khz)
		u32 AIINTMSK : 1;  // 0=interrupt masked 1=interrupt enabled
		u32 AIINT    : 1;  // audio interrupt status
		u32 AIINTVLD : 1;  // This bit controls whether AIINT is affected by the Interrupt Timing register
		                      // matching the sample counter. Once set, AIINT will hold its last value
		u32 SCRESET  : 1;  // write to reset counter
		u32 AIDFR    : 1;  // AID Frequency (0=48khz 1=32khz)
		u32          :25;
	};
	u32 hex;
};

// AI Volume Register
union AIVR
{
	AIVR() { hex = 0;}
	struct
	{
		u32 left  : 8;
		u32 right : 8;
		u32       :16;
	};
	u32 hex;
};

// STATE_TO_SAVE
// Registers
static AICR m_Control;
static AIVR m_Volume;
static u32 m_SampleCounter = 0;
static u32 m_InterruptTiming = 0;

static u64 g_LastCPUTime = 0;
static u64 g_CPUCyclesPerSample = 0xFFFFFFFFFFFULL;

static u32 g_samples_to_emit = 0;

static unsigned int g_AISSampleRate = 48000;
static unsigned int g_AIDSampleRate = 32000;

void DoState(PointerWrap &p)
{
	p.DoPOD(m_Control);
	p.DoPOD(m_Volume);
	p.Do(m_SampleCounter);
	p.Do(m_InterruptTiming);
	p.Do(g_LastCPUTime);
	p.Do(g_AISSampleRate);
	p.Do(g_AIDSampleRate);
	p.Do(g_CPUCyclesPerSample);
	p.Do(g_samples_to_emit);
}

static void UpdateInterrupts();
int et_AI;

void UpdateCallback(u64 userdata, int cycles_late);
void UpdateSamples(bool schedule_event, int cycles_late);

void Init()
{
	m_Control.hex = 0;
	m_Control.AISFR = AIS_48KHz;
	m_Volume.hex = 0;
	m_SampleCounter = 0;
	m_InterruptTiming = 0;

	g_LastCPUTime = 0;
	g_CPUCyclesPerSample = 0xFFFFFFFFFFFULL;

	g_samples_to_emit = 0;

	g_AISSampleRate = 48000;
	g_AIDSampleRate = 32000;

	et_AI = CoreTiming::RegisterEvent("AICallback", UpdateCallback);
}

void Shutdown()
{
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
	mmio->Register(base | AI_CONTROL_REGISTER,
		MMIO::DirectRead<u32>(&m_Control.hex),
		MMIO::ComplexWrite<u32>([](u32, u32 val) {
			AICR tmpAICtrl(val);

			m_Control.AIINTMSK = tmpAICtrl.AIINTMSK;
			m_Control.AIINTVLD = tmpAICtrl.AIINTVLD;

			// Set frequency of streaming audio
			if (tmpAICtrl.AISFR != m_Control.AISFR)
			{
				DEBUG_LOG(AUDIO_INTERFACE, "Change AISFR to %s", tmpAICtrl.AISFR ? "48khz":"32khz");
				m_Control.AISFR = tmpAICtrl.AISFR;
			}
			// Set frequency of DMA
			if (tmpAICtrl.AIDFR != m_Control.AIDFR)
			{
				DEBUG_LOG(AUDIO_INTERFACE, "Change AIDFR to %s", tmpAICtrl.AIDFR ? "32khz":"48khz");
				m_Control.AIDFR = tmpAICtrl.AIDFR;
			}

			g_AISSampleRate = tmpAICtrl.AISFR ? 48000 : 32000;
			g_AIDSampleRate = tmpAICtrl.AIDFR ? 32000 : 48000;

			g_CPUCyclesPerSample = SystemTimers::GetTicksPerSecond() / g_AISSampleRate;

			// Streaming counter
			if (tmpAICtrl.PSTAT != m_Control.PSTAT)
			{
				DEBUG_LOG(AUDIO_INTERFACE, "%s streaming audio", tmpAICtrl.PSTAT ? "start":"stop");
				m_Control.PSTAT = tmpAICtrl.PSTAT;
				g_LastCPUTime = CoreTiming::GetTicks();

				if (m_Control.PSTAT)
				{
					NGCADPCM::InitFilter();
					UpdateSamples(true, 0);
				}
				else
				{
					CoreTiming::RemoveEvent(et_AI);
					g_samples_to_emit = 0;
				}
			}

			// AI Interrupt
			if (tmpAICtrl.AIINT)
			{
				DEBUG_LOG(AUDIO_INTERFACE, "Clear AIS Interrupt");
				m_Control.AIINT = 0;
			}

			// Sample Count Reset
			if (tmpAICtrl.SCRESET)
			{
				DEBUG_LOG(AUDIO_INTERFACE, "Reset AIS sample counter");
				m_SampleCounter = 0;

				g_LastCPUTime = CoreTiming::GetTicks();
			}

			UpdateInterrupts();
		})
	);

	mmio->Register(base | AI_VOLUME_REGISTER,
		MMIO::DirectRead<u32>(&m_Volume.hex),
		MMIO::DirectWrite<u32>(&m_Volume.hex)
	);

	mmio->Register(base | AI_SAMPLE_COUNTER,
		MMIO::ComplexRead<u32>([](u32) {
			if (m_Control.PSTAT)
			{
				UpdateSamples(false, 0);
			}
			return m_SampleCounter;
		}),
		MMIO::DirectWrite<u32>(&m_SampleCounter)
	);

	mmio->Register(base | AI_INTERRUPT_TIMING,
		MMIO::DirectRead<u32>(&m_InterruptTiming),
		MMIO::DirectWrite<u32>(&m_InterruptTiming)
	);
}

static void UpdateInterrupts()
{
	ProcessorInterface::SetInterrupt(
		ProcessorInterface::INT_CAUSE_AI, m_Control.AIINT & m_Control.AIINTMSK);
}

static void GenerateAudioInterrupt()
{
	m_Control.AIINT = 1;
	UpdateInterrupts();
}

static void IncreaseSampleCount(const u32 amount)
{
	m_SampleCounter += amount;
	if (m_Control.AIINTVLD && (m_SampleCounter >= m_InterruptTiming))
	{
		GenerateAudioInterrupt();
	}
}

unsigned int GetAIDSampleRate()
{
	return g_AIDSampleRate;
}

void UpdateCallback(u64 userdata, int cyclesLate)
{
	UpdateSamples(true, cyclesLate);
}

void UpdateSamples(bool schedule_next_event, int cycles_late)
{
	const u64 elapsed_time = CoreTiming::GetTicks() - g_LastCPUTime;
	const u32 samples_available = static_cast<u32>(elapsed_time / g_CPUCyclesPerSample);

	g_LastCPUTime += samples_available * g_CPUCyclesPerSample;
	IncreaseSampleCount(samples_available);
	g_samples_to_emit += samples_available;

	// Read and decode samples
	static const int MAX_SAMPLES_TO_DECODE = 48000 / 1000 * 5;
	short tempPCM[MAX_SAMPLES_TO_DECODE * 2];
	unsigned samples_processed = 0;
	while (g_samples_to_emit > NGCADPCM::SAMPLES_PER_BLOCK &&
		samples_processed < MAX_SAMPLES_TO_DECODE - NGCADPCM::SAMPLES_PER_BLOCK)
	{
		u8 tempADPCM[NGCADPCM::ONE_BLOCK_SIZE];
		u32 samples_read = DVDInterface::DVDReadAudio(tempADPCM, sizeof(tempADPCM));
		if (samples_read != sizeof(tempADPCM))
		{
			if (!m_Control.AIINTVLD)
				GenerateAudioInterrupt();
			break;
		}

		NGCADPCM::DecodeBlock(tempPCM + samples_processed * 2, tempADPCM);
		samples_processed += NGCADPCM::SAMPLES_PER_BLOCK;
		g_samples_to_emit -= NGCADPCM::SAMPLES_PER_BLOCK;
	}

	// Send samples to mixer
	for (unsigned i = 0; i < samples_processed * 2; ++i)
	{
		// TODO: Fix the mixer so it can accept non-byte-swapped samples.
		tempPCM[i] = Common::swap16(tempPCM[i]);
	}
	soundStream->GetMixer()->PushStreamingSamples(tempPCM, samples_processed);

	// Schedule the next audio event.
	if (schedule_next_event)
	{
		int ticks_to_dtk = int(SystemTimers::GetTicksPerSecond() / 2000 * 5);
		CoreTiming::ScheduleEvent(ticks_to_dtk - cycles_late, et_AI);
	}
}

} // end of namespace AudioInterface
