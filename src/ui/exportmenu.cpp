// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "ui/exportmenu.h"
#include "game/signatures.h"
#include "capture/render.h"
#include "capture/fxcapture.h"
#include "capture/videoout.h"

#include <cstdio>

// =============================================================================
//  The renderer's settings, on the editor's own Export screen
// =============================================================================
//  Everything the renderer does was reachable only by editing Render.ini, which
//  meant the one feature that needs a decision per SHOT - frame rate, shutter,
//  how much motion blur - was the one feature you had to alt-tab to change. And
//  the master switch ships OFF for a good reason (see Config::enableRenderer),
//  so without a row for it the renderer is invisible to anyone who has not read
//  the ini.
//
//  WHY THIS IS NOT menu.cpp ALL OVER AGAIN. The marker menu is built in code,
//  one Scaleform call at a time, so rows there cost an injection point, an input
//  dispatcher, an index-ownership test and a help-text hook. This screen is
//  DATA-DRIVEN: CVideoEditorMenu::Open() parses VideoEditorMenu.XML into
//  ms_MenuArray and every other part of the screen indexes that array - the item
//  loop, the focus walk, left/right, the greying pass, accept. So a row appended
//  to the array IS a row of the menu. We add options and answer three questions
//  about them; the game does the rest.
//
//  What we hook and why:
//
//    Open              inject, immediately after the XML is parsed. It is
//                      re-parsed on every open (Close() Reset()s the array), so
//                      this runs once per editor session.
//    AdjustToggleValue left/right landed on one of our rows.
//    GetToggleString   the VALUE text of one of our rows. Stock returns NULL
//                      for a hash it does not know.
//    CText::Get        the LABEL text. The game looks a row's label up in the
//                      string table by hash and our keys are not in it - and
//                      the same hook supplies the description panel, so the
//                      big right-hand pane explains whichever row is focused.
//    IsItemSelectable  greying. With the renderer ON, the stock Frame Rate and
//                      Bit rate rows drive an encoder that never runs, so they
//                      grey out; with it OFF, ours do. One dword of the game's
//                      own machinery does the drawing, the arrows and the
//                      refusal to step.
//
//  THE ONE REAL CONSTRAINT: atArray::Reset() frees the option buffer through
//  the game's allocator on the next Close(), so the replacement buffer has to
//  come from the game's allocator too. Same rule as pushMenuOption() in
//  menu.cpp, and the same failure if it is broken - a CRT block handed to the
//  game's heap.
// =============================================================================
namespace exportmenu
{
	namespace
	{
		using FnOpen        = void(__fastcall*)();
		using FnAdjust      = void(__fastcall*)(int direction, unsigned* toggleHash);
		using FnToggleStr   = const char*(__fastcall*)(unsigned* toggleHash, unsigned* uniqueId);
		using FnTextGet     = const char*(__fastcall*)(void* self, unsigned hash, const char* dbg);
		using FnSelectable  = bool(__fastcall*)(unsigned menuId, int column, unsigned index);
		using FnAlloc       = void*(__fastcall*)(unsigned long long size, unsigned long long align);
		using FnFree        = void(__fastcall*)(void*);

		FnOpen       origOpen       = nullptr;
		FnAdjust     origAdjust     = nullptr;
		FnToggleStr  origToggleStr  = nullptr;
		FnTextGet    origTextGet    = nullptr;
		FnSelectable origSelectable = nullptr;

		// ---------------------------------------------------------------------
		// The rows, in the order they appear.
		//
		// Renderer first because it is the master switch and everything under it
		// greys out when it is off - at the bottom you would walk past six dead
		// rows to reach the one that brings them back. Same reasoning as the
		// Timecycle row on the marker menu's Scene page.
		// ---------------------------------------------------------------------
		enum Row
		{
			ROW_ENABLE = 0,   // Config::enableRenderer
			ROW_OUTPUT,       // renderMode + renderJpeg
			ROW_FPS,          // renderFps
			// Above the blur rows on purpose: this decides HOW the sub-frames
			// are gathered, and the two below only parameterise it. Reading
			// Shutter before knowing whether it is exact or approximate is
			// reading it in the wrong order.
			ROW_CAPTURE,      // renderCaptureMode
			ROW_SAMPLES,      // renderSamples
			ROW_SHUTTER,      // renderShutter
			ROW_HIGHLIGHT,    // renderHighlight
			ROW_AUDIO,        // renderAudio
			// Last, with Output: it is a fix-it row, not a creative one. Nobody
			// sets it on purpose - you come here because a render came out with
			// red and blue swapped.
			ROW_CHANNELS,     // renderChannelOrder
			ROW_COUNT
		};

		// Our private hashes. One per row, used as the option's cTextId, its
		// ToggleValue AND its UniqueId - the three fields the game reads back -
		// so a single value identifies the row in every hook.
		//
		// A CONTIGUOUS BLOCK on purpose. CText::Get is one of the hottest
		// functions in the game (every HUD string goes through it), and a
		// contiguous block turns "is this one of ours" into a mask and a compare
		// rather than a walk over seven values.
		//
		// The value itself is arbitrary - atStringHash is a Jenkins hash, so
		// there is no string that "means" this. It only has to not collide with
		// a real text key, and the hook is additionally gated on the editor menu
		// having a parsed XML, which is false during ordinary gameplay.
		constexpr unsigned kHashBase = 0x52454D30u;
		constexpr unsigned kHashMask = ~0xFu;

		inline unsigned rowHash(int row) { return kHashBase + (unsigned)row; }

		inline int rowForHash(unsigned h)
		{
			if ((h & kHashMask) != kHashBase) return -1;
			const int r = (int)(h - kHashBase);
			return (r >= 0 && r < ROW_COUNT) ? r : -1;
		}

