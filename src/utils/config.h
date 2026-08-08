// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <Windows.h>
#include <string>
#include "replay/shake.h"
#include "utils/paths.h"
#include "utils/log.h"   // migrateRenderIni logs; do not rely on the .cpp's include order
#include "game/signatures.h"   // REPLAY_BLOCKS_MIN/MAX - clamp against the engine's own range
#include <cstdlib>

// RockstarEditorPlus.ini, read once from beside the .asi.
struct Config
{
	// Master switch.
	bool  enabled          = true;

	// Replace the marker-to-marker position path with a Catmull-Rom spline.
	// This is the actual fix; there is little reason to turn it off except to
	// A/B against stock.
	bool  splinePosition   = true;

	// Replace camera orientation with a squad spline over the marker
	// quaternions.
	//
	// The basis convention is NO LONGER an assumption - verified 2026-07-27
	// against camReplayDirector::ApplyRotationalDampingToMatrix and the
	// matrix->Euler helper it calls. The game extracts
	//   heading = atan2(-b.x, b.y),  pitch = asin(b.z),  roll = atan2(-a.z, c.z)
	// so heading and pitch come from row b alone - those are exactly the
	// spherical angles of a forward vector - and roll is measured off rows a
	// and c. That pins a=right, b=forward, c=up, Z-up.
	//
	// rquat::toMatrixRows writes the three COLUMNS of the quaternion rotation
	// matrix, i.e. the rotated X/Y/Z axes, into a/b/c - the same convention.
	// Checked algebraically on the pure-yaw case: for q=(0,0,sin(h/2),cos(h/2))
	// it gives b=(-sin h, cos h, 0), and the game's atan2(-b.x, b.y) returns
	// exactly h.
	//
	// ON by default. Rotation is half of what makes a marker-to-marker move
	// read as smooth - splining the path but leaving the stock orientation
	// blend still kicks at every keyframe, which is the thing this mod exists
	// to fix. Set to 0 to A/B against stock.
	bool  splineOrientation = true;

	// Replace the stock FOV blend with a monotone curve through the marker FOVs.
	//
	// Stock is `fov += (target - fov) * t`, a two-point lerp written at the very
	// end of UpdateSmoothing - the same defect the path had, so a push-in kicks
	// at every keyframe even once the path and rotation glide through it.
	//
	// On by default, for the same reason SplinePosition and SplineOrientation
	// are: it is the defect this mod exists to remove, just on a third channel.
	bool  splineFov = true;

	// Spline shots that are ATTACHED to an entity - a camera mounted on a car,
	// a bike, a ped.
	//
	// These used to be skipped outright, and correctly so for as long as the
	// curve was built in world space: the marker's +0x24 world position is where
	// the camera was when you placed it, which stops being true the moment the
	// car drives off. The curve is now built in the PARENT's space instead, from
	// the same relative offsets the game itself plays back (+0x30), and
	// transformed by the parent's live matrix afterwards - so it rides the
	// vehicle exactly as stock does, but along a real curve.
	//
	// Only applies where a whole segment shares one attach target. Mixed or
	// changing targets stay on stock behaviour; see rmarker::isSplineable.
	bool  splineAttached = true;

	// Log the camera's position and orientation every frame, with the pieces it
	// was built from: the parent's origin, the curve's displacement, the
	// smoothing delta, and the frame the director wanted before we touched it.
	//
	// Separate from splineDebugLog, which is rate-limited to a line a second and
	// answers "which path did this take". This answers "what did the camera
	// actually do", which is the only way to tell a bad curve from a bad
	// transform from something downstream moving the camera afterwards. Writes
	// a line per frame - a few hundred for a clip - so leave it off otherwise.
	bool  splineTraceLog = false;

	// Re-aim look-at markers after moving them.
	//
	// A look-at marker's orientation is not authored, it is DERIVED from the
	// target every frame - and the game derives it from the position it knows
	// about, before we move the camera onto the spline. Nothing re-aims
	// afterwards, so the subject drifts off its framing by roughly the spline's
	// deviation divided by the target distance. Half a metre at ten metres is
	// about three degrees, which is visible.
	//
	// This re-solves the game's own look-at framing from the splined position.
	// Rockstar do the same thing in their blend for the same reason, so this is
	// restoring intended behaviour rather than adding an effect. Little reason
	// to turn it off except to see the difference.
	bool  lookAtReaim = true;

	// Aspect ratio used by that re-solve. 0 = the display's.
	//
	// It scales the horizontal look-at offset only, so it does nothing at all
	// unless a marker has been given one. Set it if you run stretched or
	// letterboxed and a look-at subject sits off its mark horizontally.
	float lookAtAspect = 0.0f;

	// Which page of the "Rockstar Editor+" block in the top-level marker menu is
	// folded out: 0 closed, 1 curve, 2 limits. Remembered so it does not
	// collapse every time you reopen the menu.
	//
	// A pager rather than a plain open/closed flag because the editor's
	// Scaleform column draws exactly 16 items and DISCARDS the rest with no
	// error - eight stock rows plus our header plus eight of our own is
	// seventeen, and the seventeenth simply never appeared. Was a bool; the old
	// values still load correctly (0 closed, 1 the first page).
	int   menuExpanded = 0;

	// Catmull-Rom knot parameterisation - in camera terms, how much TENSION the
	// path has.
	//   0.0 uniform    — long tangents, wide sweeping turns
	//   0.5 centripetal— the textbook no-overshoot choice
	//   1.0 chordal    — knot spacing follows chord length   (default)
	//
	// READ THIS BEFORE CHANGING IT. Alpha only reaches the camera when the
	// pacing is Continuous or Per Segment. Under NATURAL pacing - which is the
	// shipping default - the path is a Hermite over the marker TIMES and has no
	// alpha in it at all; the value still feeds segmentLength, so it moves the
	// `bow` figure in the debug log, and it moves nothing you can see. A
	// preference formed for one alpha over another was formed with Natural off.
	//
	// The measured behaviour under the modes where it DOES apply, on a real shot
	// with a 160-degree reversal, arc length over chord around the marker:
	//
	//     alpha 0.00 -> 1.11      alpha 0.50 -> 1.56      alpha 1.00 -> 3.59
	//
	// 1.0 means flowing straight through; 3.59 means the camera travels three
	// and a half times the distance it covers, i.e. a visible loop at a marker
	// the shot doubles back through. Centripetal is normally recommended because
	// it provably never cusps or self-intersects, but that theorem covers a
	// curve WITHIN one segment and protects against overshoot between
	// well-spaced markers - it gets there by shortening the tangent wherever a
	// neighbouring chord is short, which at a reversal leaves the curve no
	// momentum to sweep around.
	float alpha           = 1.0f;

	// Fit one monotone cubic through (marker time -> cumulative distance) so
	// speed is continuous ACROSS markers, not just within one. This is what
	// removes the kick at each keyframe; leave it on unless A/B testing.
	bool  smoothSpeedProfile = true;

	// Drive the path from the marker TIMES directly instead of from arc length.
	//
	// This is the third pacing choice and the one that reproduces the sibling
	// Simple Camera project exactly: position becomes a cubic Hermite through
	// the four markers over their real times, evaluated at the clock. No
	// arc-length reparameterisation, no distance profile - the curve's own
	// parameter speed drives the camera, so it eases naturally into the tight
	// parts of a path and flows through the open ones. That is what makes a
	// shot feel loose rather than metered.
	//
	// It is NOT the same as turning SmoothSpeedProfile and ArcLengthRemap off.
	// That combination leaves a UNIFORM Catmull-Rom evaluated at normalised
	// segment time, which is continuous in position but not in VELOCITY once
	// segment durations differ - measured jumps of 0.23 / 0.38 / 0.33 m/s at the
	// three markers of a real shot, which reads as a jerk at every keyframe.
	// Simple Camera's own source warns about exactly this. Taking the tangents
	// from the real times instead makes the rate match across a marker whatever
	// the durations are: the same jumps measure 0.01 / 0.00 / 0.01.
	//
	// ON by default. It is the pacing the mod was judged on, and the one that
	// makes a shot feel authored rather than metered.
	//
	// It overrides SmoothSpeedProfile and ArcLengthRemap while on - it replaces
	// the whole distance pipeline rather than tweaking it - and it also takes
	// Alpha out of the picture, since a time-parameterised Hermite has no knot
	// spacing to choose. See the note on Config::alpha.
	bool  naturalPacing = true;

