// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "ui/menu.h"
#include "game/signatures.h"
#include "replay/marker.h"
#include "replay/settings.h"
#include "replay/shake.h"

#include <cstdio>

// =============================================================================
//  Injecting rows into the editor's own camera menu
// =============================================================================
//  PopulateCameraMenu builds the column one ADD_COLUMN_ITEM_WITH_OPTIONS at a
//  time and finishes with ADD_COLUMN_HELP_TEXT. There is no clean seam at the
//  end of the item list to detour, so we watch BeginMethod and slip our rows in
//  the moment the help-text call is about to happen.
//
//  Rows are identified for input by their menu INDEX, never by the option id we
//  stamp on them, which keeps that id inside the range the stock help-text
//  array can be indexed with.
//
//  NUMERIC ROWS. A Scaleform row can only cycle, so a fixed table of steps can
//  never give precise values - you get whatever someone put in the table. So
//  numeric rows here are continuous: left/right adds or subtracts the current
//  STEP, and a Step row in each group picks that increment (0.001 up to 1.0).
//  Coarse-to-fine in two keypresses, and every value in range is reachable.
//
//  Going below a parameter's minimum returns it to "Default" (inherit the
//  global), so unsetting is reachable from the same control.
// =============================================================================
namespace menu
{
	namespace
	{
		using FnPopulate    = void(__fastcall*)(unsigned int);
		using FnMenuInput   = void(__fastcall*)(int);

		// Argument 5 is an int on Legacy but a context OUT-POINTER on Enhanced.
		// Declared pointer-sized for BOTH builds: Enhanced needs all 64 bits, and
		// Legacy's callee reads only the low 32, so widening cannot hurt it.
		// Declaring it `int` would truncate Enhanced's pointer and corrupt the
		// game's own Scaleform call - not just ours, since we hook this.
		using FnBeginMethod = char(__fastcall*)(int, int, const char*, int, void*);
		using FnAddParamStr = void(__fastcall*)(const char*, bool);
		using FnGetMarker   = void* (__fastcall*)();
		using FnUpdateText  = void(__fastcall*)(int, const char*);

		// Legacy shapes.
		using FnEndMethod   = void(__fastcall*)(bool);
		using FnArrayGrow   = void* (__fastcall*)(void*, int);

		// Enhanced shapes. EndMethod consumes the context instead of taking a
		// bool, and there is a conditional release afterwards that Legacy has no
		// equivalent for. atArray::Grow is inlined there, so we allocate with the
		// game's own allocator instead.
		using FnEndMethodEnh = void(__fastcall*)(void*, int, int);
		using FnSfRelease    = void(__fastcall*)(void*, void*, void*);
		using FnGameAlloc    = void* (__fastcall*)(unsigned long long);
		using FnGameFree     = void(__fastcall*)(void*);
		using FnGetMarkerAt  = void* (__fastcall*)(void*, int);

		// Enhanced's caller-owned Scaleform context. Layout read off the game's
		// own stack frame in UpdateItemTextValue: ptr at +0, flag byte at +8,
		// ptr at +0x10.
		struct SfCtx { void* a; unsigned long long flags; void* c; };
		constexpr unsigned long long kSfNeedsRelease = 0x40;

		// ms_activeMenuOptions element and header. Identical on both builds.
		//
		// The middle dword was called `pad` and is nothing of the sort - it is
		// CVideoEditorPlayback::MenuOption::m_editRestrictions, and writing it is
		// how a row is disabled. Confirmed in the retail decompile of
		// PopulateCameraMenu, which stores IS_ENDPOINT there for the Camera Type
		// row on the last marker, and again in UpdateMenuHelpText, which reads
		// +0 as the option and +4 as the restriction at a 0xC stride. See the
		// EDIT_RESTRICTION_* block in signatures.h for what the game then does
		// with it - three separate disable paths, all for one field.
		struct MenuOption { unsigned int id; unsigned int restriction; unsigned int toggleCount; };
		struct MenuArray  { MenuOption* data; unsigned short count; unsigned short cap; };
		static_assert(sizeof(MenuOption) == 12, "MenuOption must stay 12 bytes");

		constexpr int kVideoEditorClass = 9; // SF_BASE_CLASS_VIDEO_EDITOR

		// ---------------------------------------------------------------------
		// One Scaleform method call, per build.
		//
		// Factored out of addRow() because the help-text path needs exactly the
		// same sequence, and the two builds disagree about all of it: Enhanced
		// threads a caller-owned context through Begin/End and has a conditional
		// release afterwards, Legacy has none of that. Keeping one copy is what
		// stops the second caller from quietly getting the Legacy shape on
		// Enhanced.
		// ---------------------------------------------------------------------
		struct SfMethod
		{
			SfCtx ctx{};
			bool  open = false;

			bool begin(const char* method)
			{
				if (!game::addr_BeginMethod || !game::addr_g_MovieId) return false;
				const int movie = *(int*)game::addr_g_MovieId;

				open = ((FnBeginMethod)game::addr_BeginMethod)(
					movie, kVideoEditorClass, method, -1,
					game::isEnhanced() ? (void*)&ctx : (void*)(intptr_t)-1) != 0;
				return open;
			}

			// Strings only. Integer params are INLINED into a global param array
			// on Enhanced, so there is no AddParamInt to call there - which is
			// fine, because every method we issue takes strings.
			void param(const char* s)
			{
				if (open && game::addr_AddParamString)
					((FnAddParamStr)game::addr_AddParamString)(s, false);
			}

			void end()
			{
				if (!open) return;
				open = false;

				if (game::isEnhanced())
				{
					((FnEndMethodEnh)game::addr_EndMethod)(&ctx, 0, 0);
					// Stock code does this after every EndMethod. Skipping it
					// leaks or corrupts Scaleform state.
					if ((ctx.flags & kSfNeedsRelease) && game::addr_ScaleformRelease)
						((FnSfRelease)game::addr_ScaleformRelease)(ctx.a, &ctx, ctx.c);
				}
				else
				{
					((FnEndMethod)game::addr_EndMethod)(false);
				}
			}
		};

		FnPopulate    origPopulate       = nullptr;
		FnPopulate    origPopulateMarker = nullptr;
		FnBeginMethod origBeginMethod = nullptr;
		FnMenuInput   origMenuInput   = nullptr;

		bool g_inCamMenu = false;
		bool g_injecting = false;
		bool g_injected  = false;
		int  g_firstRow  = -1;
		int  g_rowCount  = 0;
		int  g_shakeRow  = -1;   // menu index of the stock Shake row, camera menu
		bool g_ourShakeHere = false; // marker being drawn is on our shake preset
		// Which PAGE of the global block is folded out: 0 closed, 1 curve,
		// 2 limits. It is a pager rather than a single accordion because the
		// editor's Scaleform column draws exactly kColumnCapacity items and
		// silently discards the rest - eight stock rows plus a header plus
		// eight of ours is seventeen, and the seventeenth (Zoom Limit) was
		// simply never drawn. Splitting the block is the only fix that keeps
		// every row reachable AND leaves headroom for the next one.
		enum { PAGE_CLOSED = 0, PAGE_CURVE, PAGE_LIMITS, PAGE_COUNT };
		int  g_page         = PAGE_CLOSED;
		bool g_stockShakeSet = false; // marker has a GAME shake, so the camera
		                              // menu will also draw intensity + speed

		// eMarkerShakeType: NONE, HAND, DRUNK,
		// GROUND_VIBRATION, AIR_TURBULANCE, EXPLOSION. MARKER_SHAKE_MAX is 6 and
		// UpdateShakeType wraps there, so 0..5 is everything the game can hold.
		constexpr int kShakeNone = 0;
		constexpr int kShakeLast = 5;   // EXPLOSION

		using rsettings::Param;
		using rsettings::MarkerSettings;

		// --- logical rows -----------------------------------------------------
		enum Row
		{
			ROW_GROUP = 0, ROW_STEP,
			ROW_PATH, ROW_ROT,               // per-marker enum rows
			// Global enum rows. These live in the TOP-LEVEL marker menu, not the
			// per-marker camera submenu, because none of them is a per-keyframe
			// decision - they are one setting for the whole session.
			// The header is the single row the top-level menu shows when
			// collapsed, at the bottom of the list; the rest fold out under it.
			// A real submenu is not available to us - Camera / DOF / Effects /
			// Audio each have a value in the game's eFOCUS_CONTEXT *and* a
			// populate function compiled in, so a seventh context would land the
			// state machine in a screen it has no way to draw. Expanding in
			// place is the honest version.
			ROW_G_HEADER,
			ROW_COLLISION, ROW_DISTANCE, ROW_ZOOM,
			ROW_G_PATH, ROW_G_ROT, ROW_G_FOV, ROW_G_ALPHA, ROW_G_PROFILE,
			// An ACTION row, not a value row: accept performs it, left/right do
			// nothing. It appears in every shake group because that is where you
			// are standing when you decide the take needs it everywhere, and
			// making you navigate elsewhere to press it is the tedium it exists
			// to remove.
			ROW_APPLY_ALL,
			// Simple-mode enum rows. The numeric ones live below with the rest.
			ROW_SHAKE_MODE, ROW_STOP_STILL,
			ROW_NUM_FIRST,                 // everything below is numeric
			ROW_INTENSITY = ROW_NUM_FIRST, ROW_FREQ_MUL, ROW_VARIATION,
			ROW_SPEED_AMP, ROW_SPEED_FREQ,
			ROW_TENSION, ROW_EASE_IN, ROW_EASE_OUT,
			ROW_SWAY_POS, ROW_SWAY_ROT, ROW_SWAY_FREQ,
			ROW_JIT_POS, ROW_JIT_ROT, ROW_JIT_FREQ,
			ROW_ROUGH, ROW_SEED,
			ROW_AX_LAT, ROW_AX_FWD, ROW_AX_VERT,
			ROW_AX_PITCH, ROW_AX_ROLL, ROW_AX_YAW,
			ROW_LOGICAL_MAX
		};

