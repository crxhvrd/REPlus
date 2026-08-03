// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "utils/paths.h"

#include <string>
#include "capture/fxcapture.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <psapi.h>
#include <shlwapi.h>
#include <vector>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shlwapi.lib")

namespace fxcapture
{
	namespace
	{
		// Same name and magic as Simple Camera - deliberately. One addon, one
		// channel, whichever ASI is rendering drives it. They are never
		// rendering at the same time: one runs in free roam, one in the editor.
		const char* kMappingName = "Local\\SimpleCameraFxCapture";
		constexpr uint32_t kMagic   = 0x53434658; // 'SCFX'
		constexpr uint32_t kVersion = 6;

		// One place, always, next to the exe. Every render is a numbered
		// subfolder inside it.
		// Renders go in the mod's own folder now, one numbered subfolder each -
		// see utils/paths.h for the layout. The game root is left alone.

		HANDLE          s_map   = nullptr;
		FxCaptureBlock* s_block = nullptr;

		// CreateDirectory only makes the leaf, so a configured path several
		// levels deep would fail on the first missing parent. Walk it.
		void createTree(const char* path)
		{
			char buf[MAX_PATH];
			strncpy_s(buf, sizeof(buf), path, _TRUNCATE);
			for (char* p = buf; *p; ++p)
			{
				if (*p != '\\' && *p != '/') continue;
				// Skip "C:\" and the leading slashes of a UNC path - those are
				// not directories anyone can create.
				if (p == buf || *(p - 1) == ':' || *(p - 1) == '\\') continue;
				const char save = *p;
				*p = '\0';
				CreateDirectoryA(buf, nullptr);
				*p = save;
			}
			CreateDirectoryA(buf, nullptr);
		}

		// The folder GTA5.exe lives in - NOT the folder this ASI lives in. They
		// are usually the same, but some loaders run plugins from a subfolder,
		// and renders turning up somewhere different depending on how the user
		// installed the mod is exactly the confusion this folder is meant to end.
		void gameDir(char* out, size_t cap)
		{
			GetModuleFileNameA(GetModuleHandleA(nullptr), out, (DWORD)cap);
			if (char* slash = strrchr(out, '\\')) *slash = '\0';
		}

		// =====================================================================
		//  Which ReShade is loaded, if any
		// =====================================================================
		//  "Export fell back to the vanilla encoder" has one overwhelmingly
		//  common cause: the ORDINARY ReShade build, which cannot load add-ons
		//  at all. The add-on is then never loaded, never presents, the
		//  heartbeat stays zero and Export correctly declines to divert - and
		//  nothing anywhere says why. It is in both READMEs and it still cost a
		//  support round, because people install first and read later.
		//
		//  It is decidable, and by exactly the test ReShade's own add-on header
		//  uses to find its host: walk the loaded modules for one exporting
		//  ReShadeRegisterAddon / ReShadeUnregisterAddon. Those live inside
		//  `#if RESHADE_ADDON`, so the standard build does not have them.
		//
		//  When that fails we still want to distinguish "no ReShade" from "the
		//  wrong ReShade", so a second pass looks for a module whose version
		//  resource names ReShade. Present without the exports IS the wrong
		//  build, and that is the sentence worth printing.
		// =====================================================================
		bool moduleNamesReShade(HMODULE m)
		{
			char path[MAX_PATH]{};
			if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;

			DWORD handle = 0;
			const DWORD size = GetFileVersionInfoSizeA(path, &handle);
			if (!size) return false;

			std::vector<char> buf(size);
			if (!GetFileVersionInfoA(path, handle, size, buf.data())) return false;

			// Language-agnostic: walk whatever translation the file actually has
			// rather than assuming 040904b0.
			struct LangCp { WORD lang, cp; }* tr = nullptr;
			UINT trLen = 0;
			if (!VerQueryValueA(buf.data(), "\\VarFileInfo\\Translation",
			                    (void**)&tr, &trLen) || trLen < sizeof(LangCp))
				return false;

			for (UINT i = 0; i < trLen / sizeof(LangCp); ++i)
			{
				for (const char* field : { "ProductName", "FileDescription" })
				{
					char q[128]{};
					sprintf_s(q, "\\StringFileInfo\\%04x%04x\\%s",
						tr[i].lang, tr[i].cp, field);
					char* val = nullptr; UINT vlen = 0;
					if (VerQueryValueA(buf.data(), q, (void**)&val, &vlen) && val)
						if (StrStrIA(val, "ReShade")) return true;
				}
			}
			return false;
		}

