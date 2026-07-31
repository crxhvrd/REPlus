// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// =============================================================================
//  Image-sequence renderer
// =============================================================================
//  Renders the clip to a numbered PNG/JPEG sequence through the capture
//  channel, one output frame at a time. No ffmpeg, no encoder, no dependency on
//  the game's own exporter - which also means no watermark and no profanity
//  gate, because that pipeline is never entered.
//
//  WHY THIS IS EXACT, unlike the free-roam version of the same idea. Simple
//  Camera has no seekable clock, so it chases a moving playhead with
//  SET_TIME_SCALE slow motion and a self-correcting controller, and lives with
//  the overshoot. The Rockstar Editor has a real timeline: we pause playback
//  and JUMP to start + frame*dt. Sub-samples are jumps to t + k*(dt*shutter/N).
//  The shutter is therefore mathematically exact, there is no drift to correct,
//  and wall-clock speed does not affect the result at all.
//
//  WHY IT IS A STATE MACHINE and not a loop. We have no ScriptHookV, so there
//  is no WAIT(0) to yield on. Instead pump() is called once per frame from the
//  UpdateSmoothing detour - the game's main thread, which is the only thread
//  where jumping the replay is safe - and each call advances one step. "A frame
//  was presented" is read from the addon's heartbeat.
// =============================================================================
namespace render
{
	struct Settings
	{
		float fps          = 60.0f;
		int   samples      = 1;      // motion-blur sub-samples per output frame
		float shutter      = 0.5f;   // fraction of the frame interval the shutter is open
		int   settleFrames = 3;      // frames to let the game render after a jump
		int   settleSubFrames = 1;   // ...between motion-blur sub-samples (tiny seeks)
		bool  jpeg         = false;  // false = PNG
		int   quality      = 90;     // JPEG only
		float highlight    = 0.0f;   // highlight lift during blur accumulation
		int   channelOrder = 0;      // 0 auto / 1 RGBA / 2 BGRA
	};

	Settings& settings();

	// Copy the persisted values out of the ini. Call once at startup.
	void applyConfig();

	// Begin a render across the marker range. Returns false with a reason when
	// it cannot start (no addon, not in the editor, fewer than two markers).
	bool start(const char** reason);

	// Begin at the current playhead and run until the replay clock stops
	// following our seeks - used by the Export path, where the range is the
	// whole project and no duration accessor has been resolved.
	bool startOpenEnded(const char** reason);

	void cancel();

	bool  active();
	int   frameCount();
	int   frameDone();
	const char* outputFolder();

	// Install the spinner suppression hook. Call once at startup.
	void installHooks();

	// One step. Call once per frame from the main thread.
	void pump();
}