		// Image-sequence render had a group here. It is INI-only now: the render
		// settings are not per-clip decisions you change mid-session, and the
		// editor's own Export button is already diverted to the renderer, so a
		// menu row to start one was redundant. See RockstarEditorPlus.ini.
		// GRP_CAMERA is gone with it: collision and the distance leash were its
		// only rows, and both are global, so they moved to the top-level marker
		// menu where the rest of the global settings now live.
		// GRP_SHAKE is Simple mode: one intensity, one frequency, the speed
		// coupling and the motion gate. The four groups after it are the full
		// layer model. Both are always reachable - the Shake Mode row inside
		// GRP_SHAKE decides which one actually drives the camera, and hiding
		// the other set would make that switch feel like the settings had been
		// destroyed rather than parked.
		// KEEP EVERY GROUP UNDER ~8 ROWS.
		//
		// The editor's Scaleform column renders 16 items and silently drops the
		// rest - the option array happily accepts more (the game took 17 and
		// showed 16), so an over-long group loses its LAST row with no error
		// anywhere. The stock camera menu already uses 6 of those, or 8 when a
		// game shake is set, which leaves us 8.
		//
		// That is why "Shake" is two pages rather than one long one: what the
		// shake IS, then how it reacts to movement.
		enum Group { GRP_HIDDEN = 0, GRP_SPLINE, GRP_SHAKE, GRP_MOTION,
		             GRP_SWAY, GRP_JITTER, GRP_DETAIL, GRP_AXES, GRP_MAX };

		// Which populate we are currently inside. The two menus are separate
		// screens - you are never in both - so one set of row state serves both,
		// as long as we know which one built it.
		enum MenuKind { MENU_NONE = 0, MENU_CAMERA, MENU_MARKER };
		int g_menuKind  = MENU_NONE;   // populate currently running
		int g_shownKind = MENU_NONE;   // populate that last completed, i.e. what
		                               // is on screen now - drives the refresh

		int s_group = GRP_SPLINE;
		// How many items the editor's Scaleform column actually draws. Measured,
		// not documented: a menu with 17 options rendered exactly 16 and the
		// seventeenth was gone with no error on either side.
		constexpr int kColumnCapacity = 16;

		int s_rows[16];
		int s_shown = 0;

		// Adjustment increment, shared by every numeric row.
		const float kStepVals[] = { 0.001f, 0.005f, 0.01f, 0.05f, 0.1f, 0.5f, 1.0f };
		const char* const kStepText[] = { "0.001", "0.005", "0.01", "0.05", "0.1", "0.5", "1" };
		constexpr int kStepCount = 7;
		int s_step = 2; // 0.01

		inline bool isNumeric(int row) { return row >= ROW_NUM_FIRST; }
		inline bool isGlobalRow(int row)
		{
			return row >= ROW_COLLISION && row <= ROW_G_PROFILE;
		}

		// An ACTION row: accept performs it, and there is no value to show.
		// Drawn with ADD_COLUMN_ITEM, exactly like the stock "Edit Camera" row,
		// so it carries no arrows to suggest otherwise. Its state lives in the
		// help line, which is where a row with no value column has to put it.
		inline bool isActionRow(int row) { return row == ROW_APPLY_ALL; }

		// Two presses to fire, because this overwrites the shake settings on
		// every marker in the project and there is no undo. Armed by the first
		// accept, cleared by literally anything else - see hkMenuInput.
		bool s_applyArmed = false;
		int  s_appliedTo  = -1;   // markers written by the last apply, -1 = none yet

		// Curve shape is exposed as three named choices rather than a raw alpha
		// slider - the values that matter are the three named ones, and a menu
		// row you step with left/right suits a choice better than a float.
		const float kAlphaVals[]  = { 0.0f, 0.5f, 1.0f };
		const char* const kAlphaText[] = { "Uniform", "Centripetal", "Chordal" };
		inline int alphaIndex()
		{
			const float a = Config::get().alpha;
			return a < 0.25f ? 0 : a < 0.75f ? 1 : 2;
		}

		// --- numeric descriptors ----------------------------------------------
		struct Num
		{
			Param       param;
			float       lo, hi;
			int         decimals;
			const char* suffix;
			bool        integer;
		};

		const Num* numFor(int row)
		{
			using namespace rsettings;
			// ORDER IS THE INDEX. Looked up as row - ROW_NUM_FIRST, so this
			// table has to track the enum exactly - a row inserted in one and
			// not the other silently edits the wrong parameter.
			static const Num kNums[] = {
				/* AMPLITUDE  */{ P_INTENSITY, 0.0f, 10.0f,   2, "deg",  false },
				/* FREQUENCY  */{ P_FREQ_MUL,  0.02f, 8.0f,   2, "Hz",   false },
				/* VARIATION  */{ P_VARIATION, 0.0f, 1.0f,    2, "",     false },
				/* SPEED_AMP  */{ P_SPEED_AMP, 0.0f, 3.0f,    2, "x",    false },
				/* SPEED_FREQ */{ P_SPEED_FREQ,0.0f, 2.0f,    2, "x",    false },
				/* TENSION   */ { P_ALPHA,     0.0f, 1.0f,    2, "",     false },
				/* EASE_IN   */ { P_EASE_IN,   0.0f, 0.5f,    2, "",     false },
				/* EASE_OUT  */ { P_EASE_OUT,  0.0f, 0.5f,    2, "",     false },
				/* SWAY_POS  */ { P_SWAY_POS,  0.0f, 1.0f,    3, "m",    false },
				/* SWAY_ROT  */ { P_SWAY_ROT,  0.0f, 10.0f,   2, "deg",  false },
				/* SWAY_FREQ */ { P_SWAY_FREQ, 0.01f, 30.0f,  2, "Hz",   false },
				/* JIT_POS   */ { P_JIT_POS,   0.0f, 1.0f,    3, "m",    false },
				/* JIT_ROT   */ { P_JIT_ROT,   0.0f, 10.0f,   2, "deg",  false },
				/* JIT_FREQ  */ { P_JIT_FREQ,  0.01f, 60.0f,  2, "Hz",   false },
				/* ROUGH     */ { P_OCTAVES,   1.0f, 6.0f,    0, "",     true  },
				/* SEED      */ { P_SEED,      0.0f, 9999.0f, 0, "",     true  },
				/* AX_LAT    */ { P_AX_LAT,    0.0f, 2.0f,    2, "",     false },
				/* AX_FWD    */ { P_AX_FWD,    0.0f, 2.0f,    2, "",     false },
				/* AX_VERT   */ { P_AX_VERT,   0.0f, 2.0f,    2, "",     false },
				/* AX_PITCH  */ { P_AX_PITCH,  0.0f, 2.0f,    2, "",     false },
				/* AX_ROLL   */ { P_AX_ROLL,   0.0f, 2.0f,    2, "",     false },
				/* AX_YAW    */ { P_AX_YAW,    0.0f, 2.0f,    2, "",     false },
			};
			if (!isNumeric(row)) return nullptr;
			const int i = row - ROW_NUM_FIRST;
			return (i >= 0 && i < (int)(sizeof(kNums) / sizeof(*kNums))) ? &kNums[i] : nullptr;
		}

		// The global value a row falls back to when the marker has not set it.
		// Shown greyed-in as the starting point when you first nudge a row.
		float globalValue(Param p)
		{
			const Config& c = Config::get();
			switch (p)
			{
			case rsettings::P_ALPHA:     return c.alpha;
			case rsettings::P_EASE_IN:
			case rsettings::P_EASE_OUT:  return 0.0f;
			case rsettings::P_SWAY_POS:  return c.shake.sway.posAmp;
			case rsettings::P_SWAY_ROT:  return c.shake.sway.rotAmp;
			case rsettings::P_SWAY_FREQ: return c.shake.sway.freq;
			case rsettings::P_JIT_POS:   return c.shake.jitter.posAmp;
			case rsettings::P_JIT_ROT:   return c.shake.jitter.rotAmp;
			case rsettings::P_JIT_FREQ:  return c.shake.jitter.freq;
			case rsettings::P_OCTAVES:   return (float)c.shake.octaves;
			case rsettings::P_SEED:      return (float)c.shake.seed;
			case rsettings::P_AX_LAT:    return c.shake.axisPos[0];
			case rsettings::P_AX_FWD:    return c.shake.axisPos[1];
			case rsettings::P_AX_VERT:   return c.shake.axisPos[2];
			case rsettings::P_AX_PITCH:  return c.shake.axisRot[0];
			case rsettings::P_AX_ROLL:   return c.shake.axisRot[1];
			case rsettings::P_AX_YAW:    return c.shake.axisRot[2];
			case rsettings::P_INTENSITY: return c.shake.amplitude;
			case rsettings::P_FREQ_MUL:  return c.shake.frequency;
			case rsettings::P_VARIATION: return c.shake.variation;
			case rsettings::P_SPEED_AMP: return c.shake.speedAmp;
			case rsettings::P_SPEED_FREQ:return c.shake.speedFreq;
			default:                     return 0.0f;
			}
		}

