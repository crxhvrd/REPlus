// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

namespace exportmenu
{
	// Adds the renderer's settings to the Video Editor's own EXPORT screen -
	// the one with Frame Rate / Bit rate / Export on it.
	//
	// Safe to call when the addresses did not resolve: nothing is hooked and the
	// export screen stays exactly as R* shipped it, with the renderer still
	// driven from Render.ini as before.
	void install();
}
