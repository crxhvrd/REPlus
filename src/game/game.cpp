// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "game/signatures.h"
#include "replay/marker.h"

namespace game
{
	uintptr_t addr_UpdateSmoothing    = 0;
	uintptr_t addr_GetNextMarkerIndex = 0;
	uintptr_t addr_GetPrevMarkerIndex = 0;
	uintptr_t addr_g_MarkerStorage    = 0;
	uintptr_t addr_g_ReplayTimeMs     = 0;
	uintptr_t addr_GetMaxDistanceFromPlayer = 0;
	uintptr_t addr_UpdateCollision     = 0;
	uintptr_t addr_ComputeSafePosition = 0;
	uintptr_t addr_ProfanityGetStatus  = 0;
	uintptr_t addr_JumpToNonDilated    = 0;
	uintptr_t addr_g_PlaybackController= 0;
	uintptr_t addr_PlaybackOpen        = 0;
	uintptr_t addr_g_PlaybackType      = 0;
	uintptr_t addr_g_ShouldRender      = 0;
	uintptr_t addr_MousePointerUpdate  = 0;
	uintptr_t addr_g_CursorVisible     = 0;
	uintptr_t addr_RenderSpinner       = 0;
	uintptr_t addr_BusySpinnerRender   = 0;
	uintptr_t addr_BusySpinnerOn       = 0;
	uintptr_t addr_ScaleformDrawMovie  = 0;
	uintptr_t addr_DrawSpinner         = 0;
	uintptr_t addr_ShouldShowLoading   = 0;
	uintptr_t addr_g_WantDelayedClose  = 0;
	uintptr_t addr_g_LoadingScreen     = 0;
	uintptr_t addr_g_PreCaching        = 0;
	uintptr_t addr_ReplayJumpTo        = 0;
	uintptr_t addr_SetupReplayBuffer   = 0;
	uintptr_t addr_IsPlaybackFlagSet   = 0;
	uintptr_t addr_IsWaitingOnWorldStreaming = 0;
	uintptr_t addr_g_StreamingStallTimer = 0;
	uintptr_t addr_AdvanceReaderHandleResults = 0;
	uintptr_t addr_ModelMgrLoadModel   = 0;
	uintptr_t addr_g_ReplayBufferInfo  = 0;
	uintptr_t addr_g_ReplayBlocks      = 0;
	uintptr_t addr_g_ReplayTotalBlocks = 0;
	uintptr_t addr_g_ReplayAllocator   = 0;
	uintptr_t addr_ReplayHeapAllocImm  = 0;
	uintptr_t addr_ReplayHeapCtorImm   = 0;
	bool      replayHeapWidenedEarly   = false;
	uintptr_t addr_PacketWeatherExtract= 0;
	uintptr_t addr_g_TcVarInfos        = 0;
	uintptr_t addr_SetCursorSpeed      = 0;
	uintptr_t addr_SetNextPlayBackState= 0;
	uintptr_t addr_g_ReplayMode        = 0;

	uintptr_t addr_PopulateCameraMenu   = 0;
	uintptr_t addr_PopulateMarkerMenu   = 0;
	uintptr_t addr_MenuInput            = 0;
	uintptr_t addr_GetCurrentEditMarker = 0;
	uintptr_t addr_BeginMethod          = 0;
	uintptr_t addr_AddParamString       = 0;
	uintptr_t addr_EndMethod            = 0;
	uintptr_t addr_ArrayGrow            = 0;
	uintptr_t addr_g_MovieId            = 0;
	uintptr_t addr_g_MenuOptions        = 0;
	uintptr_t addr_g_MenuFocusIndex     = 0;
	uintptr_t addr_UpdateItemText       = 0;
	uintptr_t addr_UpdateMenuHelpText   = 0;
	uintptr_t addr_g_EditMarkerIndex    = 0;

	// Enhanced-only. Legacy leaves these 0 and never reads them: it has a real
	// atArray::Grow and a real GetCurrentEditMarker to call, and its Scaleform
	// calls carry no context object.
	uintptr_t addr_ScaleformRelease     = 0;
	uintptr_t addr_GameAlloc            = 0;
	uintptr_t addr_GameFree             = 0;
	uintptr_t addr_g_EditClipController = 0;
	uintptr_t addr_g_EditClipIndex      = 0;

	// The export menu. Both builds; independent of everything above, so it can
	// resolve when the marker menu does not and vice versa.
	uintptr_t addr_VEMenuOpen           = 0;
	uintptr_t addr_VEAdjustToggle       = 0;
	uintptr_t addr_VEGetToggleString    = 0;
	uintptr_t addr_VEIsItemSelectable   = 0;
	uintptr_t addr_VEBuildMenu          = 0;
	uintptr_t addr_TextGet              = 0;
	uintptr_t addr_g_VEMenuArray        = 0;
	uintptr_t addr_g_VECurrentColumn    = 0;
	uintptr_t addr_g_VEMenuIdForColumn  = 0;
	uintptr_t addr_g_VECurrentItem      = 0;
	uintptr_t addr_MemAlloc             = 0;
	uintptr_t addr_MemFree              = 0;

	// -------------------------------------------------------------------------
	// The project's clips, off CReplayCoordinator's CReplayPlaybackController.
	//
	// Times are in a CLIP's own timeline, which is not project time and does not
	// start at zero: a clip trimmed out of the middle of a recording reports
	// something like 19200..22200, and JumpTo takes a value in exactly that
	// space. That mapping is render.cpp's job.
	//
	// BOTH builds. Enhanced takes the instance out of its inlined copy of the
	// seek; Legacy out of the real JumpToNonDilatedTimeMs, which loads it right
	// in front of the start-time call. (An older comment here claimed Legacy got
	// identity mapping - that was true only before the JTND_CONTROLLER derive.)
	// -------------------------------------------------------------------------
	namespace
	{
		// One gate for everything that talks to the controller. The mode test is
		// not paranoia: the object is a static that outlives any project, so its
		// montage pointer is null outside the editor and every accessor below
		// would answer -1 or 0 - which reads as "a project with no clips" rather
		// than "no project", and that is a far worse thing to hand a renderer.
		void* controller()
		{
			if (!addr_g_PlaybackController || !addr_g_ReplayMode) return nullptr;
			if (*(int*)addr_g_ReplayMode != gsig::REPLAYMODE_EDIT) return nullptr;
			return (void*)addr_g_PlaybackController;
		}

		template <int Slot, typename Fn>
		Fn slot(void* ctrl)
		{
			auto vtbl = *(uintptr_t**)ctrl;
			return (Fn)(*(uintptr_t*)((uint8_t*)vtbl + Slot));
		}
	}

	bool clipRangeAt(int clipIndex, float& lo, float& hi)
	{
		void* ctrl = controller();
		if (!ctrl) return false;

		using RangeFn = float(__fastcall*)(void*, int);
		lo = slot<gsig::PBC_VT_GETSTARTTIME, RangeFn>(ctrl)(ctrl, clipIndex);
		hi = slot<gsig::PBC_VT_GETENDTIME,   RangeFn>(ctrl)(ctrl, clipIndex);
		return hi > lo;
	}

	bool clipRange(float& lo, float& hi) { return clipRangeAt(-1, lo, hi); }