		void buildRows()
		{
			s_shown = 0;

			// Top-level marker menu: a fixed set of global rows. No group
			// switcher and no adjust-step row - every one of these is a named
			// choice you step with left/right, so neither is needed.
			if (g_menuKind == MENU_MARKER)
			{
				s_rows[s_shown++] = ROW_G_HEADER;
				if (g_page == PAGE_CLOSED) return;   // just the one row

				if (g_page == PAGE_CURVE)
				{
					s_rows[s_shown++] = ROW_G_PATH;
					s_rows[s_shown++] = ROW_G_ROT;
					s_rows[s_shown++] = ROW_G_FOV;
					s_rows[s_shown++] = ROW_G_ALPHA;
					s_rows[s_shown++] = ROW_G_PROFILE;
				}
				else
				{
					s_rows[s_shown++] = ROW_COLLISION;
					s_rows[s_shown++] = ROW_DISTANCE;
					s_rows[s_shown++] = ROW_ZOOM;
				}
				return;
			}

			// The shake groups only mean anything once the marker is actually on
			// our shake preset, so they are unreachable until it is. Without
			// this you could tune sway and jitter on a marker running the
			// game's Drunk shake and see nothing happen.
			if (!g_ourShakeHere && s_group > GRP_SPLINE) s_group = GRP_SPLINE;

			s_rows[s_shown++] = ROW_GROUP;
			switch (s_group)
			{
			case GRP_SPLINE:
				s_rows[s_shown++] = ROW_PATH;
				s_rows[s_shown++] = ROW_TENSION;
				s_rows[s_shown++] = ROW_EASE_IN;
				s_rows[s_shown++] = ROW_EASE_OUT;
				s_rows[s_shown++] = ROW_ROT;
				s_rows[s_shown++] = ROW_STEP;
				break;
			case GRP_SHAKE:
				s_rows[s_shown++] = ROW_SHAKE_MODE;
				s_rows[s_shown++] = ROW_INTENSITY;
				s_rows[s_shown++] = ROW_FREQ_MUL;
				s_rows[s_shown++] = ROW_VARIATION;
				s_rows[s_shown++] = ROW_STEP;
				s_rows[s_shown++] = ROW_APPLY_ALL;
				break;
			case GRP_MOTION:
				s_rows[s_shown++] = ROW_SPEED_AMP;
				s_rows[s_shown++] = ROW_SPEED_FREQ;
				s_rows[s_shown++] = ROW_STOP_STILL;
				s_rows[s_shown++] = ROW_STEP;
				s_rows[s_shown++] = ROW_APPLY_ALL;
				break;
			case GRP_SWAY:
				s_rows[s_shown++] = ROW_SWAY_POS;
				s_rows[s_shown++] = ROW_SWAY_ROT;
				s_rows[s_shown++] = ROW_SWAY_FREQ;
				s_rows[s_shown++] = ROW_STEP;
				s_rows[s_shown++] = ROW_APPLY_ALL;
				break;
			case GRP_JITTER:
				s_rows[s_shown++] = ROW_JIT_POS;
				s_rows[s_shown++] = ROW_JIT_ROT;
				s_rows[s_shown++] = ROW_JIT_FREQ;
				s_rows[s_shown++] = ROW_STEP;
				s_rows[s_shown++] = ROW_APPLY_ALL;
				break;
			case GRP_DETAIL:
				s_rows[s_shown++] = ROW_ROUGH;
				s_rows[s_shown++] = ROW_SEED;
				s_rows[s_shown++] = ROW_APPLY_ALL;
				break;
			case GRP_AXES:
				s_rows[s_shown++] = ROW_AX_LAT;
				s_rows[s_shown++] = ROW_AX_FWD;
				s_rows[s_shown++] = ROW_AX_VERT;
				s_rows[s_shown++] = ROW_AX_PITCH;
				s_rows[s_shown++] = ROW_AX_ROLL;
				s_rows[s_shown++] = ROW_AX_YAW;
				s_rows[s_shown++] = ROW_STEP;
				s_rows[s_shown++] = ROW_APPLY_ALL;
				break;
			default: break; // GRP_HIDDEN
			}
		}

		const char* rowLabel(int row)
		{
			switch (row)
			{
			case ROW_GROUP:     return "Rockstar Editor+";
			case ROW_STEP:      return "Adjust Step";
			case ROW_PATH:      return "Spline Path";
			case ROW_ROT:       return "Spline Rotation";
			case ROW_TENSION:   return "Spline Tension";
			case ROW_EASE_IN:   return "Spline Ease In";
			case ROW_EASE_OUT:  return "Spline Ease Out";
			case ROW_SWAY_POS:  return "Sway Move";
			case ROW_SWAY_ROT:  return "Sway Rotate";
			case ROW_SWAY_FREQ: return "Sway Frequency";
			case ROW_JIT_POS:   return "Jitter Move";
			case ROW_JIT_ROT:   return "Jitter Rotate";
			case ROW_JIT_FREQ:  return "Jitter Frequency";
			case ROW_ROUGH:     return "Shake Roughness";
			case ROW_SEED:      return "Shake Seed";
			case ROW_AX_LAT:    return "Axis Lateral";
			case ROW_AX_FWD:    return "Axis Forward";
			case ROW_AX_VERT:   return "Axis Vertical";
			case ROW_AX_PITCH:  return "Axis Pitch";
			case ROW_AX_ROLL:   return "Axis Roll";
			case ROW_AX_YAW:    return "Axis Yaw";
			case ROW_COLLISION: return "Camera Collision";
			case ROW_DISTANCE:  return "Distance Limit";
			case ROW_ZOOM:      return "Zoom Range";
			case ROW_G_PATH:    return "Spline Path";
			case ROW_G_ROT:     return "Spline Rotation";
			case ROW_G_FOV:     return "Spline Zoom";
			case ROW_G_ALPHA:   return "Curve Shape";
			case ROW_G_PROFILE: return "Speed Profile";
			case ROW_G_HEADER:  return "Rockstar Editor+";
			case ROW_APPLY_ALL: return "Apply Shake to All";
			case ROW_SHAKE_MODE: return "Shake Mode";
			case ROW_INTENSITY:  return "Shake Amplitude";
			case ROW_FREQ_MUL:   return "Shake Frequency";
			case ROW_VARIATION:  return "Shake Variation";
			case ROW_SPEED_AMP:  return "Motion -> Intensity";
			case ROW_SPEED_FREQ: return "Motion -> Speed";
			case ROW_STOP_STILL: return "Stop When Still";
			default:            return "";
		}
		}


		// --- value text --------------------------------------------------------
		const char* valueText(int row, const MarkerSettings& s)
		{
			static char buf[64];

			if (row == ROW_GROUP)
			{
				switch (s_group)
				{
				case GRP_HIDDEN: return "Closed";
				case GRP_SPLINE: return "Spline";
				case GRP_SHAKE:  return "Shake";
				case GRP_MOTION: return "Shake: Motion";
				case GRP_SWAY:   return "Shake: Sway";
				case GRP_JITTER: return "Shake: Jitter";
				case GRP_DETAIL: return "Shake: Detail";
				case GRP_AXES:   return "Shake: Axes";
				default:         return "Closed";
				}
			}
			if (row == ROW_STEP) return kStepText[s_step];

			// Action rows have no value element at all - they are added with
			// ADD_COLUMN_ITEM, like the stock "Edit Camera". Null is the signal
			// for that, both to addRow() and to the in-place refresh. Their
			// armed/done state lives in the help line instead; see rowHelp().
			if (isActionRow(row)) return nullptr;
			// Simple mode reads its default from the ini when the marker has no
			// override, exactly like every other row - so what is displayed is
			// what will actually run, not the sentinel behind it.
			if (row == ROW_SHAKE_MODE)
			{
				const bool simple = s.has(rsettings::P_SIMPLE)
					? s.v[rsettings::P_SIMPLE] > 0.5f
					: Config::get().shake.simpleMode;
				return simple ? "Simple" : "Complex";
			}
			if (row == ROW_STOP_STILL)
			{
				const bool on = s.has(rsettings::P_STOP_STILL)
					? s.v[rsettings::P_STOP_STILL] > 0.5f
					: Config::get().shake.stopWhenStill;
				return on ? "On" : "Off";
			}
			if (row == ROW_PATH)
				return (s.path == rsettings::PathMode::Inherit ? "Default"
				              : s.path == rsettings::PathMode::Stock   ? "ValStockPath" : "ValSpline");
			if (row == ROW_ROT)
				return (s.orient == rsettings::OrientMode::Inherit ? "Default"
				              : s.orient == rsettings::OrientMode::Stock   ? "ValStock" : "ValSpline");
			if (row == ROW_COLLISION)
				return (Config::get().disableCameraCollision ? "Off" : "On");
			if (row == ROW_DISTANCE)
				return (Config::get().unlimitedCameraDistance ? "Unlimited" : "Stock (30m)");
			if (row == ROW_ZOOM)
			{
				// The editor's own zoom readout is 45 / fov, so quote the range
				// in the same units the user sees on screen rather than in FOV.
				const Config& c = Config::get();
				if (!c.uncapZoom) return "Stock (0.45x-4.50x)";
				sprintf_s(buf, "%.2fx-%.2fx", 45.0f / c.zoomMaxFov, 45.0f / c.zoomMinFov);
				return buf;
			}

			// --- global rows (top-level marker menu) ---
			if (row == ROW_G_HEADER)
				return g_page == PAGE_CURVE  ? "Curve"
				     : g_page == PAGE_LIMITS ? "Limits"
				                             : "Closed";
			if (row == ROW_G_PATH)
				return (Config::get().splinePosition ? "On" : "Off");
			if (row == ROW_G_ROT)
				return (Config::get().splineOrientation ? "On" : "Off");
			if (row == ROW_G_FOV)
				return (Config::get().splineFov ? "On" : "Off");
			if (row == ROW_G_ALPHA)
				return kAlphaText[alphaIndex()];
			if (row == ROW_G_PROFILE)
			{
				const Config& c = Config::get();
				return c.naturalPacing      ? "Natural"
				     : c.smoothSpeedProfile ? "Continuous"
				                            : "Per Segment";
			}

			const Num* n = numFor(row);
			if (!n) return "";
			if (!s.has(n->param)) return "Default";

			const float v = s.v[n->param];
			snprintf(buf, sizeof(buf), "%.*f%s", n->decimals, v, n->suffix);
			return buf;
		}

