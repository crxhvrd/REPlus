// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdint>

// =============================================================================
//  Accumulated depth-of-field sessions, driven from ReShade
// =============================================================================
//  The capture add-on we already require for rendering also contains a
//  depth-of-field controller: it walks the camera around a lens aperture, one
//  step per frame, and blends the results in its own shader. That produces real
//  optical bokeh rather than a screen-space approximation.
//
//  It has always been dead weight here, because it drives the camera through
//  the IGCS camera-tools interface and nothing in the process implements that
//  while the Rockstar Editor is up. The add-on finds camera tools by walking the
//  loaded modules and taking the first one that exports
//  IGCS_StartScreenshotSession - it does not care WHICH module that is, and an
//  .asi is a module like any other. So implementing four functions is the whole
//  integration.
//
//  STATIC SHOTS ONLY, deliberately. The replay clock is never moved: every
//  sample sees the same frozen instant from a different point on the lens, so
//  the only thing varying across the accumulation is the aperture.
//
//  This did once step the clock per sample as well, which made the same pass
//  integrate over a shutter interval and produced depth of field and motion
//  blur in one image. It worked, and it was still the wrong tool. A moving
//  subject already picks up blur from the frames a session accumulates; the
//  mod's own renderer does motion blur properly, with an exact shutter and
//  without competing for the sample budget; and blur smears the bokeh discs,
//  which is the one thing this path exists to produce. Timing belongs to the
//  renderer, bokeh belongs here.
//
//  THREADING. Everything the add-on calls arrives on ReShade's present thread.
//  Seeking the replay and writing the camera are main-thread-only, for the same
//  reason render::pump() exists. So the exported functions record intent and
//  return; pump() and applyToFrame() do the work from the UpdateSmoothing
//  detour. The add-on's own frame-wait covers the one-frame latency this adds.
// =============================================================================
namespace dofsession
{
	// --- called from the IGCS exports, on ReShade's thread --------------------
	// These validate and record; they never touch the game.

	// Mirrors ScreenshotSessionStartReturnCode: 0 ok, 1 camera not enabled,
	// 2 a path is playing, 3 already in a session, 4 feature not available.
	int  requestStart(uint8_t type);
	void requestMove(float stepLeftRight, float stepUpDown, float fovDegrees,
	                 bool fromStartPosition);
	void requestPanorama(float stepAngle);
	void requestEnd();

	// --- main thread ----------------------------------------------------------

	// A session is running. The UI suppression and the render's own "do not
	// start" checks both key off this.
	bool active();

	// One step: performs any deferred start/end, seeks the clock for a new
	// sample, and re-asserts the hidden HUD. Call once per frame from the
	// UpdateSmoothing detour, beside render::pump().
	void pump();

	// Offset the camera onto the aperture. Call AFTER the spline and the shake -
	// this has to be the last writer, or the editor's own camera update puts it
	// back.
	void applyToFrame(void* director);
}