		// --- value tables -----------------------------------------------------
		// A Scaleform row can only CYCLE, so unlike the marker menu - which has
		// a step row and continuous values - these are fixed lists. The lists are
		// the values people actually pick; a hand-edited ini can hold anything
		// and the row will show it, then snap to the nearest entry when stepped.
		const float kFps[] = { 23.976f, 24.0f, 25.0f, 30.0f, 48.0f, 50.0f,
		                       60.0f, 100.0f, 120.0f };
		// 1 is "Off" - one sample per output frame IS no motion blur.
		const int   kSamples[] = { 1, 2, 4, 8, 16, 24, 32, 48, 64, 96, 128, 192, 256 };

		constexpr int   kShutterSteps   = 20;    // 0.05 .. 1.00
		constexpr float kShutterStep    = 0.05f;
		constexpr int   kHighlightSteps = 21;    // 0.00 .. 1.00
		constexpr float kHighlightStep  = 0.05f;

		template <typename T, size_t N>
		int nearestIndex(const T (&table)[N], T value)
		{
			int best = 0;
			double bestD = 1e30;
			for (size_t i = 0; i < N; ++i)
			{
				const double d = (double)table[i] > (double)value
					? (double)table[i] - (double)value : (double)value - (double)table[i];
				if (d < bestD) { bestD = d; best = (int)i; }
			}
			return best;
		}

		inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

		// --- what we injected, and where --------------------------------------
		struct Injected
		{
			int      menuIndex  = -1;  // index into ms_MenuArray.CVideoEditorMenuItems
			int      linkIndex  = -1;  // ...of the description panel it links to
			int      stockCount = 0;   // options the XML shipped with
			unsigned headerHash = 0;   // description panel's title key
			unsigned bodyHash   = 0;   // ...and its body key
		};
		Injected g_inj;

		// Which menu BuildMenu is drawing right now, or -1.
		//
		// This is the gate on every text substitution, and it exists because the
		// two cheaper gates are both unsound:
		//
		//   * "is the hash one of ours" - a real text key can equal one of our
		//     invented ones, and worse, the export page's HEADER key is a real
		//     key that we substitute deliberately.
		//   * "is the cursor on one of our rows" - ms_iMenuIdForColumn keeps a
		//     stale entry for a column after you navigate back out of the export
		//     screen, so scanning it still answers yes while a different menu is
		//     being built.
		//
		// The second one shipped, and the project menu's Export row - which
		// shares its text key with the export page's header - started drawing as
		// "Audio". The menu index is unambiguous, so it replaces the scan.
		int g_building = -1;

		// --- ms_MenuArray accessors -------------------------------------------
		//
		// Every one of these re-reads the live array rather than caching it. The
		// array is freed and re-parsed on each editor open, and g_inj.menuIndex
		// outlives that, so a cached pointer is a use-after-free waiting for
		// someone to close the editor.
		inline unsigned char* menuItems()
		{
			if (!game::addr_g_VEMenuArray) return nullptr;
			return *(unsigned char**)game::addr_g_VEMenuArray;
		}

		inline int menuItemCount()
		{
			if (!game::addr_g_VEMenuArray) return 0;
			return *(unsigned short*)(game::addr_g_VEMenuArray + gsig::VEMENU_ARRAY_COUNT_OFF);
		}

		// The export menu item, or null when the array is not loaded (i.e. the
		// editor menu is closed) or has been re-parsed under us.
		unsigned char* exportItem()
		{
			if (g_inj.menuIndex < 0) return nullptr;
			unsigned char* items = menuItems();
			if (!items || g_inj.menuIndex >= menuItemCount()) return nullptr;

			unsigned char* item = items + (size_t)g_inj.menuIndex * gsig::VEMENU_ITEM_STRIDE;
			if (*(unsigned*)(item + gsig::VEMI_MENUID) != gsig::VEMENU_EXPORT_SETTINGS)
				return nullptr;
			return item;
		}

		int optionCount(unsigned char* item)
		{
			return *(unsigned short*)(item + gsig::VEMI_OPTIONS + 8);
		}

		unsigned char* optionAt(unsigned char* item, int index)
		{
			unsigned char* data = *(unsigned char**)(item + gsig::VEMI_OPTIONS);
			if (!data || index < 0 || index >= optionCount(item)) return nullptr;
			return data + (size_t)index * gsig::VEMENU_OPT_STRIDE;
		}

		unsigned optionToggle(unsigned char* item, int index)
		{
			unsigned char* opt = optionAt(item, index);
			return opt ? *(unsigned*)(opt + gsig::VEMO_TOGGLEVALUE) : 0u;
		}

		// What an option on the export screen IS, in the only terms this file
		// cares about. ONE function, because the greying hook and the
		// description pane have to agree about which rows are inactive - if they
		// drift you get a live row explaining why it is disabled, or a greyed one
		// with nothing to say for itself.
		enum { FOCUS_OTHER = -1, FOCUS_STOCK_GREYED = -2 };

		int classify(unsigned char* item, int index)
		{
			const unsigned toggle = optionToggle(item, index);

			const int row = rowForHash(toggle);
			if (row >= 0) return row;

			// A STOCK settings row while the renderer owns Export. Frame Rate and
			// Bit rate configure the game's encoder, and the game's encoder is
			// not the thing that will run.
			//
			// Rows with no ToggleValue are actions - Export itself - and are
			// never ours to disable.
			if (toggle != 0 && Config::get().enableRenderer) return FOCUS_STOCK_GREYED;

			return FOCUS_OTHER;
		}

		// Which row the cursor is on: one of ours, a stock row we have greyed,
		// or neither.
		//
		// Only ever consulted while the description panel is being built, and the
		// caller has already established that the panel belongs to our menu - so
		// the column holding the export list is the one whose menu id matches,
		// and a stale entry elsewhere cannot be mistaken for it because g_building
		// has already ruled the whole call out.
		int focusedRow()
		{
			unsigned char* item = exportItem();
			if (!item || !game::addr_g_VEMenuIdForColumn || !game::addr_g_VECurrentItem)
				return FOCUS_OTHER;

			const int* menuFor = (const int*)game::addr_g_VEMenuIdForColumn;
			const int* current = (const int*)game::addr_g_VECurrentItem;

			for (int col = 0; col < 3; ++col)
				if (menuFor[col] == g_inj.menuIndex)
					return classify(item, current[col]);

			return FOCUS_OTHER;
		}