		// A numeric row has no fixed option count; the stock menu passes -1 for
		// its own free-form values (shake intensity does exactly this).
		int optionCount(int row)
		{
			switch (row)
			{
			case ROW_GROUP: return GRP_MAX;
			case ROW_STEP:  return kStepCount;
			case ROW_PATH:
			case ROW_ROT:   return 3;
			case ROW_COLLISION:
			case ROW_DISTANCE:
			case ROW_ZOOM:
			case ROW_G_PATH:
			case ROW_G_ROT:
			case ROW_G_FOV:
			case ROW_SHAKE_MODE:
			case ROW_STOP_STILL: return 2;
			case ROW_G_ALPHA:
			case ROW_G_HEADER:  return PAGE_COUNT;
			case ROW_G_PROFILE: return 3;
			default: return -1;
			}
		}

		// -------------------------------------------------------------------------
		// The help line for one of our rows.
		//
		// Vanilla looks a text key up in sc_EditMarkerMenuHelpText, indexed by the
		// option id, and hands TheText.Get's result to UPDATE_COLUMN_HELP_TEXT. Our
		// rows all carry MARKER_MENU_OPTION_HELP_TEXT, whose entry in that array is
		// the empty string - which is exactly why they showed a blank help line for
		// as long as they existed.
		//
		// UPDATE_COLUMN_HELP_TEXT takes a LOCALIZED STRING rather than a key, so
		// these go straight through with nothing added to the game's string table.
		// English only, deliberately: the alternative is shipping a text-table patch
		// per language, and a wrong-language help line is worse than an English one.
		//
		// A restricted row MUST answer here, because the reason is the whole point
		// of disabling it - see rowRestriction().
		// -------------------------------------------------------------------------
		const char* rowHelp(int row)
		{
			static char buf[192];
			const Config& c = Config::get();

			switch (row)
			{
			case ROW_GROUP:
				return g_ourShakeHere
					? "Which group of Rockstar Editor+ settings this marker shows."
					: "Which group of Rockstar Editor+ settings this marker shows. "
					  "Set the Shake row to Rockstar Editor+ to reach the shake pages.";
			case ROW_STEP:
				return "How far one press of left or right moves a value on this page.";

			case ROW_PATH:
				return "Whether this segment follows the curve. Default uses the ini; "
				       "Stock hands this one segment back to the game.";
			case ROW_ROT:
				return "Whether this segment's camera rotation is splined. "
				       "Default uses the ini.";
			case ROW_TENSION:
				return c.naturalPacing
					? "No effect under Natural pacing: the path is then driven from the "
					  "marker times and has no knot spacing to choose. Set Speed Profile "
					  "to Continuous or Per Segment first."
					: "How tightly the curve is pulled around this marker. "
					  "0 sweeps wide, 1 hugs the marks.";
			case ROW_EASE_IN:
				return "Fraction of this segment spent accelerating out of the marker. "
				       "Overrides the cross-marker speed profile.";
			case ROW_EASE_OUT:
				return "Fraction of this segment spent decelerating into the next marker.";

			case ROW_SHAKE_MODE:
				return "Simple drives one amplitude and one frequency. Complex exposes "
				       "the Sway and Jitter layers directly.";
			case ROW_INTENSITY:
				return "Shake size, in DEGREES of rotation (and 5 cm of movement per "
				       "degree). 2.0 to 2.5 reads as handheld.";
			case ROW_FREQ_MUL:
				return "Shake rate in HERTZ, for the slow layer. 0.35 is a three-second "
				       "wander; 0.20 is slower.";
			case ROW_VARIATION:
				return "How much the shake swells and settles over time. 0 is "
				       "statistically flat second after second.";
			case ROW_SPEED_AMP:
				return "Extra shake amplitude when the camera is moving fast. About 1.0 "
				       "is the single biggest step towards an operator rather than an effect.";
			case ROW_SPEED_FREQ:
				return "Extra shake rate when the camera is moving fast.";
			case ROW_STOP_STILL:
				return "Fade the shake out while the camera is parked.";

			case ROW_SWAY_POS:   return "Slow layer: movement, in metres.";
			case ROW_SWAY_ROT:   return "Slow layer: rotation, in degrees. This is what sells handheld.";
			case ROW_SWAY_FREQ:  return "Slow layer rate, in hertz.";
			case ROW_JIT_POS:    return "Fast layer: movement, in metres. Engine buzz and rattle.";
			case ROW_JIT_ROT:    return "Fast layer: rotation, in degrees.";
			case ROW_JIT_FREQ:   return "Fast layer rate, in hertz. Wants to be well clear of the slow one.";
			case ROW_ROUGH:      return "Octaves of detail within each shake layer.";
			case ROW_SEED:       return "Reroll the shake without changing its character.";

			case ROW_AX_LAT:     return "Weight on side-to-side movement. 0 disables that axis.";
			case ROW_AX_FWD:     return "Weight on forward movement. Reads as a zoom pump, so it ships lower.";
			case ROW_AX_VERT:    return "Weight on vertical movement. 0 disables that axis.";
			case ROW_AX_PITCH:   return "Weight on pitch. 0 disables that axis.";
			case ROW_AX_ROLL:    return "Weight on roll. Moves the corners rather than the centre.";
			case ROW_AX_YAW:     return "Weight on yaw. 0 disables that axis.";

			case ROW_APPLY_ALL:
				if (s_applyArmed)
					return "Press again to copy THIS marker's shake onto every marker in "
					       "the project. There is no undo.";
				if (s_appliedTo >= 0)
				{
					snprintf(buf, sizeof(buf),
						"Copied this marker's shake onto %d marker(s). Curve and easing "
						"were left alone.", s_appliedTo);
					return buf;
				}
				return "Copy this marker's shake settings onto every marker in the "
				       "project. Shake only.";

			// --- global rows, top-level marker menu ---
			case ROW_G_HEADER:
				return "Rockstar Editor+ settings for the whole session. Accept pages "
				       "through Curve and Limits.";
			case ROW_G_PATH:  return "Replace the marker-to-marker camera PATH with a curve.";
			case ROW_G_ROT:   return "Replace the marker-to-marker camera ROTATION with a curve.";
			case ROW_G_FOV:   return "Replace the marker-to-marker ZOOM blend with a curve.";
			case ROW_G_ALPHA:
				return c.naturalPacing
					? "No effect under Natural pacing: a time-driven path has no knot "
					  "spacing to choose. Change Speed Profile first."
					: "How tightly the curve is pulled around each marker. If a shot "
					  "loops where it doubles back, try Uniform.";
			case ROW_G_PROFILE:
				return "Natural paces the shot from the marker times. Continuous fits one "
				       "speed curve across all markers. Per Segment paces each on its own.";
			case ROW_COLLISION:
				return "Off lets the camera pass through geometry, which also stops the "
				       "push-off bending the path away from your markers.";
			case ROW_DISTANCE:
				return "Lift the editor's 30 m leash from the player. Far out you will "
				       "see LOD pop - that is streaming, not this.";
			case ROW_ZOOM:
				return "Widen the editor's zoom range beyond the stock 0.45x-4.50x. "
				       "The ends are set in the ini.";
			default:
				return "";
			}
		}

		// -------------------------------------------------------------------------
		// Is this row disabled, and why?
		//
		// This is the game's own mechanism, and writing one dword buys three
		// behaviours we would otherwise have to fake: UpdateEditMenuState greys the
		// row, SetCurrentItemIntoCorrectState draws it without arrows, and the stock
		// input dispatcher plays ERROR on accept instead of acting. See the
		// EDIT_RESTRICTION_* block in signatures.h.
		//
		// Gated on the help-text hook having resolved. A disabled row whose reason
		// we cannot state falls back to the GAME's restriction string, which
		// describes something else entirely ("first person camera"), and a greyed
		// row with a misleading explanation is worse than a live one that quietly
		// does nothing. So on a build where the hook is missing, nothing is greyed.
		// -------------------------------------------------------------------------
		constexpr unsigned int kOurRestriction = gsig::EDIT_RESTRICTION_CAMERA_BLOCKED;

