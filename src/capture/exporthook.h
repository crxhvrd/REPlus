// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// Redirects the editor's Export button to the image-sequence renderer.
// See exporthook.cpp for why this is one rewritten argument and not menu rows.
namespace exporthook
{
	void install();

	// True between diverting a bake and the render actually starting. The
	// renderer polls this from its per-frame pump, because playback is not
	// usable at the moment Open is called.
	bool pending();
	void clearPending();
}
