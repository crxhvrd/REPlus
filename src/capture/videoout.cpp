// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "capture/videoout.h"

#include "utils/paths.h"

#include <string>
#include <fstream>

namespace videoout
{
	namespace
	{
		HANDLE s_proc    = nullptr;   // ffmpeg
		HANDLE s_stdin   = nullptr;   // our end of its stdin pipe
		bool   s_started = false;
		int    s_pushed  = 0;         // frames handed over
		char   s_outPath[MAX_PATH]{};

		// Quote a path for a command line. Paths here come from the ini and from
		// the render folder, so they can contain spaces; without this ffmpeg
		// silently treats the tail as another argument.
		std::string quoted(const char* s)
		{
			std::string q = "\"";
			q += s;
			q += "\"";
			return q;
		}

		// --- encoder presets -------------------------------------------------
		//
		// One .ini per codec in the mod's presets folder, each holding the
		// ffmpeg arguments and a matching container. A preset is just those two
		// ini values under a name, so anything expressible in RenderVideoArgs
		// works as a preset - the point is keeping several to hand and
		// switching by name instead of pasting argument strings.
		//
		// Examples are written once if a file is missing, and never overwritten,
		// so edits to them stick.
		void writeDefaultPresets()
		{
			struct P { const char* name; const char* ext; const char* args; const char* note; };
			static const P kDefaults[] = {
				{ "h264",       "mp4", "-c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p",
				  "Near-lossless h.264. Plays anywhere. The safe default." },
				{ "h265",       "mp4", "-c:v libx265 -crf 20 -preset slow -pix_fmt yuv420p",
				  "Similar quality at roughly half the size, slower to encode." },
				{ "nvenc_hevc", "mp4", "-c:v hevc_nvenc -cq 20 -preset p7 -pix_fmt yuv420p",
				  "GPU encode - much faster, needs an NVIDIA card." },
				{ "prores_hq",  "mov", "-c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le",
				  "ProRes 422 HQ, 10-bit, for grading. Large files." },
				{ "lossless",   "mkv", "-c:v libx264 -qp 0 -preset veryslow",
				  "Mathematically lossless. Archival; very large." },
			};

			const std::string dir = paths::sub("presets");
			for (const P& p : kDefaults)
			{
				const std::string path = dir + p.name + ".ini";
				if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

				std::ofstream f(path);
				if (!f) continue;
				f << "; " << p.note << "\n"
				  << "; Use with RenderVideoPreset=" << p.name
				  << " in RockstarEditorPlus.ini\n\n"
				  << "[Preset]\n"
				  << "Args=" << p.args << "\n"
				  << "Ext="  << p.ext  << "\n";
			}
		}

		// Fills args/ext from a named preset. False when there is no usable
		// preset by that name, and the caller keeps the ini's own values.
		bool loadPreset(const char* name, std::string& args, std::string& ext)
		{
			if (!name || !*name) return false;

			const std::string path = paths::sub("presets") + name + ".ini";
			if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				logger::write("info", "video: preset '%s' not found - using RenderVideoArgs", name);
				return false;
			}

			char buf[1024]{};
			GetPrivateProfileStringA("Preset", "Args", "", buf, sizeof(buf), path.c_str());
			if (!buf[0])
			{
				logger::write("info", "video: preset '%s' has no Args - using RenderVideoArgs", name);
				return false;
			}
			args = buf;

			char e[64]{};
			GetPrivateProfileStringA("Preset", "Ext", "mp4", e, sizeof(e), path.c_str());
			ext = e;
			return true;
		}