		// --- labels, values and descriptions ----------------------------------
		//
		// UNPREFIXED, so the block reads as part of the same list rather than as
		// an add-on bolted to the bottom of it. These carried an "RE+ " prefix
		// for a while, to tell our Frame Rate apart from the stock one directly
		// above it; the prefix is gone and the greying does that job instead -
		// the stock row is drawn inactive and its description says why.
		//
		// English only, the same call menu.cpp makes for the marker menu: the
		// alternative is shipping a text-table patch per language, and a
		// wrong-language label is worse than an English one. The stock rows
		// beside these are localized, so a Russian or French install will read
		// as mixed - that is the cost, and it is the one the mod already pays
		// everywhere else.
		const char* rowLabel(int row)
		{
			switch (row)
			{
			case ROW_ENABLE:    return "Rockstar Editor+";
			case ROW_OUTPUT:    return "Output";
			case ROW_FPS:       return "Frame Rate";
			case ROW_CAPTURE:   return "Capture Mode";
			case ROW_SAMPLES:   return "Motion Blur";
			case ROW_SHUTTER:   return "Shutter";
			case ROW_HIGHLIGHT: return "Highlight Boost";
			case ROW_AUDIO:     return "Audio";
			case ROW_CHANNELS:  return "Colour Channels";
			default:            return "";
			}
		}

		const char* rowValue(int row)
		{
			static char buf[64];
			const Config& c = Config::get();

			switch (row)
			{
			case ROW_ENABLE:
				return c.enableRenderer ? "On" : "Off";

			case ROW_OUTPUT:
				if (c.wantsVideo())
				{
					// Name the container the encoder will ACTUALLY produce
					// rather than a generic "Video" - a ProRes or lossless setup
					// should not read as MP4.
					//
					// Through videoout, not Config::renderVideoExt: a named
					// RenderVideoPreset carries its own extension and overrides
					// the ini key, so reading the key directly would say mp4 over
					// a render writing video.mkv.
					//
					// snprintf, not sprintf_s: the extension comes from a
					// hand-edited ini, and sprintf_s answers an overflow by
					// invoking the invalid-parameter handler rather than by
					// truncating.
					snprintf(buf, sizeof(buf), "Video (.%s)", videoout::outputExtension());
					return buf;
				}
				return c.renderJpeg ? "JPEG Frames" : "PNG Frames";

			case ROW_FPS:
				// %g so 23.976 keeps its decimals and 60 does not grow any.
				snprintf(buf, sizeof(buf), "%g fps", c.renderFps);
				return buf;

			case ROW_CAPTURE:
				// Named the way Render.ini names them, so the row and the key
				// cannot be mistaken for two different settings.
				return c.renderCaptureMode == 1 ? "Sliding" : "Walking";

			case ROW_SAMPLES:
				if (c.renderSamples <= 1) return "Off";
				snprintf(buf, sizeof(buf), "%d samples", c.renderSamples);
				return buf;

			case ROW_SHUTTER:
				// Both units. The ini key is a fraction of the frame interval;
				// the number a camera operator thinks in is the angle.
				snprintf(buf, sizeof(buf), "%.2f (%.0f deg)",
					c.renderShutter, c.renderShutter * 360.0f);
				return buf;

			case ROW_HIGHLIGHT:
				if (c.renderHighlight <= 0.0f) return "Off";
				snprintf(buf, sizeof(buf), "%.2f", c.renderHighlight);
				return buf;

			case ROW_AUDIO:
				return c.renderAudio ? "On" : "Off";

			case ROW_CHANNELS:
				return c.renderChannelOrder == 1 ? "RGBA"
				     : c.renderChannelOrder == 2 ? "BGRA"
				                                 : "Auto";

			default:
				return "";
			}
		}

