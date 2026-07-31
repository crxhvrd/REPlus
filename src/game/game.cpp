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
	uintptr_t addr_ScaleformRenderMovie= 0;
	uintptr_t addr_DrawSpinner         = 0;
	uintptr_t addr_ShouldShowLoading   = 0;
	uintptr_t addr_g_WantDelayedClose  = 0;
	uintptr_t addr_g_LoadingScreen     = 0;
	uintptr_t addr_g_PreCaching        = 0;
	uintptr_t addr_ReplayJumpTo        = 0;
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
	uintptr_t addr_g_EditMarkerIndex    = 0;

	// Enhanced-only. Legacy leaves these 0 and never reads them: it has a real
	// atArray::Grow and a real GetCurrentEditMarker to call, and its Scaleform
	// calls carry no context object.
	uintptr_t addr_ScaleformRelease     = 0;
	uintptr_t addr_GameAlloc            = 0;
	uintptr_t addr_GameFree             = 0;
	uintptr_t addr_g_EditClipController = 0;
	uintptr_t addr_g_EditClipIndex      = 0;

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

	bool replayBusy()
	{
		// The black-logo frames come from this call, not from a flag.
		if (addr_ShouldShowLoading)
		{
			using Fn = bool(__fastcall*)();
			if (((Fn)addr_ShouldShowLoading)()) return true;
		}
		if (addr_g_LoadingScreen && *(unsigned char*)addr_g_LoadingScreen) return true;
		if (addr_g_PreCaching    && *(unsigned char*)addr_g_PreCaching)    return true;
		return false;
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
		logger::write("info", "  %-22s = %p (rva 0x%llX)", what, (void*)a,
			(uint64_t)(a - memory::base()));
		return a;
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

		if (const char* p = pick(gsig::SCALEFORM_RENDERMOVIE); p && *p)
		{
			addr_ScaleformRenderMovie = memory::scan(p).address;
			logger::write("info", "  ScaleformRenderMovie = %p (rva 0x%llX)",
				(void*)addr_ScaleformRenderMovie,
				(uint64_t)(addr_ScaleformRenderMovie ? addr_ScaleformRenderMovie - memory::base() : 0));
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