		void reportReShade()
		{
			HMODULE mods[1024]{};
			DWORD   need = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
			{
				logger::write("info", "capture: could not enumerate modules - "
					"cannot tell which ReShade is loaded");
				return;
			}
			const DWORD count = (need > sizeof(mods) ? sizeof(mods) : need) / sizeof(HMODULE);

			HMODULE addonCapable = nullptr, looksLikeReShade = nullptr;
			for (DWORD i = 0; i < count; ++i)
			{
				if (GetProcAddress(mods[i], "ReShadeRegisterAddon") &&
				    GetProcAddress(mods[i], "ReShadeUnregisterAddon"))
				{
					addonCapable = mods[i];
					break;
				}
				if (!looksLikeReShade && moduleNamesReShade(mods[i]))
					looksLikeReShade = mods[i];
			}

			char path[MAX_PATH]{};
			if (addonCapable)
			{
				GetModuleFileNameA(addonCapable, path, MAX_PATH);
				logger::write("info", "capture: ReShade WITH add-on support -> %s", path);
			}
			else if (looksLikeReShade)
			{
				GetModuleFileNameA(looksLikeReShade, path, MAX_PATH);
				logger::write("info",
					"capture: !! ReShade is loaded (%s) but it is the ORDINARY build - it "
					"cannot load add-ons at all, so IgcsConnector will never run and Export "
					"will always fall back to the game's own encoder. Reinstall ReShade and "
					"choose the version WITH full add-on support.", path);
			}
			else
			{
				logger::write("info",
					"capture: no ReShade in this process. Rendering and depth of field need "
					"ReShade WITH full add-on support; everything else works without it.");
			}
		}

	}

