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
//  THE SHUTTER. By default a session integrates one frozen instant: every
//  sample is the same moment from a different point on the lens, so the only
//  thing varying is the aperture. That is the right answer for a locked-off
//  shot and it is what an add-on that knows nothing about the extension gets.
//
//  When the add-on drives the timed entry point, the clock is stepped per
//  sample too and one pass produces depth of field AND motion blur.
//
//  THE SHUTTER IS THE ADD-ON'S, ENTIRELY - there is no setting on this side and
//  deliberately no fallback. Placing samples in an exposure needs the sample
//  COUNT and the aperture ring each point came from, and neither is knowable
//  here: the total is never reported over the interface. Anything this side
//  could invent from a running index alone would be worse, and would need
//  guessing at when a burst had started to avoid walking the clock during
//  setup. The add-on has both facts already.
//
//  An earlier attempt at this was removed for looking wrong, and the reasons
//  are worth recording because none of them was the idea itself:
//
//    * Time advanced monotonically with the sample index, and the index runs
//      centre-outward through the aperture rings - so time was CORRELATED with
//      radius, and a moving object's trail came out as a radial sweep, tight
//      bokeh at one end and wide at the other. The add-on now stratifies the
//      interval and shuffles it against the point order.
//    * Setup moves were stepped as well, so dragging the bokeh-size slider
//      walked the clock forward with no way back. Gone with the running index:
//      each sample now carries its own absolute offset from t0.
//    * The blend can run before the seek has landed, which puts the previous
//      sample's instant into the accumulator and reads as a clean double image.
//      That one is not fixable here - it is Frames To Wait in the add-on's own
//      panel - so it is detected and named in the log instead of hidden.
//
//  The camera is deliberately NOT pinned while the clock moves: the spline
//  re-evaluates and carries it along its path, so a moving shot blurs as a
//  whole. The add-on's alignment cancels the aperture's parallax only, which is
//  exactly right - the aperture is the lens, the spline is the camera body, and
//  only the first is supposed to leave the focus plane sharp.
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

	// As requestMove, plus where in the exposure this sample belongs -
	// milliseconds after the instant the session opened. Only the add-on can
	// supply this; an untimed step leaves the clock alone.
	void requestMoveTimed(float stepLeftRight, float stepUpDown, float fovDegrees,
	                      bool fromStartPosition, float timeOffsetMs);

	// Has the frame on screen caught up with the last step we were handed?
	// Answers TRUE whenever the question is not ours - no session, or one
	// delegated to another camera tool - so it can never stall the add-on.
	bool sampleReady();

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
