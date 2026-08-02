// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "utils/paths.h"
#include "game/game.h"
#include "replay/settings.h"

#include <map>
#include <mutex>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace rsettings
{
	namespace
	{
		// Marker times are floats but always whole milliseconds in practice.
		// Quantising to an integer key avoids float-equality lookups failing on
		// a 1-ulp difference between what we stored and what we read back.
		//
		// The CLIP INDEX is folded into the high half, so two clips in one
		// project stop sharing entries just because their markers happen to sit
		// at the same time. The file itself is per project - between the two,
		// the old "everything in one file keyed by time" collision is gone.
		using Key = int64_t;
		inline Key keyOf(int clip, float timeMs)
		{
			return ((Key)(uint32_t)clip << 32) | (uint32_t)llroundf(timeMs);
		}
		inline int  clipOf(Key k)   { return (int)(uint32_t)((uint64_t)k >> 32); }
		inline long timeOf(Key k)   { return (long)(uint32_t)(uint64_t)k; }

		// Which clip lookups are scoped to. Maintained by syncScope().
		int g_clip = 0;

		std::map<Key, MarkerSettings> g_entries;

		// The path twice over. g_wpath is what the file calls actually use -
		// project names are UTF-8 and the -A APIs would reinterpret those bytes
		// in the system codepage, turning a Russian project name into mojibake on
		// disk. g_path is the UTF-8 form, kept only so the log can name the file;
		// the log is UTF-8 already, so it prints correctly there.
		std::string                   g_path;
		std::wstring                  g_wpath;

		bool                          g_dirty = false;

		// Tick count of the most recent change, for the debounce in tick().
		// 0 = nothing pending.
		unsigned long                 g_dirtyAt = 0;

		// The camera hook reads these on the sim thread while the ReShade
		// overlay writes them on the render thread. A std::map is not safe
		// under that, so every entry point takes this lock. Contention is nil:
		// the reader does one lookup per frame.
		std::mutex                    g_mutex;

		// Flat text. Deliberately not JSON: no dependency, trivially diffable,
		// and easy to hand-edit if a project needs rescuing.
		//
		//   RockstarEditorPlus v6
		//   clip 0
		//   marker 2000 path=2 orient=1 shake=1 alpha=0.5 easeIn=1
		//   marker 7500 swayPos=0.02
		//   clip 1
		//   marker 0 orient=2
		//
		// A `clip` line sets the scope for the `marker` lines under it. Fields are
		// NAMED and only written when set, which is both far smaller than the old
		// positional block and immune to the reordering problem that left
		// P_UNUSED_SINE stuck as a permanent hole.
		constexpr const char* kHeader  = "RockstarEditorPlus v";
		constexpr int         kVersion = 6;

		// The flag and its timestamp are ONE piece of state and must move
		// together. tick() debounces on the timestamp, so a g_dirty raised
		// without one is a change that never gets flushed at all - which is
		// exactly what happened to an adopted legacy side-car: bindProject set
		// the flag to force a rewrite under the new name, left the timestamp at
		// zero, and the file stayed unwritten until the user happened to edit a
		// marker. Clearing had the mirror problem, leaving a stale timestamp from
		// the outgoing project behind.
		//
		// Both callers already hold g_mutex.
		inline void markDirty()
		{
			g_dirty   = true;
			g_dirtyAt = GetTickCount();
		}

		inline void clearDirty()
		{
			g_dirty   = false;
			g_dirtyAt = 0;
		}

		std::wstring widen(const std::string& s, UINT codepage)
		{
			if (s.empty()) return {};
			const int n = MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(),
			                                  nullptr, 0);
			if (n <= 0) return {};
			std::wstring w((size_t)n, L'\0');
			MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(), &w[0], n);
			return w;
		}

		// Make a project name safe to put in a filename WITHOUT flattening it.
		//
		// Only the characters Windows actually rejects are replaced, and every one
		// of those is ASCII - so the UTF-8 bytes of a non-Latin name pass through
		// untouched and 'Проект 25' stays 'Проект 25'. The previous version
		// allowed only [A-Za-z0-9 _-], which turned every Cyrillic name into a row
		// of underscores: unreadable, and worse, it collided - two different names
		// of the same byte length produced the same file, which is the exact bug
		// per-project scoping exists to fix.
		std::string sanitise(const char* name)
		{
			std::string out;
			for (const char* p = name; *p; ++p)
			{
				const unsigned char c = (unsigned char)*p;
				const bool illegal = c < 0x20 || c == '<' || c == '>' || c == ':' ||
				                     c == '"' || c == '/' || c == '\\' || c == '|' ||
				                     c == '?' || c == '*' || c == 0x7F;
				out += illegal ? '_' : (char)c;
			}

			// Windows silently drops trailing dots and spaces, so a name ending in
			// one would resolve to a file we did not think we asked for.
			while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();

			// Reserved device names. A project called CON or LPT1 is absurd but
			// costs nothing to survive, and the failure without this is an
			// unopenable file rather than anything that explains itself.
			static const char* const kDevices[] = {
				"CON","PRN","AUX","NUL",
				"COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
				"LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9",
			};
			for (const char* d : kDevices)
				if (_stricmp(out.c_str(), d) == 0) { out.insert(out.begin(), '_'); break; }

			return out.empty() ? std::string("project") : out;
		}

		// What sanitise() used to do. Kept solely to find files written by the
		// builds that shipped it, so a non-Latin project can be reunited with its
		// settings instead of appearing to have lost them.
		std::string sanitiseAscii(const char* name)
		{
			std::string out;
			for (const char* p = name; *p; ++p)
			{
				const char c = *p;
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
				out += ok ? c : '_';
			}
			return out;
		}
	}

	// One entry per Param, in enum order. Short, because the audience for these
	// is somebody opening the file to rescue a project by hand.
	//
	// A name is part of the FILE FORMAT once shipped. Renaming one silently
	// drops that value out of every side-car already on disk - the loader will
	// not recognise the old key and skip it. Add freely, rename never.
	const char* paramName(Param p)
	{
		static const char* const kNames[P_COUNT] = {
			"alpha", "easeIn", "easeOut",
			"swayPos", "swayRot", "swayFreq",
			"jitPos", "jitRot", "jitFreq",
			"octaves", "seed",
			"axLat", "axFwd", "axVert", "axPitch", "axRoll", "axYaw",
			"simple", "intensity", "freqMul",
			"speedAmp", "speedFreq", "stopStill", "variation",
			"unusedSine",
		};
		static_assert(sizeof(kNames) / sizeof(*kNames) == P_COUNT,
			"a Param was added without a name - it would serialise as \"?\" and "
			"collide with every other unnamed one");
		return (p >= 0 && p < P_COUNT) ? kNames[p] : "?";
	}

	Param paramFromName(const char* name)
	{
		if (!name) return P_COUNT;
		for (int i = 0; i < P_COUNT; ++i)
			if (strcmp(name, paramName((Param)i)) == 0) return (Param)i;
		return P_COUNT;
	}

	MarkerSettings get(float timeMs)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_entries.find(keyOf(g_clip, timeMs));
		return it == g_entries.end() ? MarkerSettings{} : it->second;
	}

	void set(float timeMs, const MarkerSettings& s)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const Key k = keyOf(g_clip, timeMs);
		if (s.isDefault())
		{
			if (g_entries.erase(k)) markDirty();
		}
		else
		{
			g_entries[k] = s;
			markDirty();
		}
	}

	void rekey(float oldTimeMs, float newTimeMs)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const Key from = keyOf(g_clip, oldTimeMs), to = keyOf(g_clip, newTimeMs);
		if (from == to) return;

		auto it = g_entries.find(from);
		if (it == g_entries.end()) return;

		MarkerSettings s = it->second;
		g_entries.erase(it);
		g_entries[to] = s;
		markDirty();
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
		clearDirty();

		// A newly-opened project starts on its first clip. syncScope() corrects
		// this on the next frame if it did not; without the reset, a switch that
		// happens while clipIndex() is briefly -1 would leave lookups scoped to
		// a clip of the OUTGOING project.
		g_clip = 0;

		if (!projectName || !*projectName) { g_path.clear(); g_wpath.clear(); return; }

		const std::string safe = sanitise(projectName);

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
		//
		// Two encodings meet here, which is why the halves are widened apart: the
		// folder came out of a -A call so it is in the system codepage, while the
		// name came out of the game so it is UTF-8. Widening the concatenation
		// under either one would corrupt the other half.
		const std::string folder = paths::sub("markers");
		g_path  = folder + safe + ".txt";                       // for the log only
		g_wpath = widen(folder, CP_ACP) + widen(safe + ".txt", CP_UTF8);

		// Adopt a game-root file from the previous layout. Moved, not copied, so
		// there is only ever one file holding a project's overrides.
		if (GetFileAttributesW(g_wpath.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			const std::wstring prev = widen(dir, CP_ACP) +
				L"RockstarEditorPlus_" + widen(safe + ".txt", CP_UTF8);
			if (MoveFileW(prev.c_str(), g_wpath.c_str()))
				logger::write("info", "settings: moved a game-root file into markers");

			// Builds between the per-project split and the Unicode filenames
			// mangled every non-ASCII byte to '_', so a project whose name is not
			// Latin has its settings sitting under a row of underscores. Claim it
			// - it is unambiguously this project's, and it was written days ago at
			// most.
			const std::string ascii = sanitiseAscii(projectName);
			if (ascii != safe)
			{
				const std::wstring mangled = widen(folder + ascii + ".txt", CP_ACP);
				if (MoveFileW(mangled.c_str(), g_wpath.c_str()))
					logger::write("info", "settings: adopted '%s.txt' as '%s.txt'",
						ascii.c_str(), safe.c_str());
			}
		}

		std::ifstream in(g_wpath);
		if (!in)
		{
			// The tool used to be called SmoothBlendFix. Adopt an existing
			// side-car from that name rather than silently starting empty and
			// losing per-marker work; the next save writes the new filename.
			const std::wstring legacy = widen(dir, CP_ACP) +
				L"SmoothBlendFix_" + widen(safe + ".txt", CP_UTF8);
			in.open(legacy);
			if (in)
			{
				logger::write("info", "settings: adopting a legacy SmoothBlendFix file");
				markDirty(); // force a rewrite under the new name
			}
		}
		if (!in)
		{
			logger::write("info", "settings: new project '%s'", safe.c_str());

			// Builds before per-project binding put everything in one file called
			// default.txt, whatever project it came from. Point at it rather than
			// adopting it: its entries belong to an unknown mix of projects, so
			// pulling them in here would just move the collision instead of
			// ending it. Renaming it by hand is a decision only the user can make.
			const std::string shared = paths::sub("markers") + "default.txt";
			if (safe != "default" &&
			    GetFileAttributesA(shared.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				logger::write("info",
					"settings: markers\\default.txt is from before per-project "
					"settings and is no longer read - rename it to '%s.txt' to "
					"claim it for this project",
					safe.c_str());
			}
			return;
		}

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
				"settings: '%s' is v%d, this build writes v%d - not loading "
				"(delete it, or re-set the markers you care about)",
				g_path.c_str(), ver, kVersion);
			return;
		}

		std::string line;
		if (ver < 6)
		{
			// v4/v5: <time> <path> <orient> <count> <values...> [shake]
			//
			// These predate both per-project files and clip scoping, so every
			// entry lands in clip 0. That is a guess, but it is the right one
			// for the common case - a file written before this existed came
			// from a project whose markers were all being edited in one clip.
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
					// Skip parameters a newer build wrote that we do not know
					// about, so the v5 flag after them still lines up.
					for (int i = take; i < n; ++i) { float junk; if (!(ls >> junk)) break; }
				}

				// v5 trailing flag. Absent in a v4 file, which leaves it false.
				int sh = 0;
				if (ls >> sh) s.ourShake = sh != 0;

				g_entries[keyOf(0, (float)t)] = s;
			}

			// Rewrite in the current format on the next flush, so the file stops
			// being one bad edit away from the positional-column trap.
			markDirty();
		}
		else
		{
			// v6+: `clip <n>` scopes the `marker` lines that follow it.
			//
			// An unrecognised key is skipped rather than failing the line, and
			// an unrecognised LINE is skipped rather than failing the file, so a
			// side-car written by a future build still loads what it can here.
			int clip = 0;
			while (std::getline(in, line))
			{
				std::istringstream ls(line);
				std::string what;
				if (!(ls >> what)) continue;

				if (what == "clip") { ls >> clip; continue; }
				if (what != "marker") continue;

				long long t;
				if (!(ls >> t)) continue;

				MarkerSettings s;
				std::string field;
				while (ls >> field)
				{
					const size_t eq = field.find('=');
					if (eq == std::string::npos) continue;

					const std::string key = field.substr(0, eq);
					const char* val = field.c_str() + eq + 1;

					if (key == "path")
					{
						const int p = atoi(val);
						s.path = (PathMode)(p < 0 || p > 2 ? 0 : p);
					}
					else if (key == "orient")
					{
						const int o = atoi(val);
						s.orient = (OrientMode)(o < 0 || o > 2 ? 0 : o);
					}
					else if (key == "shake")
					{
						s.ourShake = atoi(val) != 0;
					}
					else
					{
						const Param p = paramFromName(key.c_str());
						if (p != P_COUNT) s.v[p] = (float)atof(val);
					}
				}

				g_entries[keyOf(clip, (float)t)] = s;
			}
		}

		logger::write("info", "settings: loaded %d marker override(s) from %s (v%d)",
			(int)g_entries.size(), g_path.c_str(), ver);
	}

	// Follow the editor: re-bind when the open project changes, re-scope when the
	// edited clip changes.
	//
	// Only the PROJECT costs anything - one file per project, all its clips
	// inside - so stepping between clips is a single integer store. That is why
	// this is safe to call every frame.
	void syncScope()
	{
		// A null name is not "no project". It is also what a project mid-load
		// reads as, and what everything reads as while the editor is closing.
		// Unbinding on it would flush and clear the store on a transient, so a
		// null is simply ignored and the last real binding stands until another
		// real one replaces it.
		static std::string bound;

		const char* name = game::projectName();

		// Nothing to write into is worse than writing into the wrong file: an
		// unbound store accepts every edit and silently discards it at flush
		// time, with no file to show for it. So if the project name cannot be
		// read at all, fall back to the shared file this used to always use.
		// Settings still leak between projects there - but they SURVIVE, and the
		// log says why.
		if (!name && bound.empty() && game::isEditModeActive())
		{
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				logger::write("info",
					"settings: could not read the project name - falling back to "
					"the shared markers\\default.txt. Settings will be kept, but "
					"they are shared across projects again.");
			}
			name = "default";
		}

		if (name && bound != name)
		{
			logger::write("info", "settings: project is now '%s'", name);
			bound = name;
			bindProject(name);
		}

		// -1 is "not in the editor". Same argument: keep the last real scope.
		const int clip = game::clipIndex();
		if (clip >= 0 && clip != g_clip)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_clip = clip;
		}
	}

	// Deferred flush. Call every frame; it writes at most once per quiet period.
	//
	// save() rewrites the WHOLE side-car - open with trunc, walk every entry,
	// close - and the menu used to call it on each left/right step. Held input
	// repeats around 30 times a second, so adjusting one value meant thirty full
	// file rewrites a second, which is what made changing a marker feel heavy in
	// a project with a few markers in it.
	//
	// Nothing needs the file to be current mid-keypress; it only has to be right
	// by the time the game closes or the project changes. So the menu now just
	// dirties the store and this coalesces the burst into one write once the
	// user stops moving. The explicit save() calls that remain are the ones with
	// a real deadline - project switch, shutdown.
	void tick()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_dirty) return;

			// ~0.5s of quiet. Long enough to swallow a held adjustment, short
			// enough that a crash costs one edit rather than a session.
			if (GetTickCount() - g_dirtyAt < 500) return;
		}
		save();
	}

	void save()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_dirty) return;

		// Unbound with pending edits. syncScope()'s fallback exists so this
		// cannot normally happen; say so rather than dropping the write on the
		// floor, which is how "I set a shake and no file appeared" happens.
		if (g_path.empty())
		{
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				logger::write("info",
					"settings: !! %d marker override(s) pending with no project "
					"bound - not saved", (int)g_entries.size());
			}
			return;
		}

		std::ofstream out(g_wpath, std::ofstream::out | std::ofstream::trunc);
		if (!out)
		{
			logger::write("info", "settings: !! could not write %s", g_path.c_str());
			return;
		}

		out << kHeader << kVersion << '\n';

		// g_entries is keyed clip-major, so iterating it in order already groups
		// by clip - the header only has to be emitted when the high half changes.
		int clip = -1;
		for (const auto& kv : g_entries)
		{
			const MarkerSettings& s = kv.second;
			if (clipOf(kv.first) != clip)
			{
				clip = clipOf(kv.first);
				out << "clip " << clip << '\n';
			}

			out << "marker " << timeOf(kv.first);
			if (s.path   != PathMode::Inherit)   out << " path="   << (int)s.path;
			if (s.orient != OrientMode::Inherit) out << " orient=" << (int)s.orient;
			if (s.ourShake)                      out << " shake=1";
			// Only what is actually set. An inherit is the absence of a field
			// rather than a -1, which keeps a lightly-edited marker to one short
			// line instead of a row of two dozen sentinels.
			for (int i = 0; i < P_COUNT; ++i)
				if (s.has((Param)i)) out << ' ' << paramName((Param)i) << '=' << s.v[i];
			out << '\n';
		}

		clearDirty();
	}
}