		// ---------------------------------------------------------------------
		// The description panel's BODY. Long-form on purpose: this is the one
		// place in the mod with room for it, and every one of these settings
		// costs render time, so "what does this actually buy me" is the question
		// worth answering in advance rather than after a two-hour render.
		//
		// A DESCRIPTION MUST NOT DEPEND ON ITS OWN ROW'S VALUE. Left/right ends
		// in RebuildColumn, which passes bBuildBranches=false and so rebuilds the
		// list column ONLY - the description pane is rebuilt on a focus change
		// (GoToItem asks for GOTO_REBUILD_PREVIEW_PANE) and not on a value step.
		// So text keyed on this row's own value would still be describing the
		// previous one until you moved the cursor, which reads as the toggle
		// having failed.
		//
		// Depending on ANOTHER row's value is fine, and Audio does: changing that
		// value means the cursor is on that other row, and moving back here
		// rebuilds the pane on the way.
		// ---------------------------------------------------------------------
		const char* rowHelp(int row)
		{
			// Branches may read anything EXCEPT this row's own value - see the
			// note above. Config, the capture host, another row: all fine, all
			// unchanged by a press on this one.
			const Config& c = Config::get();

			switch (row)
			{
			case ROW_ENABLE:
			{
				// THE REQUIREMENT IS STATED UNCONDITIONALLY, and the current
				// answer is appended to it.
				//
				// An ASI cannot read the GPU back buffer; a ReShade add-on can,
				// which is the whole reason this dependency exists. It used to be
				// mentioned only when the add-on was already missing - which is
				// the one moment the reader has no idea what they are being asked
				// to install. Saying it up front is the difference between
				// knowing now and finding out when a two-hour render falls back
				// to the stock encoder.
				//
				// Safe to branch on: neither which ReShade is loaded nor whether
				// the add-on is presenting changes because somebody pressed left,
				// so this does not depend on its own row's value. See the note
				// above rowHelp.
				static char b[900];

				const char* status =
					fxcapture::hostState() == fxcapture::HOST_NONE
					  ? "NOT MET: there is no ReShade in this process at all."
					: fxcapture::hostState() == fxcapture::HOST_NO_ADDONS
					  ? "NOT MET: ReShade is loaded, but it is the ORDINARY build - it "
					    "cannot load add-ons at all. Reinstall it and pick the version "
					    "with full add-on support."
					: !fxcapture::addonPresent()
					  ? "NOT MET: ReShade is the right build, but the capture add-on is "
					    "not presenting. Check that IgcsConnector.addon64 is the copy "
					    "bundled with this mod, that it is enabled, and that nothing else "
					    "(an ENB d3d11.dll in front of ReShade) owns presenting."
					: "Met: ReShade with add-on support and the capture add-on are both "
					  "live.";

				snprintf(b, sizeof(b),
					"Whether Export renders through Rockstar Editor+ or through the game's "
					"own encoder.\n\n"
					"On: no watermark, no codec re-compression, and the settings below "
					"apply - true accumulation motion blur with an exact shutter, any frame "
					"rate, and either a video file or an image sequence. The game's own "
					"Frame Rate and Bit rate rows go inactive. Off: Export is exactly as "
					"shipped, which is what you want if you use another export mod - those "
					"hang off the game's bake and will simply never fire while this is on."
					"\n\n"
					"REQUIRES ReShade installed WITH FULL ADD-ON SUPPORT, plus the "
					"IgcsConnector.addon64 that ships with Rockstar Editor+. A plain ASI "
					"cannot read the GPU back buffer; that add-on is what does. Any other "
					"IgcsConnector will NOT work - the bundled one carries the capture "
					"channel this mod talks to.\n\n"
					"%s",
					status);
				return b;
			}

			case ROW_OUTPUT:
				return "What the render produces. The capture is identical either way - "
				       "same seeks, same accumulation blur - this only decides what "
				       "happens to each finished frame.\n\n"
				       "Video hands frames to ffmpeg as they finish and deletes them; the "
				       "codec and container come from Render.ini. PNG and JPEG leave a "
				       "numbered sequence on disk for compositing. A long shot is a lot of "
				       "frames: expect gigabytes.";

			case ROW_FPS:
				return "Output frame rate. Independent of the frame rate the game is "
				       "actually running at - the playhead is seeked to each output "
				       "frame's exact time, so a slideshow and a smooth session produce "
				       "byte-identical footage.\n\n"
				       "23.976 and 24 are the film rates; 25 and 50 are PAL. Higher rates "
				       "cost proportionally more render time.";

			case ROW_CAPTURE:
				// Contrasts BOTH modes rather than describing the selected one,
				// for the state-free reason above - and it is the better shape
				// anyway: on a two-way choice what you want to know is what you
				// would be switching TO.
				return "How the sub-frames of each output frame are gathered.\n\n"
				       "Walking pauses the clip and seeks to each sub-frame's exact "
				       "instant. The shutter is mathematically exact and frame times are "
				       "deterministic, but a seeked frame is COLD: particles do not step, "
				       "and anything with temporal history - TAA, SSR reprojection, the "
				       "ray-traced accumulation on Enhanced - is reset at every sample.\n\n"
				       "Sliding plays the clip in slow motion and exposes consecutive "
				       "presented frames instead, so all of that keeps simulating. Its "
				       "shutter is approximate - samples land where the game presented "
				       "rather than on exact midpoints - and it is usually the FASTER of "
				       "the two: it needs Samples/Shutter frames where Walking needs a "
				       "settle frame per sample as well. At a 360-degree shutter that is "
				       "roughly 3x; at 180 the two are level, and below that Walking wins.";

			case ROW_SAMPLES:
				return "Sub-frames averaged into each output frame. Real accumulation "
				       "blur: correct on rotation, on transparency and on everything a "
				       "screen-space filter cannot see.\n\n"
				       "Off means one capture per frame - fast, perfectly sharp, and it "
				       "strobes on anything moving quickly. This is also the setting that "
				       "costs the render: every sample is a full redraw, so 64 samples is "
				       "roughly 64 times the render time of Off. 32 to 64 is the usual "
				       "answer.";

			case ROW_SHUTTER:
				return "How much of each frame interval the shutter is open, spread across "
				       "the sub-samples above.\n\n"
				       "0.50 is a 180 degree shutter, the film convention, and reads as "
				       "natural motion. 1.00 smears the whole interval and is the dreamier "
				       "look. Below about 0.25 you are back to strobing whatever the "
				       "sample count says. No effect while Motion Blur is Off.";

			case ROW_HIGHLIGHT:
				return "Lifts highlights while the sub-samples are accumulated.\n\n"
				       "A plain average pulls every specular hit down towards the mean, so "
				       "highlights that should streak end up as grey mush - the more "
				       "samples you take, the worse it gets. This keeps the streak bright "
				       "the way film does. It matters most at high sample counts; there is "
				       "no correct value, only a taste one.";

			case ROW_AUDIO:
				// Both outputs, because both work. See rowEnabled() for the
				// belief this used to carry instead.
				if (!c.wantsVideo())
					return "Record the project's sound. It is written as audio.wav "
					       "alongside the frames, and assemble.txt gets the ffmpeg "
					       "arguments that mux it onto whichever conform you pick - so "
					       "the sound survives however you choose to encode later.\n\n"
					       "One Export press does both passes: the project plays through "
					       "once at normal speed to capture audio, then rewinds to the "
					       "first clip and renders the frames. It adds the project's own "
					       "duration to the render, once.";

				return "Record the project's sound and mux it into the video.\n\n"
				       "One Export press does both passes: the project plays through once "
				       "at normal speed to capture audio, then rewinds to the first clip "
				       "and renders the frames without leaving playback. It adds the "
				       "project's own duration to the render - a six minute film costs six "
				       "extra minutes, once.\n\n"
				       "The recording is WASAPI loopback aimed at the game's own process, "
				       "so anything else playing on the machine stays out of it.";

			case ROW_CHANNELS:
				return "Which order the add-on reads the back buffer's colour channels "
				       "in.\n\n"
				       "Auto lets it work that out from the buffer's own format. It is "
				       "KNOWN TO GET IT WRONG on Legacy, FiveM especially, where renders "
				       "come out with the colours wrong while the screen looks fine - so "
				       "if that is what you are seeing, step this to RGBA, and if that does "
				       "not do it, BGRA. It only affects what is written to disk.\n\n"
				       "If neither forced order fixes it, the problem is not channel order "
				       "and this row is not the answer - worth reporting rather than "
				       "cycling, because it means something else is transforming the "
				       "frame.";

			default:
				return "";
			}
		}