	// Legacy per-segment constant-speed remap. Only used when
	// SmoothSpeedProfile is off, or on a marker with an explicit ease — it
	// normalises each segment independently, which is precisely what makes
	// velocity step at the joins.
	bool  arcLengthRemap  = true;

	// Log the shake's resolved output once a second - the amplitudes that
	// actually reached the camera, not the ini's. Kept because a per-marker
	// override silently beats the ini and nothing else surfaces that; it is the
	// first thing to ask for when someone reports "setting X does nothing".
	bool  shakeDebugLog = false;

	// The same idea for the curve: log which path each frame took, once a
	// second. Says whether a mounted or look-at segment was splined and, when it
	// was not, exactly which condition declined it - the two look identical from
	// the chair otherwise.
	bool  splineDebugLog = false;

	// Diagnostic: multiply how far the curve departs from the straight line
	// between two markers. 1 = normal. Endpoints are unaffected at any value,
	// so the markers are still hit exactly and the timing cannot drift.
	//
	// Exists to separate "the curve never reaches the camera" from "the curve
	// reaches the camera and is too subtle to see" - which look the same from
	// the chair and need completely different fixes. Set it to 10, play the
	// shot: an obvious wide swing means the write lands and the question is
	// tuning; no change at all means it is being discarded downstream.
	float splineDebugGain = 1.0f;

	// Lift the editor's "camera too far from the player" leash. Stock is 30 m,
	// after which the editor snaps back to the recorded camera and shows the
	// out-of-range warning.
	//
	// Caveat worth knowing: the limit exists partly because the world is only
	// streamed around the player. Far out you will see LOD pop and missing
	// map — that is streaming, not this hook misbehaving.
	bool  unlimitedCameraDistance = true;
	float maxCameraDistance       = 20000.0f;

	// ---------------------------------------------------------------------
	//  The editor "buffering" between every action
	// ---------------------------------------------------------------------
	//  After every seek, pause and marker jump the editor precaches, and it
	//  will not proceed until the GLOBAL, whole-game streaming request count
	//  has been zero for ten consecutive frames. A modded install never
	//  reaches that: the scene streamer re-scores the PVS every frame and the
	//  replay preloader adds requests inside the same loop. So it always runs
	//  to the give-up timer - 200 frames, which is ~6.7 s at 30 fps and ~20 s
	//  at 10 - with all editor input dead throughout.
	//
	//  It is an idle requirement, not a capacity one, which is why heap, pool
	//  and VRAM adjusters have never fixed it. This drops that global gate.
	//  The replay's OWN preload completion check still runs immediately
	//  afterwards, so the entities for the current window are still waited on;
	//  we simply stop waiting for the whole map streamer to fall silent.
	//
	//  Not a shortcut: this is the branch the engine itself takes after 6600,
	//  taken immediately instead. On by default because the stock behaviour is
	//  unusable on any heavily modded install and harmless on a light one
	//  (where the gate was already being satisfied in a few frames anyway).
	bool  fastPrecache = true;

	// The precache carries a SECOND, independent 6600-unit budget for audio,
	// reached after the streaming one: it stops the replay music on entry and
	// then waits for it to re-prepare. On a clip with radio music that is its
	// own 200 frames, so without this the streaming fix can look like it did
	// nothing. Same reasoning, same shipped branch - only the interactive
	// editor, never a render.
	bool  fastPrecacheAudio = true;

	// Suspended automatically while an image-sequence render is running, so
	// captured frames keep the stock guarantee. Set to 0 only to prove that
	// the render is unaffected - it costs a full stall per rendered frame.
	bool  fastPrecacheDuringRender = false;

	// Honour per-marker SPEED (slow motion / fast motion) when rendering.
	//
	// A marker carries a speed of 5, 20, 35, 50, 100, 125, 150, 175 or 200 %, and
	// the editor keeps two clocks because of it: the authored timeline the
	// markers sit on, and real elapsed time. The renderer used to advance the
	// authored one uniformly, so a slow-motion section rendered at full speed and
	// produced too few frames - the authored speed was silently discarded.
	//
	// With this on, the render clock is real time and each seek converts back,
	// which is exactly what the engine's own export does. The audio pass already
	// worked this way (it records a real-time playthrough), so this also removes
	// an A/V length mismatch on any project that uses speed markers.
	//
	// ON, because the old output was simply wrong. Set to 0 to reproduce a render
	// made by an earlier build, or to A/B the difference - note the frame count
	// and the output duration both change on a project that uses speed markers.
	bool  renderMarkerSpeed = true;

	// Hard ceiling, in ms of REAL time, on how long one precache may wait for
	// the replay's own +/-4s preload to finish.
	//
	// This is the last wait left once both stall gates are bypassed, and it is
	// the only one the engine gives no timer at all: HandleResults answers
	// "every preload request satisfied", and a request that cannot be satisfied
	// only leaves the queue after an internal timeout of 10 SECONDS of real
	// time. One un-streamable entity in the window therefore costs ten seconds
	// per action, on top of everything else - and if a dense frame's requests do
	// not fit the request array, nothing ends the wait at all.
	//
	// The preloading still happens; only the blocking stops. What is not ready
	// in time arrives through the urgent path instead, which UrgentModelLoadMs
	// already bounds. 0 disables the ceiling and restores stock behaviour.
	//
	// 1500 leaves room for a genuine window of entities to stream on a healthy
	// install while capping the pathological case at well under the 10 s it
	// would otherwise take.
	int   precacheMaxMs = 1500;

	// Ceiling, in ms, on CReplayModelManager's urgent model load - the
	// blocking spin that freezes the main thread outright when an entity's
	// model cannot be streamed. Stock is 5000 for the first failure and 1000
	// for every one after, per manager.
	//
	// The failure path is already graceful (the entity simply does not appear
	// that frame), so a low value trades a five-second hang for a missing car
	// for a moment. 0 restores stock.
	float urgentModelLoadMs = 250.0f;

	// How many 4 MB blocks the replay RECORDER gets.
	//
	// This is what decides how long a clip can be, and why that length changes
	// with the scene: recording writes into a ring of fixed blocks, so a street
	// full of traffic fills them in seconds where an empty road lasts a minute.
	// It is a memory budget, not a timer.
	//
	// Stock is 7 (28 MB), which is what this ships as - the feature is opt-in
	// because it is the one setting that costs real memory. 20 blocks is 80 MB
	// and roughly triples recording length.
	//
	// Going above the count the game settles on needs the replay heap widened
	// first; limits.cpp does that and clamps back down if it could not. Values
	// outside the supported range are clamped.
	// 128, not the game's 30. It is measured rather than optimistic: 128 blocks
	// records ~1m45 of dense city where stock gives ~17 seconds, and it has been
	// run on both builds without a crash. The cost is ~604 MB reserved for the
	// session, which is the right trade for a tool whose entire job is capturing
	// footage - being handed a 17-second ceiling by default is the worse
	// surprise. Anyone tight on memory can drop it; the log states what it got.
	int replayBlocks = gsig::REPLAY_BLOCKS_DEFAULT;

	// Report every profanity check as passed.
	//
	// The check is a Social Club round-trip made when naming a project or an
	// export. On an offline or downgraded build it can never complete, and the
	// editor treats "unavailable" as fatal - so exporting becomes impossible.
	// This makes it always return the same answer a successful check gives.
	//
	// It only affects local naming of your own single-player recordings.
	bool  bypassProfanityFilter = true;

	// Let a clip recorded in first person - or during a cutscene, or with camera
	// movement disabled - be given a free camera.
	//
	// The editor greys the entire Camera submenu on such a clip, so the recorded
	// view is the only one it will ever have. That is a UI gate and nothing more:
	// the camera code has no notion of these restrictions, so a marker set to a
	// free camera plays back normally.
	//
	// ON by default. The restriction removes a capability rather than protecting
	// one, and a clip you cannot re-frame is usually a clip you have to re-record.
	bool  unlockCameraRestrictions = true;