	// Real elapsed output time -> the authored (non-dilated) time to seek to.
	//
	// This is the whole of slow-motion support. Marker speed makes the two clocks
	// diverge, and everything else in the renderer already works in non-dilated
	// time, so converting here is enough - see the note in signatures.h.
	//
	// Mirrors the engine's own export loop: take the time INTO the current clip
	// on the real clock, then let the controller fold in that clip's speed
	// markers. ConvertTimeToNonDilatedTimeMs already adds the non-dilated time to
	// the clip's start, so what comes back is project-absolute and needs no
	// further arithmetic.
	//
	// Returns the input unchanged when there is no controller: with no dilation
	// information the honest answer is the identity, which is exactly the old
	// behaviour rather than a wrong seek.
	float dilatedToNonDilatedMs(float dilatedProjectMs)
	{
		void* ctrl = controller();
		if (!ctrl) return dilatedProjectMs;

		const int ci = clipIndex();
		if (ci < 0) return dilatedProjectMs;

		using LenFn  = float(__fastcall*)(void*, int);
		using ConvFn = float(__fastcall*)(void*, int, float);

		const float toClip = slot<gsig::PBC_VT_LENTOCLIPMS, LenFn>(ctrl)(ctrl, ci);

		// Clamp rather than pass a negative: a negative time into the clip is
		// only reachable transiently while a clip step is in flight, and the
		// conversion is not defined for it.
		float intoClip = dilatedProjectMs - toClip;
		if (!(intoClip > 0.0f)) intoClip = 0.0f;

		return slot<gsig::PBC_VT_TOTONDILATED, ConvFn>(ctrl)(ctrl, ci, intoClip);
	}

	// The inverse: authored time -> real elapsed time.
	//
	// Needed because the two capture modes measure opposite things. Walking seeks
	// to a time, so it converts real -> authored. Sliding lets the clip PLAY and
	// watches the clock, so what it observes is authored and has to be mapped
	// onto the output timeline before it can decide a frame is due.
	//
	// Callers use the DIFFERENCE of two of these within a clip, which cancels the
	// per-clip base the conversion adds - so it does not matter that the return
	// is project-absolute.
	float nonDilatedToDilatedMs(float nonDilatedProjectMs)
	{
		void* ctrl = controller();
		if (!ctrl) return nonDilatedProjectMs;

		const int ci = clipIndex();
		if (ci < 0) return nonDilatedProjectMs;

		using LenFn  = float(__fastcall*)(void*, int);
		using ConvFn = float(__fastcall*)(void*, int, float);

		const float toClip = slot<gsig::PBC_VT_GETSTARTTIME, LenFn>(ctrl)(ctrl, ci);

		float intoClip = nonDilatedProjectMs - toClip;
		if (!(intoClip > 0.0f)) intoClip = 0.0f;

		return slot<gsig::PBC_VT_TODILATED, ConvFn>(ctrl)(ctrl, ci, intoClip);
	}

	// Total REAL duration of the project, i.e. how long the finished video runs.
	// Summed per clip because GetClipTrimmedTimeMs is the dilated length of each
	// trimmed clip; the non-dilated spans the renderer uses elsewhere would give
	// the authored length instead, which is the wrong number for a frame count
	// the moment any marker changes speed. 0 when unavailable.
	float totalDilatedMs()
	{
		void* ctrl = controller();
		if (!ctrl) return 0.0f;

		const int n = clipCount();
		if (n <= 0) return 0.0f;

		using Fn = float(__fastcall*)(void*, int);
		auto trimmed = slot<gsig::PBC_VT_CLIPTRIMMEDMS, Fn>(ctrl);

		float total = 0.0f;
		for (int i = 0; i < n; ++i)
		{
			const float d = trimmed(ctrl, i);
			if (d > 0.0f) total += d;
		}
		return total;
	}

	int clipIndex()
	{
		void* ctrl = controller();
		if (!ctrl) return -1;
		using Fn = int(__fastcall*)(void*);
		return slot<gsig::PBC_VT_GETCLIPINDEX, Fn>(ctrl)(ctrl);
	}

	int clipCount()
	{
		void* ctrl = controller();
		if (!ctrl) return -1;
		using Fn = int(__fastcall*)(void*);
		const int n = slot<gsig::PBC_VT_GETCLIPCOUNT, Fn>(ctrl)(ctrl);

		// The accessor answers -1 for an invalid controller, which would sail
		// through an `i < count` loop as "no clips" but through `i + 1 < count`
		// as "no next clip" - two different wrong answers from one value. Fold
		// it to 0 so callers only have to handle one.
		return n < 0 ? 0 : n;
	}

	bool jumpToClip(int clipIndex, float clipTimeMs)
	{
		void* ctrl = controller();
		if (!ctrl) return false;
		if (clipIndex < 0 || clipIndex >= clipCount()) return false;

		// A NEGATIVE time means "wherever this clip starts, and keep playing".
		// Everything here wants the paused form, so callers pass a real time and
		// the engine pauses on arrival - see PBC_VT_JUMPTOCLIP.
		using Fn = void(__fastcall*)(void*, int, float);
		slot<gsig::PBC_VT_JUMPTOCLIP, Fn>(ctrl)(ctrl, clipIndex, clipTimeMs);
		return true;
	}

	bool jumpProjectTo(float timeMs, unsigned jumpOptions)
	{
		if (addr_JumpToNonDilated)
		{
			// Legacy: a real function. `this` is unused by the callee, so a null
			// is fine and saves resolving GetPlaybackController().
			using Fn = bool(__fastcall*)(void*, float, unsigned);
			return ((Fn)addr_JumpToNonDilated)(nullptr, timeMs, jumpOptions);
		}

		// Enhanced: Clang inlined it, so there is nothing to call. Reproduce the
		// inlined body exactly - clamp to the project range off the controller's
		// vtable, then JumpTo. Doing this rather than a plain clip-level jumpTo
		// is what lets a render cross clip boundaries.
		if (!addr_g_PlaybackController || !addr_ReplayJumpTo || !addr_g_ReplayMode)
			return jumpTo(timeMs, jumpOptions);

		if (*(int*)addr_g_ReplayMode != gsig::REPLAYMODE_EDIT) return false;

		float lo = 0.0f, hi = 0.0f;
		if (!clipRange(lo, hi)) return jumpTo(timeMs, jumpOptions);

		float t = timeMs;
		if (t > hi) t = hi;
		if (t < lo) t = lo;

		// The editor itself passes 5 here; 0 is what our Legacy path uses.
		return jumpTo(t, jumpOptions ? jumpOptions : gsig::JUMPOPTS_EDITOR_SEEK);
	}

	// Which of the three sub-conditions is holding replayBusy() true, or nullptr
	// if none is.
	//
	// Worth having separately because "busy" is three unrelated things and they
	// mean different things to a user: a loading screen is transient, a stuck
	// precache in a heavily modded install is not. A report of "never settled to
	// EDIT (saw 2)" - i.e. the mode was already EDIT and it was this that
	// blocked - could not be taken any further without it.
	const char* replayBusyReason()
	{
		if (addr_ShouldShowLoading)
		{
			using Fn = bool(__fastcall*)();
			if (((Fn)addr_ShouldShowLoading)()) return "ShouldShowLoading";
		}
		if (addr_g_LoadingScreen && *(unsigned char*)addr_g_LoadingScreen) return "g_LoadingScreen";
		if (addr_g_PreCaching    && *(unsigned char*)addr_g_PreCaching)    return "g_PreCaching (streaming precache)";
		return nullptr;
	}

