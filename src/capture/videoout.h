// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// =============================================================================
//  Video output — the finished frames, streamed into ffmpeg
// =============================================================================
//  The renderer already produces exactly the frames a video wants: paused
//  playback, an exact seek per sub-sample, and the addon accumulating them into
//  one output frame. All that was ever missing is an encoder on the end.
//
//  This does NOT change how frames are captured. The addon still writes
//  frame_%06d.png; we hand each completed one to ffmpeg's stdin and delete it.
//  Two consequences worth stating, because they are why this shape was chosen:
//
//    * NO ADDON CHANGES. The shared-memory protocol, the accumulation and the
//      highlight boost all stay exactly as they are - which matters, because
//      that half is the part that already works on both game builds.
//    * NO DISK BLOW-UP. Frames are consumed as they appear rather than piling
//      up, so a ten-minute render costs a couple of files at a time instead of
//      eighteen thousand.
//
//  It is deliberately a child process rather than linked libavcodec: no new
//  build dependency, the encoder is swappable from the ini, and an ffmpeg that
//  dies cannot take the game with it.
// =============================================================================
namespace videoout
{
	// Overrides AudioFromFile for the next encode. Set by the render when a
	// real-time audio pass has produced a wav of its own.
	void setAudio(const char* path);

	// True when a render should also produce a video (the ini asked for one and
	// ffmpeg was found). Cheap; safe to call every frame.
	bool active();

	// Starts ffmpeg for a render. `folder` is the render's output directory,
	// used to place the video beside the frames. Returns false and logs if
	// ffmpeg could not be started - the caller carries on writing frames, so a
	// missing encoder costs the video, not the render.
	bool begin(const char* folder, float fps);

	// Hands one finished frame to the encoder. Called at the moment the addon
	// has written it, i.e. after the last sub-sample of that output frame.
	// Deletes the file afterwards unless KeepFrames is set.
	void pushFrame(const char* path);

	// Closes ffmpeg's stdin and waits for it to finish writing the container.
	// `complete` is false when the render aborted - the partial video is still
	// finalised, since a truncated-but-playable file beats a corrupt one.
	void end(bool complete);
}
