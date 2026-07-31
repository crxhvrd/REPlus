// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// =============================================================================
//  Audio capture for the real-time pass
// =============================================================================
//  The frame render cannot produce sound: it freezes the clock and seeks, and
//  the audio engine only behaves while time actually runs. So a render that
//  wants audio plays the project through ONCE at normal speed with this
//  recording, then does the frame pass and muxes the result.
//
//  The capture is WASAPI process loopback aimed at our own process id, so it
//  records what the GAME outputs and nothing else - Discord, a browser or music
//  playing alongside do not end up in the render.
// =============================================================================
namespace audioout
{
	// Starts recording into <folder>\audio.wav. False if the device could not
	// be opened, in which case the caller should carry on without audio rather
	// than fail the render.
	bool begin(const char* folder);

	// Open the gate to record, close it to drop samples.
	//
	// This is what keeps the two passes aligned. A real-time playthrough stops
	// for a genuine load at every clip boundary, while the frame pass holds
	// through it and the video has no gap there - so recording those seconds
	// would push everything after the first clip out of sync by however long
	// the load took. Dropping them instead makes the wav match the timeline the
	// frames describe.
	void gate(bool open);

	// Stops the thread and patches the wav header with the real sizes.
	void end();

	bool active();

	// Seconds of audio actually written, ignoring anything dropped by the gate.
	// The render stops the pass when this reaches the video's own length, which
	// is why a load pause cannot shorten the result - it just takes longer.
	double seconds();

	// Path to the finished wav, or "" if nothing usable was recorded.
	const char* path();
}