	bool replayBusy()
	{
		// The black-logo frames come from this call, not from a flag.
		return replayBusyReason() != nullptr;
	}

	bool requestPlaybackClose()
	{
		if (!addr_g_WantDelayedClose) return false;
		*(unsigned char*)addr_g_WantDelayedClose = 1;
		return true;
	}

	void setCursorVisible(bool visible)
	{
		if (addr_g_CursorVisible) *(unsigned char*)addr_g_CursorVisible = visible ? 1 : 0;
	}

	void setEditorHudVisible(bool visible)
	{
		if (addr_g_ShouldRender) *(unsigned char*)addr_g_ShouldRender = visible ? 1 : 0;
	}

	bool editorHudControllable() { return addr_g_ShouldRender != 0; }

	// Press Export for the user. The detour sits on this address, so calling it
	// runs our own hook first and the open is diverted exactly like a real
	// button press - which is the point: the render path stays one path.
	void openPlayback(int type)
	{
		if (!addr_PlaybackOpen) return;
		using Fn = void(__fastcall*)(int, unsigned);
		((Fn)addr_PlaybackOpen)(type, 0);
	}

	int playbackType()
	{
		return addr_g_PlaybackType ? *(int*)addr_g_PlaybackType : -1;
	}

	// CVideoEditorPlayback::Close() reads this to decide which menu the editor
	// comes back to, so an export that ran as a diverted preview has to hand it
	// back before playback closes. See render::finish().
	void setPlaybackType(int type)
	{
		if (addr_g_PlaybackType) *(int*)addr_g_PlaybackType = type;
	}

	bool isEnhanced() { return IsEnhanced(); }

	static const char* pick(const gsig::Sig& s) { return isEnhanced() ? s.enh : s.leg; }
	static const gsig::Derive& pickD(const gsig::DerivePair& d)
	{
		return isEnhanced() ? d.enh : d.leg;
	}

	// Resolve a rel32/disp32 operand at a fixed offset inside a scanned
	// function. The opcode is verified first: these are interior offsets, so if
	// a future build shifts the function's body we want a clean 0 rather than a
	// pointer built from whatever bytes happen to be there.
	static uintptr_t derive(uintptr_t base, const gsig::Derive& d, const char* what)
	{
		if (!base) return 0;
		const unsigned char* insn = (const unsigned char*)(base + d.insn);
		if (memcmp(insn, d.op, (size_t)d.opLen) != 0)
		{
			logger::write("info", "  !! %s: opcode mismatch at +0x%X вЂ” build drifted", what, d.insn);
			return 0;
		}
		// rip() resolves disp+4; `extra` accounts for any trailing immediate
		// that makes the instruction longer than that.
		const uintptr_t a = memory(base).add(d.disp).rip().address + d.extra;

		// The result must land INSIDE the game module. A RIP-relative operand
		// always does, so anything outside means we read the displacement from
		// the wrong place - and the opcode check above cannot catch that, since
		// it validates the instruction at `insn` while the displacement is taken
		// from `disp`. Get those two out of step and you get a plausible-looking
		// pointer that only fails when something finally dereferences it.
		//
		// That is not hypothetical: JTND_CONTROLLER carried disp=3 instead of
		// 0x43, resolved ~190MB past the end of the image, logged happily as
		// rva 0xF5A0E22, and crashed the game on the first Export with the
		// renderer enabled. A range check turns that into a clean bail at
		// resolve time and one obvious log line.
		if (a < memory::base() || a >= memory::base() + memory::imageSize())
		{
			logger::write("info",
				"  !! %s: derived %p is outside the module (rva 0x%llX) - refusing it. "
				"The Derive's disp is probably wrong; it is an offset from the "
				"FUNCTION START, i.e. insn + opLen.",
				what, (void*)a, (uint64_t)(a - memory::base()));
			return 0;
		}

		logger::write("info", "  %-22s = %p (rva 0x%llX)", what, (void*)a,
			(uint64_t)(a - memory::base()));
		return a;
	}

	// Same idea for the operand MSVC encodes as an ABSOLUTE RVA rather than
	// RIP-relative - `mov r8d,[r14+rdx*4+0x1F63EC0]`, where r14 already holds
	// the image base. Only Legacy needs this; Clang uses ordinary LEAs.
	//
	// The range check is the same one and matters more here, not less: a wrong
	// disp32 read out of an unrelated instruction is just as plausible-looking,
	// and there is no rel32 arithmetic to make it obviously silly.
	static uintptr_t deriveAbs(uintptr_t base, const gsig::DeriveAbs& d, const char* what)
	{
		if (!base) return 0;
		const unsigned char* insn = (const unsigned char*)(base + d.insn);
		if (memcmp(insn, d.op, (size_t)d.opLen) != 0)
		{
			logger::write("info", "  !! %s: opcode mismatch at +0x%X - build drifted", what, d.insn);
			return 0;
		}

		const uintptr_t a = memory::base() + (uintptr_t)*(const uint32_t*)(base + d.disp);
		if (a < memory::base() || a >= memory::base() + memory::imageSize())
		{
			logger::write("info",
				"  !! %s: derived %p is outside the module - refusing it", what, (void*)a);
			return 0;
		}

		logger::write("info", "  %-22s = %p (rva 0x%llX)", what, (void*)a,
			(uint64_t)(a - memory::base()));
		return a;
	}

	uintptr_t addr_g_Project = 0;

	// The open project's name, validated rather than trusted.
	//
	// The montage offset is derived (see signatures.h), and a derived offset is
	// exactly the kind of thing that goes stale on a game update - so check that
	// what came back actually looks like a name before handing it to anything
	// that will build a filename out of it. A wrong offset then costs the
	// caller a null and a fallback, instead of putting arbitrary bytes into a
	// path.
	const char* projectName()
	{
		if (!addr_g_Project) return nullptr;

		const auto* project = *(const unsigned char* const*)addr_g_Project;
		if (!project) return nullptr;

		const auto* montage = *(const unsigned char* const*)(project + gsig::PROJECT_MONTAGE_OFF);
		if (!montage) return nullptr;

		const char* name = (const char*)(montage + gsig::MONTAGE_NAME_OFF);

		// Printable, NUL-terminated, non-empty, and not absurdly long.
		for (int i = 0; i < gsig::MONTAGE_NAME_MAX; ++i)
		{
			const unsigned char c = (unsigned char)name[i];
			if (c == 0)
			{
				if (i == 0) break;
				return name;
			}
			if (c < 0x20 || c == 0x7F) break;
		}

		// The offset is derived, so it can go stale on a game update - and when it
		// does, the only symptom the user sees is that per-marker settings quietly
		// stop being saved. Dump the head of the montage once so the next log says
		// where the name actually moved to, instead of needing another RE session
		// to find out that it moved at all.
		{
			static bool dumped = false;
			if (!dumped)
			{
				dumped = true;
				logger::write("info",
					"settings: montage name not at +0x%X - dumping montage head so "
					"the offset can be corrected", gsig::MONTAGE_NAME_OFF);

				char line[128];
				for (int row = 0; row < 0x100; row += 16)
				{
					int n = sprintf_s(line, "  +%03X ", row);
					for (int i = 0; i < 16; ++i)
						n += sprintf_s(line + n, sizeof(line) - n, "%02X ", montage[row + i]);
					n += sprintf_s(line + n, sizeof(line) - n, " ");
					for (int i = 0; i < 16; ++i)
					{
						const unsigned char c = montage[row + i];
						n += sprintf_s(line + n, sizeof(line) - n, "%c",
							(c >= 0x20 && c < 0x7F) ? c : '.');
					}
					logger::write("info", "%s", line);
				}
			}
		}
		return nullptr;
	}

