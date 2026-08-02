// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

namespace limits
{
	// Lifts the free camera's maximum distance from the player. No-op when the
	// ini has it disabled or the address did not resolve.
	void install();

	// Per-frame. Installs the hooks that cannot be placed at startup because they
	// hang off an object the editor only creates once a clip is open. Cheap: a
	// bool after the first success.
	void tick();

	// Per-frame. Keeps the leash lifted by rewriting the director's metadata,
	// which is the only thing that works where the distance getter is inlined
	// (Enhanced). Restores the original the moment the option is turned off.
	void applyDistanceLimit(void* director);

	// Per-frame. Widens the editor's zoom range by rewriting the free camera's
	// MinFov/MaxFov metadata. Locates those fields by shape and does nothing at
	// all if the match is ambiguous. Restores them when switched back off.
	void applyZoomLimit(void* director);
}