	// Let the editor camera pass through world geometry, and stop the attach
	// entity shoving it away.
	//
	// ON by default, which is the opposite of the cautious choice and the right
	// one for the job. Collision is a gameplay behaviour: it exists so a player
	// camera cannot end up inside a wall. A CAMERA OPERATOR wants the opposite -
	// pushing through a doorframe, a windscreen or a fence is most of what a
	// cinematic move is, and the push-off does not just block those, it
	// silently bends the path away from the markers you placed. Leaving it on
	// made the mod fight its own spline.
	//
	// The cost is that you can end up inside solid map with no visual
	// reference. That is recoverable in a frame - scrub, or move the camera -
	// whereas a shot the push-off quietly ruined is not obvious at all.
	bool  disableCameraCollision = true;

	// Stop the editor raising its loading spinner for seeks. That spinner is
	// what puts a ring in the middle of the screen when you scrub the timeline,
	// and what gets burned into rendered frames, since a render seeks constantly.
	// Scoped to the video-editor spinner source only, so save, script and cloud
	// spinners still behave normally.
	bool  hideEditorSpinner = true;

	// Widen the editor's zoom range. Stock is a 10x span (0.45x..4.50x); this
	// replaces the camera metadata's MinFov/MaxFov with the values below.
	// 1..130 is the widest possible - camFrame::SetFov clamps there whatever we
	// do, and that clamp is shared with every camera in the game.
	// ON by default. The stock 0.45x-4.50x range is a gameplay-camera choice,
	// and this is not a gameplay camera - a long lens is ordinary cinematography
	// and the range is the first thing anyone runs into. The ends below are
	// camFrame::SetFov's own clamp, so nothing outside them was ever reachable
	// and widening to them cannot produce a FOV the engine would refuse.
	bool  uncapZoom  = true;
	float zoomMinFov = 1.0f;
	float zoomMaxFov = 130.0f;

	// --- scene: the clip's time of day and weather -----------------------
	// Both are recorded per frame and replayed, so these are substituted on
	// their way out of the replay buffer rather than set afterwards - see
	// scene.cpp. Nothing is written to the .clip; turn either off and the clip
	// plays exactly as recorded again.
	//
	// Off by default, deliberately: the recorded time and weather are what the
	// clip IS, and a mod that quietly re-lit every clip on first launch would
	// be wrong even if the look were nicer.
	bool  overrideTimeOfDay = false;
	// Minutes past midnight, 0..1439. The menu steps it in quarter hours; the
	// ini can hold any minute.
	int   timeOfDay = 12 * 60;

	bool  overrideWeather = false;
	// Index into weather.xml's load order, matching what the clip records.
	// scene.cpp has the stock name list.
	int   weatherType = 0;          // EXTRASUNNY
	// Second type to blend towards, or -1 for none. Blending is what the
	// weather system does natively between two types, so a half-way state
	// between CLEAR and RAIN is an ordinary value here rather than a hack.
	int   weatherBlendTo = -1;
	float weatherBlend   = 0.0f;    // 0..1, weatherType -> weatherBlendTo
	// Wet roads and puddles, independent of the type. Negative keeps whatever
	// the clip recorded, which is NOT the same as zero - a clip shot in the
	// rain has wet ground that a dry preset should not necessarily scrub off.
	float weatherWetness = -1.0f;

	// A clip also records the fully RESOLVED timecycle keyframe every frame and
	// replays it, which is why moving the clock on its own only moves the sun:
	// the sky, ambient and fog are baked. On, the recorded keyframe is stood
	// down while an override is active and the timecycle is evaluated live from
	// the new time and weather. Off keeps the clip's own lighting, which is
	// worth having when the clip carried a timecycle modifier - an interior
	// grade, a mission colour grade - that the live evaluation cannot know
	// about. Only does anything while a time or weather override is on.
	bool  liveTimecycle = true;

	// Which group of our rows the marker's CAMERA submenu is showing:
	//   0 hidden   1 spline   2 sway   3 jitter   4 detail   5 axes
	// Persisted so it survives a restart. Defaults to hidden - the row should
	// take one line until you ask it for more, the same way the top-level
	// accordion does.
	int menuGroup = 0;

	// Index into the menu's adjust-step table (0.001 .. 1.0).
	int menuStep = 2;

	// Path to the ini, so menu toggles can be written straight back.
	std::string iniPath;
	std::string renderIniPath;