	bool menuReady()
	{
		const bool common =
			addr_PopulateCameraMenu && addr_MenuInput &&
			addr_BeginMethod && addr_AddParamString && addr_EndMethod &&
			addr_g_MovieId && addr_g_MenuOptions && addr_g_MenuFocusIndex;
		if (!common) return false;

		// The two builds need different pieces. Enhanced inlines both
		// GetCurrentEditMarker and atArray::Grow, so requiring them there would
		// refuse a perfectly good resolve; instead it needs the clip globals we
		// walk by hand and the allocator we grow the option array with.
		if (isEnhanced())
			return addr_g_EditClipController && addr_g_EditClipIndex &&
			       addr_g_EditMarkerIndex && addr_GameAlloc && addr_GameFree &&
			       addr_ScaleformRelease;

		return addr_GetCurrentEditMarker && addr_ArrayGrow;
	}

	// Everything the menu injection needs. Failure here is non-fatal: the
	// spline still works, you just don't get the in-editor rows.
	static void resolveMenu()
	{
		const char* pPcm = pick(gsig::PLAYBACK_POPULATECAMERAMENU);
		const char* pMi  = pick(gsig::PLAYBACK_MENUINPUT);
		if (!pPcm || !*pPcm || !pMi || !*pMi) return;

		addr_PopulateCameraMenu = memory::scan(pPcm).address;
		addr_MenuInput          = memory::scan(pMi).address;
		logger::write("info", "  PopulateCameraMenu     = %p", (void*)addr_PopulateCameraMenu);
		logger::write("info", "  MenuInput              = %p", (void*)addr_MenuInput);

		// The top-level marker menu, where the global rows go. Optional: if it
		// does not resolve we simply lose those rows, and the camera submenu
		// carries on regardless.
		if (const char* pMm = pick(gsig::PLAYBACK_POPULATEMARKERMENU); pMm && *pMm)
		{
			addr_PopulateMarkerMenu = memory::scan(pMm).address;
			logger::write("info", "  PopulateMarkerMenu     = %p (rva 0x%llX)",
				(void*)addr_PopulateMarkerMenu,
				(uint64_t)(addr_PopulateMarkerMenu ? addr_PopulateMarkerMenu - memory::base() : 0));
		}

		// Help text. Optional on both builds, and signed rather than derived out
		// of MenuInput so it does not depend on that identification - it is the
		// only thing the menu still needed from there.
		const char* pUit = pick(gsig::PLAYBACK_UPDATEITEMTEXT);
		addr_UpdateItemText = (pUit && *pUit) ? memory::scan(pUit).address : 0;
		logger::write("info", "  UpdateItemTextValue    = %p", (void*)addr_UpdateItemText);

		// The help-text choke point. Optional on both builds: without it our
		// rows keep the blank help line they had before, and the per-row
		// restrictions stay off (see menu.cpp - a greyed row whose reason we
		// cannot state would be worse than an enabled one).
		const char* pUmh = pick(gsig::PLAYBACK_UPDATEMENUHELPTEXT);
		addr_UpdateMenuHelpText = (pUmh && *pUmh) ? memory::scan(pUmh).address : 0;
		logger::write("info", "  UpdateMenuHelpText     = %p (rva 0x%llX)",
			(void*)addr_UpdateMenuHelpText,
			(uint64_t)(addr_UpdateMenuHelpText ? addr_UpdateMenuHelpText - memory::base() : 0));

		// ms_project, Legacy: out of UpdateMenuHelpText, which loads it for the
		// music/ambient help cases. Enhanced gets it below instead - it already
		// resolves the same pointer under the g_EditClipController name, but not
		// until the Enhanced derive block further down.
		if (!isEnhanced() && addr_UpdateMenuHelpText)
		{
			addr_g_Project = derive(addr_UpdateMenuHelpText,
				gsig::UMH_MSPROJECT_LEG, "g_Project");
		}

		// The PCM_* / MI_* values are INTERIOR OFFSETS, and they do not transfer
		// between builds - Enhanced's bodies are a different shape entirely. So
		// each build derives from its own set: Legacy from PCM_* / MI_*,
		// Enhanced from PCM_E_*, taken from its own disassembly.
		if (isEnhanced())
		{
			const uintptr_t b = addr_PopulateCameraMenu;

			addr_g_MovieId        = derive(b, gsig::PCM_E_MOVIEID,     "g_MovieId");
			addr_BeginMethod      = derive(b, gsig::PCM_E_BEGINMETHOD, "BeginMethod");
			addr_AddParamString   = derive(b, gsig::PCM_E_ADDPARAMSTR, "AddParamString");
			addr_EndMethod        = derive(b, gsig::PCM_E_ENDMETHOD,   "EndMethod");
			addr_ScaleformRelease = derive(b, gsig::PCM_E_SFRELEASE,   "ScaleformRelease");
			addr_g_MenuOptions    = derive(b, gsig::PCM_E_MENUOPTIONS, "g_MenuOptions");

			// Enhanced takes the focus index from PopulateCameraMenu's prologue
			// rather than from MenuInput, so the menu needs nothing derived out
			// of the input handler at all.
			addr_g_MenuFocusIndex = derive(b, gsig::PCM_E_FOCUSINDEX,  "g_MenuFocusIndex");

			// GetCurrentEditMarker is inlined here (and GetCurrentClip inside
			// it), so there is no function to resolve - menu.cpp walks the clip
			// array itself using these three.
			addr_g_EditClipController = derive(b, gsig::PCM_E_CLIPCTRL,      "g_EditClipController");

			// Same pointer, honest name. It is the open PROJECT; what menu.cpp
			// treats as "the clip array" at +0x320 is really the montage, whose
			// clip array happens to sit at its start.
			addr_g_Project = addr_g_EditClipController;
			addr_g_EditClipIndex      = derive(b, gsig::PCM_E_CLIPINDEX,     "g_EditClipIndex");
			addr_g_EditMarkerIndex    = derive(b, gsig::PCM_E_EDITMARKERIDX, "g_EditMarkerIndex");

			// atArray::Grow is inlined too, so we grow the option array by hand
			// with the game's own allocator - PopulateCameraMenu frees that
			// buffer on the next rebuild, so a CRT block would crash. Taking the
			// free from inside this same function guarantees the pair matches.
			addr_GameFree = derive(b, gsig::PCM_E_FREE, "GameFree");

			const char* pAlloc = pick(gsig::GAME_ALLOC);
			addr_GameAlloc = (pAlloc && *pAlloc) ? memory::scan(pAlloc).address : 0;
			logger::write("info", "  GameAlloc              = %p", (void*)addr_GameAlloc);

			// Help text and target editing have no Enhanced addresses yet. Both
			// are optional and degrade quietly: rows still draw and toggle, they
			// just carry no help line and the attach / look-at / DOF controls
			// stay disabled.
			return;
		}

		const uintptr_t b = addr_PopulateCameraMenu;
		addr_GetCurrentEditMarker = derive(b, gsig::PCM_GETMARKER,   "GetCurrentEditMarker");
		addr_g_MovieId            = derive(b, gsig::PCM_MOVIEID,     "g_MovieId");
		addr_BeginMethod          = derive(b, gsig::PCM_BEGINMETHOD, "BeginMethod");
		addr_AddParamString       = derive(b, gsig::PCM_ADDPARAMSTR, "AddParamString");
		addr_EndMethod            = derive(b, gsig::PCM_ENDMETHOD,   "EndMethod");
		addr_g_MenuOptions        = derive(b, gsig::PCM_MENUOPTIONS, "g_MenuOptions");
		addr_ArrayGrow            = derive(b, gsig::PCM_GROW,        "ArrayGrow");
		addr_g_MenuFocusIndex     = derive(addr_MenuInput, gsig::MI_FOCUSINDEX, "g_MenuFocusIndex");
	}

