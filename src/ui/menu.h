// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

namespace menu
{
	// Installs the Video Editor camera-menu row injection. Safe to call when
	// the menu addresses did not resolve — it simply does nothing and the
	// spline keeps working off the ini alone.
	void install();
}