		// Where ffmpeg is looked for, in order.
		//
		// The BUNDLED copy comes first, because that is how this is meant to
		// ship: drop ffmpeg.exe into the mod's folder and video output works
		// with no setup. Searching PATH first would mean whichever unrelated
		// ffmpeg a user happens to have installed silently decides how their
		// renders encode - and those builds vary in which codecs they carry, so
		// a preset that works on one machine fails on another.
		//
		// PATH is still last, so an existing install is used rather than
		// demanding a second copy.
		//
		// Returns empty if none of them work, so begin() reports it once rather
		// than failing per frame.
		std::string findFfmpeg()
		{
			const Config& cfg = Config::get();

			// 1. Explicitly configured. A path that is not there is a mistake
			//    worth naming, not a reason to quietly use a different ffmpeg.
			if (!cfg.ffmpegPath.empty())
			{
				if (GetFileAttributesA(cfg.ffmpegPath.c_str()) != INVALID_FILE_ATTRIBUTES)
					return cfg.ffmpegPath;
				logger::write("info", "video: FfmpegPath '%s' does not exist", cfg.ffmpegPath.c_str());
				return {};
			}

			// 2. Bundled, in the mod's own folder - the intended install.
			const std::string bundled = paths::file("ffmpeg.exe");
			if (GetFileAttributesA(bundled.c_str()) != INVALID_FILE_ATTRIBUTES)
				return bundled;

			// 3. Beside the game, where other tools sometimes leave one.
			const std::string beside = paths::gameDir() + "ffmpeg.exe";
			if (GetFileAttributesA(beside.c_str()) != INVALID_FILE_ATTRIBUTES)
				return beside;

			// 4. Anything on PATH.
			char found[MAX_PATH]{};
			if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, found, nullptr))
				return found;

			return {};
		}
	}

	bool active()
	{
		return s_started && s_stdin != nullptr;
	}

	std::string s_audioOverride;
	void setAudio(const char* path) { s_audioOverride = path ? path : ""; }

	bool begin(const char* folder, float fps)
	{
		s_started = false;
		s_pushed  = 0;
		s_outPath[0] = '\0';

		const Config& cfg = Config::get();
		if (!cfg.wantsVideo()) return false;

		const std::string exe = findFfmpeg();
		if (exe.empty())
		{
			logger::write("info",
				"video: ffmpeg not found - writing frames only.");
			logger::write("info",
				"video: put ffmpeg.exe in %s, or set FfmpegPath in the ini.",
				paths::baseDir().c_str());
			return false;
		}

		writeDefaultPresets();

		// A named preset overrides the inline args. Falling back rather than
		// failing on a bad name would silently produce a differently-encoded
		// video, so loadPreset says so in the log and leaves the ini's values.
		std::string args = cfg.renderVideoArgs;
		std::string ext  = cfg.renderVideoExt;
		if (loadPreset(cfg.renderVideoPreset.c_str(), args, ext))
			logger::write("info", "video: preset '%s'", cfg.renderVideoPreset.c_str());

		snprintf(s_outPath, sizeof(s_outPath), "%s\\video.%s", folder, ext.c_str());

		// stdin pipe. Only the READ end is inheritable: ffmpeg needs that one,
		// and leaving our write end inheritable would keep the pipe open in the
		// child, so the encoder would never see EOF and never exit.
		SECURITY_ATTRIBUTES sa{};
		sa.nLength        = sizeof(sa);
		sa.bInheritHandle = TRUE;

		HANDLE childRead = nullptr, parentWrite = nullptr;
		if (!CreatePipe(&childRead, &parentWrite, &sa, 1 << 20))
		{
			logger::write("info", "video: could not create the encoder pipe");
			return false;
		}
		SetHandleInformation(parentWrite, HANDLE_FLAG_INHERIT, 0);

		// -f image2pipe reads whatever the addon wrote, concatenated. The frame
		// rate is stated BEFORE -i so it applies to the input: image2pipe has no
		// timestamps of its own, and setting it only on the output would leave
		// ffmpeg assuming 25 and duplicating or dropping frames to reach it.
		// Audio comes in as a SECOND input to this same process rather than a
		// remux afterwards: the frames are already streaming through here, so
		// muxing costs nothing extra and there is no intermediate file to clean
		// up or leave behind on a cancelled render.
		//
		// -shortest matters. Our video length is frame count over fps, while the
		// borrowed track is however long the game's export ran, and those agree
		// only approximately - without it the file ends up padded to whichever
		// stream is longer.
		// A wav from our own audio pass wins over AudioFromFile: it was recorded
		// from this project moments ago, where the ini setting is whatever the
		// user last pointed at.
		const std::string src = !s_audioOverride.empty() ? s_audioOverride : cfg.audioFromFile;
		std::string audio;
		if (!src.empty())
		{
			if (GetFileAttributesA(src.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				audio = " -i " + quoted(src.c_str()) +
				        " -map 0:v -map 1:a -c:a aac -b:a 320k -shortest";
				logger::write("info", "video: muxing audio from %s", src.c_str());
			}
			else
			{
				logger::write("info", "video: audio source not found, rendering silent - %s",
					src.c_str());
			}
		}

		char cmd[4096];
		const int need = snprintf(cmd, sizeof(cmd),
			"%s -hide_banner -loglevel error -y -f image2pipe -framerate %g -i -%s %s %s",
			quoted(exe.c_str()).c_str(), fps,
			audio.c_str(),
			args.c_str(),
			quoted(s_outPath).c_str());

		// Truncation here is not a cosmetic problem: the OUTPUT PATH is last, so
		// a command line that does not fit loses the destination and ffmpeg
		// either fails or writes somewhere unintended. Preset args alone can run
		// to 1023 characters before any of the three paths are added, so this is
		// reachable rather than theoretical.
		if (need < 0 || need >= (int)sizeof(cmd))
		{
			logger::write("info",
				"video: encoder command line too long (%d chars, limit %d) - "
				"shorten RenderVideoArgs or the output path. Writing frames only.",
				need, (int)sizeof(cmd) - 1);
			return false;
		}

		STARTUPINFOA si{};
		si.cb         = sizeof(si);
		si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdInput  = childRead;
		si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

		PROCESS_INFORMATION pi{};
		const BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

		// Ours to close either way: on success the child holds its own copy, and
		// on failure nothing should be left holding the pipe open.
		CloseHandle(childRead);

		if (!ok)
		{
			CloseHandle(parentWrite);
			logger::write("info", "video: could not start ffmpeg (error %lu)", GetLastError());
			return false;
		}

		CloseHandle(pi.hThread);
		s_proc    = pi.hProcess;
		s_stdin   = parentWrite;
		s_started = true;

		logger::write("info", "video: encoding to %s @ %g fps [%s]",
			s_outPath, fps, args.c_str());
		return true;
	}

	void pushFrame(const char* path)
	{
		if (!active() || !path) return;

		// The addon has signalled the frame is done, but "done writing" and
		// "closed" are not the same instant - a first open can still lose the
		// race. A few short retries cost nothing next to a lost frame, which
		// would desync the whole video from that point on.
		HANDLE f = INVALID_HANDLE_VALUE;
		for (int attempt = 0; attempt < 20; ++attempt)
		{
			f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f != INVALID_HANDLE_VALUE) break;
			Sleep(5);
		}
		if (f == INVALID_HANDLE_VALUE)
		{
			logger::write("info", "video: could not read %s - frame dropped", path);
			return;
		}

		char    buf[64 * 1024];
		DWORD   got = 0;
		bool    bad = false;
		while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
		{
			DWORD off = 0;
			while (off < got)
			{
				DWORD wrote = 0;
				if (!WriteFile(s_stdin, buf + off, got - off, &wrote, nullptr) || wrote == 0)
				{
					bad = true;
					break;
				}
				off += wrote;
			}
			if (bad) break;
		}
		CloseHandle(f);

		if (bad)
		{
			// ffmpeg exited or the pipe broke. Stop feeding it rather than
			// blocking the render thread on every remaining frame.
			logger::write("info", "video: encoder pipe closed early after %d frame(s)", s_pushed);
			CloseHandle(s_stdin);
			s_stdin = nullptr;
			return;
		}

		++s_pushed;
		if (!Config::get().renderKeepFrames) DeleteFileA(path);
	}

	void end(bool complete)
	{
		if (!s_started) return;

		// Closing stdin is what tells ffmpeg the stream ended; without it the
		// encoder waits forever and the container is never finalised.
		if (s_stdin)
		{
			CloseHandle(s_stdin);
			s_stdin = nullptr;
		}

		if (s_proc)
		{
			// Bounded: a hung encoder must not hang the game. 30s is far more
			// than finalising a container takes, even for a long render.
			if (WaitForSingleObject(s_proc, 30000) == WAIT_TIMEOUT)
			{
				logger::write("info", "video: ffmpeg did not exit - terminating");
				TerminateProcess(s_proc, 1);
			}
			DWORD code = 0;
			GetExitCodeProcess(s_proc, &code);
			CloseHandle(s_proc);
			s_proc = nullptr;

			logger::write("info", "video: %s - %d frame(s) -> %s (ffmpeg exit %lu)",
				complete ? "finished" : "ended early", s_pushed, s_outPath, code);
		}

		s_started = false;
		s_pushed  = 0;
	}
}