	bool exportMenuReady()
	{
		return addr_VEMenuOpen && addr_VEAdjustToggle && addr_VEGetToggleString &&
		       addr_TextGet && addr_VEIsItemSelectable && addr_VEBuildMenu &&
		       addr_g_VEMenuArray && addr_g_VECurrentColumn &&
		       addr_g_VEMenuIdForColumn && addr_g_VECurrentItem &&
		       addr_MemAlloc && addr_MemFree;
	}

	// The export screen's own rows. Independent of resolveMenu() above - the two
	// menus share nothing but the movie - so one can fail without taking the
	// other with it. Failure here leaves Export exactly as R* shipped it.
	static void resolveExportMenu()
	{
		auto scan1 = [](const gsig::Sig& s, const char* what) -> uintptr_t {
			const char* p = pick(s);
			if (!p || !*p) return 0;
			const uintptr_t a = memory::scan(p, true).address;
			logger::write("info", "  %-22s = %p (rva 0x%llX)", what, (void*)a,
				(uint64_t)(a ? a - memory::base() : 0));
			return a;
		};

		addr_VEMenuOpen        = scan1(gsig::VEMENU_OPEN,            "VEMenu::Open");
		addr_VEAdjustToggle    = scan1(gsig::VEMENU_ADJUSTTOGGLE,    "VEMenu::AdjustToggle");
		addr_VEGetToggleString = scan1(gsig::VEMENU_GETTOGGLESTRING, "VEMenu::GetToggleStr");
		addr_VEBuildMenu       = scan1(gsig::VEMENU_BUILDMENU,       "VEMenu::BuildMenu");
		addr_TextGet           = scan1(gsig::VEMENU_TEXTGET,         "CText::Get");
		addr_MemAlloc          = scan1(gsig::VEMENU_MEMALLOC,        "MemAlloc");
		addr_MemFree           = scan1(gsig::VEMENU_MEMFREE,         "MemFree");

		if (const uintptr_t b = addr_VEAdjustToggle)
		{
			addr_g_VECurrentColumn  = derive(b, pickD(gsig::ATV_CURRENTCOLUMN),    "ms_iCurrentColumn");
			addr_VEIsItemSelectable = derive(b, pickD(gsig::ATV_ISITEMSELECTABLE), "IsItemSelectable");

			// The one place the two builds need different derive KINDS rather
			// than different offsets: MSVC indexes both arrays off a register
			// holding the image base, so the array address is an absolute RVA
			// sitting in the instruction's disp32.
			if (isEnhanced())
			{
				addr_g_VEMenuIdForColumn = derive(b, gsig::ATV_E_MENUIDFORCOL, "ms_iMenuIdForColumn");
				addr_g_VECurrentItem     = derive(b, gsig::ATV_E_CURRENTITEM,  "ms_iCurrentItem");
			}
			else
			{
				addr_g_VEMenuIdForColumn = deriveAbs(b, gsig::ATV_L_MENUIDFORCOL, "ms_iMenuIdForColumn");
				addr_g_VECurrentItem     = deriveAbs(b, gsig::ATV_L_CURRENTITEM,  "ms_iCurrentItem");
			}
		}

		// ms_MenuArray comes out of IsItemSelectable's own bounds check, which
		// reads the u16 element count at +8 - the shallowest reference to the
		// array in either binary.
		if (addr_VEIsItemSelectable)
		{
			const uintptr_t count = derive(addr_VEIsItemSelectable,
				pickD(gsig::IIS_MENUARRAY_COUNT), "ms_MenuArray.count");
			if (count) addr_g_VEMenuArray = count - gsig::VEMENU_ARRAY_COUNT_OFF;
		}
	}