	// One-time split of the render settings into Render.ini.
	//
	// Written from the values just loaded, which are the user's own: every
	// render key defaults to whatever the old combined ini said, so a tuned
	// install migrates intact rather than being reset to shipped defaults.
	// Getting that backwards would silently undo somebody's settings, which is
	// exactly the sort of change nobody notices until a render comes out wrong.
	//
	// The keys are then deleted from the main ini, because a value living in two
	// files is worse than either arrangement - the copy that does nothing is
	// invariably the one you edit. Their comment lines are left behind; harmless,
	// and cheaper than trying to rewrite a file the user may have reformatted.
	void migrateRenderIni(const std::string& rini, const std::string& mainIni) const
	{
		FILE* f = nullptr;
		if (fopen_s(&f, rini.c_str(), "wb") != 0 || !f) return;

		fprintf(f,
			"; RockstarEditorPlus - render settings\n"
			";\n"
			"; Split out of RockstarEditorPlus.ini, which now holds only the editor\n"
			"; patches (camera, shake, limits). Values here were carried over from it.\n"
			";\n"
			"; The eight settings people change per shot - EnableRenderer, RenderMode,\n"
			"; RenderCaptureMode, RenderFps, RenderSamples, RenderShutter,\n"
			"; RenderHighlightBoost and RenderAudio - are also rows on the editor's own\n"
			"; Export screen, under the stock Frame Rate and Bit rate. They are ONE\n"
			"; setting with two editors, not two settings: a row writes straight back\n"
			"; into this file, so the menu and this file can never disagree.\n"
			";\n"
			"; This file is read ONCE, at startup. Editing it while the game is running\n"
			"; therefore does nothing until the next launch - use the menu for anything\n"
			"; you want to change mid-session. Everything not listed above has no row and\n"
			"; is set-once here.\n"
			";\n"
			"; EDITING NOTE: inline \"; ...\" comments are only safe on NUMBER and 0/1\n"
			"; keys. Windows' ini API does NOT strip them, so on a PATH or STRING key\n"
			"; the comment ends up inside the value. Those keep their comment on the\n"
			"; line above, as below.\n"
			"\n[Render]\n\n");

		fprintf(f,
			"; ============================================================================\n"
			";  Capture - exact-seek frames with accumulation motion blur\n"
			"; ============================================================================\n"
			"; THE MASTER SWITCH. Must be 1 for anything in this file to happen: the\n"
			"; Export button is the only trigger, and this is what diverts it away\n"
			"; from the game's own encoder. It is NOT a choice of output format -\n"
			"; that is RenderMode below.\n"
			";\n"
			"; Ships OFF. Diverting the bake means other editor export tools (EVE,\n"
			"; EVER) never fire - they hang off the game's own bake pipeline, so they\n"
			"; do not error, they just silently never run. Turn this on when you want\n"
			"; RE+ to be the thing that handles Export.\n"
			"EnableRenderer=%d\n\n"
			"; How the sub-frames are gathered.\n"
			";   Walking  pause and seek to each sub-frame's exact instant. Exact\n"
			";            shutter, deterministic, faster - but a seeked frame is COLD:\n"
			";            particles do not step and anything with temporal history\n"
			";            (TAA, SSR, RT accumulation) is wrong at every sample.\n"
			";   Sliding  play the clip in slow motion and expose consecutive presented\n"
			";            frames. Keeps all of that coherent. Slower, and the shutter is\n"
			";            approximate. Simulation fidelity over sharpness.\n"
			"RenderCaptureMode=%s\n\n"
			"RenderFps=%g%s; output rate, independent of your actual framerate\n"
			"RenderSamples=%d%s; sub-frames averaged per output frame. 1 = no blur\n"
			"RenderShutter=%g%s; 1.0 = 360 degrees, 0.5 = the 180 film convention\n"
			"RenderSettleFrames=%d%s; redraw wait after seeking to a new output frame\n"
			"RenderSettleSubFrames=%d%s; ...between blur sub-samples. Never 0\n"
			"RenderHighlightBoost=%g%s; keeps specular streaks bright through accumulation\n"
			"RenderJpeg=%d%s; 0 = PNG\n"
			"RenderQuality=%d%s; JPEG only\n"
			"; 0 auto (the add-on reads the back-buffer format) / 1 RGBA / 2 BGRA.\n"
			"; Auto is known to get this wrong on Legacy, FiveM especially. Try 1, then 2.\n"
			"RenderChannelOrder=%d%s; 0 auto / 1 RGBA / 2 BGRA\n"
			"RenderHideHud=%d%s; hide the editor HUD and cursor while rendering\n"
			"ExportCloseWhenDone=%d%s; return to the editor menus after an Export render\n"
			"; Empty = RockstarEditorPlus\\Captures\\\n"
			"RenderOutputFolder=%s\n\n",
			enableRenderer ? 1 : 0,
			renderCaptureMode == 1 ? "Sliding" : "Walking",
			renderFps, "              ", renderSamples, "          ",
			renderShutter, "          ", renderSettleFrames, "     ",
			renderSettleSubFrames, "  ", renderHighlight, "   ",
			renderJpeg ? 1 : 0, "               ", renderQuality, "            ",
			renderChannelOrder, "      ",
			renderHideHud ? 1 : 0, "            ",
			exportCloseWhenDone ? 1 : 0, "      ",
			renderOutputFolder.c_str());

		// The two audio keys used to be passed to this call with no placeholders
		// to land in - so they were silently dropped and neither ever appeared
		// in a generated Render.ini. RenderAudio in particular was unreachable
		// unless you knew to type the key in yourself.
		// The backslashes in the comment lines below are doubled for the same
		// reason: "\f" is a formfeed, not a path separator, and the written file
		// had a control character in it where the folder name should be.
		fprintf(f,
			"; ============================================================================\n"
			";  Output\n"
			"; ============================================================================\n"
			"; What a finished frame becomes. The capture itself is identical either\n"
			"; way - same seeks, same accumulation blur - so this is purely the\n"
			"; deliverable, not a quality setting.\n"
			";   Video   streamed to ffmpeg as frames finish, then deleted\n"
			";   Frames  numbered PNG/JPEG sequence, with an assemble.txt beside it\n"
			"RenderMode=%s\n"
			"RenderKeepFrames=%d%s; Video mode: keep the frames as well\n"
			"; Empty = RockstarEditorPlus\\ffmpeg.exe, then beside the exe, then PATH.\n"
			"FfmpegPath=%s\n\n"
			"; Named preset from presets\\ (h264, h265, nvenc_hevc, prores_hq, lossless).\n"
			"; A preset supplies both the args and the extension, overriding the two below.\n"
			"RenderVideoPreset=%s\n\n"
			"; Used when no preset is named. Ext must match the codec or ffmpeg refuses.\n"
			"RenderVideoArgs=%s\n"
			"RenderVideoExt=%s\n\n"
			"; ============================================================================\n"
			";  Audio\n"
			"; ============================================================================\n"
			"; Record the project's sound and mux it into the video. One Export press does\n"
			"; both passes: the project plays through once at normal speed to capture the\n"
			"; sound, then rewinds to clip one and renders the frames. Works in either\n"
			"; RenderMode: Video muxes it in, Frames leaves it as audio.wav beside the\n"
			"; sequence with the ffmpeg line to attach it written into assemble.txt.\n"
			"RenderAudio=%d\n\n"
			"; Or borrow the audio track from an existing file - normally a stock GTA\n"
			"; export of the same project. Ignored while RenderAudio is on.\n"
			"AudioFromFile=%s\n\n",
			wantsVideo() ? "Video" : "Frames",
			renderKeepFrames ? 1 : 0, "         ",
			ffmpegPath.c_str(), renderVideoPreset.c_str(),
			renderVideoArgs.c_str(), renderVideoExt.c_str(),
			renderAudio ? 1 : 0, audioFromFile.c_str());

		fprintf(f,
			"; ============================================================================\n"
			";  Depth of field  (ReShade / IgcsConnector)\n"
			"; ============================================================================\n"
			"; The capture add-on can also walk the camera around a lens aperture and blend\n"
			"; the result, giving real optical bokeh. There is nothing to configure here -\n"
			"; it is driven entirely from ReShade's own overlay. It needs the editor open,\n"
			"; a free-camera marker, playback PAUSED, and IgcsDof.fx in your shaders folder.\n"
			";\n"
			"; For STATIC shots. The replay clock is never moved during a session, so every\n"
			"; sample sees the same instant from a different point on the lens. Motion blur\n"
			"; is the renderer's job (RenderSamples / RenderShutter above), where the\n"
			"; shutter is exact and does not have to share a sample budget with the bokeh.\n");

		fclose(f);

		// EVERY key the split moved. A key missing from here is carried into
		// Render.ini and then LEFT BEHIND in the old one, so it exists twice and
		// editing the copy that is no longer read does nothing - the exact
		// failure the split was meant to end.
		//
		// RenderCaptureMode, RenderAudio and AudioFromFile were all missing. The
		// first was found when it became a menu row (a row that writes a key
		// which the migration does not clean up leaves the user two of them);
		// the two audio keys are the same omission as the one the comment above
		// records for the WRITING side, fixed there and not here.
		static const char* const kMoved[] = {
			"ExportAsImageSequence", "EnableRenderer", "RenderMode",
			"RenderCaptureMode",
			"RenderFps", "RenderSamples", "RenderShutter",
			"RenderSettleFrames", "RenderSettleSubFrames", "RenderHighlightBoost",
			"RenderJpeg", "RenderQuality", "RenderChannelOrder", "RenderHideHud",
			"ExportCloseWhenDone", "RenderOutputFolder", "RenderVideo",
			"RenderKeepFrames", "FfmpegPath", "RenderVideoPreset",
			"RenderVideoArgs", "RenderVideoExt",
			"RenderAudio", "AudioFromFile",
		};
		for (const char* k : kMoved)
			WritePrivateProfileStringA("RockstarEditorPlus", k, nullptr, mainIni.c_str());

		logger::write("info",
			"config: render settings moved to Render.ini (%d keys), carried over from the old ini",
			(int)(sizeof(kMoved) / sizeof(kMoved[0])));
	}

	// Persist a toggle changed from the in-editor menu, so it survives a
	// restart the same way an ini edit would.
	void writeBool(const char* key, bool value) const
	{
		if (iniPath.empty()) return;
		WritePrivateProfileStringA("RockstarEditorPlus", key, value ? "1" : "0", iniPath.c_str());
	}

	void writeInt(const char* key, int value) const
	{
		if (iniPath.empty()) return;
		char buf[32]{};
		sprintf_s(buf, "%d", value);
		WritePrivateProfileStringA("RockstarEditorPlus", key, buf, iniPath.c_str());
	}

	void writeFloat(const char* key, float value) const
	{
		if (iniPath.empty()) return;
		char buf[64]{};
		sprintf_s(buf, "%g", value);
		WritePrivateProfileStringA("RockstarEditorPlus", key, buf, iniPath.c_str());
	}

	// The same three for the render settings, which live in their own file under
	// their own section. A render row changed from the editor's export menu has
	// to land where the loader reads it - writing it to the main ini would
	// appear to work and then be overwritten by Render.ini on the next start,
	// since the split reader takes the main ini only as the DEFAULT.
	void writeRenderBool(const char* key, bool value) const
	{
		if (renderIniPath.empty()) return;
		WritePrivateProfileStringA("Render", key, value ? "1" : "0", renderIniPath.c_str());
	}

	void writeRenderInt(const char* key, int value) const
	{
		if (renderIniPath.empty()) return;
		char buf[32]{};
		sprintf_s(buf, "%d", value);
		WritePrivateProfileStringA("Render", key, buf, renderIniPath.c_str());
	}

	void writeRenderFloat(const char* key, float value) const
	{
		if (renderIniPath.empty()) return;
		char buf[64]{};
		sprintf_s(buf, "%g", value);
		WritePrivateProfileStringA("Render", key, buf, renderIniPath.c_str());
	}

