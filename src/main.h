// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

#include <Windows.h>
#include <cmath>
#include <cstring>

#include "utils/log.h"
#include "utils/memory.h"
#include "utils/config.h"

#include "game/game.h"
#include "replay/smoothblend.h"
#include "replay/settings.h"
#include "replay/limits.h"
#include "ui/menu.h"

inline const char* ExeName()
{
	static char name[MAX_PATH] = { 0 };
	if (!name[0])
	{
		char path[MAX_PATH];
		GetModuleFileNameA(GetModuleHandleA(nullptr), path, MAX_PATH);
		const char* fn = strrchr(path, '\\');
		strncpy_s(name, fn ? fn + 1 : path, _TRUNCATE);
	}
	return name;
}

inline bool IsEnhanced()
{
	return _stricmp(ExeName(), "GTA5_Enhanced.exe") == 0;
}

// FiveM runs the game in its own subprocess, FiveM_b<build>_GTAProcess.exe.
//
// Detected by MODULE rather than by process name: the name carries a build
// number that changes with every FiveM update, whereas asi-five.dll is by
// definition already loaded at the moment it is loading us. The name test is
// kept only as a fallback in case those modules are ever renamed.
//
// FiveM is a SUPPORTED host. This is not a gate - it selects the start path,
// because the deferred-init detour used everywhere else cannot be installed
// there (see main.cpp).
inline bool IsFiveM()
{
	return GetModuleHandleA("asi-five.dll") != nullptr ||
	       GetModuleHandleA("citizen-game-main.dll") != nullptr ||
	       GetModuleHandleA("scripting-gta.dll") != nullptr ||
	       strstr(ExeName(), "GTAProcess") != nullptr;
}

// Hosts the mod will install into: singleplayer GTA V, either build, and FiveM.
//
// FiveM took three separate fixes to reach, all of them recorded here because
// each looked like the answer at the time and only the last one was:
//   * the deferred-init detour on kernel32!GetSystemTimeAsFileTime cannot be
//     placed there (MH_ERROR_MEMORY_ALLOC - no trampoline within +/-2GB of
//     kernel32 once CEF, V8 and mono are loaded), so init routes through
//     ScriptHookV instead. See main.cpp.
//   * FiveM ships its own msvcp140 (14.34) in FiveM.app\bin and the game
//     subprocess loads that rather than the system's. Built against a newer
//     toolset, the inlined STL bound to an older runtime and died on the first
//     std::mutex lock. Fixed by linking the CRT statically - see CMakeLists.
//   * fx_asi_build.rc must declare the running build id or the plugin is
//     refused outright.
//
// With those in place nothing else about FiveM needs special handling: the
// Legacy signature set resolves there unchanged.
inline bool IsSupportedGame()
{
	return IsFiveM() ||
	       _stricmp(ExeName(), "GTA5_Enhanced.exe") == 0 ||
	       _stricmp(ExeName(), "GTA5.exe") == 0;
}