	bool resolve()
	{
		// Enhanced has not been analysed; its patterns are deliberately empty so
		// we resolve nothing and stay inert rather than hooking on a guess.
		const char* pUpdate = pick(gsig::REPLAYDIRECTOR_UPDATESMOOTHING);
		const char* pNext   = pick(gsig::REPLAYDIRECTOR_GETNEXTMARKERINDEX);

		if (!pUpdate || !*pUpdate || !pNext || !*pNext)
		{
			logger::write("info", "  no signature set for this build (%s) вЂ” dormant", ExeName());
			return false;
		}

		addr_UpdateSmoothing = memory::scan(pUpdate).address;
		logger::write("info", "  UpdateSmoothing      = %p (rva 0x%llX)",
			(void*)addr_UpdateSmoothing,
			(uint64_t)(addr_UpdateSmoothing ? addr_UpdateSmoothing - memory::base() : 0));

		addr_GetNextMarkerIndex = memory::scan(pNext).address;
		logger::write("info", "  GetNextMarkerIndex   = %p (rva 0x%llX)",
			(void*)addr_GetNextMarkerIndex,
			(uint64_t)(addr_GetNextMarkerIndex ? addr_GetNextMarkerIndex - memory::base() : 0));

		if (addr_GetNextMarkerIndex)
		{
			// Both of these live INSIDE GetNextMarkerIndex: the marker-storage
			// global as a RIP-relative operand, and GetPreviousMarkerIndex as a
			// call rel32. Deriving them beats scanning вЂ” GetPreviousMarkerIndex
			// has an unremarkable prologue that would not pattern uniquely.
			addr_g_MarkerStorage = derive(addr_GetNextMarkerIndex,
				pickD(gsig::GNMI_MARKERSTORAGE), "g_MarkerStorage");

			// Legacy ONLY. Clang inlines GetPreviousMarkerIndex on Enhanced, so
			// there is no call rel32 at this offset there - reading it anyway
			// would produce a garbage pointer that prevMarkerIndex() would then
			// CALL, because the fallback only engages on a null. Guarded by the
			// opcode check as well, so a drifted Legacy build fails clean too.
			if (!isEnhanced())
			{
				addr_GetPrevMarkerIndex = derive(addr_GetNextMarkerIndex,
					gsig::GNMI_GETPREV, "GetPrevMarkerIndex");
			}
		}

		if (addr_UpdateSmoothing)
		{
			addr_g_ReplayTimeMs = derive(addr_UpdateSmoothing,
				pickD(gsig::US_REPLAYTIME), "g_ReplayTimeMs");
		}

		// Optional feature; failing to resolve it just leaves the leash in place.
		if (const char* pMd = pick(gsig::REPLAYDIRECTOR_GETMAXDISTANCE); pMd && *pMd)
		{
			addr_GetMaxDistanceFromPlayer = memory::scan(pMd).address;

			// Replay-mode global, straight off this function's first
			// instruction. NOT via memory::rip(): that assumes the operand is
			// the last 4 bytes of the instruction, but `cmp [rip+d], imm8` has
			// a trailing byte, so the displacement is relative to base+7.
			if (addr_GetMaxDistanceFromPlayer)
			{
				const unsigned char* insn = (const unsigned char*)addr_GetMaxDistanceFromPlayer;
				if (memcmp(insn, gsig::GMD_REPLAYMODE_OPCODE,
				           sizeof(gsig::GMD_REPLAYMODE_OPCODE)) == 0)
				{
					const int32_t disp = *(const int32_t*)(insn + gsig::GMD_REPLAYMODE_DISP_OFF);
					addr_g_ReplayMode = addr_GetMaxDistanceFromPlayer +
					                    gsig::GMD_REPLAYMODE_INSN_LEN + disp;
					logger::write("info", "  g_ReplayMode         = %p (rva 0x%llX)",
						(void*)addr_g_ReplayMode,
						(uint64_t)(addr_g_ReplayMode - memory::base()));
				}
			}
			logger::write("info", "  GetMaxDistFromPlayer = %p (rva 0x%llX)",
				(void*)addr_GetMaxDistanceFromPlayer,
				(uint64_t)(addr_GetMaxDistanceFromPlayer
					? addr_GetMaxDistanceFromPlayer - memory::base() : 0));
		}

		if (const char* p = pick(gsig::FREECAM_UPDATECOLLISION); p && *p)
		{
			addr_UpdateCollision = memory::scan(p).address;
			logger::write("info", "  UpdateCollision      = %p (rva 0x%llX)", (void*)addr_UpdateCollision,
				(uint64_t)(addr_UpdateCollision ? addr_UpdateCollision - memory::base() : 0));
		}
		if (const char* p = pick(gsig::FREECAM_COMPUTESAFEPOSITION); p && *p)
		{
			addr_ComputeSafePosition = memory::scan(p).address;
			logger::write("info", "  ComputeSafePosition  = %p (rva 0x%llX)", (void*)addr_ComputeSafePosition,
				(uint64_t)(addr_ComputeSafePosition ? addr_ComputeSafePosition - memory::base() : 0));
		}

		if (const char* p = pick(gsig::REPLAYMGR_JUMPTO); p && *p)
		{
			addr_ReplayJumpTo = memory::scan(p).address;
			logger::write("info", "  ReplayJumpTo         = %p (rva 0x%llX)", (void*)addr_ReplayJumpTo,
				(uint64_t)(addr_ReplayJumpTo ? addr_ReplayJumpTo - memory::base() : 0));
		}

		// Optional: without it the clip keeps its own recorded time and weather.
		if (const char* p = pick(gsig::PACKETWEATHER_EXTRACT); p && *p)
		{
			addr_PacketWeatherExtract = memory::scan(p).address;
			logger::write("info", "  PacketWeatherExtract = %p (rva 0x%llX)",
				(void*)addr_PacketWeatherExtract,
				(uint64_t)(addr_PacketWeatherExtract ? addr_PacketWeatherExtract - memory::base() : 0));
		}

		// Optional: without it an overridden clip keeps its baked lighting.
		if (const char* p = pick(gsig::TIMECYCLE_VARINFOS); p && *p)
		{
			addr_g_TcVarInfos = memory::scan(p).address;
			logger::write("info", "  TimecycleVarTable    = %p (rva 0x%llX)",
				(void*)addr_g_TcVarInfos,
				(uint64_t)(addr_g_TcVarInfos ? addr_g_TcVarInfos - memory::base() : 0));
		}

		// Optional: without it a clip recorded in first person keeps the
		// recorded camera no matter what its markers ask for.
		if (const char* p = pick(gsig::REPLAY_ISPLAYBACKFLAGSET); p && *p)
		{
			addr_IsPlaybackFlagSet = memory::scan(p).address;
			logger::write("info", "  IsPlaybackFlagSet    = %p (rva 0x%llX)",
				(void*)addr_IsPlaybackFlagSet,
				(uint64_t)(addr_IsPlaybackFlagSet ? addr_IsPlaybackFlagSet - memory::base() : 0));
		}

		// Optional: without it the editor keeps the stock precache wait, i.e. the
		// full 200-frame give-up after every seek on a modded install.
		if (const char* p = pick(gsig::REPLAY_ISWAITINGONWORLDSTREAMING); p && *p)
		{
			addr_IsWaitingOnWorldStreaming = memory::scan(p).address;
			logger::write("info", "  IsWaitingOnWorldStrm = %p (rva 0x%llX)",
				(void*)addr_IsWaitingOnWorldStreaming,
				(uint64_t)(addr_IsWaitingOnWorldStreaming
					? addr_IsWaitingOnWorldStreaming - memory::base() : 0));

			// Its own give-up compare names the stall timer, and the audio one
			// is its neighbour. Only used for the audio half of the fix; a
			// failed derive leaves that off and the streaming half unaffected.
			addr_g_StreamingStallTimer = derive(addr_IsWaitingOnWorldStreaming,
				pickD(gsig::IWWS_STALLTIMER), "g_StreamingStallTimer");
		}

		// Optional: without it the precache's preload wait stays unbounded, so a
		// single un-streamable entity holds it for 10 s of real time.
		if (const char* p = pick(gsig::REPLAY_HANDLERESULTS); p && *p)
		{
			addr_AdvanceReaderHandleResults = memory::scan(p).address;
			logger::write("info", "  AdvReader::HandleRes = %p (rva 0x%llX)",
				(void*)addr_AdvanceReaderHandleResults,
				(uint64_t)(addr_AdvanceReaderHandleResults
					? addr_AdvanceReaderHandleResults - memory::base() : 0));
		}

		// Optional: without it an un-streamable entity still hangs the main
		// thread for up to five seconds while it spins LoadAllRequestedObjects.
		if (const char* p = pick(gsig::REPLAY_MODELMGR_LOADMODEL); p && *p)
		{
			addr_ModelMgrLoadModel = memory::scan(p).address;
			logger::write("info", "  ModelMgr::LoadModel  = %p (rva 0x%llX)",
				(void*)addr_ModelMgrLoadModel,
				(uint64_t)(addr_ModelMgrLoadModel
					? addr_ModelMgrLoadModel - memory::base() : 0));
		}

		// Optional: without it recording keeps the stock 7-block budget.
		if (const char* p = pick(gsig::REPLAY_SETUPREPLAYBUFFER); p && *p)
		{
			addr_SetupReplayBuffer = memory::scan(p).address;
			logger::write("info", "  SetupReplayBuffer    = %p (rva 0x%llX)",
				(void*)addr_SetupReplayBuffer,
				(uint64_t)(addr_SetupReplayBuffer ? addr_SetupReplayBuffer - memory::base() : 0));

			// The buffer info struct, via the allocated-count field its prologue
			// loads. Instrumentation only - it is how we find out what the ring
			// ACTUALLY is at runtime rather than what we asked for.
			if (addr_SetupReplayBuffer)
			{
				const uintptr_t allocField = derive(addr_SetupReplayBuffer,
					pickD(gsig::SRB_BLOCKSALLOCATED), "g_ReplayBlocksAllocated");
				if (allocField)
					addr_g_ReplayBufferInfo = allocField - gsig::BUFINFO_ALLOCATED_OFF;
			}
		}

		// The block count the RECORDING path reads. Taken off that call site
		// rather than hooked, because the call can happen before we are up.
		if (const char* p = pick(gsig::REPLAY_RECORDBUFFER_SITE); p && *p)
		{
			const uintptr_t site = memory::scan(p).address;
			if (site)
			{
				addr_g_ReplayBlocks = derive(site, pickD(gsig::RBS_NUMBLOCKS), "g_ReplayBlocks");

				// Enhanced only - the clamp that actually bounds a recording.
				if (isEnhanced())
					addr_g_ReplayTotalBlocks =
						derive(site, gsig::RBS_TOTALBLOCKS, "g_ReplayTotalBlocks");
			}
			else
				logger::write("info", "  replay record site not found - block count unchanged");
		}

		// The two heap-size immediates, plus the allocator object we test to
		// find out whether the heap has already been committed. Resolved as a
		// PAIR - see signatures.h for why a half-resolve must be treated as
		// none at all.
		if (const char* p = pick(gsig::REPLAY_HEAP_ALLOC_SITE); p && *p)
		{
			const uintptr_t a = memory::scan(p, true).address;
			const char* q = pick(gsig::REPLAY_HEAP_CTOR_SITE);
			const uintptr_t c = (q && *q) ? memory::scan(q, true).address : 0;

			// The allocator object. Enhanced leas it at the top of its own init
			// block; Legacy leas it into rbp just after the reservation, i.e.
			// inside the alloc site above.
			if (isEnhanced())
			{
				if (const char* h = pick(gsig::REPLAY_HEAP_INIT_SITE); h && *h)
				{
					const uintptr_t site = memory::scan(h, true).address;
					if (site)
						addr_g_ReplayAllocator =
							derive(site, gsig::RHI_ALLOCATOR_OBJ_ENH, "g_ReplayAllocatorObj");
				}
			}
			else if (a)
			{
				addr_g_ReplayAllocator =
					derive(a, gsig::RHI_ALLOCATOR_OBJ_LEG, "g_ReplayAllocatorObj");
			}

			if (a && c)
			{
				addr_ReplayHeapAllocImm = a + gsig::RHA_SIZE_IMM_OFF;
				addr_ReplayHeapCtorImm  = c + (isEnhanced() ? gsig::RHC_SIZE_IMM_OFF_ENH
				                                           : gsig::RHC_SIZE_IMM_OFF_LEG);
				logger::write("info",
					"  ReplayHeapSize       = %p / %p (rva 0x%llX / 0x%llX, currently %u MB)",
					(void*)addr_ReplayHeapAllocImm, (void*)addr_ReplayHeapCtorImm,
					(uint64_t)(addr_ReplayHeapAllocImm - memory::base()),
					(uint64_t)(addr_ReplayHeapCtorImm - memory::base()),
					*(unsigned*)addr_ReplayHeapAllocImm / (1024u * 1024u));
			}
			else if (replayHeapWidenedEarly)
			{
				// Expected, not a failure. The early path already patched the
				// size immediate, and the patterns key on that immediate's stock
				// value - so they can no longer match the very sites they came
				// from. The addresses it handed over are already in place.
				logger::write("info",
					"  ReplayHeapSize       = %p / %p (patched at attach, currently %u MB)",
					(void*)addr_ReplayHeapAllocImm, (void*)addr_ReplayHeapCtorImm,
					*(unsigned*)addr_ReplayHeapAllocImm / (1024u * 1024u));
			}
			else
			{
				logger::write("info",
					"  replay heap sites incomplete (alloc=%d ctor=%d) - heap left alone",
					a != 0, c != 0);
			}
		}

		if (const char* p = pick(gsig::REPLAYMGR_SETCURSORSPEED); p && *p)
		{
			addr_SetCursorSpeed = memory::scan(p).address;
			logger::write("info", "  SetCursorSpeed       = %p (rva 0x%llX)", (void*)addr_SetCursorSpeed,
				(uint64_t)(addr_SetCursorSpeed ? addr_SetCursorSpeed - memory::base() : 0));
			addr_SetNextPlayBackState = derive(addr_SetCursorSpeed,
				pickD(gsig::SCS_SETNEXTPLAYBACKSTATE), "SetNextPlayBackState");
		}

		// Optional: without it the profanity bypass is simply unavailable.
		if (const char* p = pick(gsig::PROFANITY_POLLSITE); p && *p)
		{
			const uintptr_t site = memory::scan(p).address;
			if (site)
				addr_ProfanityGetStatus = derive(site, gsig::PPS_GETSTATUS, "ProfanityGetStatus");
			else
				logger::write("info", "  profanity poll site not found - bypass unavailable");
		}

		if (const char* p = pick(gsig::MOUSEPOINTER_UPDATE); p && *p)
		{
			addr_MousePointerUpdate = memory::scan(p).address;
			addr_g_CursorVisible = derive(addr_MousePointerUpdate,
				pickD(gsig::MP_CURSORVISIBLE), "g_CursorVisible");
		}

		if (const char* p = pick(gsig::VIDEOEDITOR_RENDERSPINNER); p && *p)
		{
			addr_RenderSpinner = memory::scan(p).address;
			logger::write("info", "  RenderSpinner        = %p (rva 0x%llX)",
				(void*)addr_RenderSpinner,
				(uint64_t)(addr_RenderSpinner ? addr_RenderSpinner - memory::base() : 0));
		}
		// The game's OTHER spinner, and the one that was still getting into
		// rendered frames on Enhanced. Unrelated to RenderAnimatedSpinner above.
		if (const char* p = pick(gsig::VIDEOEDITOR_DRAWSPINNER); p && *p)
		{
			addr_DrawSpinner = memory::scan(p).address;
			logger::write("info", "  DrawSpinner          = %p (rva 0x%llX)",
				(void*)addr_DrawSpinner,
				(uint64_t)(addr_DrawSpinner ? addr_DrawSpinner - memory::base() : 0));
		}

		if (const char* p = pick(gsig::SCALEFORM_DRAWMOVIE); p && *p)
		{
			addr_ScaleformDrawMovie = memory::scan(p).address;
			logger::write("info", "  ScaleformDrawMovie   = %p (rva 0x%llX)",
				(void*)addr_ScaleformDrawMovie,
				(uint64_t)(addr_ScaleformDrawMovie ? addr_ScaleformDrawMovie - memory::base() : 0));
		}

		if (const char* p = pick(gsig::BUSYSPINNER_ON); p && *p)
		{
			addr_BusySpinnerOn = memory::scan(p).address;
			logger::write("info", "  BusySpinnerOn        = %p (rva 0x%llX)",
				(void*)addr_BusySpinnerOn,
				(uint64_t)(addr_BusySpinnerOn ? addr_BusySpinnerOn - memory::base() : 0));
		}

		if (const char* p = pick(gsig::BUSYSPINNER_RENDER); p && *p)
		{
			addr_BusySpinnerRender = memory::scan(p).address;
			logger::write("info", "  BusySpinnerRender    = %p (rva 0x%llX)",
				(void*)addr_BusySpinnerRender,
				(uint64_t)(addr_BusySpinnerRender ? addr_BusySpinnerRender - memory::base() : 0));
		}
		if (const char* p = pick(gsig::VIDEOEDITOR_SHOULDSHOWLOADING); p && *p)
		{
			addr_ShouldShowLoading = memory::scan(p).address;
			logger::write("info", "  ShouldShowLoading    = %p (rva 0x%llX)",
				(void*)addr_ShouldShowLoading,
				(uint64_t)(addr_ShouldShowLoading ? addr_ShouldShowLoading - memory::base() : 0));
		}

		// Optional: without it renders cannot cross clip boundaries.
		if (const char* p = pick(gsig::REPLAY_JUMPTONONDILATED); p && *p)
		{
			addr_JumpToNonDilated = memory::scan(p).address;
			logger::write("info", "  JumpToNonDilated     = %p (rva 0x%llX)",
				(void*)addr_JumpToNonDilated,
				(uint64_t)(addr_JumpToNonDilated ? addr_JumpToNonDilated - memory::base() : 0));

			// Legacy's playback controller. Enhanced gets it from the inlined
			// seek below; here the same instance is loaded inside this function,
			// which is the only place either build exposes it. clipRange() needs
			// it to map project time into the clip's own time, so without this
			// Legacy multi-clip renders freeze on clip two's first frame.
			if (addr_JumpToNonDilated && !addr_g_PlaybackController)
				addr_g_PlaybackController =
					derive(addr_JumpToNonDilated, gsig::JTND_CONTROLLER, "g_PlaybackController");
		}

		// Enhanced only: the seek is inlined, so pattern the inlined block and
		// take g_ReplayMode and the controller instance out of it.
		if (const char* p = pick(gsig::VIDEOEDITOR_INLINED_SEEK); p && *p)
		{
			const uintptr_t site = memory::scan(p).address;
			if (site)
			{
				addr_g_ReplayMode = derive(site, gsig::IS_REPLAYMODE, "g_ReplayMode");
				addr_g_PlaybackController =
					derive(site, gsig::IS_CONTROLLER, "g_PlaybackController");
			}
			else
			{
				logger::write("info", "  inlined seek not found - project seeking unavailable");
			}
		}

		// Optional: without it Export keeps the game's own encoder.
		if (const char* p = pick(gsig::VIDEOEDITOR_PLAYBACK_OPEN); p && *p)
		{
			addr_PlaybackOpen = memory::scan(p).address;
			logger::write("info", "  PlaybackOpen         = %p (rva 0x%llX)", (void*)addr_PlaybackOpen,
				(uint64_t)(addr_PlaybackOpen ? addr_PlaybackOpen - memory::base() : 0));

			if (addr_PlaybackOpen)
			{
				// All five come out of Open. The offsets and even the encodings
				// differ per build, so each is a DerivePair; see signatures.h.
				addr_g_PlaybackType =
					derive(addr_PlaybackOpen, pickD(gsig::PO_PLAYBACKTYPE),     "g_PlaybackType");
				addr_g_ShouldRender =
					derive(addr_PlaybackOpen, pickD(gsig::PO_SHOULDRENDER),     "g_ShouldRender");
				addr_g_LoadingScreen =
					derive(addr_PlaybackOpen, pickD(gsig::PO_LOADINGSCREEN),    "g_LoadingScreen");
				addr_g_PreCaching =
					derive(addr_PlaybackOpen, pickD(gsig::PO_PRECACHING),       "g_PreCaching");
				addr_g_WantDelayedClose =
					derive(addr_PlaybackOpen, pickD(gsig::PO_WANTDELAYEDCLOSE), "g_WantDelayedClose");
			}
		}

		resolveMenu();
		logger::write("info", "  menu injection: %s", menuReady() ? "available" : "UNAVAILABLE");

		resolveExportMenu();
		logger::write("info", "  export menu rows: %s",
			exportMenuReady() ? "available" : "UNAVAILABLE");

		// GetPreviousMarkerIndex is deliberately NOT required: Clang inlines it
		// on Enhanced, so there is nothing to resolve there and prevMarkerIndex()
		// falls back to walking the markers itself.
		const bool ok = addr_UpdateSmoothing && addr_GetNextMarkerIndex &&
		                addr_g_MarkerStorage && addr_g_ReplayTimeMs;
		if (!ok)
			logger::write("info", "  !! incomplete resolve вЂ” hook will NOT be installed");
		return ok;
	}

