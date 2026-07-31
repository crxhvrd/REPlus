// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "utils/paths.h"
#include "replay/settings.h"

#include <map>
#include <mutex>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cmath>

namespace rsettings
{
	namespace
	{
		// Marker times are floats but always whole milliseconds in practice.
		// Quantising to an integer key avoids float-equality lookups failing on
		// a 1-ulp difference between what we stored and what we read back.
		using Key = int64_t;
		inline Key keyOf(float timeMs) { return (Key)llroundf(timeMs); }

		std::map<Key, MarkerSettings> g_entries;
		std::string                   g_path;
		bool                          g_dirty = false;

		// The camera hook reads these on the sim thread while the ReShade
		// overlay writes them on the render thread. A std::map is not safe
		// under that, so every entry point takes this lock. Contention is nil:
		// the reader does one lookup per frame.
		std::mutex                    g_mutex;

		// Flat text, one marker per line. Deliberately not JSON: no dependency,
		// trivially diffable, and easy to hand-edit if a project needs rescuing.
		//   v1
		//   <timeMs> <alpha> <easeIn> <easeOut> <path> <orient>
		constexpr const char* kHeader = "RockstarEditorPlus v";
	}

	MarkerSettings get(float timeMs)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_entries.find(keyOf(timeMs));
		return it == g_entries.end() ? MarkerSettings{} : it->second;
	}

	void set(float timeMs, const MarkerSettings& s)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const Key k = keyOf(timeMs);
		if (s.isDefault())
		{
			if (g_entries.erase(k)) g_dirty = true;
		}
		else
		{
			g_entries[k] = s;
			g_dirty = true;
		}
	}

	void rekey(float oldTimeMs, float newTimeMs)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const Key from = keyOf(oldTimeMs), to = keyOf(newTimeMs);
		if (from == to) return;

		auto it = g_entries.find(from);
		if (it == g_entries.end()) return;

		MarkerSettings s = it->second;
		g_entries.erase(it);
		g_entries[to] = s;
		g_dirty = true;
	}

	int count()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return (int)g_entries.size();
	}

	void bindProject(const char* projectName)
	{
		save(); // don't lose edits made to the outgoing project

		std::lock_guard<std::mutex> lock(g_mutex);
		g_entries.clear();
		g_dirty = false;

		if (!projectName || !*projectName) { g_path.clear(); return; }

		// Sanitise: project names come from user input and land in a path.
		std::string safe;
		for (const char* p = projectName; *p; ++p)
		{
			const char c = *p;
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
			safe += ok ? c : '_';
		}

		char dir[MAX_PATH]{};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   (LPCSTR)&bindProject, &self);
		GetModuleFileNameA(self, dir, MAX_PATH);
		if (char* slash = strrchr(dir, '\\')) *(slash + 1) = '\0';

		// Per-project marker overrides now live in the mod's markers\ folder.
		// `dir` is still resolved above because the legacy adoption below has to
		// look where older builds wrote.
		g_path = paths::sub("markers") + safe + ".txt";

		// Adopt a game-root file from the previous layout. Moved, not copied, so
		// there is only ever one file holding a project's overrides.
		if (GetFileAttributesA(g_path.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			const std::string prev = std::string(dir) + "RockstarEditorPlus_" + safe + ".txt";
			if (MoveFileA(prev.c_str(), g_path.c_str()))
				logger::write("info", "settings: moved %s into markers", prev.c_str());
		}

		std::ifstream in(g_path);
		if (!in)
		{
			// The tool used to be called SmoothBlendFix. Adopt an existing
			// side-car from that name rather than silently starting empty and
			// losing per-marker work; the next save writes the new filename.
			const std::string legacy = std::string(dir) + "SmoothBlendFix_" + safe + ".txt";
			in.open(legacy);
			if (in)
			{
				logger::write("info", "settings: adopting legacy file %s", legacy.c_str());
				g_dirty = true; // force a rewrite under the new name
			}
		}
		if (!in) { logger::write("info", "settings: new project '%s'", safe.c_str()); return; }

		std::string header;
		std::getline(in, header);
		if (header.rfind(kHeader, 0) != 0)
		{
			logger::write("info", "settings: '%s' has an unrecognised header - ignoring",
				g_path.c_str());
			return;
		}

		// The v4 layout replaced a fixed field list with a counted parameter
		// block, so an older file's columns mean different things. Parsing one
		// anyway would silently produce wrong values rather than obviously
		// wrong ones, so refuse it and keep the file for manual rescue.
		const int ver = atoi(header.c_str() + strlen(kHeader));
		if (ver < 4)
		{
			logger::write("info",
				"settings: '%s' is v%d, this build writes v4 - not loading "
				"(delete it, or re-set the markers you care about)",
				g_path.c_str(), ver);
			return;
		}

		// <time> <path> <orient> <count> <values...>
		// The explicit count means a file written by a future build with more
		// parameters still loads here: we take what we understand and ignore
		// the rest, rather than failing the whole line.
		std::string line;
		while (std::getline(in, line))
		{
			std::istringstream ls(line);
			long long t; int p, o;
			if (!(ls >> t >> p >> o)) continue;

			MarkerSettings s;
			s.path   = (PathMode)(p   < 0 || p > 2 ? 0 : p);
			s.orient = (OrientMode)(o < 0 || o > 2 ? 0 : o);

			int n = 0;
			if (ls >> n)
			{
				const int take = n > P_COUNT ? P_COUNT : n;
				for (int i = 0; i < take; ++i)
				{
					float f;
					if (!(ls >> f)) break;
					s.v[i] = f;
				}
				// Skip any parameters a newer build wrote that we do not know
				// about, so the v5 flag after them still lines up.
				for (int i = take; i < n; ++i) { float junk; if (!(ls >> junk)) break; }
			}

			// v5 trailing flag. Absent in a v4 file, which just leaves it false.
			int sh = 0;
			if (ls >> sh) s.ourShake = sh != 0;

			g_entries[(Key)t] = s;
		}

		logger::write("info", "settings: loaded %d marker override(s) from %s",
			(int)g_entries.size(), g_path.c_str());
	}

	void save()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_dirty || g_path.empty()) return;

		std::ofstream out(g_path, std::ofstream::out | std::ofstream::trunc);
		if (!out)
		{
			logger::write("info", "settings: !! could not write %s", g_path.c_str());
			return;
		}

		out << kHeader << "5" << '\n';
		for (const auto& kv : g_entries)
		{
			const MarkerSettings& s = kv.second;
			// <time> <path> <orient> <count> <values...>
			// The explicit count is what lets a future build add parameters
			// without invalidating files written by this one.
			out << kv.first << ' ' << (int)s.path << ' ' << (int)s.orient
			    << ' ' << (int)P_COUNT;
			for (int i = 0; i < P_COUNT; ++i) out << ' ' << s.v[i];
			// v5 appends the shake flag AFTER the counted block, so a v4 file
			// still parses here - it simply has no flag and reads back false.
			out << ' ' << (s.ourShake ? 1 : 0);
			out << '\n';
		}

		g_dirty = false;
	}
}
