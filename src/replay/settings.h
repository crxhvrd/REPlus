// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdint>

// =============================================================================
//  Per-marker settings
// =============================================================================
//  sReplayMarkerInfo is a fixed 0xB8 struct serialised straight into .clip
//  files - there is nowhere to put our own fields without breaking the format
//  Rockstar's own loader validates. So settings live in a side-car file keyed
//  by the marker's non-dilated time (+0x70).
//
//  Why time is the key: the editor refuses two markers at the same time, so it
//  is unique, and unlike an index it survives markers being added or removed
//  earlier in the timeline. It does NOT survive dragging a marker, which is
//  why rekey() exists.
//
//  Numeric overrides live in one flat array rather than named members. They all
//  mean the same thing (-1 = inherit the global), they are all edited by the
//  same menu code, and they all serialise identically - so adding a knob is one
//  enum entry instead of a change in four places.
// =============================================================================
namespace rsettings
{
	// How a segment's orientation should be driven.
	enum class OrientMode : uint8_t
	{
		Inherit = 0,  // use the global ini setting
		Stock,        // leave the game's slerp + rotational springs alone
		Squad,        // squad spline over the marker quaternions
	};

	// Whether this segment gets a curve at all.
	enum class PathMode : uint8_t
	{
		Inherit = 0,  // use the global ini setting
		Stock,        // hands off - stock Linear/Smooth for this segment
		Spline,       // Catmull-Rom
	};

	enum Param : int
	{
		P_ALPHA = 0,    // Catmull-Rom knot parameterisation
		P_EASE_IN,
		P_EASE_OUT,

		P_SWAY_POS,     // metres
		P_SWAY_ROT,     // degrees
		P_SWAY_FREQ,
		P_JIT_POS,
		P_JIT_ROT,
		P_JIT_FREQ,
		P_OCTAVES,
		P_SEED,

		P_AX_LAT,       // per-axis weights
		P_AX_FWD,
		P_AX_VERT,
		P_AX_PITCH,
		P_AX_ROLL,
		P_AX_YAW,

		// APPEND ONLY, and only here at the end. The side-car writes the count
		// it knows and the loader takes min(written, P_COUNT), skipping any
		// extras a newer build left - so growing this list forwards and
		// backwards compatible, while inserting in the middle silently
		// reinterprets every value after the insertion point.
		P_SIMPLE,       // 0/1: simple mode instead of the full layer model
		P_INTENSITY,    // simple: master amplitude
		P_FREQ_MUL,     // simple: scales both layer frequencies
		P_SPEED_AMP,    // speed -> amplitude coupling
		P_SPEED_FREQ,   // speed -> frequency coupling
		P_STOP_STILL,   // 0/1: fade the shake out when the camera is parked
		P_VARIATION,    // how unevenly the shake plays out over time
		P_UNUSED_SINE,  // was the noise-model switch; sines are unconditional
		                // now. Kept as a HOLE rather than removed - the side-car
		                // is positional, so deleting it would shift every value
		                // after it in files already on disk.

		P_COUNT
	};

	struct MarkerSettings
	{
		// -1 means "unset, inherit the global". 0 is a legitimate value for
		// every parameter here (an axis weight of 0 kills that axis), which is
		// why the sentinel is negative rather than zero.
		float      v[P_COUNT];
		PathMode   path   = PathMode::Inherit;
		OrientMode orient = OrientMode::Inherit;

		// This marker uses OUR shake instead of one of the game's presets.
		//
		// It is stored here rather than as a 7th value of the marker's own
		// m_shakeType, deliberately. eMarkerShakeType only runs 0..5 and
		// UpdateShakeType wraps at MAX, so a 7th value could only be written by
		// hand - and it would be SAVED INTO THE PROJECT FILE, where stock code
		// on a machine without this mod would then consume it. Keeping the flag
		// on our side means the marker still holds a valid MARKER_SHAKE_NONE and
		// such a project just plays back with no shake.
		bool       ourShake = false;

		MarkerSettings() { for (int i = 0; i < P_COUNT; ++i) v[i] = -1.0f; }

		bool has(Param p) const { return v[p] >= 0.0f; }

		bool isDefault() const
		{
			if (path != PathMode::Inherit || orient != OrientMode::Inherit) return false;
			if (ourShake) return false;
			for (int i = 0; i < P_COUNT; ++i) if (v[i] >= 0.0f) return false;
			return true;
		}
	};

	// Look up settings for a marker time. Never fails - returns an all-inherit
	// record when there is no entry.
	MarkerSettings get(float timeMs);

	// Insert or update. Entries that revert to all-inherit are dropped so the
	// side-car does not accumulate no-op rows.
	void set(float timeMs, const MarkerSettings& s);

	// Move an entry when a marker is dragged. Call from a
	// SetMarkerNonDilatedTimeMs path, or settings orphan at the old timestamp.
	void rekey(float oldTimeMs, float newTimeMs);

	// Bind to a project. Loads the side-car next to the ASI.
	void bindProject(const char* projectName);

	// Flush to disk if anything changed. Cheap to call often.
	//
	// NOT cheap when it does write: the whole side-car is rewritten. Call it on
	// events with a real deadline (project switch, shutdown), not on every value
	// change - use tick() for that.
	void save();

	// Per-frame. Flushes once the store has been quiet for ~0.5s, so a held
	// adjustment costs one write instead of one per input repeat.
	void tick();

	// Number of markers with non-default settings.
	int count();
}
