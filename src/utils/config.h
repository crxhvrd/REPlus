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
			"RenderFps=%g%s; output rate, independent of your actual framerate\n"
			"RenderSamples=%d%s; sub-frames averaged per output frame. 1 = no blur\n"
			"RenderShutter=%g%s; 1.0 = 360 degrees, 0.5 = the 180 film convention\n"
			"RenderSettleFrames=%d%s; redraw wait after seeking to a new output frame\n"
			"RenderSettleSubFrames=%d%s; ...between blur sub-samples. Never 0\n"
			"RenderHighlightBoost=%g%s; keeps specular streaks bright through accumulation\n"
			"RenderJpeg=%d%s; 0 = PNG\n"
			"RenderQuality=%d%s; JPEG only\n"
			"RenderChannelOrder=%d%s; 0 auto / 1 RGBA / 2 BGRA - only if red and blue swap\n"
			"RenderHideHud=%d%s; hide the editor HUD and cursor while rendering\n"
			"ExportCloseWhenDone=%d%s; return to the editor menus after an Export render\n"
			"; Empty = RockstarEditorPlus\\Captures\\\n"
			"RenderOutputFolder=%s\n\n",
			enableRenderer ? 1 : 0,
			renderFps, "              ", renderSamples, "          ",
			renderShutter, "          ", renderSettleFrames, "     ",
			renderSettleSubFrames, "  ", renderHighlight, "   ",
			renderJpeg ? 1 : 0, "               ", renderQuality, "            ",
			renderChannelOrder, "       ", renderHideHud ? 1 : 0, "            ",
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
			"; sound, then rewinds to clip one and renders the frames. Needs RenderMode=Video.\n"
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

		static const char* const kMoved[] = {
			"ExportAsImageSequence", "EnableRenderer", "RenderMode",
			"RenderFps", "RenderSamples", "RenderShutter",
			"RenderSettleFrames", "RenderSettleSubFrames", "RenderHighlightBoost",
			"RenderJpeg", "RenderQuality", "RenderChannelOrder", "RenderHideHud",
			"ExportCloseWhenDone", "RenderOutputFolder", "RenderVideo",
			"RenderKeepFrames", "FfmpegPath", "RenderVideoPreset",
			"RenderVideoArgs", "RenderVideoExt",
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
	// It also has to agree with renderAudio below, which needs a video to mux
	// into. Shipping audio on with Frames would be a default that silently
	// cannot do what it says.
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

	// Record the project's sound in real time, then render the frames, and mux
	// the two. Both passes run off ONE Export press: the sound pass plays the
	// project through at normal speed, and the renderer then steps the playhead
	// back to clip one and starts the frames without leaving playback.
	//
	// (It used to take two presses. A plain seek cannot return to the start of
	// a project - it clamps to the clip on screen, and the sound pass ends on
	// the last one - so re-pressing Export was the only reliable rewind. The
	// clip-step call the multi-clip renderer uses does it directly.)
	//
	// On by default now that it costs one press rather than two. A silent
	// render is almost never what someone wanted, and the failure was quiet -
	// you found out when you opened the file. Needs renderMode Video, which is
	// also the default; the two are set together deliberately.
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

	// Leave the editor's playback and return to its menus once an export render
	// finishes, the same way a real export does. Only applies to renders started
	// from the Export button - a render started from the camera menu leaves you
	// where you were.
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
		renderSettleFrames = rInt("RenderSettleFrames", renderSettleFrames);
		renderSettleSubFrames = rInt("RenderSettleSubFrames", renderSettleSubFrames);
		if (renderSettleSubFrames < 1) renderSettleSubFrames = 1;
		renderJpeg         = rBool("RenderJpeg", renderJpeg);
		renderQuality      = rInt("RenderQuality", renderQuality);
		renderHighlight    = rFloat("RenderHighlightBoost", renderHighlight);
		renderChannelOrder = rInt("RenderChannelOrder", renderChannelOrder);
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

		renderKeepFrames      = rBool("RenderKeepFrames", renderKeepFrames);
		renderAudio           = rBool("RenderAudio", renderAudio);
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