	void writeRenderStr(const char* key, const char* value) const
	{
		if (renderIniPath.empty()) return;
		WritePrivateProfileStringA("Render", key, value, renderIniPath.c_str());
	}

	// --- image sequence render ------------------------------------------------
	// Persisted like everything else, so a render setup survives a restart -
	// these are shot settings, not scratch state, and re-dialling them every
	// session would be the most annoying thing about the feature.
	// Defaults are a baked 360-degree shutter at 30fps: 64 sub-frames averaged
	// per output frame. Expensive - 64 captures per frame - but it is what the
	// footage actually wants, and the result is a real accumulation blur rather
	// than a screen-space approximation.
	//
	// Highlight boost is high for the same reason. A plain average of 64 samples
	// pulls every specular hit down towards the mean, so highlights that should
	// streak end up as grey mush; lifting them during accumulation keeps the
	// streak bright the way film does. It matters more the more samples you
	// take, which is exactly this configuration.
	//
	// 0.90 rather than the 0.99 this used to ship with - backed off the cap by
	// preference after looking at real output. The lift happens inside the
	// capture addon, not here; this value is only passed through, so treat the
	// two numbers as a taste setting rather than as a formula with a right
	// answer.
	// Which renderer drives the capture.
	//
	//   0 WALKING  - pause, seek to each sub-sample time, grab, average. Exact
	//                shutter, deterministic frame times, and the simpler
	//                failure mode: when it goes wrong you get a repeated
	//                frame, which is obvious.
	//
	//   1 SLIDING  - let the clip PLAY, in slow motion, advancing to each
	//                frame's mark and then exposing RenderSamples consecutive
	//                presents. THE DEFAULT.
	//
	// Sliding exists for what walking cannot do rather than to do it better. A
	// seeked frame is COLD: particle systems do not step, and anything with
	// temporal history - TAA, SSR reprojection, the RT accumulation on Enhanced -
	// is reset or wrong at every single sample. Playing the clip keeps all of
	// that coherent.
	//
	// RenderSamples and RenderShutter mean the same thing in both modes, and
	// there is nothing extra to configure: the playback speed is derived from a
	// measurement taken on the first frame.
	//
	// Sliding's shutter is APPROXIMATE - the samples land on the instants the
	// game presented rather than on exact midpoints - so walking is still the
	// one to pick when placement has to be exact, or when a sliding render comes
	// out looking wrong.
	//
	// It is NOT the slower one, which this comment claimed for a long time on
	// the reasoning that "the world has to actually run". Count the frames:
	//     walking  = RenderSettleFrames + Samples * (1 + RenderSettleSubFrames)
	//     sliding  = Samples / Shutter
	// At the shipped defaults that is 3 + 64*2 = 131 against 64, so sliding is
	// around 2x faster at a 360-degree shutter. The two are level at 180, and
	// below that walking wins.
	//
	// It said 3x for a long time - in this comment, in presetmaker's help and in
	// the export menu - directly above the formula that gives 2.05. The "level at
	// 180" claim is the tell: N/shutter == 2N only at shutter 0.5, which pins the
	// 360-degree ratio at 2. Where walking's second present goes is no mystery
	// either: one to redraw at the seeked time, one to capture it, and they
	// cannot overlap without capturing a frame the world has already left.
	int   renderCaptureMode  = 1;

	float renderFps          = 30.0f;
	int   renderSamples      = 64;
	float renderShutter      = 1.0f;
	int   renderSettleFrames = 3;

	// Settle for a motion-blur SUB-sample, which moves the clock by a fraction
	// of a millisecond rather than a whole frame. Separate from the above
	// because it is paid once per sample: at 64 samples the old shared value
	// spent 192 frames waiting per output frame, and 189 of those were for
	// seeks too small to need them.
	//
	// Do not set this to 0. One redraw per sub-sample is what MAKES the blur -
	// without it all 64 samples capture the same image and average back to a
	// single sharp frame.
	int   renderSettleSubFrames = 1;
	bool  renderJpeg         = false;
	int   renderQuality      = 90;
	float renderHighlight    = 0.90f;
	int   renderChannelOrder = 0;

	// Hide the editor's playback HUD while rendering - timer, transport row,
	// scrub bar and the instructional button strip. Off only if you actually
	// want them burned into the frames.
	//
	// Also covers a depth-of-field session, for the same reason and through the
	// same suppression: that blend runs during ReShade's effect pass, which is
	// after the game has drawn its own UI into the frame.
	bool  renderHideHud = true;

	// (A depth-of-field session has no settings here. It is driven entirely from
	// ReShade's own panel, and it is for STATIC shots - motion blur is the
	// renderer's job, above.)

	// --- what the render produces ---------------------------------------------
	// The capture is IDENTICAL either way - same seeks, same accumulation blur.
	// This only decides what happens to each finished frame: handed to ffmpeg
	// and deleted, or left on disk.
	//
	// Replaced the old RenderVideo bool, which read as "also do a video" when it
	// is really a choice between two outputs.
	enum class RenderMode
	{
		Frames = 0,   // numbered PNG/JPEG sequence, plus an assemble.txt
		Video,        // streamed to ffmpeg as frames finish, then deleted
	};

	// Video by default. This used to be Frames on the grounds that video needs
	// ffmpeg present - but the mod ships ffmpeg.exe in its own folder and looks
	// there first, so that objection no longer holds. A finished file is the
	// better default deliverable: a folder of eighteen thousand PNGs is only
	// useful to someone who already intended to composite, and they can say so.
	//
	// This used to carry a second reason - that renderAudio "needs a video to mux
	// into", so audio-on with Frames would be a default that could not do what it
	// says. That was never true of this renderer: the audio pass writes a
	// standalone audio.wav and runs in either mode, and writeAssembleHelp()
	// already emits the ffmpeg line that attaches it. Frames+audio is a perfectly
	// good deliverable. The claim had spread to the README, the generated
	// Render.ini and the export menu before anyone checked it against the code.
	RenderMode renderMode = RenderMode::Video;

	bool wantsVideo() const { return renderMode == RenderMode::Video; }

	// Kept alongside the video when set. The default deletes each frame once the
	// encoder has taken it, which is the point of streaming rather than batching.
	bool  renderKeepFrames = false;

	// Empty = look for ffmpeg.exe on PATH. Set it to skip the search or to pin a
	// specific build.
	std::string ffmpegPath;

	// Everything between the input and the output file. This is the whole
	// encoder configuration, deliberately left as one opaque string so any
	// codec ffmpeg supports is reachable without new ini keys - ProRes, HEVC,
	// NVENC, whatever - without this needing to know about them.
	std::string renderVideoArgs = "-c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p";

	// Container. Must agree with the codec above; ffmpeg will refuse otherwise.
	std::string renderVideoExt = "mp4";

	// A file whose AUDIO track is muxed into the render - normally a stock GTA
	// export of the same project. The game's own bake already produces audio in
	// sync with the project timeline and handles clip boundaries, so borrowing
	// that track is far less machinery than capturing sound ourselves.
	std::string audioFromFile;

	// Record the project's sound in real time, then render the frames. Both
	// passes run off ONE Export press: the sound pass plays the project through
	// at normal speed, and the renderer then steps the playhead back to clip one
	// and starts the frames without leaving playback.
	//
	// INDEPENDENT OF renderMode. The pass writes a standalone audio.wav into the
	// render folder either way; Video mode then muxes it into the encode, and
	// Frames mode leaves it beside the sequence with the ffmpeg arguments to
	// attach it written into assemble.txt.
	//
	// (It used to take two presses. A plain seek cannot return to the start of
	// a project - it clamps to the clip on screen, and the sound pass ends on
	// the last one - so re-pressing Export was the only reliable rewind. The
	// clip-step call the multi-clip renderer uses does it directly.)
	//
	// On by default now that it costs one press rather than two. A silent
	// render is almost never what someone wanted, and the failure was quiet -
	// you found out when you opened the file.
	bool renderAudio = true;

	// Name of a preset in the mod's presets folder, e.g. "h265". When set and
	// found it supplies both the args and the extension, overriding the two
	// above. Empty = use them directly.
	std::string renderVideoPreset;