	int nextMarkerIndex(int fromIndex)
	{
		if (!addr_GetNextMarkerIndex) return -1;
		using Fn = int(__fastcall*)(int);
		return ((Fn)addr_GetNextMarkerIndex)(fromIndex);
	}

	int prevMarkerIndex(int fromIndex)
	{
		if (addr_GetPrevMarkerIndex)
		{
			using Fn = int(__fastcall*)(int);
			return ((Fn)addr_GetPrevMarkerIndex)(fromIndex);
		}

		// Enhanced INLINES GetPreviousMarkerIndex into GetNextMarkerIndex, so
		// there is no function to call. Reimplement it: walk back from the given
		// index for the first marker that is not an anchor. This mirrors what
		// the inlined loop does, and uses the same vtable slots and the same
		// `markerType != 2` test the game itself uses.
		void* storage = markerStorage();
		if (!storage || fromIndex <= 0) return -1;

		for (int i = fromIndex - 1; i >= 0; --i)
		{
			void* m = rstorage::tryGetMarker(storage, i);
			if (m && rmarker::get<uint8_t>(m, rmarker::OFF_MarkerType) != 2)
				return i;
		}
		return -1;
	}

	bool jumpTo(float timeMs, unsigned jumpOptions)
	{
		if (!addr_ReplayJumpTo) return false;
		using Fn = bool(__fastcall*)(float, unsigned);
		return ((Fn)addr_ReplayJumpTo)(timeMs, jumpOptions);
	}

