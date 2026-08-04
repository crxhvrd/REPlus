// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "capture/exporthook.h"
#include "capture/render.h"
#include "capture/fxcapture.h"
#include "game/signatures.h"

// =============================================================================
//  Export -> image sequence
// =============================================================================
//  The editor's Export button ends up in CVideoEditorUi::TriggerExport, which
//  calls CVideoEditorPlayback::Open(PLAYBACK_TYPE_BAKE). Bake is the game's own
//  encoder path: its codec, its watermark, its profanity check.
//
//  We intercept that single call and rewrite the argument to
//  PLAYBACK_TYPE_PREVIEW_FULL_PROJECT, so Export opens an ordinary full-project
//  playback instead, and the image-sequence renderer drives it.
//
//  Why one argument rather than menu rows: the export screen is built by
//  CVideoEditorMenu from R*'s own menu metadata, and its input handler indexes
//  that array. Rows we drew would not exist in it, so focus and TriggerAction
//  would run off the end. Rewriting an argument touches none of that - the menu
//  is untouched and keeps working exactly as shipped.
// =============================================================================
namespace exporthook
{
	namespace
	{
		using FnOpen = void(__fastcall*)(int, unsigned);
		FnOpen origOpen = nullptr;

		// Set when we have diverted a bake, cleared once the render starts.
		// The render cannot begin here: Open has not finished setting playback
		// up yet, and the replay clock is not usable until it has.
		bool s_pending = false;

		// How many times Open has actually been intercepted. Zero after the user
		// has pressed Export is the whole diagnosis: the pattern resolved to
		// something, but not to the function the editor calls.
		unsigned s_opens = 0;

		// The first bytes at the target, sampled AFTER our detour is written.
		//
		// Another ASI that patches the same function with a raw jump - EVE/EVER
		// install absolute jumps rather than trampolines - overwrites us with no
		// error anywhere: MinHook reported success, our log says "hooked", and
		// the detour is simply gone. That is invisible today and it is a real
		// possibility on a machine running several export mods.
		uint8_t  s_prologue[16]{};
		bool     s_prologueSaved = false;

		void __fastcall hkOpen(int type, unsigned arg)
		{
			// Report each playback type the first time it comes through.
			//
			// Without this, "Export produced a normal bake" and "Export never
			// reached us" look identical in the log - both are simply an absence
			// of lines, and the two have completely different causes. One report
			// cost a round trip for exactly that reason.
			//
			// Deduplicated by type, so this is a handful of lines a session, not
			// a per-open stream. type 0 = PREVIEW_FULL_PROJECT, 1 = BAKE.
			{
				// EVERY call, not the first per type.
				//
				// This used to deduplicate by type to keep the log short, and that
				// cost a diagnosis: a user reported Export falling back to the
				// vanilla encoder, and their log contained no Open line at all.
				// With dedup in place "the hook never fired" and "it fired again
				// on a type already seen" are the same absence, so there was no
				// way to tell a bad pattern from a bad decision. Open is pressed
				// by hand a few times a session - there is nothing to save here.
				++s_opens;
				logger::write("info", "export: playback Open(type=%d, arg=%u) [call %u]%s",
					type, arg, s_opens,
					type == gsig::PLAYBACK_TYPE_BAKE ? "  <- this is Export/bake" : "");
			}

			if (type == gsig::PLAYBACK_TYPE_BAKE &&
			    Config::get().enableRenderer)
			{
				if (fxcapture::addonPresent())
				{
					type      = gsig::PLAYBACK_TYPE_PREVIEW_FULL_PROJECT;
					s_pending = true;
					logger::write("info", "export: bake diverted to the RE+ renderer");
					// Anything else that hooks the game's own export sees
					// nothing at all now: the bake it waits for never starts.
					// Those tools hang off the bake pipeline, so they do not
					// error - they simply never fire. Say so here rather than
					// leaving two mods to look broken in each other's presence.
					logger::write("info",
						"export: the game's own bake will NOT run - if you meant to use "
						"another video-export mod, set EnableRenderer=0");
				}
				else
				{
					// Falling through to a real bake would be worse than doing
					// nothing: the user asked for an image sequence and would
					// silently get a watermarked, codec-compressed video.
					//
					// Say WHY, not just that. "Addon not loaded" sends people
					// hunting for a missing file, but the usual cause is an addon
					// that IS loaded and simply never presents - its heartbeat
					// only ticks from ReShade's present callback, so anything that
					// takes over presenting (an ENB d3d11.dll in front of
					// ReShade's dxgi.dll, add-on support switched off) leaves the
					// file sitting there doing nothing. The channel state
					// distinguishes those without a second round trip.
					logger::write("info",
						"export: capture addon not usable - leaving the game's own export "
						"alone. channel=%s heartbeat=%u reshade=%s. Rendering needs ReShade "
						"WITH FULL ADD-ON SUPPORT plus the IgcsConnector.addon64 BUNDLED "
						"WITH THIS MOD - any other IgcsConnector lacks the capture channel "
						"and will present happily without ever grabbing a frame. If both are "
						"right, check the add-on is enabled and that nothing else (ENB) owns "
						"present.",
						fxcapture::available() ? "mapped" : "NOT MAPPED",
						fxcapture::heartbeat(),
						fxcapture::hostState() == fxcapture::HOST_ADDONS   ? "with add-ons"
						: fxcapture::hostState() == fxcapture::HOST_NO_ADDONS ? "ORDINARY BUILD"
						                                                     : "NOT LOADED");
				}
			}

			origOpen(type, arg);
		}
	}

	bool     pending()   { return s_pending; }
	void     clearPending() { s_pending = false; }
	unsigned openCount() { return s_opens; }

	bool hookIntact()
	{
		if (!s_prologueSaved || !game::addr_PlaybackOpen) return false;
		return memcmp(s_prologue, (const void*)game::addr_PlaybackOpen,
		              sizeof(s_prologue)) == 0;
	}

	void install()
	{
		if (!game::addr_PlaybackOpen)
		{
			logger::write("info", "export: Playback::Open unresolved - Export stays stock");
			return;
		}
		if (!memory(game::addr_PlaybackOpen).hook(hkOpen, &origOpen, "PlaybackOpen"))
		{
			logger::write("info",
				"export: !! the Open hook did NOT install - Export will run the game's "
				"own bake. Nothing below this line about RE+ rendering will happen.");
			return;
		}

		memcpy(s_prologue, (const void*)game::addr_PlaybackOpen, sizeof(s_prologue));
		s_prologueSaved = true;

		logger::write("info", "export: hooked at %p (RE+ renderer = %s)",
			(void*)game::addr_PlaybackOpen,
			Config::get().enableRenderer ? "on" : "off");
	}
}