		unsigned int rowRestriction(int row)
		{
			if (!game::addr_UpdateMenuHelpText) return gsig::EDIT_RESTRICTION_NONE;

			// Curve Shape reaches nothing under Natural pacing - the path is then
			// a Hermite over the marker times, which has no knot spacing. The
			// value still moves and the camera does not, which is exactly the
			// sort of question this field exists to answer.
			if ((row == ROW_TENSION || row == ROW_G_ALPHA) && Config::get().naturalPacing)
				return kOurRestriction;

			return gsig::EDIT_RESTRICTION_NONE;
		}

		// --- editing -----------------------------------------------------------
		void adjust(int row, int delta, MarkerSettings& s)
		{
			if (row == ROW_GROUP)
			{
				s_group += delta;
				if (s_group < 0) s_group = 0;
				if (s_group >= GRP_MAX) s_group = GRP_MAX - 1;
				Config::get().writeInt("MenuGroup", s_group);
				return;
			}
			if (row == ROW_STEP)
			{
				s_step += delta;
				if (s_step < 0) s_step = 0;
				if (s_step >= kStepCount) s_step = kStepCount - 1;
				Config::get().writeInt("MenuStep", s_step);
				return;
			}
			// Per-marker booleans. Written as a real 0/1 rather than left on the
			// inherit sentinel, because a row you can see and step should mean
			// what it says on THIS marker - "it depends what the ini says" is
			// not a state a toggle can usefully display.
			if (row == ROW_SHAKE_MODE)
			{
				s.v[rsettings::P_SIMPLE] = (delta > 0) ? 1.0f : 0.0f;
				return;
			}
			if (row == ROW_STOP_STILL)
			{
				s.v[rsettings::P_STOP_STILL] = (delta > 0) ? 1.0f : 0.0f;
				return;
			}
			if (row == ROW_PATH)
			{
				int p = (int)s.path + delta;
				if (p < 0) p = 0; if (p > 2) p = 2;
				s.path = (rsettings::PathMode)p;
				return;
			}
			if (row == ROW_ROT)
			{
				int o = (int)s.orient + delta;
				if (o < 0) o = 0; if (o > 2) o = 2;
				s.orient = (rsettings::OrientMode)o;
				return;
			}
			if (row == ROW_COLLISION)
			{
				Config& c = Config::get();
				c.disableCameraCollision = delta > 0;
				c.writeBool("DisableCameraCollision", c.disableCameraCollision);
				return;
			}
			if (row == ROW_DISTANCE)
			{
				Config& c = Config::get();
				c.unlimitedCameraDistance = delta > 0;
				c.writeBool("UnlimitedCameraDistance", c.unlimitedCameraDistance);
				return;
			}
			if (row == ROW_ZOOM)
			{
				// Only the on/off switch is here. The two FOV ends stay in the
				// ini: they are a taste setting you pick once, and stepping them
				// from a menu row would need a numeric row per end.
				Config& c = Config::get();
				c.uncapZoom = delta > 0;
				c.writeBool("UncapZoom", c.uncapZoom);
				return;
			}

			// --- global rows. Session settings, so they persist like the rest ---
			if (row == ROW_G_HEADER)
			{
				// Steps Closed -> Curve -> Limits -> Closed, in whichever
				// direction you poke it. Accept advances the same way - see
				// hkMenuInput.
				g_page = (g_page + (delta > 0 ? 1 : PAGE_COUNT - 1)) % PAGE_COUNT;
				Config::get().writeInt("MenuExpanded", g_page);
				return;
			}
			if (row == ROW_G_PATH)
			{
				Config& c = Config::get();
				c.splinePosition = delta > 0;
				c.writeBool("SplinePosition", c.splinePosition);
				return;
			}
			if (row == ROW_G_ROT)
			{
				Config& c = Config::get();
				c.splineOrientation = delta > 0;
				c.writeBool("SplineOrientation", c.splineOrientation);
				return;
			}
			if (row == ROW_G_FOV)
			{
				Config& c = Config::get();
				c.splineFov = delta > 0;
				c.writeBool("SplineFov", c.splineFov);
				return;
			}
			if (row == ROW_G_ALPHA)
			{
				Config& c = Config::get();
				int i = alphaIndex() + delta;
				if (i < 0) i = 0;
				if (i > 2) i = 2;
				c.alpha = kAlphaVals[i];
				c.writeFloat("Alpha", c.alpha);
				return;
			}
			if (row == ROW_G_PROFILE)
			{
				// Per Segment -> Continuous -> Natural, cycling. Three named
				// choices on one row rather than three rows: the column only
				// draws sixteen items and they are mutually exclusive anyway.
				//
				// Natural is not "both the others off" - that combination
				// leaves a uniform curve sampled at normalised segment time,
				// which steps in VELOCITY wherever two segments differ in
				// duration. It owns its own flag for that reason.
				Config& c = Config::get();
				const int cur  = c.naturalPacing ? 2 : (c.smoothSpeedProfile ? 1 : 0);
				const int next = (cur + (delta > 0 ? 1 : 2)) % 3;

				c.smoothSpeedProfile = (next == 1);
				c.naturalPacing      = (next == 2);
				c.writeBool("SmoothSpeedProfile", c.smoothSpeedProfile);
				c.writeBool("NaturalPacing", c.naturalPacing);
				return;
			}

			const Num* n = numFor(row);
			if (!n) return;

			// First nudge starts from whatever the global is, so a row does not
			// jump to zero the moment you touch it.
			float cur = s.has(n->param) ? s.v[n->param] : globalValue(n->param);

			float step = kStepVals[s_step];
			if (n->integer && step < 1.0f) step = 1.0f;

			cur += step * (float)delta;

			// Dropping below the minimum returns the row to Default, so
			// unsetting an override is reachable from the same control.
			if (cur < n->lo - 1e-6f) { s.v[n->param] = -1.0f; return; }
			if (cur > n->hi) cur = n->hi;
			if (n->integer) cur = floorf(cur + 0.5f);

			s.v[n->param] = cur;
		}

		// --- game accessors ---------------------------------------------------

		// Legacy calls GetCurrentEditMarker. Enhanced inlines it - and inlines
		// GetCurrentClip inside it - so there is nothing to call there and this
		// reproduces the expansion. Both builds end at the same place: marker
		// storage vtable slot 0xB0, GetMarker(index), which is identical on both.
		void* currentMarker()
		{
			if (!game::isEnhanced())
				return ((FnGetMarker)game::addr_GetCurrentEditMarker)();

			if (!game::addr_g_EditClipController || !game::addr_g_EditClipIndex ||
			    !game::addr_g_EditMarkerIndex)
				return nullptr;

			const uintptr_t controller = *(uintptr_t*)game::addr_g_EditClipController;
			if (!controller) return nullptr;

			// atArray<CVideoProjectClip*>: data at +0, u16 count at +8.
			auto* clips = *(unsigned char**)(controller + 0x320);
			if (!clips) return nullptr;

			const int idx   = *(int*)game::addr_g_EditClipIndex;
			const int count = *(unsigned short*)(clips + 8);
			if (idx < 0 || idx >= count) return nullptr;

			void* clip = (*(void***)clips)[idx];
			if (!clip) return nullptr;

			auto** vtbl = *(void***)clip;
			return ((FnGetMarkerAt)vtbl[0xB0 / sizeof(void*)])(
				clip, *(int*)game::addr_g_EditMarkerIndex);
		}

		int menuOptionCount() { return *(unsigned short*)(game::addr_g_MenuOptions + 8); }

		void pushMenuOption(int toggleCount, unsigned int restriction)
		{
			if (!game::isEnhanced())
			{
				auto* slot = (unsigned int*)((FnArrayGrow)game::addr_ArrayGrow)(
					(void*)game::addr_g_MenuOptions, 0x10);
				if (!slot) return;
				slot[0] = (unsigned int)gsig::OURS_OPTION_ID;
				slot[1] = restriction;
				slot[2] = (unsigned int)toggleCount;
				return;
			}

			// Enhanced inlines atArray::Grow into every caller, so there is no
			// Grow to call and we reallocate by hand.
			//
			// The allocator MUST be the game's: PopulateCameraMenu frees this
			// buffer at the top of the next rebuild, and a CRT block handed to
			// the game's heap would crash. Growing is not optional either -
			// stock leaves only a handful of spare slots and we add ~10 rows.
			auto* arr = (MenuArray*)game::addr_g_MenuOptions;
			if (arr->count >= arr->cap)
			{
				if (!game::addr_GameAlloc || !game::addr_GameFree) return;

				const unsigned short newCap = (unsigned short)(arr->count + 0x10);
				auto* buf = (MenuOption*)((FnGameAlloc)game::addr_GameAlloc)(
					(unsigned long long)newCap * sizeof(MenuOption));
				if (!buf) return;

				for (unsigned short i = 0; i < arr->count; ++i) buf[i] = arr->data[i];
				if (arr->data) ((FnGameFree)game::addr_GameFree)(arr->data);
				arr->data = buf;
				arr->cap  = newCap;
			}

			arr->data[arr->count].id          = (unsigned int)gsig::OURS_OPTION_ID;
			arr->data[arr->count].restriction = restriction;
			arr->data[arr->count].toggleCount = (unsigned int)toggleCount;
			arr->count += 1;
		}

