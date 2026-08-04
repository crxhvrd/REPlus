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

	// Times CVideoEditorPlayback::Open has been intercepted this session.
	// Reported in the readiness line so that "Export did nothing" can be told
	// apart from "Export was diverted and the render failed later".
	unsigned openCount();

	// False when our detour is no longer the code at the hook site - i.e.
	// something patched over us after we installed. Compares the bytes we left
	// there against what is there now.
	bool hookIntact();

	// Was the detour ever written at all? A failed INSTALL and a detour someone
	// else overwrote look identical from hookIntact() alone, and they need
	// completely different advice.
	bool hookInstalled();
	void clearPending();
}