		// The description for a STOCK row we have greyed out.
		//
		// Vanilla greys rows all over this UI and leaves the pane to say why, so
		// this is the screen's own idiom rather than something of ours. It is
		// also the answer to the one thing that could read as a bug: with the
		// game's Frame Rate row inactive and ours live two rows below, an English
		// install shows "Frame Rate" twice. Saying which one is driving the
		// export - on the row you are actually looking at - is what turns that
		// from a duplicate into a hand-over.
		//
		// Only the BODY is substituted; the pane's heading is the screen's own
		// ("Export") and stays in the player's language.
		const char* stockGreyedHelp()
		{
			return "Inactive: Export is being rendered by Rockstar Editor+, which does "
			       "not use the game's encoder, so this setting would not reach "
			       "anything.\n\n"
			       "Its own Frame Rate and Output rows are below. Set the Rockstar "
			       "Editor+ row to Off to hand Export back to the game and bring this "
			       "one live again.";
		}

		// --- editing ----------------------------------------------------------
		//
		// Each of these writes Config, persists to Render.ini and re-applies, so
		// a value changed here is the value the next render uses and the value
		// the next session starts with. The renderer copies its settings out of
		// Config once, at applyConfig(), which is why the last step is not
		// optional.
		void step(int row, int delta)
		{
			Config& c = Config::get();

			switch (row)
			{
			case ROW_ENABLE:
				c.enableRenderer = delta > 0;
				c.writeRenderBool("EnableRenderer", c.enableRenderer);
				break;

			case ROW_OUTPUT:
			{
				// One row for two settings, because "video or a sequence" is one
				// decision and PNG-or-JPEG only means anything once you have
				// chosen a sequence.
				const int cur  = c.wantsVideo() ? 0 : (c.renderJpeg ? 2 : 1);
				const int next = clampi(cur + delta, 0, 2);

				c.renderMode = (next == 0) ? Config::RenderMode::Video
				                           : Config::RenderMode::Frames;
				if (next != 0) c.renderJpeg = (next == 2);

				c.writeRenderStr("RenderMode", next == 0 ? "Video" : "Frames");
				c.writeRenderBool("RenderJpeg", c.renderJpeg);
				break;
			}

			case ROW_FPS:
			{
				const int i = clampi(nearestIndex(kFps, c.renderFps) + delta,
				                     0, (int)(sizeof(kFps) / sizeof(*kFps)) - 1);
				c.renderFps = kFps[i];
				c.writeRenderFloat("RenderFps", c.renderFps);
				break;
			}

			case ROW_CAPTURE:
				c.renderCaptureMode = delta > 0 ? 1 : 0;
				// Written as the WORD, not as 0/1: that is what the loader reads
				// (and it accepts Sliding / Slide / Play), and a Render.ini that
				// suddenly said "RenderCaptureMode=1" where it used to say
				// "Sliding" would look like a different key.
				c.writeRenderStr("RenderCaptureMode",
					c.renderCaptureMode == 1 ? "Sliding" : "Walking");
				break;

			case ROW_SAMPLES:
			{
				const int i = clampi(nearestIndex(kSamples, c.renderSamples) + delta,
				                     0, (int)(sizeof(kSamples) / sizeof(*kSamples)) - 1);
				c.renderSamples = kSamples[i];
				c.writeRenderInt("RenderSamples", c.renderSamples);
				break;
			}

			case ROW_SHUTTER:
			{
				// Slot 0 is 0.05, not 0: a zero shutter is a closed shutter, and
				// every sample would land on the same instant.
				const int cur = (int)(c.renderShutter / kShutterStep + 0.5f) - 1;
				const int i   = clampi(cur + delta, 0, kShutterSteps - 1);
				c.renderShutter = (float)(i + 1) * kShutterStep;
				c.writeRenderFloat("RenderShutter", c.renderShutter);
				break;
			}

			case ROW_HIGHLIGHT:
			{
				const int cur = (int)(c.renderHighlight / kHighlightStep + 0.5f);
				const int i   = clampi(cur + delta, 0, kHighlightSteps - 1);
				c.renderHighlight = (float)i * kHighlightStep;
				c.writeRenderFloat("RenderHighlightBoost", c.renderHighlight);
				break;
			}

			case ROW_AUDIO:
				c.renderAudio = delta > 0;
				c.writeRenderBool("RenderAudio", c.renderAudio);
				break;

			case ROW_CHANNELS:
			{
				// Cycles rather than clamps. There are three values, none is a
				// "more" or "less" of the others, and the whole point of the row
				// is trying the other two - so walking off either end should
				// bring you back round rather than parking you at BGRA.
				const int next = (c.renderChannelOrder + (delta > 0 ? 1 : 2)) % 3;
				c.renderChannelOrder = next;
				c.writeRenderInt("RenderChannelOrder", next);
				break;
			}

			default:
				return;
			}

			render::applyConfig();
		}