		// A row. `value` null makes it an ACTION row rather than a value row -
		// the same distinction the stock menu draws with ADD_COLUMN_ITEM for
		// "Edit Camera" and ADD_COLUMN_ITEM_WITH_OPTIONS for everything that
		// carries a value. An action row has no value element and no arrows, so
		// there is nothing for left/right to appear to do.
		//
		// Literal strings, not TheText.Get - our keys do not exist in the game's
		// string table, so it would hand back blanks.
		void addRow(const char* label, const char* value, int toggleCount,
		            unsigned int restriction)
		{
			SfMethod m;
			if (!m.begin(value ? "ADD_COLUMN_ITEM_WITH_OPTIONS" : "ADD_COLUMN_ITEM"))
				return;

			m.param(label);
			if (value) m.param(value);
			m.end();

			pushMenuOption(toggleCount, restriction);
		}

		// Set the help line under the column. Takes a LOCALIZED string, not a
		// text key - which is the whole reason our rows can have help text at
		// all without an entry in the game's string table.
		void emitHelpText(const char* text)
		{
			SfMethod m;
			if (!m.begin("UPDATE_COLUMN_HELP_TEXT")) return;
			m.param(text ? text : "");
			m.end();
		}

		// Does this Scaleform method add a row to the column?
		inline bool isRowMethod(const char* m)
		{
			return strcmp(m, "ADD_COLUMN_ITEM_WITH_OPTIONS") == 0 ||
			       strcmp(m, "ADD_COLUMN_ITEM") == 0;
		}

		// Option id of the stock row most recently pushed into
		// ms_activeMenuOptions, or -1 if none yet.
		//
		// Stock pushes its option entry AFTER EndMethod, so by the time the next
		// row's BeginMethod arrives, the previous row's id is already in the
		// array. That makes this an EXACT anchor, which counting rows is not:
		// the camera menu's rows are conditional (look-at and attach only exist
		// on a free camera, mount type only with an attach target, shake speed
		// only once a shake is set), so the Nth row is not a stable position.
		//
		// Returns -1 when the array is still EMPTY, which is a real option-id
		// value as far as a comparison is concerned - never use -1 as a
		// "no anchor" sentinel against it, or the first row will match.
		int lastStockOptionId()
		{
			if (!game::addr_g_MenuOptions) return -1;
			auto* arr = (const MenuArray*)game::addr_g_MenuOptions;
			if (!arr->data || arr->count == 0) return -1;
			return (int)arr->data[arr->count - 1].id;
		}

		void injectRows()
		{
			g_firstRow = -1;
			g_rowCount = 0;

			g_ourShakeHere = false;

			// The top-level menu's rows are global, so none of the per-marker
			// gating below applies - they show on every marker, including ones
			// with no camera edit at all. A default MarkerSettings is enough:
			// valueText/adjust read Config for these rows and never touch it.
			MarkerSettings s{};

			if (g_menuKind != MENU_MARKER)
			{
				void* marker = currentMarker();
				if (!marker) return;

				// Mirror where the stock blend rows appear.
				if (rmarker::camType(marker) != rmarker::CAM_FREE) return;
				if (rmarker::blendType(marker) == rmarker::BLEND_NONE) return;

				s = rsettings::get(rmarker::timeMs(marker));

				// Our shake presents as a seventh entry on the game's own Shake
				// row. The row stays the game's - we only overwrite the value it
				// displays, so the marker keeps a valid MARKER_SHAKE_NONE and a
				// project opened without this mod is still sound.
				// Only RECORD it here. The relabel itself has to happen after
				// the populate returns - see relabelShakeRow().
				g_ourShakeHere = s.ourShake;
			}

			buildRows();
			g_firstRow = menuOptionCount();
			for (int i = 0; i < s_shown; ++i)
			{
				const int row = s_rows[i];
				addRow(rowLabel(row), valueText(row, s), optionCount(row),
				       rowRestriction(row));
			}
			g_rowCount = menuOptionCount() - g_firstRow;

			// The column renders 16 items and drops the rest without saying so -
			// the array accepts them, the movie just never draws them, so an
			// over-long group loses its last row and nothing anywhere reports
			// it. That cost a "why is Apply Shake to All missing" round trip.
			// If a group ever grows past the budget again, say so here.
			const int total = g_firstRow + g_rowCount;
			if (total > kColumnCapacity)
				logger::write("info",
					"menu: %d rows but the column only draws %d - the last %d will "
					"NOT be visible. Split the group.",
					total, kColumnCapacity, total - kColumnCapacity);

			logger::write("info", "menu: injected kind=%d first=%d count=%d total=%d",
				g_menuKind, g_firstRow, g_rowCount, total);
		}

		// Does the row the cursor is on actually belong to us?
		//
		// The index alone is not enough, and assuming it was is a real bug people
		// hit: g_shownKind and g_firstRow are only updated by OUR two populate
		// hooks, so opening a submenu we do not hook - Audio, for one - leaves
		// both stale. Focus then lands on the same index our header occupied and
		// every ownership test passes, which is why stepping "Microphone Type"
		// paged the Rockstar Editor+ row instead.
		//
		// So ask the live menu array rather than our own memory of it. Both
		// populate paths stamp OURS_OPTION_ID into the option they push, and the
		// game rebuilds that array for every menu, so a row carrying our id is
		// one we put there during THIS populate.
		//
		// The index range is still checked: OURS_OPTION_ID is 0x27, which is also
		// the stock HELP_TEXT id, so the id on its own could match a stock row.
		// Neither test is sufficient alone; together they are.
		bool ownsRow(int menuIndex)
		{
			if (g_rowCount <= 0 || g_firstRow < 0)            return false;
			if (menuIndex < g_firstRow)                       return false;
			if (menuIndex >= g_firstRow + g_rowCount)         return false;
			if (!game::addr_g_MenuOptions)                    return false;

			const int count = menuOptionCount();
			if (menuIndex >= count)                           return false;

			// Entry stride and the id at offset 0 are the same on both builds -
			// see pushMenuOption, which writes through MenuArray on Enhanced and
			// a 0x10-byte slot from ArrayGrow on Legacy.
			auto* arr = (MenuArray*)game::addr_g_MenuOptions;
			if (!arr->data)                                   return false;
			return arr->data[menuIndex].id == (unsigned int)gsig::OURS_OPTION_ID;
		}

		// Is the row under the cursor the STOCK Shake row, right now?
		//
		// g_shakeRow on its own is not enough, for precisely the reason ownsRow()
		// above exists. It is written only by our populate hooks, so a submenu we
		// do NOT hook - Audio, for one - leaves it pointing at whatever index the
		// camera menu last drew the Shake row at. Focus landing there then takes
		// the shake-preset branch in hkMenuInput, and the common case is the bad
		// one: a marker sitting on MARKER_SHAKE_NONE stepped LEFT gets silently
		// switched onto our preset, with the keypress swallowed so the stock row
		// the user was actually on does not step either.
		//
		// Same fix ownsRow got: ask the live option array, which the game rebuilds
		// for every menu. The remembered index stays as the other half of the
		// test - the id proves it is a Shake row, the index proves it is the one
		// this populate drew.
		bool focusIsStockShakeRow(int menuIndex)
		{
			if (menuIndex < 0 || menuIndex != g_shakeRow) return false;
			if (!game::addr_g_MenuOptions)                return false;

			auto* arr = (const MenuArray*)game::addr_g_MenuOptions;
			if (!arr->data || menuIndex >= menuOptionCount()) return false;
			return arr->data[menuIndex].id == (unsigned int)gsig::OPT_CAMERA_SHAKE;
		}

		int logicalAt(int menuIndex)
		{
			if (!ownsRow(menuIndex)) return ROW_GROUP;
			const int i = menuIndex - g_firstRow;
			return (i >= 0 && i < s_shown) ? s_rows[i] : ROW_GROUP;
		}

		// Rewrite the stock Shake row's value to name our preset.
		//
		// This MUST run after the populate has RETURNED, not from inside it.
		// Called mid-populate it is silently discarded - the column is still
		// being built, so the update is lost when it commits, which looked
		// exactly like the preset "not being there" even though the marker state
		// was already correct. The game itself only ever calls
		// UpdateItemTextValue from its input handler, never from a populate;
		// that is the clue it is a post-build operation.
		void relabelShakeRow()
		{
			if (!g_ourShakeHere || g_shakeRow < 0 || !game::addr_UpdateItemText) return;
			((FnUpdateText)game::addr_UpdateItemText)(g_shakeRow, "Rockstar Editor+");
		}

		// --- hooks ------------------------------------------------------------
		void __fastcall hkPopulate(unsigned int focusToRestore)
		{
			const bool outer = !g_inCamMenu;
			if (outer)
			{
				g_inCamMenu = true; g_injected = false;
				g_menuKind  = MENU_CAMERA;

				// Decides where our row goes: with a game shake set the menu
				// draws intensity and speed after Shake, so the anchor has to
				// move past them to keep us directly above Edit Camera.
				void* m = currentMarker();
				g_stockShakeSet = m &&
					rmarker::get<uint8_t>(m, rmarker::OFF_ShakeType) != kShakeNone;
			}
			origPopulate(focusToRestore);
			if (outer)
			{
				g_inCamMenu = false; g_shownKind = g_menuKind; g_menuKind = MENU_NONE;
				relabelShakeRow();   // after the column is committed, not during
			}
		}