	// Attach / look-at / DOF target stepping lived here. It existed only for the
	// ReShade overlay's target panel; the in-editor menu never called it, and
	// ReShade is now purely the IGCS capture path. Removed 2026-07-27 together
	// with the panel, the six MI_* stepper derives and GetReplayDirector.
	// ENHANCED_PORT.md records the addresses on both builds if it is ever wanted
	// back - the Enhanced six are found via the shared core's caller list.

	int replayMode()
	{
		return addr_g_ReplayMode ? *(int*)addr_g_ReplayMode : -1;
	}

	bool isEditModeActive()
	{
		if (!addr_g_ReplayMode) return true; // unknown: do not hide anything
		return *(int*)addr_g_ReplayMode == gsig::REPLAYMODE_EDIT;
	}

	bool transportReady()
	{
		return addr_SetNextPlayBackState != 0;
	}

	void playbackPlay()
	{
		if (!addr_SetNextPlayBackState) return;
		using Fn = void(__fastcall*)(unsigned);
		((Fn)addr_SetNextPlayBackState)(gsig::RS_PLAY_FWD);
	}

	void playbackPause()
	{
		if (!addr_SetNextPlayBackState) return;
		using Fn = void(__fastcall*)(unsigned);
		((Fn)addr_SetNextPlayBackState)(gsig::RS_PAUSE);
	}

	// SetCursorSpeed drives both the rate and the play direction: it issues
	// PLAY|BACK for negative speeds and PLAY|FWD otherwise, and the exact 1.0
	// case additionally resets the cursor to NORMAL.
	void playbackSetSpeed(float speed)
	{
		if (!addr_SetCursorSpeed) return;
		using Fn = void(__fastcall*)(float);
		((Fn)addr_SetCursorSpeed)(speed);
	}

	void* markerStorage()
	{
		if (!addr_g_MarkerStorage) return nullptr;
		return *(void**)addr_g_MarkerStorage;
	}
}