		// Is one of our rows live right now? Only the renderer being off disables
		// anything, and the master switch itself always stays live - grey that
		// and the page would have no way back.
		//
		// Audio used to be greyed in Frames mode, on the reasoning that there is
		// no container to mux a track into. That was wrong, and wrong about our
		// own renderer: the audio pass writes a standalone audio.wav into the
		// render folder and runs whatever the output mode is, and
		// writeAssembleHelp() already puts the ffmpeg line that muxes it onto
		// whichever conform you pick into assemble.txt. A sequence with a wav
		// beside it is a perfectly good deliverable - arguably the better one,
		// since the sound survives however you choose to encode later.
		bool rowEnabled(int row)
		{
			if (row == ROW_ENABLE) return true;
			return Config::get().enableRenderer;
		}

		// =====================================================================
		//  Injection
		// =====================================================================
		void inject()
		{
			// Reset FIRST. Everything below reads through g_inj.menuIndex, and a
			// stale one from the previous editor session points into an array
			// that has just been freed and re-parsed.
			g_inj = Injected{};

			unsigned char* items = menuItems();
			const int n = menuItemCount();
			if (!items || n <= 0 || n > 512)
			{
				logger::write("info", "exportmenu: no menu data (%d items) - left stock", n);
				return;
			}

			int idx = -1;
			for (int i = 0; i < n; ++i)
			{
				const unsigned id = *(unsigned*)(items + (size_t)i * gsig::VEMENU_ITEM_STRIDE
				                                 + gsig::VEMI_MENUID);
				if (id == gsig::VEMENU_EXPORT_SETTINGS) { idx = i; break; }
			}
			if (idx < 0)
			{
				logger::write("info",
					"exportmenu: EXPORT_SETTINGS not in the menu data - left stock");
				return;
			}

			unsigned char* item = items + (size_t)idx * gsig::VEMENU_ITEM_STRIDE;

			// Refuse anything that is not a plain, code-independent option list.
			// The other column types fill themselves from file views and preview
			// data and do not read Option[] at all.
			if (*(int*)(item + gsig::VEMI_COLUMNTYPE)     != gsig::VEMENU_COL_TYPE_LIST ||
			    *(int*)(item + gsig::VEMI_COLUMNDATATYPE) != gsig::VEMENU_COL_DATA_STANDARD)
			{
				logger::write("info",
					"exportmenu: EXPORT_SETTINGS is column type %d/%d, not a standard list "
					"- left stock",
					*(int*)(item + gsig::VEMI_COLUMNTYPE),
					*(int*)(item + gsig::VEMI_COLUMNDATATYPE));
				return;
			}

			unsigned char** dataPtr  = (unsigned char**)(item + gsig::VEMI_OPTIONS);
			unsigned short* countPtr = (unsigned short*)(item + gsig::VEMI_OPTIONS + 8);
			unsigned short* capPtr   = (unsigned short*)(item + gsig::VEMI_OPTIONS + 10);

			unsigned char* data = *dataPtr;
			const int count = (int)*countPtr;
			if (!data || count <= 0 || count > 64)
			{
				logger::write("info", "exportmenu: option array looks wrong (%d) - left stock",
					count);
				return;
			}

			// Already ours? Open() re-parses every time so this should never
			// fire, but injecting twice would double the rows and orphan a
			// buffer, and that is worth one loop to rule out.
			for (int i = 0; i < count; ++i)
			{
				if (rowForHash(*(unsigned*)(data + (size_t)i * gsig::VEMENU_OPT_STRIDE
				                            + gsig::VEMO_TOGGLEVALUE)) >= 0)
				{
					logger::write("info", "exportmenu: rows already present - skipping");
					return;
				}
			}

			// Where to insert, and which stock option to clone.
			//
			// After the LAST row that carries a ToggleValue, i.e. below Bit rate
			// and above Export. Found by the toggle rather than by row number
			// because the shipped XML is data we do not control: if a game update
			// adds an export setting our block still lands with the settings and
			// still stays above the button that starts the render.
			int insert = -1, templ = -1;
			for (int i = 0; i < count; ++i)
			{
				if (*(unsigned*)(data + (size_t)i * gsig::VEMENU_OPT_STRIDE
				                 + gsig::VEMO_TOGGLEVALUE) != 0)
				{
					insert = i + 1;
					templ  = i;
				}
			}
			if (insert < 0)
			{
				// No settings rows at all - sit above the last row, which is the
				// action one, and clone it for its Block and LinkMenuId.
				insert = count - 1;
				templ  = insert;
				if (insert < 0) return;
			}

			const int total = count + ROW_COUNT;
			const size_t bytes = (size_t)total * gsig::VEMENU_OPT_STRIDE;

			// THE GAME'S ALLOCATOR, not the CRT. atArray::Reset() frees this
			// buffer through rage's own Free() when the editor menu closes.
			auto* buf = (unsigned char*)((FnAlloc)game::addr_MemAlloc)(bytes, 16);
			if (!buf)
			{
				logger::write("info", "exportmenu: allocation of %zu bytes failed - left stock",
					bytes);
				return;
			}

			memcpy(buf, data, (size_t)insert * gsig::VEMENU_OPT_STRIDE);

			for (int r = 0; r < ROW_COUNT; ++r)
			{
				unsigned char* o = buf + (size_t)(insert + r) * gsig::VEMENU_OPT_STRIDE;

				// Clone a stock settings row first, so Block and LinkMenuId are
				// whatever the shipped XML says a row on this screen should
				// carry - LinkMenuId in particular is what keeps the right-hand
				// description panel alive when the cursor is on one of ours.
				memcpy(o, data + (size_t)templ * gsig::VEMENU_OPT_STRIDE,
				       gsig::VEMENU_OPT_STRIDE);

				*(unsigned*)(o + gsig::VEMO_TEXTID)      = rowHash(r);
				*(unsigned*)(o + gsig::VEMO_TOGGLEVALUE) = rowHash(r);
				*(unsigned*)(o + gsig::VEMO_UNIQUEID)    = rowHash(r);

				// Ours are value rows and nothing else. A cloned TriggerAction
				// would fire the stock row's action on accept; a cloned
				// JumpMenuId would navigate away from the screen. Context is
				// cleared because selectability is entirely ours - see
				// hkIsItemSelectable.
				*(unsigned*)(o + gsig::VEMO_TRIGGERACTION)   = 0;
				*(unsigned*)(o + gsig::VEMO_JUMPMENUID)      = 0;
				*(unsigned*)(o + gsig::VEMO_WARNINGTEXT)     = 0;
				*(unsigned*)(o + gsig::VEMO_DEPENDENTACTION) = 0;
				*(unsigned*)(o + gsig::VEMO_CONTEXT)         = 0;
			}

			memcpy(buf + (size_t)(insert + ROW_COUNT) * gsig::VEMENU_OPT_STRIDE,
			       data + (size_t)insert * gsig::VEMENU_OPT_STRIDE,
			       (size_t)(count - insert) * gsig::VEMENU_OPT_STRIDE);

			// Hand the array over BEFORE releasing the old buffer, so the live
			// pointer is never a freed one. Ordered this way round rather than
			// the obvious one because CText::Get is hooked and runs on the render
			// thread: it is gated on g_inj.menuIndex, which is still -1 here, but
			// a window where ms_MenuArray itself points at freed memory is worth
			// closing even when nothing of ours would look through it.
			*dataPtr  = buf;
			*countPtr = (unsigned short)total;
			*capPtr   = (unsigned short)total;

			((FnFree)game::addr_MemFree)(data);

			// The description panel's two text keys, so the CText::Get hook can
			// recognise them and substitute per-row text.
			//
			// Read out of the LINKED menu item's content block rather than
			// guessed: the panel is a menu of its own (COL_TYPE_BASIC_PAGE) and
			// the row's LinkMenuId is what points at it.
			const unsigned link = *(unsigned*)(buf + (size_t)insert * gsig::VEMENU_OPT_STRIDE
			                                   + gsig::VEMO_LINKMENUID);
			if (link)
			{
				for (int i = 0; i < n; ++i)
				{
					unsigned char* it = items + (size_t)i * gsig::VEMENU_ITEM_STRIDE;
					if (*(unsigned*)(it + gsig::VEMI_MENUID) != link) continue;

					// The INDEX as well as the keys. The keys alone cannot say
					// whether the panel is the thing being drawn, and the header
					// key is shared with a row on another menu - see hkTextGet.
					g_inj.linkIndex = i;

					if (auto* content = *(unsigned char**)(it + gsig::VEMI_CONTENT))
					{
						g_inj.headerHash = *(unsigned*)(content + gsig::VEMBP_HEADER);
						g_inj.bodyHash   = *(unsigned*)(content + gsig::VEMBP_BODY);
					}
					break;
				}
			}

			g_inj.stockCount = count;
			// LAST, and deliberately so: it is what every reader gates on, so
			// nothing ever sees a half-built g_inj or a half-swapped array.
			g_inj.menuIndex  = idx;

			logger::write("info",
				"exportmenu: %d rows added to EXPORT_SETTINGS at index %d "
				"(menu %d, %d stock -> %d total%s)",
				(int)ROW_COUNT, insert, idx, count, total,
				g_inj.headerHash ? "" : ", no description panel");

			if (total > gsig::VEMENU_ITEMS_VISIBLE)
				logger::write("info",
					"exportmenu: %d rows is past the %d the column shows at once - "
					"the list will scroll",
					total, gsig::VEMENU_ITEMS_VISIBLE);
		}