		// The top-level marker menu. Same injection machinery, different anchor
		// and a different row set - see buildRows().
		void __fastcall hkPopulateMarker(unsigned int focusToRestore)
		{
			const bool outer = !g_inCamMenu;
			if (outer)
			{
				g_inCamMenu = true; g_injected = false;
				g_menuKind  = MENU_MARKER;
			}
			origPopulateMarker(focusToRestore);
			if (outer) { g_inCamMenu = false; g_shownKind = g_menuKind; g_menuKind = MENU_NONE; }
		}

		// Write the marker's shake type, flagging the property as edited so the
		// editor saves it and the timeline reflects it - the same bit the game's
		// own UpdateShakeType sets.
		void setShakeType(void* marker, int type)
		{
			rmarker::setField<uint8_t>(marker, rmarker::OFF_ShakeType,
			                           (uint8_t)type, rmarker::BIT_ShakeType);
		}

		// -------------------------------------------------------------------
		// Copy this marker's SHAKE settings onto every marker in the project.
		//
		// Shake is per-marker by design - it is a per-shot decision - but the
		// common case is one shake for the whole take, and setting fifteen
		// values on every keyframe by hand is the tedium this removes.
		//
		// SHAKE ONLY. Alpha, ease and the path/orient modes are deliberately
		// not copied: those describe how a segment is CURVED, and flattening
		// them across a project would silently destroy per-corner tuning that
		// has nothing to do with what the user asked for.
		//
		// The inherit sentinels are copied as-is rather than resolved, so this
		// means "make every marker match this one" literally - including the
		// values it leaves to the global ini.
		//
		// Returns the number of markers written.
		int applyShakeToAll(const MarkerSettings& src)
		{
			void* storage = game::markerStorage();
			if (!storage) return 0;

			const int n = rstorage::markerCount(storage);
			int written = 0;

			for (int i = 0; i < n; ++i)
			{
				void* m = rstorage::tryGetMarker(storage, i);
				if (!m) continue;

				const float key = rmarker::timeMs(m);
				MarkerSettings s = rsettings::get(key);

				// Two runs, because the shake parameters are not contiguous:
				// the simple-mode block was appended after the axis weights
				// (append-only, see rsettings::Param) and the spline values sit
				// between them. Copying the whole span would drag alpha and
				// ease across too.
				for (int p = rsettings::P_SWAY_POS; p <= rsettings::P_AX_YAW; ++p)
					s.v[p] = src.v[p];
				for (int p = rsettings::P_SIMPLE; p <= rsettings::P_VARIATION; ++p)
					s.v[p] = src.v[p];
				s.ourShake = src.ourShake;

				// Our shake requires the marker to hold MARKER_SHAKE_NONE, or
				// the game applies one of its own presets on top of ours. The
				// per-marker flag and the marker's own field have to move
				// together - see MarkerSettings::ourShake.
				if (src.ourShake) setShakeType(m, kShakeNone);

				rsettings::set(key, s);
				++written;
			}

			rsettings::save();
			logger::write("info", "menu: applied shake to %d marker(s) (ourShake=%d)",
				written, (int)src.ourShake);
			return written;
		}

		// Rebuild whichever menu is currently on screen. Editing a row refreshes
		// the column the way the game itself does, and it must be the RIGHT
		// column: this used to call PopulateCameraMenu unconditionally, which
		// would rebuild the camera submenu while the user sits in the top-level
		// one.
		void rebuildShownMenu(int focus)
		{
			const bool marker = (g_shownKind == MENU_MARKER);
			const uintptr_t fn = marker
				? game::addr_PopulateMarkerMenu : game::addr_PopulateCameraMenu;
			if (!fn) return;

			// PopulateEditMarkerMenu does NOT clear ms_activeMenuOptions - its
			// caller does. PopulateCameraMenu is the opposite: it opens by
			// freeing the array itself. So calling the marker one directly
			// APPENDS to the previous contents, and every index we then record
			// is offset by however many rows were already there (8 became 18).
			// That desyncs g_firstRow from the real focus index, which silently
			// breaks input, help text and the accordion all at once.
			//
			// Resetting count is enough and needs no allocator: an atArray with
			// a live buffer and count 0 is perfectly valid, stock refills it,
			// and pushMenuOption still grows it if capacity runs out. Freeing
			// here would also mean owning the free on both builds.
			if (marker)
				if (auto* arr = (MenuArray*)game::addr_g_MenuOptions) arr->count = 0;

			((FnPopulate)fn)((unsigned int)focus);
		}

		// -------------------------------------------------------------------------
		// Refresh ONE row in place, which is what the stock menu actually does for
		// a value change.
		//
		// Vanilla only rebuilds a column when the SET of rows changes; an ordinary
		// left/right calls UpdateItemTextValue on the focused row and re-runs the
		// help text, and that is all. Every one of our steps used to re-run the
		// whole populate - freeing and refilling the option array and re-issuing
		// every Scaleform call in the column - and a held key repeats around thirty
		// times a second.
		//
		// Action rows are skipped: they were added with ADD_COLUMN_ITEM and have no
		// value element for UPDATE_LIST_ITEM_ELEMENT to write into. Their state is
		// in the help line, which is refreshed either way.
		// -------------------------------------------------------------------------
		void refreshRow(int menuIndex, int row, const MarkerSettings& s)
		{
			if (const char* value = valueText(row, s))
			{
				if (game::addr_UpdateItemText)
					((FnUpdateText)game::addr_UpdateItemText)(menuIndex, value);
			}
			emitHelpText(rowHelp(row));
		}

		// Does stepping this row change WHICH rows exist, or whether one of them is
		// ENABLED? Only those need the column rebuilt.
		//
		// The greying is applied by the game's own UpdateEditMenuState, which runs
		// at the end of a populate and nowhere else - so a restriction that changes
		// has to go through one, even though no row appeared or disappeared.
		inline bool rowChangesLayout(int row)
		{
			return row == ROW_GROUP        // switches the whole group
			    || row == ROW_G_HEADER     // pages the global block
			    || row == ROW_G_PROFILE;   // decides whether Curve Shape is greyed
		}

		// `b` is pointer-sized so Enhanced's context pointer survives the
		// pass-through untouched - see FnBeginMethod.
		char __fastcall hkBeginMethod(int movie, int cls, const char* method, int a, void* b)
		{
			// Cheap gate first: this runs for every Scaleform call in the game.
			if (g_inCamMenu && !g_injecting && method)
			{
				const bool row  = isRowMethod(method);
				const bool help = strcmp(method, "ADD_COLUMN_HELP_TEXT") == 0;

				if (!g_injected && (row || help))
				{
					// Go in as soon as the anchor row has been pushed - i.e.
					// immediately before the NEXT row is added. The help call is
					// the fallback: on a marker where the anchor never appears we
					// still show up, just at the end as before.
					// Camera submenu: follow the Shake row, so our shake settings
					// sit with the stock shake ones instead of at the end.
					//
					// Top-level menu: no anchor at all, so injection falls
					// through to the help-text point - the game adds that after
					// every row, i.e. the very bottom. The collapsed accordion
					// is one row there and reads better below the stock list.
					//
					// Expressed as a flag rather than a sentinel id on purpose:
					// lastStockOptionId() returns -1 for an EMPTY array, so
					// using -1 to mean "no anchor" matched on the very first row
					// and put us at the top of the menu instead of the bottom.
					// Edit Camera ("Настройка камеры") is always the LAST stock
					// row of the camera submenu, and we want to sit directly
					// above it. Which row precedes it depends on the shake: with
					// a game shake set the menu also draws intensity and speed,
					// so we follow Shake Speed; with none (including our own
					// preset, which parks the marker on None) the Shake row is
					// the last one before it.
					//
					// Anchoring on "the row before Edit Camera" is not possible
					// directly - the option id only lands in the array AFTER its
					// row is built, so there is no way to see one coming.
					const int anchorId = g_stockShakeSet
						? gsig::OPT_SHAKE_SPEED : gsig::OPT_CAMERA_SHAKE;
					const bool useAnchor = (g_menuKind != MENU_MARKER);
					const bool atAnchor  = useAnchor && row &&
					                       lastStockOptionId() == anchorId;

					// Where the stock Shake row landed. Tracked separately from
					// the anchor, which moves down to Shake Speed when a game
					// shake is set - we always need the Shake row itself, to
					// relabel its value and to recognise it under the cursor.
					if (row && g_menuKind == MENU_CAMERA &&
					    lastStockOptionId() == gsig::OPT_CAMERA_SHAKE)
						g_shakeRow = menuOptionCount() - 1;

					if (atAnchor || help)
					{
						g_injected  = true;
						g_injecting = true;   // our own addRow() re-enters this hook
						injectRows();
						g_injecting = false;
					}
				}

			}
			return origBeginMethod(movie, cls, method, a, b);
		}