	void init()
	{
		if (s_block) return;

		SetLastError(0);
		s_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		                           0, sizeof(FxCaptureBlock), kMappingName);
		if (!s_map)
		{
			logger::write("info", "capture: could not create the shared channel");
			return;
		}
		const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);

		s_block = (FxCaptureBlock*)MapViewOfFile(s_map, FILE_MAP_ALL_ACCESS, 0, 0,
		                                         sizeof(FxCaptureBlock));
		if (!s_block)
		{
			CloseHandle(s_map);
			s_map = nullptr;
			return;
		}

		// Only zero it if we are the one who created it. Simple Camera or the
		// addon may already own this block and be mid-render.
		if (!existed) memset(s_block, 0, sizeof(FxCaptureBlock));

		s_block->magic   = kMagic;
		s_block->version = kVersion;
		if (s_block->quality == 0) s_block->quality = 90;

		logger::write("info", "capture: channel mapped (%s), addon=%s",
			existed ? "joined" : "created",
			s_block->addonHeartbeat ? "present" : "not seen yet");

		// Which ReShade, if any. The answer decides whether rendering can work
		// at all, and it is knowable here rather than after a failed Export.
		reportReShade();
	}

	bool available()    { return s_block != nullptr; }

	// Read through volatile.
	//
	// These three are written by ANOTHER PROCESS - the capture addon - and are
	// polled here in a loop that does nothing else. Read as plain fields, the
	// compiler is entitled to hoist them out of the caller's polling loop and
	// spin forever on a stale copy; nothing in the source tells it the value can
	// change under it. The block is shared memory, so it never can be cached
	// that way safely.
	namespace
	{
		inline uint32_t vread(const uint32_t& field)
		{
			return *(const volatile uint32_t*)&field;
		}
	}

	bool addonPresent() { return s_block && vread(s_block->addonHeartbeat) != 0; }
	bool lastDone()     { return !s_block || vread(s_block->ackId) == vread(s_block->requestId); }

	uint32_t heartbeat() { return s_block ? vread(s_block->addonHeartbeat) : 0; }

	bool requestSample(const char* fullPath, int sampleCount, int sampleIndex)
	{
		if (!s_block || !fullPath) return false;

		strncpy_s(s_block->outPath, sizeof(s_block->outPath), fullPath, _TRUNCATE);
		s_block->sampleCount = sampleCount < 1 ? 1u : (uint32_t)sampleCount;
		s_block->sampleIndex = (uint32_t)sampleIndex;
		s_block->status      = 0;

		// The ordering below is the entire handshake, so it needs a barrier
		// rather than a comment.
		//
		// x86 will not reorder the stores at run time, but the COMPILER will:
		// none of these fields is volatile and the increment does not depend on
		// the path copy, so at /O2 it is free to sink the copy past the bump.
		// The addon would then act on a request whose outPath is still the
		// previous frame's - which writes one frame twice and loses another,
		// and only under optimisation, on some builds, some of the time.
		std::atomic_thread_fence(std::memory_order_release);

		// Bumped LAST, so every field is in place before the addon can see the
		// request. Same ordering rule as Simple Camera's writer.
		*(volatile uint32_t*)&s_block->requestId = s_block->requestId + 1;
		return true;
	}

	void setQuality(int q)
	{
		if (!s_block) return;
		if (q < 1) q = 1;
		if (q > 100) q = 100;
		s_block->quality = (uint32_t)q;
	}

	void setHighlightBoost(float b)
	{
		if (!s_block) return;
		if (b < 0.0f) b = 0.0f;
		if (b > 0.99f) b = 0.99f;
		s_block->highlightBoost = b;
	}

	void setChannelOrder(int order)
	{
		if (!s_block) return;
		if (order < 0 || order > 2) order = 0;
		s_block->channelOrder = (uint32_t)order;
	}

	bool newSequenceFolder(const char* baseIn, char* outFolder, int cap)
	{
		if (!s_block) return false;

		char base[MAX_PATH];
		if (baseIn && *baseIn)
		{
			strncpy_s(base, sizeof(base), baseIn, _TRUNCATE);
			// A trailing separator would give us "dir\\render_0001".
			for (size_t n = strlen(base); n && (base[n - 1] == '\\' || base[n - 1] == '/'); --n)
				base[n - 1] = '\0';
			createTree(base);
		}
		else
		{
			// sub() hands back a trailing separator; drop it so the
			// numbered subfolder below does not end up double-slashed.
			std::string caps = paths::sub("Captures");
			if (!caps.empty() && caps.back() == '\\') caps.pop_back();
			strncpy_s(base, sizeof(base), caps.c_str(), _TRUNCATE);
		}

		for (int n = 1; n < 10000; ++n)
		{
			char folder[MAX_PATH];
			sprintf_s(folder, "%s\\render_%04d", base, n);
			if (GetFileAttributesA(folder) != INVALID_FILE_ATTRIBUTES) continue;
			if (!CreateDirectoryA(folder, nullptr)) continue;
			if (outFolder && cap > 0) strncpy_s(outFolder, cap, folder, _TRUNCATE);
			return true;
		}
		return false;
	}

	// Same resolution as above, without creating anything - see the header.
	std::string captureBaseDir(const char* base)
	{
		if (base && *base)
		{
			std::string b = base;
			while (!b.empty() && (b.back() == '\\' || b.back() == '/')) b.pop_back();
			return b;
		}
		std::string caps = paths::sub("Captures");
		if (!caps.empty() && caps.back() == '\\') caps.pop_back();
		return caps;
	}
}
