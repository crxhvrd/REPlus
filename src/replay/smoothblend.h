// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

namespace smoothblend
{
	// Installs the camReplayDirector::UpdateSmoothing detour. Safe to call when
	// nothing resolved — it simply does nothing.
	void install();

	// The camReplayDirector instance seen by the most recent hook call, or null
	// before playback has run. We are handed it every frame anyway, so the UI
	// can read the live camera frame without resolving the director global.
	void* lastDirector();
}
