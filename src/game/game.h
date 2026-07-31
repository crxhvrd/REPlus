// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdint>

// Resolved retail addresses. 0 == unresolved; the hook is simply not installed
// and the game keeps its stock behaviour.
namespace game
{
	extern uintptr_t addr_UpdateSmoothing;       // camReplayDirector::UpdateSmoothing(this)
	extern uintptr_t addr_GetNextMarkerIndex;    // camReplayDirector::GetNextMarkerIndex(int)
	extern uintptr_t addr_GetPrevMarkerIndex;    // camReplayDirector::GetPreviousMarkerIndex(int)
	extern uintptr_t addr_g_MarkerStorage;       // IReplayMarkerStorage** (global slot)
	extern uintptr_t addr_g_ReplayTimeMs;        // float, CReplayMgr current time relative ms
	extern uintptr_t addr_GetMaxDistanceFromPlayer; // camReplayDirector::GetMaxDistanceAllowedFromPlayer(bool)
	extern uintptr_t addr_UpdateCollision;       // camReplayFreeCamera::UpdateCollision(initialPos, cameraPos)
	extern uintptr_t addr_ComputeSafePosition;   // camReplayFreeCamera::ComputeSafePosition(entity, cameraPos)
	extern uintptr_t addr_ProfanityGetStatus;    // netProfanityFilter::GetStatusForRequest
	extern uintptr_t addr_ReplayJumpTo;          // CReplayMgrInternal::JumpTo(float ms, u32 opts)
	extern uintptr_t addr_JumpToNonDilated;      // CVideoProjectPlaybackController::JumpToNonDilatedTimeMs
	extern uintptr_t addr_g_PlaybackController;  // the controller instance (Enhanced)
	extern uintptr_t addr_PlaybackOpen;          // CVideoEditorPlayback::Open(type, u32)
	extern uintptr_t addr_g_PlaybackType;        // int, ms_playbackType (1 = BAKE)
	extern uintptr_t addr_g_ShouldRender;        // bool, ms_shouldRender (editor HUD)
	extern uintptr_t addr_MousePointerUpdate;    // CMousePointer per-frame update
	extern uintptr_t addr_g_CursorVisible;       // bool, CMousePointer ms_State.m_bVisible
	extern uintptr_t addr_RenderSpinner;         // CPauseMenu::RenderAnimatedSpinner
	extern uintptr_t addr_BusySpinnerRender;     // CBusySpinner::Render - a SECOND spinner
	extern uintptr_t addr_BusySpinnerOn;         // CBusySpinner::On - where a spinner is RAISED
	extern uintptr_t addr_ScaleformRenderMovie;  // CScaleformMgr::RenderMovie - every movie's draw
	extern uintptr_t addr_DrawSpinner;           // the spinner's real draw, under RenderAnimatedSpinner
	extern uintptr_t addr_ShouldShowLoading;     // CVideoEditorInterface::ShouldShowLoadingScreen
	extern uintptr_t addr_g_WantDelayedClose;    // bool, ms_bWantDelayedClose
	extern uintptr_t addr_g_LoadingScreen;       // bool, ms_bLoadingScreen
	extern uintptr_t addr_g_PreCaching;          // bool, ms_bPreCaching

	// True while the editor is showing its loading screen or pre-caching a
	// scene - i.e. while the frame on screen is not the clip. Nothing should be
	// captured during this.
	bool replayBusy();

	// Ask the editor to close playback at its next safe point, the same way a
	// finished bake does. Returns false when unresolved.
	bool requestPlaybackClose();

	// Hide the mouse cursor. Must be re-asserted every frame: the input code
	// rewrites this whenever the mouse is in use.
	void setCursorVisible(bool visible);

	// Hide or restore the editor's playback HUD - timer, transport, scrub bar
	// and instructional buttons. This is the same flag the hide-HUD key toggles.
	void setEditorHudVisible(bool visible);
	bool editorHudControllable();

	extern uintptr_t addr_SetCursorSpeed;        // CReplayMgrInternal::SetCursorSpeed(float)
	extern uintptr_t addr_SetNextPlayBackState;  // CReplayMgrInternal::SetNextPlayBackState(u32)
	extern uintptr_t addr_g_ReplayMode;          // int, CReplayMgr mode (2 = EDIT)

	// Raw CReplayMgr mode, or -1 when unresolved. EDIT is 2, but a full-project
	// preview runs in a different one - which is why anything long-running
	// should compare against the mode it STARTED in rather than against EDIT.
	int  replayMode();

	// True while the Rockstar Editor is active. False during normal gameplay,
	// which is what the overlay uses to stay out of the way.
	bool isEditModeActive();

