// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "AudioCommon/NullSoundStream.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/SystemTimers.h"

void NullSound::SoundLoop()
{
}

bool NullSound::Start()
{
	return true;
}

void NullSound::SetVolume(int volume)
{
}

void NullSound::Update()
{
	m_mixer->Mix(realtimeBuffer, m_mixer->GetAvailableSamples());
}

void NullSound::Clear(bool mute)
{
	m_muted = mute;
}

void NullSound::Stop()
{
}
