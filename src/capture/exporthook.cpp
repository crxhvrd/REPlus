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
				static unsigned s_seen = 0;
				if (type >= 0 && type < 32 && !(s_seen & (1u << type)))
				{
					s_seen |= (1u << type);
					logger::write("info", "export: playback Open(type=%d, arg=%u)%s",
						type, arg,
						type == gsig::PLAYBACK_TYPE_BAKE ? "  <- this is Export/bake" : "");
				}
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
						"alone. channel=%s heartbeat=%u. If IgcsConnector.addon64 is present, "
						"it is loaded but never presenting: check ReShade has add-on support, "
						"that IgcsDOF is enabled, and that nothing else (ENB) owns present.",
						fxcapture::available() ? "mapped" : "NOT MAPPED",
						fxcapture::heartbeat());
				}
			}

			origOpen(type, arg);
		}
	}

	bool pending() { return s_pending; }
	void clearPending() { s_pending = false; }

	void install()
	{
		if (!game::addr_PlaybackOpen)
		{
			logger::write("info", "export: Playback::Open unresolved - Export stays stock");
			return;
		}
		memory(game::addr_PlaybackOpen).hook(hkOpen, &origOpen);
		logger::write("info", "export: hooked (RE+ renderer = %s)",
			Config::get().enableRenderer ? "on" : "off");
	}
}