		// =====================================================================
		//  Hooks
		// =====================================================================
		void __fastcall hkOpen()
		{
			origOpen();   // parses the XML into ms_MenuArray
			inject();
		}

		// Left/right on a row. The game's own guard is IsItemSelectable on the
		// focused item, which is also what greys the row - so it is repeated
		// here rather than assumed: without it, a greyed row would refuse to
		// redraw (the original bails) while still having changed the value.
		bool focusSelectable()
		{
			if (!game::addr_g_VECurrentColumn || !game::addr_g_VEMenuIdForColumn ||
			    !game::addr_g_VECurrentItem || !game::addr_VEIsItemSelectable)
				return true;

			const int col = *(const int*)game::addr_g_VECurrentColumn;
			if (col < 0 || col >= 3) return false;

			const int menuId = ((const int*)game::addr_g_VEMenuIdForColumn)[col];
			const int item   = ((const int*)game::addr_g_VECurrentItem)[col];

			// The hooked address on purpose: our own greying has to count.
			return ((FnSelectable)game::addr_VEIsItemSelectable)(
				(unsigned)menuId, col, (unsigned)item);
		}

		void __fastcall hkAdjust(int direction, unsigned* toggleHash)
		{
			const int row = toggleHash ? rowForHash(*toggleHash) : -1;

			if (row >= 0 && exportItem() && direction != 0 && focusSelectable())
				step(row, direction > 0 ? 1 : -1);

			// Always. For our rows the original matches nothing in its own hash
			// chain and simply plays the stock tail - SetItemSelected then a
			// column rebuild - which is exactly the refresh we want, and it is
			// the game's own so it cannot drift from what a stock row does.
			origAdjust(direction, toggleHash);
		}

		const char* __fastcall hkGetToggleString(unsigned* toggleHash, unsigned* uniqueId)
		{
			if (toggleHash)
			{
				const int row = rowForHash(*toggleHash);
				if (row >= 0) return rowValue(row);
			}
			return origToggleStr(toggleHash, uniqueId);
		}