	// THE MASTER SWITCH for everything in this file.
	//
	// Sends the editor's own Export button to this mod's renderer instead of the
	// game's encoder; off leaves Export exactly as shipped and nothing else here
	// does anything.
	//
	// It was called ExportAsImageSequence, which was actively misleading: it
	// reads like a choice of output format, so people turned it OFF when they
	// wanted a video - and got the game's watermarked encoder instead. What the
	// render PRODUCES is RenderMode; this is only whether we run at all.
	//
	// SHIPS OFF, and that is a deliberate reversal.
	//
	// Diverting the bake is not a private change: other editor export tools -
	// EVE, EVER - hang off the game's own bake pipeline, so once we redirect it
	// they never fire. They do not error, they simply never run, which is the
	// worst way for two mods to conflict. exporthook already logs that warning
	// at the moment it happens; shipping this on made the mod give that warning
	// to people who had not asked for a renderer at all.
	//
	// Everything else the mod does - spline, shake, editor limits - is additive
	// and shares the editor happily. The renderer is the one exclusive feature,
	// so it is the one thing that should be opted into rather than out of. Set
	// EnableRenderer=1 in Render.ini when you want it.
	bool  enableRenderer = false;

	// Leave the editor's playback and return to its menus once a render
	// finishes, the same way a real export does.
	//
	// This used to say "only applies to renders started from the Export button -
	// a render started from the camera menu leaves you where you were". There is
	// no camera-menu render any more, so it applies to every render there is.
	bool  exportCloseWhenDone = true;

	// Where sequences are written. Empty = a single RockstarEditorPlus_Captures
	// folder beside GTA5.exe, with one numbered subfolder per render. Set it to
	// an absolute path to render somewhere with more room - image sequences are
	// large, and the game drive is often not where you want them.
	std::string renderOutputFolder;

	// Global procedural shake. Fully parameterised - per-marker settings
	// override the four main terms; the per-axis weights are global.
	shake::Params shake;

	// Only take over when the marker's blend is set to Smooth. With this off
	// the spline also replaces Linear blends, which is a much bigger change to
	// existing projects than most people want.
	bool  onlySmoothBlend = true;

