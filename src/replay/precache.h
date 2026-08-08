// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// The editor's per-action stall ("buffering"). See precache.cpp for what it is
// and why no amount of heap adjustment ever fixed it.
namespace precache
{
	void install();

	// One line, the first time a precache is short-circuited and again if the
	// picture changes. Called from the per-frame tick so it can report the
	// stall that was actually skipped rather than a hook that was merely
	// installed.
	void tick();
}
