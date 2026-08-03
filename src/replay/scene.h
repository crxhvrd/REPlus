// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// =============================================================================
//  Time of day and weather, changed live during playback
// =============================================================================
//  A clip records the clock and the weather every frame and playback replays
//  them, so neither can be changed from the editor: whatever you set is
//  overwritten on the next frame. This substitutes different values on their
//  way out of the buffer instead of fighting them afterwards.
//
//  Nothing is written to the .clip on disk. The override lives for as long as
//  the option is on, and the clip plays back exactly as recorded the moment it
//  is turned off.
// =============================================================================
namespace scene
{
	// Hooks CPacketWeather::Extract. Safe to call when the address did not
	// resolve; the feature simply stays off.
	void install();

	// True once the hook is in place, i.e. the menu rows can do something.
	bool ready();

	// True when the timecycle can actually be re-lit, i.e. its variable table
	// resolved.
	bool canRelight();

	// True when the Scene overrides do anything. Timecycle is the master
	// switch: on "As Recorded" the clip's baked lighting stands, so changing
	// the time or the weather could only produce a sun that moves across an
	// unchanged sky. Both the hook and the menu read this, so what the rows say
	// and what playback does cannot drift apart.
	bool overridesActive();

	// Per-frame, and it MUST also run outside the editor: the flag this manages
	// is read while recording as well, so it has to go back the moment the
	// editor is left. Cheap - one compare in the common case.
	void tick();

	// Puts back the one clock packet we may have patched. Call before anything
	// that tears playback down, so the buffer is left exactly as it was found.
	void restore();

	// Number of weather types the game has, for the menu's row stepping.
	int  weatherTypeCount();

	// Display name for a weather index, e.g. "THUNDER". Never null.
	const char* weatherTypeName(int index);
}