	// Seek playback. Returns true if a jump was actually issued (the game
	// no-ops when already within 0.001ms of the target).
	bool jumpTo(float timeMs, unsigned jumpOptions);

	// Seek the whole PROJECT timeline - crosses clip boundaries, which jumpTo
	// cannot. Falls back to jumpTo when unresolved. Requires the replay mode to
	// be EDIT; returns false otherwise.
	bool clipRange(float& lo, float& hi);  // current clip's own time span
	bool jumpProjectTo(float timeMs, unsigned jumpOptions);

	// --- The project's clips ---------------------------------------------
	//
	// All four are the same CReplayPlaybackController the seek above uses, and
	// all four answer nothing useful outside the editor: clipIndex is -1,
	// clipCount is 0, and the two range calls return false.
	//
	// A clip index of -1 means "the one on screen".
	bool clipRangeAt(int clipIndex, float& lo, float& hi);
	int  clipIndex();
	int  clipCount();

	// Step to another clip. This is the ONLY way across a clip boundary - a
	// plain seek clamps to the clip it is already in - and it works while
	// playback is paused, which is what lets a frame render walk a whole
	// project. Asynchronous: the engine performs the transition on a later
	// update, so watch clipIndex() rather than assuming it has happened.
	bool jumpToClip(int clipIndex, float clipTimeMs);

	// Transport. All no-op safely when unresolved.
	void playbackPlay();
	void playbackPause();
	void playbackSetSpeed(float speed);   // <0 rewinds, 1.0 = normal
	bool transportReady();

	// --- Video Editor menu (all 0 if the menu feature failed to resolve) ---
	extern uintptr_t addr_PopulateCameraMenu;  // void(u32 focusToRestore)
	// The TOP-LEVEL marker menu (Camera / DOF / Effects / Audio / Speed / ...).
	// Our global rows go here; the camera submenu above is per-marker.
	extern uintptr_t addr_PopulateMarkerMenu;  // void(u32 focusToRestore)
	extern uintptr_t addr_MenuInput;           // void(int navCode)
	extern uintptr_t addr_GetCurrentEditMarker;// sReplayMarkerInfo*()
	extern uintptr_t addr_BeginMethod;         // bool(int movie,int cls,const char*,int,int)
	extern uintptr_t addr_AddParamString;      // void(const char*, bool)
	extern uintptr_t addr_EndMethod;           // void(bool)
	extern uintptr_t addr_ArrayGrow;           // MenuOption*(void* arr, int grow)
	extern uintptr_t addr_g_MovieId;           // int
	extern uintptr_t addr_g_MenuOptions;       // atArray{ void* data; u16 count; u16 cap }
	extern uintptr_t addr_g_MenuFocusIndex;    // int
	extern uintptr_t addr_UpdateItemText;      // void(int index, const char* text)

	// Enhanced-only; 0 on Legacy, which never reads them. Enhanced inlines
	// atArray::Grow and GetCurrentEditMarker, and threads a context object
	// through its Scaleform calls, so it needs pieces Legacy does not.
	extern uintptr_t addr_ScaleformRelease;    // void(void*, void*, void*)
	extern uintptr_t addr_GameAlloc;           // void*(u64 size)  - 16-byte aligned
	extern uintptr_t addr_GameFree;            // void(void*)
	extern uintptr_t addr_g_EditClipController;// CVideoProjectPlaybackController*
	extern uintptr_t addr_g_EditClipIndex;     // int

	// True when every menu address above resolved.
	bool menuReady();

	// Index of the marker the editor currently has open. Enhanced needs it
	// because GetCurrentEditMarker is inlined there and menu.cpp walks the clip
	// array by hand; Legacy leaves it 0 and calls GetCurrentEditMarker instead.
	extern uintptr_t addr_g_EditMarkerIndex;

	// The playback the editor currently has open, or -1 when unresolved.
	void openPlayback(int type);
	int  playbackType();
	void setPlaybackType(int type);

	bool isEnhanced();

	// Scan per-build signatures and populate the addresses above.
	// Returns true only if everything the hook needs resolved.
	bool resolve();

	// Convenience wrappers around the resolved neighbour-index helpers. These
	// are the game's own, so they inherit its anchor-skipping and its
	// end-of-timeline count-1 adjustment вЂ” reimplementing that is how you get
	// off-by-one bugs at the last marker.
	int nextMarkerIndex(int fromIndex);
	int prevMarkerIndex(int fromIndex);

	// Dereferences the global slot to the live IReplayMarkerStorage*.
	void* markerStorage();
}

