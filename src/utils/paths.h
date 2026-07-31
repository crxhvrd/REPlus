// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <Windows.h>
#include <string>

// =============================================================================
//  Where this mod keeps its things
// =============================================================================
//  Everything lives under ONE folder beside the .asi — literally beside the
//  .asi, resolved from this module's own handle rather than from the host
//  executable, because under FiveM those are different folders entirely:
//
//    <install>\RockstarEditorPlus.asi
//    <install>\RockstarEditorPlus\
//        RockstarEditorPlus.ini      settings
//        RockstarEditorPlus.log      log
//        presets\                    encoder presets, one .ini each
//        Captures\                   renders - frames and/or video
//        markers\                    per-project marker overrides
//
//  Previously the ini, the log, the marker files and the capture folder were
//  four separate things scattered through the game root, which is both untidy
//  in a directory that already holds hundreds of files and makes the mod
//  awkward to remove.
//
//  Header-only because config.h needs this before anything else is up, and
//  there is not enough here to justify a translation unit.
// =============================================================================
namespace paths
{
	namespace detail
	{
		inline std::string exeDir()
		{
			char p[MAX_PATH]{};
			GetModuleFileNameA(GetModuleHandleA(nullptr), p, MAX_PATH);
			if (char* slash = strrchr(p, '\\')) *(slash + 1) = '\0';
			return p;
		}

		// Anchor whose address is guaranteed to lie inside THIS module, so the
		// lookup below finds the .asi and not whatever loaded it.
		inline void moduleAnchor() {}

		inline std::string asiDir()
		{
			char p[MAX_PATH]{};
			HMODULE self = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                       reinterpret_cast<LPCSTR>(&moduleAnchor), &self))
			{
				GetModuleFileNameA(self, p, MAX_PATH);
				if (char* slash = strrchr(p, '\\')) *(slash + 1) = '\0';
				return p;
			}
			return exeDir();   // cannot happen in practice; keep a sane answer
		}
	}

	// "<host exe>\" — where the running executable lives, trailing slash
	// included. NOT necessarily where the .asi is: see asiDir/baseDir below.
	// Kept for the legacy-layout migration and the ffmpeg-beside-the-game
	// fallback, both of which really do mean the game folder.
	inline const std::string& gameDir()
	{
		static const std::string d = detail::exeDir();
		return d;
	}

	// "<folder holding RockstarEditorPlus.asi>\", trailing slash included.
	//
	// Resolved from our OWN module, not from the host executable. In a normal
	// install the two are the same folder and this changes nothing — but they
	// are not the same under FiveM, which runs the game out of
	//     FiveM.app\data\cache\subprocess\
	// while the .asi sits in FiveM.app\plugins\. Keying off the exe put the log
	// and the ini into the subprocess cache, miles from the folder the user
	// actually installed to, and left the ini they edited being silently
	// ignored in favour of a default one written next to the game process.
	inline const std::string& asiDir()
	{
		static const std::string d = detail::asiDir();
		return d;
	}

	// "<asi folder>\RockstarEditorPlus\" — created on first use.
	inline const std::string& baseDir()
	{
		static const std::string d = [] {
			std::string b = asiDir() + "RockstarEditorPlus\\";
			CreateDirectoryA(b.c_str(), nullptr);
			return b;
		}();
		return d;
	}

	// A subfolder of the base, created if missing. Trailing slash included.
	inline std::string sub(const char* name)
	{
		std::string d = baseDir() + name + "\\";
		CreateDirectoryA(d.c_str(), nullptr);
		return d;
	}

	// A file directly in the base folder.
	inline std::string file(const char* name)
	{
		return baseDir() + name;
	}

	// One-time move of a file from the old layout into the new one.
	//
	// Moved rather than read-in-place on purpose: falling back to the old
	// location would leave settings split across two files, where editing the
	// obvious one silently does nothing. Never overwrites - if both exist the
	// new one wins and the old is left alone rather than destroyed.
	inline bool migrate(const char* legacyRelPath, const std::string& newPath)
	{
		const std::string old = gameDir() + legacyRelPath;
		if (GetFileAttributesA(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
		if (GetFileAttributesA(old.c_str())     == INVALID_FILE_ATTRIBUTES) return false;
		return MoveFileA(old.c_str(), newPath.c_str()) != 0;
	}
}