		// -------------------------------------------------------------------------
		// The help line.
		//
		// CVideoEditorPlayback::UpdateMenuHelpText is the single choke point every
		// focus change goes through - the tail of each Populate*, the focus-change
		// block in the input dispatcher, and the mouse hover path all call it. So
		// hooking it covers every route by construction, the same argument that put
		// the spinner hook on the callee rather than its wrapper.
		//
		// For a stock row we stand aside entirely. For one of ours the original
		// would look up sc_EditMarkerMenuHelpText[MARKER_MENU_OPTION_HELP_TEXT],
		// which is the empty string - so it would blank the line, which is what it
		// has always done.
		// -------------------------------------------------------------------------
		using FnUpdateHelp = void(__fastcall*)(unsigned int);
		FnUpdateHelp origUpdateMenuHelpText = nullptr;

		void __fastcall hkUpdateMenuHelpText(unsigned int menuIndex)
		{
			if (ownsRow((int)menuIndex))
			{
				emitHelpText(rowHelp(logicalAt((int)menuIndex)));
				return;
			}
			origUpdateMenuHelpText(menuIndex);
		}

		// Re-run the help line for whatever is focused now. Used after we change a
		// row's own state without moving focus, which is the one case the game
		// never has to handle for itself.
		void refreshHelpForFocus()
		{
			if (!game::addr_g_MenuFocusIndex) return;
			const int focus = *(int*)game::addr_g_MenuFocusIndex;
			if (ownsRow(focus)) emitHelpText(rowHelp(logicalAt(focus)));
		}

		void __fastcall hkMenuInput(int navCode)
		{
			const bool isToggle = (navCode == gsig::NAV_RIGHT || navCode == gsig::NAV_LEFT);
			const int  focus    = game::addr_g_MenuFocusIndex
				? *(int*)game::addr_g_MenuFocusIndex : -1;
			// ownsRow, not an index range: see its comment. A stale g_firstRow
			// from a menu we do not hook otherwise makes us claim a stock row.
			const bool ours     = ownsRow(focus);

			// Accept on "Apply Shake to All". Two presses: the first arms, the
			// second writes every marker. Checked before the header case below
			// because that one is MENU_MARKER and this is MENU_CAMERA, so they
			// cannot both match - the ordering is for readability, not
			// correctness.
			if (ours && g_shownKind == MENU_CAMERA &&
			    logicalAt(focus) == ROW_APPLY_ALL)
			{
				if (navCode == gsig::NAV_ACCEPT)
				{
					if (!s_applyArmed)
					{
						s_applyArmed = true;
						s_appliedTo  = -1;
					}
					else
					{
						s_applyArmed = false;
						void* marker = currentMarker();
						s_appliedTo = marker
							? applyShakeToAll(rsettings::get(rmarker::timeMs(marker)))
							: 0;
					}
					// An action row has no value element - its state IS the help
					// line, so that is the only thing to redraw. No rebuild:
					// nothing about the column changed.
					refreshHelpForFocus();
					return;   // swallow: the row is ours, stock has no such row
				}
				// Left/right on an action row do nothing, but they must still be
				// swallowed - letting them through would step whatever stock row
				// happens to sit at this index.
				if (isToggle)
				{
					if (s_applyArmed) { s_applyArmed = false; refreshHelpForFocus(); }
					return;
				}
			}

			// Any other input disarms. A confirmation that survives navigating
			// away is a confirmation you will eventually trigger by accident.
			if (s_applyArmed && logicalAt(focus) != ROW_APPLY_ALL)
				s_applyArmed = false;

			// Accept on the header folds it out, matching how the stock
			// submenu rows above it open. Left/right work too - the row is
			// drawn with arrows, so it should respond to them.
			if (navCode == gsig::NAV_ACCEPT && g_shownKind == MENU_MARKER &&
			    ownsRow(focus) && focus == g_firstRow)
			{
				g_page = (g_page + 1) % PAGE_COUNT;
				Config::get().writeInt("MenuExpanded", g_page);
				rebuildShownMenu(focus);
				return;   // swallow: stock must not also act on the accept
			}

			// The stock Shake row gains a seventh choice after Explosion:
			// "Rockstar Editor+". It is our flag rather than a marker value -
			// see MarkerSettings::ourShake for why a real 7th m_shakeType is not
			// an option. Stepping onto it parks the marker on MARKER_SHAKE_NONE
			// so the game applies no shake of its own and ours takes over.
			if (isToggle && g_shownKind == MENU_CAMERA && focusIsStockShakeRow(focus))
			{
				void* marker = currentMarker();
				if (marker)
				{
					const int delta = (navCode == gsig::NAV_RIGHT) ? 1 : -1;
					const float key = rmarker::timeMs(marker);
					MarkerSettings s = rsettings::get(key);
					const int type = rmarker::get<uint8_t>(marker, rmarker::OFF_ShakeType);

					bool handled = false;
					if (s.ourShake)
					{
						// Leaving ours: forward to None, back to Explosion.
						s.ourShake = false;
						setShakeType(marker, delta > 0 ? kShakeNone : kShakeLast);
						handled = true;
					}
					else if (delta > 0 && type == kShakeLast)
					{
						s.ourShake = true;                 // Explosion -> ours
						setShakeType(marker, kShakeNone);
						handled = true;
					}
					else if (delta < 0 && type == kShakeNone)
					{
						s.ourShake = true;                 // None -> back to ours
						handled = true;
					}

					if (handled)
					{
						rsettings::set(key, s);
						// No save() here - set() dirties the store and
						// rsettings::tick() flushes once the user stops. This ran
						// on every input repeat and rewrote the whole side-car.
						rebuildShownMenu(focus);
						return;   // swallow: stock must not also step the type
					}
				}
				// Anything else is an ordinary stock step - fall through.
			}

			if (isToggle && ours)
			{
				const int row   = logicalAt(focus);
				const int delta = (navCode == gsig::NAV_RIGHT) ? 1 : -1;

				// A disabled row ignores left and right, exactly as a restricted
				// stock row does - the game's dispatcher gates its whole toggle
				// switch on IsEnabled(). It cannot do that for us, because our
				// rows never reach it: we swallow them here first. So the same
				// test has to be made here, or a row drawn greyed and without
				// arrows would still quietly change its value.
				if (rowRestriction(row) != gsig::EDIT_RESTRICTION_NONE)
					return;   // swallow: still our row, just not a live one

				// Global rows and the two UI rows write config or scratch state,
				// so they need no marker at all. Keeping them off the per-marker
				// path also means they still work on a marker we would otherwise
				// skip.
				if (isGlobalRow(row) || row == ROW_GROUP || row == ROW_STEP)
				{
					MarkerSettings ignored{};
					adjust(row, delta, ignored);

					if (rowChangesLayout(row)) rebuildShownMenu(focus);
					else                       refreshRow(focus, row, ignored);
					return;
				}

				void* marker = currentMarker();
				if (marker)
				{
					const float key = rmarker::timeMs(marker);
					MarkerSettings s = rsettings::get(key);

					adjust(row, delta, s);
					rsettings::set(key, s);   // flushed by rsettings::tick()

					// One row changed, so redraw one row - which is what the
					// stock menu does for a value step. Rebuilding the whole
					// column was re-issuing every Scaleform call in it on every
					// input repeat, around thirty times a second on a held key.
					if (rowChangesLayout(row)) rebuildShownMenu(focus);
					else                       refreshRow(focus, row, s);
				}
				return; // swallow - never let our rows reach the stock switch
			}

			origMenuInput(navCode);
		}
	}

	void install()
	{
		if (!game::menuReady())
		{
			logger::write("info", "menu: addresses unavailable - no editor rows (ini still works)");
			return;
		}

		s_group = Config::get().menuGroup;
		// Fall back to hidden, not spline. An ini written before the render and
		// camera groups were removed can name a group that no longer exists, and
		// silently reopening on some other group is worse than starting closed.
		if (s_group < 0 || s_group >= GRP_MAX) s_group = GRP_HIDDEN;
		s_step = Config::get().menuStep;
		if (s_step < 0 || s_step >= kStepCount) s_step = 2;
		g_page = Config::get().menuExpanded;
		if (g_page < 0 || g_page >= PAGE_COUNT) g_page = PAGE_CLOSED;

		memory(game::addr_PopulateCameraMenu).hook(hkPopulate,    &origPopulate);
		memory(game::addr_BeginMethod).hook(hkBeginMethod, &origBeginMethod);
		memory(game::addr_MenuInput).hook(hkMenuInput,   &origMenuInput);

		// Optional: without it we simply lose the global rows. The camera
		// submenu does not depend on this one resolving.
		if (game::addr_PopulateMarkerMenu)
			memory(game::addr_PopulateMarkerMenu).hook(hkPopulateMarker, &origPopulateMarker);

		// Optional: without it our rows keep the blank help line they had
		// before, and rowRestriction() stops greying anything - see the note
		// there on why a reason we cannot state is worse than no greying.
		if (game::addr_UpdateMenuHelpText)
			memory(game::addr_UpdateMenuHelpText).hook(hkUpdateMenuHelpText,
			                                          &origUpdateMenuHelpText);

		// UpdateItemTextValue does two jobs now: it rewrites the stock Shake
		// row's value for our preset, and it is the in-place row refresh that
		// replaced rebuilding the whole column on every left/right.
		logger::write("info", "menu: rows injected (itemText=%s, help=%s, global=%s)",
			game::addr_UpdateItemText      ? "yes" : "no",
			game::addr_UpdateMenuHelpText  ? "yes" : "no",
			game::addr_PopulateMarkerMenu  ? "yes" : "no");
	}
}