	// `self` is this ASI's own module handle, from DllMain. It is no longer used
	// to locate anything: paths::asiDir() resolves the mod's folder from the
	// module itself, so every consumer agrees on one answer instead of each
	// deriving its own. The parameter stays because the caller has it to hand and
	// a future path question may want it.
	//
	// This used to compute the directory here and then throw it away — a
	// `(void)path;` sat under a comment promising the ini was read from beside
	// the .asi, while paths:: was actually keying off the host executable. Same
	// folder in a normal install, so nothing showed; wildly different under
	// FiveM.
	void load(HMODULE self)
	{
		(void)self;
		// An install from the old layout is moved across on first run rather than
		// read where it lies: leaving it in the game root would mean two files,
		// one of which silently does nothing when edited.
		std::string ini = paths::file("RockstarEditorPlus.ini");
		paths::migrate("RockstarEditorPlus.ini", ini);
		iniPath = ini;

		// Render settings live in their own file. The editor patches (camera,
		// shake, limits) and the renderer have almost nothing to do with each
		// other and are edited at completely different times, so one file for
		// both was mostly something to scroll past.
		//
		// Every render key takes its default from the MAIN ini, so an install
		// that predates the split keeps its tuned values on the first run after
		// upgrading. They are then written to Render.ini and removed from the
		// old file, leaving exactly one home per key - the same reasoning as the
		// folder migration above: two files where one silently does nothing is
		// worse than either alone.
		const std::string rini = paths::file("Render.ini");
		const bool splitAlready =
			GetFileAttributesA(rini.c_str()) != INVALID_FILE_ATTRIBUTES;
		renderIniPath = rini;

		auto rBool = [&](const char* key, bool def) {
			const bool d = GetPrivateProfileIntA("RockstarEditorPlus", key,
				def ? 1 : 0, ini.c_str()) != 0;
			return GetPrivateProfileIntA("Render", key, d ? 1 : 0, rini.c_str()) != 0;
		};
		auto rInt = [&](const char* key, int def) {
			const int d = GetPrivateProfileIntA("RockstarEditorPlus", key, def, ini.c_str());
			return GetPrivateProfileIntA("Render", key, d, rini.c_str());
		};
		auto rFloat = [&](const char* key, float def) {
			char db[64]{}, b[64]{}, hard[64]{};
			sprintf_s(hard, "%g", def);
			GetPrivateProfileStringA("RockstarEditorPlus", key, hard, db, sizeof(db), ini.c_str());
			GetPrivateProfileStringA("Render", key, db, b, sizeof(b), rini.c_str());
			return (float)atof(b);
		};
		auto rStr = [&](const char* key, const char* def, char* out, DWORD n) {
			char d[1024]{};
			GetPrivateProfileStringA("RockstarEditorPlus", key, def, d, sizeof(d), ini.c_str());
			GetPrivateProfileStringA("Render", key, d, out, n, rini.c_str());

			// Cut an inline comment off the value.
			//
			// GetPrivateProfileString does NOT strip these - a line reading
			//     RenderCaptureMode=Sliding   ; Walking = ...
			// hands back the whole tail, so a comparison against "Sliding"
			// fails and the setting silently reads as its default. That cost a
			// full render: the ini said Sliding, the renderer ran Walking, and
			// nothing anywhere said otherwise.
			//
			// The ini header warns about this and the shipped file avoids it by
			// writing string keys bare. That is not enough - these files are
			// hand-edited, and a user adding a comment should not silently lose
			// the setting. Handled HERE so it cannot happen again on any key.
			if (char* semi = strchr(out, ';')) *semi = '\0';
			for (size_t i = strlen(out); i > 0 && (out[i-1] == ' ' || out[i-1] == '\t'); --i)
				out[i-1] = '\0';
		};

		auto getBool = [&](const char* key, bool def) {
			return GetPrivateProfileIntA("RockstarEditorPlus", key, def ? 1 : 0, ini.c_str()) != 0;
		};
		auto getFloat = [&](const char* key, float def) {
			char buf[64]{}, defbuf[64]{};
			sprintf_s(defbuf, "%g", def);
			GetPrivateProfileStringA("RockstarEditorPlus", key, defbuf, buf, sizeof(buf), ini.c_str());
			return (float)atof(buf);
		};

		menuGroup = GetPrivateProfileIntA("RockstarEditorPlus", "MenuGroup", menuGroup, ini.c_str());
		menuStep  = GetPrivateProfileIntA("RockstarEditorPlus", "MenuStep",  menuStep,  ini.c_str());

		enabled           = getBool("Enabled", enabled);
		splinePosition    = getBool("SplinePosition", splinePosition);
		splineOrientation = getBool("SplineOrientation", splineOrientation);
		splineFov         = getBool("SplineFov", splineFov);
		splineAttached    = getBool("SplineAttached", splineAttached);
		splineTraceLog    = getBool("SplineTraceLog", splineTraceLog);
		lookAtReaim       = getBool("LookAtReaim", lookAtReaim);
		lookAtAspect      = getFloat("LookAtAspect", lookAtAspect);
		if (lookAtAspect < 0.0f) lookAtAspect = 0.0f;
		menuExpanded      = GetPrivateProfileIntA("RockstarEditorPlus", "MenuExpanded", menuExpanded, ini.c_str());
		smoothSpeedProfile = getBool("SmoothSpeedProfile", smoothSpeedProfile);
		naturalPacing     = getBool("NaturalPacing", naturalPacing);
		arcLengthRemap    = getBool("ArcLengthRemap", arcLengthRemap);
		onlySmoothBlend   = getBool("OnlySmoothBlend", onlySmoothBlend);
		alpha             = getFloat("Alpha", alpha);

		unlimitedCameraDistance = getBool("UnlimitedCameraDistance", unlimitedCameraDistance);
		disableCameraCollision  = getBool("DisableCameraCollision", disableCameraCollision);

		fastPrecache            = getBool("FastPrecache", fastPrecache);
		fastPrecacheAudio       = getBool("FastPrecacheAudio", fastPrecacheAudio);
		fastPrecacheDuringRender= getBool("FastPrecacheDuringRender", fastPrecacheDuringRender);
		urgentModelLoadMs       = getFloat("UrgentModelLoadMs", urgentModelLoadMs);
		precacheMaxMs = GetPrivateProfileIntA("RockstarEditorPlus", "PrecacheMaxMs",
			precacheMaxMs, ini.c_str());

		// Below ~100 ms nothing could stream even on a healthy install, which
		// would push every entity onto the urgent path for no gain. 0 is the
		// documented "no ceiling" value and is left alone.
		if (precacheMaxMs < 0) precacheMaxMs = 0;
		if (precacheMaxMs > 0 && precacheMaxMs < 100) precacheMaxMs = 100;
		if (precacheMaxMs > (int)gsig::PRELOAD_TIME_MAX_MS)
			precacheMaxMs = (int)gsig::PRELOAD_TIME_MAX_MS;

		// Below ~16 ms the urgent load cannot complete even a resident model's
		// bookkeeping inside one call, which turns every entity creation into a
		// guaranteed miss. Negative is meaningless; 0 is the documented
		// "restore stock" value and is left alone.
		if (urgentModelLoadMs < 0.0f) urgentModelLoadMs = 0.0f;
		if (urgentModelLoadMs > 0.0f && urgentModelLoadMs < 16.0f) urgentModelLoadMs = 16.0f;
		if (urgentModelLoadMs > gsig::MODELMGR_TIMEOUT_STOCK)
			urgentModelLoadMs = gsig::MODELMGR_TIMEOUT_STOCK;
		hideEditorSpinner        = getBool("HideEditorSpinner", hideEditorSpinner);
		uncapZoom               = getBool("UncapZoom", uncapZoom);
		zoomMinFov              = getFloat("ZoomMinFov", zoomMinFov);
		zoomMaxFov              = getFloat("ZoomMaxFov", zoomMaxFov);

		// Keep the pair sane whatever the ini says. UpdateZoom divides MaxFov by
		// MinFov to get the zoom range, so min > max would make Clamp's low bound
		// exceed its high bound, and a zero min would divide by zero. 1..130 is
		// camFrame::SetFov's own clamp, so nothing outside it is reachable.
		if (zoomMinFov < 1.0f)   zoomMinFov = 1.0f;
		if (zoomMinFov > 130.0f) zoomMinFov = 130.0f;
		if (zoomMaxFov > 130.0f) zoomMaxFov = 130.0f;
		if (zoomMaxFov < zoomMinFov) zoomMaxFov = zoomMinFov;

		overrideTimeOfDay = getBool("OverrideTimeOfDay", overrideTimeOfDay);
		timeOfDay = GetPrivateProfileIntA("RockstarEditorPlus", "TimeOfDay",
			timeOfDay, ini.c_str());
		if (timeOfDay < 0)    timeOfDay = 0;
		if (timeOfDay > 1439) timeOfDay = 1439;

		overrideWeather = getBool("OverrideWeather", overrideWeather);
		weatherType = GetPrivateProfileIntA("RockstarEditorPlus", "WeatherType",
			weatherType, ini.c_str());
		weatherBlendTo = GetPrivateProfileIntA("RockstarEditorPlus", "WeatherBlendTo",
			weatherBlendTo, ini.c_str());
		weatherBlend   = getFloat("WeatherBlend", weatherBlend);
		weatherWetness = getFloat("WeatherWetness", weatherWetness);
		// Upper bounds are left to scene.cpp, which is the only thing that knows
		// how many weather types there are. A negative BlendTo means "none" and
		// a negative Wetness means "as recorded", so neither is clamped up here.
		if (weatherType < 0)     weatherType = 0;
		if (weatherBlend < 0.0f) weatherBlend = 0.0f;
		if (weatherBlend > 1.0f) weatherBlend = 1.0f;
		if (weatherWetness > 1.0f) weatherWetness = 1.0f;
		liveTimecycle = getBool("LiveTimecycle", liveTimecycle);

		bypassProfanityFilter   = getBool("BypassProfanityFilter", bypassProfanityFilter);
		unlockCameraRestrictions = getBool("UnlockCameraRestrictions", unlockCameraRestrictions);
		maxCameraDistance       = getFloat("MaxCameraDistance", maxCameraDistance);

		replayBlocks = GetPrivateProfileIntA("RockstarEditorPlus", "ReplayBlocks",
			replayBlocks, ini.c_str());
		// Only the outer bound here. Anything above the engine's own 36 needs the
		// replay HEAP widened first, and whether that is possible is not knowable
		// until the signatures have resolved - so limits::install() makes that
		// call and clamps this back to 36 if it could not.
		if (replayBlocks < gsig::REPLAY_BLOCKS_MIN)      replayBlocks = gsig::REPLAY_BLOCKS_MIN;
		if (replayBlocks > gsig::REPLAY_BLOCKS_HARD_MAX) replayBlocks = gsig::REPLAY_BLOCKS_HARD_MAX;

		renderFps          = rFloat("RenderFps", renderFps);
		renderSamples      = rInt("RenderSamples", renderSamples);
		renderShutter      = rFloat("RenderShutter", renderShutter);

		// None of these were checked at all, and the render pipeline divides by
		// two of them. RenderFps=0 is the sharp edge: s_dt = 1000/fps becomes
		// inf, the frame count collapses to 2, and sampleTime() hands the replay
		// a NaN to seek to. Negative runs the output timeline backwards.
		//
		// The ceilings are deliberately loose rather than opinionated - rendering
		// at a high rate and blending in post is a real workflow the capture code
		// explicitly supports - so these only catch values that cannot be meant.
		if (!(renderFps > 0.0f) || renderFps > 1000.0f)
		{
			logger::write("info",
				"config: RenderFps=%.4g is out of range - using 30. The render divides "
				"by this, so 0 or negative produces a NaN seek rather than a bad video.",
				renderFps);
			renderFps = 30.0f;
		}

		// <=1 already means "no blur" everywhere downstream, so the floor is only
		// about keeping the divisor sane; the ceiling is what stops a stray extra
		// digit turning one output frame into an hour of capture.
		if (renderSamples < 1)    renderSamples = 1;
		if (renderSamples > 4096) renderSamples = 4096;

		// The value is a FRACTION of the frame interval - 1.0 already IS 360
		// degrees - but it is presented as "Shutter angle", which invites
		// entering 180. Nothing checked it, and 180 sailed through: at 24fps
		// that asks for a 7500 ms exposure per frame, the sliding controller
		// chases it to its 1.0x speed ceiling, and a single "frame" consumes the
		// entire clip. Observed on a real render - 5 frames delivered out of 83,
		// with ffmpeg exiting 0 as though nothing were wrong, which is the worst
		// possible way for a setting to be wrong.
		//
		// Above 2 is not a fraction anyone means: it would expose each frame for
		// more than twice its own interval. Read it as the degrees it plainly is
		// and say so, rather than clamping to 2 and quietly delivering something
		// nobody asked for either.
		if (renderShutter > 2.0f)
		{
			const float asDegrees = renderShutter;
			renderShutter = asDegrees / 360.0f;
			logger::write("info",
				"config: RenderShutter=%.4g looks like DEGREES - reading it as %.4g "
				"(%.4g/360). The key is a fraction of the frame interval: 1.0 is a "
				"360-degree shutter, 0.5 the 180-degree film convention.",
				asDegrees, renderShutter, asDegrees);
		}

		// A zero or negative shutter means every sample lands on the same instant
		// - no blur, and in sliding mode a target the controller cannot converge
		// on. Keep it just above zero instead.
		if (!(renderShutter > 0.0f)) renderShutter = 1.0f;
		if (renderShutter < 0.01f)   renderShutter = 0.01f;
		if (renderShutter > 2.0f)    renderShutter = 2.0f;
		renderSettleFrames = rInt("RenderSettleFrames", renderSettleFrames);
		renderSettleSubFrames = rInt("RenderSettleSubFrames", renderSettleSubFrames);
		if (renderSettleSubFrames < 1) renderSettleSubFrames = 1;

		// A negative settle is silently "none"; a huge one multiplies every
		// frame's cost by itself and reads as a hang. Both are worth refusing.
		if (renderSettleFrames < 0)   renderSettleFrames = 0;
		if (renderSettleFrames > 240) renderSettleFrames = 240;

		renderJpeg         = rBool("RenderJpeg", renderJpeg);
		renderQuality      = rInt("RenderQuality", renderQuality);

		// JPEG quality is a percentage. Out of range it reaches the encoder as
		// whatever the integer happened to be.
		if (renderQuality < 1)   renderQuality = 1;
		if (renderQuality > 100) renderQuality = 100;

		// THIS LINE WAS MISSING. The key was declared, copied into the render
		// settings, handed to the add-on and listed in kMoved so the migration
		// deleted it from the old ini - everything except being read. So it sat
		// at its default of 0 (Auto) whatever anyone wrote in the file, and the
		// reports were exactly what you would expect: "inverted colours, and it
		// makes no difference whether I set Auto, RGBA or BGRA", because only
		// Auto ever ran.
		renderChannelOrder = rInt("RenderChannelOrder", renderChannelOrder);
		renderHighlight    = rFloat("RenderHighlightBoost", renderHighlight);
		renderHideHud         = rBool("RenderHideHud", renderHideHud);
		// Both of these were renamed. Read the OLD key first so an existing
		// Render.ini keeps its meaning, then let the new one override if it is
		// present - so an ini written before the rename behaves identically, and
		// one written after wins outright. Silently reverting somebody's video
		// output to a frame dump because a key changed name is exactly the sort
		// of thing nobody notices until the render is finished.
		enableRenderer = rBool("ExportAsImageSequence", enableRenderer);
		enableRenderer = rBool("EnableRenderer", enableRenderer);

		exportCloseWhenDone   = rBool("ExportCloseWhenDone", exportCloseWhenDone);

		if (rBool("RenderVideo", renderMode == RenderMode::Video))
			renderMode = RenderMode::Video;
		{
			char m[32]{};
			rStr("RenderMode", "", m, sizeof(m));
			if (m[0])
			{
				// Accept the obvious spellings rather than one exact token: this
				// is hand-edited, and "Sequence" failing silently to Frames-that-
				// happened-to-be-the-default is indistinguishable from it working.
				renderMode = (_stricmp(m, "Video") == 0 || _stricmp(m, "MP4") == 0)
					? RenderMode::Video : RenderMode::Frames;
			}
		}

		{
			char m[32]{};
			rStr("RenderCaptureMode", "", m, sizeof(m));
			// Same tolerance as RenderMode above: hand-edited, so accept the
			// obvious spellings rather than silently falling back to the default,
			// which is indistinguishable from the setting having worked.
			if (m[0])
				renderCaptureMode = (_stricmp(m, "Sliding") == 0 ||
				                     _stricmp(m, "Slide")   == 0 ||
				                     _stricmp(m, "Play")    == 0) ? 1 : 0;
		}

		renderKeepFrames      = rBool("RenderKeepFrames", renderKeepFrames);
		renderAudio           = rBool("RenderAudio", renderAudio);
		renderMarkerSpeed     = rBool("RenderMarkerSpeed", renderMarkerSpeed);
		{
			char b[MAX_PATH]{};   rStr("FfmpegPath", "", b, sizeof(b));
			ffmpegPath = b;

			char a[1024]{};       rStr("RenderVideoArgs", renderVideoArgs.c_str(), a, sizeof(a));
			renderVideoArgs = a;

			char e[64]{};         rStr("RenderVideoExt", renderVideoExt.c_str(), e, sizeof(e));
			renderVideoExt = e;

			char pr[128]{};       rStr("RenderVideoPreset", "", pr, sizeof(pr));
			renderVideoPreset = pr;

			char of[MAX_PATH]{};  rStr("RenderOutputFolder", "", of, sizeof(of));
			renderOutputFolder = of;

			char au[MAX_PATH]{};  rStr("AudioFromFile", "", au, sizeof(au));
			audioFromFile = au;
		}

		if (!splitAlready) migrateRenderIni(rini, ini);

		shake.sway.posAmp   = getFloat("SwayMove", shake.sway.posAmp);
		shake.sway.rotAmp   = getFloat("SwayRotate", shake.sway.rotAmp);
		shake.sway.freq     = getFloat("SwayFrequency", shake.sway.freq);
		shake.jitter.posAmp = getFloat("JitterMove", shake.jitter.posAmp);
		shake.jitter.rotAmp = getFloat("JitterRotate", shake.jitter.rotAmp);
		shake.jitter.freq   = getFloat("JitterFrequency", shake.jitter.freq);
		shake.octaves = GetPrivateProfileIntA("RockstarEditorPlus", "ShakeRoughness",
			shake.octaves, ini.c_str());
		shake.seed    = GetPrivateProfileIntA("RockstarEditorPlus", "ShakeSeed",
			shake.seed, ini.c_str());

		shake.axisPos[0] = getFloat("ShakeAxisLateral",  shake.axisPos[0]);
		shake.axisPos[1] = getFloat("ShakeAxisForward",  shake.axisPos[1]);
		shake.axisPos[2] = getFloat("ShakeAxisVertical", shake.axisPos[2]);
		shake.axisRot[0] = getFloat("ShakeAxisPitch",    shake.axisRot[0]);
		shake.axisRot[1] = getFloat("ShakeAxisRoll",     shake.axisRot[1]);
		shake.axisRot[2] = getFloat("ShakeAxisYaw",      shake.axisRot[2]);

		// Simple mode and speed coupling. Defaulted ON with a usable intensity,
		// deliberately: the four layer amplitudes above all default to 0, so a
		// fresh install used to produce no shake at all until several numbers
		// had been found and set. Meeting the feature switched off is not a
		// neutral default, it is a broken first impression.
		shake.simpleMode = GetPrivateProfileIntA("RockstarEditorPlus",
			"ShakeSimpleMode", shake.simpleMode ? 1 : 0, ini.c_str()) != 0;
		// Renamed from ShakeIntensity/ShakeFrequencyMul when the two became
		// real units - degrees and hertz - rather than abstract multipliers.
		// The old keys are deliberately NOT read: a 1.0 that used to mean
		// "0.52 degrees through a hidden ratio" is not a 1.0 that means one
		// degree, and silently reinterpreting it would change every shot.
		shake.amplitude  = getFloat("ShakeAmplitude",  shake.amplitude);
		shake.frequency  = getFloat("ShakeFrequency",  shake.frequency);
		shake.variation  = getFloat("ShakeVariation",     shake.variation);

		shakeDebugLog  = GetPrivateProfileIntA("RockstarEditorPlus",
			"ShakeDebugLog", shakeDebugLog ? 1 : 0, ini.c_str()) != 0;
		splineDebugLog = GetPrivateProfileIntA("RockstarEditorPlus",
			"SplineDebugLog", splineDebugLog ? 1 : 0, ini.c_str()) != 0;
		splineDebugGain = getFloat("SplineDebugGain", splineDebugGain);
		if (splineDebugGain < 0.0f)   splineDebugGain = 0.0f;
		if (splineDebugGain > 100.0f) splineDebugGain = 100.0f;
		shake.speedAmp   = getFloat("ShakeSpeedAmp",      shake.speedAmp);
		shake.speedFreq  = getFloat("ShakeSpeedFreq",     shake.speedFreq);
		shake.speedRef   = getFloat("ShakeSpeedRef",      shake.speedRef);
		shake.stopWhenStill = GetPrivateProfileIntA("RockstarEditorPlus",
			"ShakeStopWhenStill", shake.stopWhenStill ? 1 : 0, ini.c_str()) != 0;

		auto fixLayer = [](shake::Layer& L) {
			if (L.posAmp < 0.0f) L.posAmp = 0.0f;
			if (L.rotAmp < 0.0f) L.rotAmp = 0.0f;
			if (L.freq  <= 0.0f) L.freq   = 1.0f;
		};
		fixLayer(shake.sway);
		fixLayer(shake.jitter);
		if (shake.octaves < 1) shake.octaves = 1;
		if (shake.octaves > 6) shake.octaves = 6;
		if (maxCameraDistance < 1.0f) maxCameraDistance = 1.0f;

		if (alpha < 0.0f) alpha = 0.0f;
		if (alpha > 1.0f) alpha = 1.0f;
	}

	static Config& get()
	{
		static Config cfg;
		return cfg;
	}
};