		// ---------------------------------------------------------------------
		// The row LABEL, and the description panel.
		//
		// This runs for every string the game looks up, everywhere - so the
		// common path has to be two compares and a return. The gate is
		// menuItemCount(), which is non-zero only while the editor's menu screen
		// has a parsed XML: CVideoEditorMenu::Close() Reset()s the array, and
		// IsOpen() is that same test in the game's own code.
		// ---------------------------------------------------------------------
		const char* __fastcall hkTextGet(void* self, unsigned hash, const char* dbg)
		{
			// g_building is -1 for every text lookup in the game that is not a
			// menu being drawn, which is almost all of them - so this is one
			// compare on the hot path.
			if (g_building >= 0 && g_inj.menuIndex >= 0)
			{
				// Our own row labels, and ONLY while our own menu is the one
				// being drawn. A real text key that happened to equal one of our
				// invented hashes would otherwise be rewritten anywhere in the
				// game's UI.
				if (g_building == g_inj.menuIndex)
				{
					const int row = rowForHash(hash);
					if (row >= 0) return rowLabel(row);
				}

				// The description panel. Its two keys are REAL game keys - and
				// at least one of them is shared with a row on another menu - so
				// this is gated on the panel itself being the menu under
				// construction, not merely on the key matching.
				if (g_building == g_inj.linkIndex && hash != 0 &&
				    (hash == g_inj.headerHash || hash == g_inj.bodyHash))
				{
					const int focus = focusedRow();

					if (focus >= 0)
						return (hash == g_inj.headerHash) ? rowLabel(focus) : rowHelp(focus);

					// A stock row we greyed. Its heading is the screen's own and
					// is left alone - only the body explains the hand-over.
					if (focus == FOCUS_STOCK_GREYED && hash == g_inj.bodyHash)
						return stockGreyedHelp();
				}
			}
			return origTextGet(self, hash, dbg);
		}

		// Records which menu is being drawn, for the text hook above.
		//
		// Saves and restores rather than clearing to -1: BuildMenu recurses to
		// build the linked column, and the basic page's own build must not leave
		// the outer list's build looking like "no menu".
		using FnBuildMenu = int(__fastcall*)(unsigned index, char branches, unsigned startColumn);
		FnBuildMenu origBuildMenu = nullptr;

		int __fastcall hkBuildMenu(unsigned index, char branches, unsigned startColumn)
		{
			const int prev = g_building;
			g_building = (int)index;
			const int r = origBuildMenu(index, branches, startColumn);
			g_building = prev;
			return r;
		}

		// ---------------------------------------------------------------------
		// Greying, through the game's own mechanism.
		//
		// IsItemEnabled is a thin wrapper over this, and BuildMenu's last pass
		// calls it per row to issue SET_ITEMS_GREYED_OUT; AdjustToggleValue
		// calls it before stepping anything. So one answer here draws the row
		// grey AND refuses to change it, with nothing of ours drawing anything.
		//
		// Two directions:
		//   renderer ON  - the stock Frame Rate and Bit rate rows configure an
		//                  encoder that will not run, so they go grey.
		//   renderer OFF - our rows configure a renderer that will not run.
		//
		// Rows stay HIGHLIGHTABLE either way (that is CanHighlightItem, which we
		// leave alone), so you can still read a greyed row's description.
		// ---------------------------------------------------------------------
		bool __fastcall hkIsItemSelectable(unsigned menuId, int column, unsigned index)
		{
			if ((int)menuId == g_inj.menuIndex)
			{
				if (unsigned char* item = exportItem())
				{
					const int what = classify(item, (int)index);

					if (what >= 0)
					{
						if (!rowEnabled(what)) return false;
					}
					else if (what == FOCUS_STOCK_GREYED)
					{
						// A stock settings row while we own the export. Nothing
						// is destroyed: the row still exists, still highlights,
						// still explains itself, and comes back live the moment
						// the renderer is switched off.
						return false;
					}
				}
			}
			return origSelectable(menuId, column, index);
		}
	}

	void install()
	{
		if (!game::exportMenuReady())
		{
			logger::write("info",
				"exportmenu: addresses unavailable - the Export screen stays stock "
				"(the renderer still reads Render.ini)");
			return;
		}

		// Open goes on LAST, and only if the three that answer for a row are in
		// place. It is the one that adds the rows, and a row nobody can label,
		// value or step is worse than no row at all - the other three failing
		// would leave the export screen carrying seven blank, inert entries.
		//
		// The three below are inert until something is injected (each one gates
		// on our own hashes or on g_inj), so installing them and then bailing
		// costs nothing.
		// BuildMenu goes on FIRST. It is what tells the text hook which menu is
		// being drawn, and without it that hook has no sound gate at all - so it
		// is required, not optional.
		const bool build  = memory(game::addr_VEBuildMenu)
			.hook(hkBuildMenu, &origBuildMenu, "VEMenu::BuildMenu");
		const bool text   = memory(game::addr_TextGet)
			.hook(hkTextGet, &origTextGet, "CText::Get");
		const bool value  = memory(game::addr_VEGetToggleString)
			.hook(hkGetToggleString, &origToggleStr, "VEMenu::GetToggleString");
		const bool adjust = memory(game::addr_VEAdjustToggle)
			.hook(hkAdjust, &origAdjust, "VEMenu::AdjustToggle");

		if (!build || !text || !value || !adjust)
		{
			logger::write("info",
				"exportmenu: a required hook did not install (build=%d text=%d value=%d "
				"adjust=%d) - no rows will be added and the Export screen stays stock",
				(int)build, (int)text, (int)value, (int)adjust);
			return;
		}

		// Optional: without it nothing greys, so the stock encoder rows and ours
		// are both live at once. Cosmetic - the renderer still ignores theirs and
		// they still ignore it.
		const bool grey = memory(game::addr_VEIsItemSelectable)
			.hook(hkIsItemSelectable, &origSelectable, "VEMenu::IsItemSelectable");

		if (!memory(game::addr_VEMenuOpen).hook(hkOpen, &origOpen, "VEMenu::Open"))
		{
			logger::write("info", "exportmenu: Open did not hook - Export screen stays stock");
			return;
		}

		logger::write("info", "exportmenu: installed, %d rows (greying=%s)",
			(int)ROW_COUNT, grey ? "yes" : "no");
	}
}
