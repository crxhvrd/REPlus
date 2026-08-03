// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "replay/limits.h"
#include "replay/marker.h"   // rdirector offsets for the metadata override
#include "game/signatures.h"

// =============================================================================
//  Removing the free camera's leash
// =============================================================================
//  camReplayDirector::GetMaxDistanceAllowedFromPlayer() is the single source of
//  truth for how far the editor free camera may travel. Everything downstream
//  reads it:
//
//    camReplayFreeCamera::IsPositionOutsidePlayerLimits()  — the test itself
//    camReplayFreeCamera::SetToSafePosition()              — yanks you back in
//    camReplayDirector::Update()                           — swaps to the
//                                                            recorded camera
//    EDIT_WARNING_OUT_OF_RANGE                             — the UI toast
//    the boundary-sphere renderer
//
//  So one return value governs the whole limitation, and overriding it is far
//  safer than neutering each consumer.
//
//  Note the metadata route exists too — m_MaxCameraDistanceFromPlayer lives at
//  metadata+0x38 and is editable in cameras.ymt — but that field's own range
//  limit stops well short of what we want, and going through the hook keeps
//  this independent of the ymt.
// =============================================================================
namespace limits
{
	namespace
	{
		using FnGetMaxDist = float(__fastcall*)(void*, char);
		FnGetMaxDist origGetMaxDist = nullptr;

		float __fastcall hkGetMaxDist(void* self, char considerEditMode)
		{
			const Config& cfg = Config::get();
			if (cfg.unlimitedCameraDistance)
				return cfg.maxCameraDistance;

			return origGetMaxDist(self, considerEditMode);
		}

		// Both collision entry points take the DESIRED position in their out
		// parameter and overwrite it with a constrained one. Returning 0 without
		// writing leaves the desired position untouched, so the camera passes
		// straight through — no need to fake a "no hit" shape-test result.
		using FnUpdateCollision = char(__fastcall*)(void*, float*, float*);
		using FnComputeSafePos  = char(__fastcall*)(void*, void*, float*);

		FnUpdateCollision origUpdateCollision = nullptr;
		FnComputeSafePos  origComputeSafePos  = nullptr;

		char __fastcall hkUpdateCollision(void* self, float* initialPos, float* cameraPos)
		{
			if (Config::get().disableCameraCollision) return 0;
			return origUpdateCollision(self, initialPos, cameraPos);
		}

		char __fastcall hkComputeSafePosition(void* self, void* entity, float* cameraPos)
		{
			if (Config::get().disableCameraCollision) return 0;
			return origComputeSafePos(self, entity, cameraPos);
		}

		// =====================================================================
		//  How long a clip can be
		// =====================================================================
		//  Not a duration limit - a MEMORY one, which is why the answer changes
		//  with the scene. CReplayMgrInternal records into a ring of fixed 4 MB
		//  blocks and a busy street fills them in seconds where an empty road
		//  lasts a minute. Stock is 7 blocks, 28 MB.
		//
		//  SetupReplayBuffer is the whole patch: it compares the request against
		//  what is allocated, and frees and reallocates itself when the request
		//  grows. So we raise one argument and the engine does the rest - no
		//  allocator of ours, no startup timing to get right, and because it
		//  only grows on demand it sizes the buffer once and never churns.
		//
		//  See signatures.h for why 36 is a hard ceiling and why this is viable
		//  on PC at all.
		// =====================================================================
		using FnSetupReplayBuffer = char(__fastcall*)(unsigned short, unsigned short);
		FnSetupReplayBuffer origSetupReplayBuffer = nullptr;

		// The LIVE block counts, for the one line we log on a successful setup.
		//
		// Worth reporting rather than assuming: the configured pair is what we
		// asked for, and the engine moves blocks between the normal and temp
		// rings while a save is in flight, so the two can differ. They did
		// not in testing, but "the ring is what we set it to" is the whole
		// premise of the feature and it costs one read to state it as fact.
		void appendRing(char* out, int cap)
		{
			if (!game::addr_g_ReplayBufferInfo) { out[0] = '\0'; return; }
			const auto* b = (const unsigned char*)game::addr_g_ReplayBufferInfo;
			auto u16at = [b](int off) { return *(const unsigned short*)(b + off); };

			snprintf(out, cap, ", ring %u+%u of %u allocated",
				u16at(gsig::BUFINFO_CURRENTBLOCKS), u16at(gsig::BUFINFO_CURRENTTEMP),
				u16at(gsig::BUFINFO_ALLOCATED_OFF));
		}

		char __fastcall hkSetupReplayBuffer(unsigned short normal, unsigned short temp)
		{
			const int want = Config::get().replayBlocks;

			// ONLY the recording setup. Loading a clip calls this as
			// (header.PhysicalBlockCount, 0) and teardown as (0, 0), so a
			// non-zero temp count is what marks the recording path. Inflating a
			// LOAD would tell the buffer it holds more blocks than the clip ever
			// wrote, and playback would walk blocks nothing filled.
			if (temp == 0) return origSetupReplayBuffer(normal, temp);

			// `normal` usually already carries our value, because the global we
			// write at install is what this call site loads. Raising here as
			// well only matters when the write was overwritten by a settings
			// apply between the two.
			unsigned short use = normal;
			if (want > (int)use) use = (unsigned short)want;

			// Lift the clamp Enable() just applied.
			//
			// TotalNumberOfReplayBlocks is capped at 42 a few instructions
			// before this call, and IT is what bounds how
			// much of the buffer a recording actually uses - not the count we pass
			// below. Left alone, a 70-block buffer records 42 blocks' worth and
			// stops: 24 seconds where the allocation was good for 35.
			//
			// Written here rather than patched because the clamp has already run
			// by the time we are called, and nothing reads the value until after.
			//
			// BOTH globals, and that is not belt-and-braces. The engine
			// recomputes the total as
			//     TotalNumberOfReplayBlocks = NumberOfReplayBlocks + NumberOfTempReplayBlocks
			// so leaving NumberOfReplayBlocks at the settings value (30, which is
			// what arrives as `normal`) means the next recompute puts the total
			// straight back to 36 and undoes the line above. Our install-time
			// write to this global is overwritten by the settings apply long
			// before recording starts, which is why it has to happen again here.
			if (game::addr_g_ReplayBlocks)
				*(unsigned short*)game::addr_g_ReplayBlocks = use;
			if (game::addr_g_ReplayTotalBlocks)
				*(unsigned short*)game::addr_g_ReplayTotalBlocks =
					(unsigned short)(use + temp);

			// Descending retry.
			//
			// Above the engine's own 36 this is an experiment, and the failure
			// mode is unforgiving: a failed SetupReplayBuffer has ALREADY called
			// FreeMemory, so simply returning false leaves the game with no
			// recording buffer at all - no clips, no error the user can read.
			//
			// So walk the request down until something takes, ending at the
			// count the game asked for. An over-ambitious ReplayBlocks then
			// costs a shorter clip and a log line, never a broken recorder.
			unsigned short attempt = use;
			for (;;)
			{
				if (const char ok = origSetupReplayBuffer(attempt, temp))
				{
					static int s_reported = 0;
					if (s_reported != (int)attempt)
					{
						s_reported = attempt;
						char ring[64];
						appendRing(ring, sizeof(ring));
						logger::write("info",
							"limits: recording buffer = %u blocks (%d MB, +%u temp)%s%s",
							attempt,
							((int)attempt * gsig::REPLAY_BLOCK_BYTES) / (1024 * 1024), temp,
							ring,
							attempt > gsig::REPLAY_BLOCKS_SAFE_MAX
								? "  [above the game's own 30]" : "");
					}
					return ok;
				}

				if (attempt <= normal)
				{
					logger::write("info",
						"limits: !! SetupReplayBuffer failed even at the game's own %u "
						"blocks - recording is broken, and this is not our doing.", normal);
					return 0;
				}

				const unsigned short next = (unsigned short)(attempt / 2) > normal
					? (unsigned short)(attempt / 2) : normal;
				logger::write("info",
					"limits: could not allocate %u replay blocks (%d MB) - retrying at %u. "
					"Lower ReplayBlocks in the ini to stop this happening every time.",
					attempt, ((int)attempt * gsig::REPLAY_BLOCK_BYTES) / (1024 * 1024), next);
				attempt = next;
			}
		}

		// Report every profanity check as already passed. The situation this
		// exists for: the check cannot complete offline, and the editor treats
		// an unavailable filter as a hard failure rather than skipping it.
		using FnProfanityStatus = int(__fastcall*)(const void*);
		FnProfanityStatus origProfanityStatus = nullptr;

		int __fastcall hkProfanityStatus(const void* token)
		{
			if (Config::get().bypassProfanityFilter)
				return gsig::PROFANITY_RESULT_STRING_OK;
			return origProfanityStatus(token);
		}

		// ---------------------------------------------------------------------
		//  Editing a clip that was recorded in first person
		// ---------------------------------------------------------------------
		//  Record anything from the first-person view and the editor will not let
		//  you put a free camera on it - the whole Camera submenu is greyed, so
		//  the clip is stuck with the recorded view forever.
		//
		//  It is one virtual: IsMarkerControlEditable(control). For the camera
		//  controls it answers FIRST_PERSON / CUTSCENE / CAMERA_BLOCKED depending
		//  on flags baked into the recording, and any non-zero answer disables the
		//  row. Answering NONE for the camera controls is the whole unlock -
		//  nothing in the camera code consults it, so once the menu lets a marker
		//  be set to a free camera, the camera simply runs it.
		//
		//  Only the CAMERA controls are overridden. The same virtual answers for
		//  volume and DOF rows, and those restrictions are about a marker having
		//  no audio or DOF being off in the graphics options - real conditions
		//  where the row genuinely has nothing to edit. Blanket-clearing would
		//  turn those into rows that accept input and do nothing.
		using FnIsEditable = unsigned(__fastcall*)(void*, int);
		FnIsEditable origIsEditable = nullptr;

		unsigned __fastcall hkIsEditable(void* storage, int control)
		{
			const unsigned stock = origIsEditable(storage, control);

			if (!Config::get().unlockCameraRestrictions) return stock;
			if (control != gsig::MARKER_CONTROL_CAMERA &&
			    control != gsig::MARKER_CONTROL_CAMERA_TYPE) return stock;

			// IS_ANCHOR / IS_ENDPOINT / NEEDS_BLEND are structural - the first and
			// last markers of a clip really cannot take an arbitrary camera, and
			// clearing those produces a marker the game cannot play back. Only the
			// three RECORDING-derived reasons are lifted.
			if (stock != gsig::EDIT_RESTRICTION_FIRST_PERSON &&
			    stock != gsig::EDIT_RESTRICTION_CUTSCENE &&
			    stock != gsig::EDIT_RESTRICTION_CAMERA_BLOCKED) return stock;

			static unsigned s_reported = ~0u;
			if (s_reported != stock)
			{
				s_reported = stock;
				static const char* const kWhy[] = {
					"none", "first person", "cutscene", "camera blocked" };
				logger::write("info", "limits: camera unlocked on a '%s' clip",
					kWhy[stock < 4 ? stock : 0]);
			}
			return gsig::EDIT_RESTRICTION_NONE;
		}

		// The SECOND lock, and the one that actually froze the camera.
		//
		// Unlocking the menu lets a marker be set to a free camera, but the
		// director never sees that: it asks whether the camera-disabled flag is
		// set on the frame packet being played, and returns the recorded camera
		// when it is. Every frame recorded in first person carries that flag.
		//
		// The first attempt here cleared the flag on the clip's aggregate global -
		// and the log proved it landed, 0x9 -> 0x8, with no effect whatsoever. The
		// aggregate is what the MENU reads; the director reads the per-frame packet
		// instead. Two different pieces of storage for the same-named flag.
		//
		// Answering false for that one bit is the fix. Nothing else is touched:
		// every caller passes a single flag, and bit 0 has exactly one - the camera
		// selection. The in-vehicle, aircraft-shadow and first-person-VFX queries
		// that share this function are passed straight through.
		using FnIsPlaybackFlagSet = char(__fastcall*)(unsigned);
		FnIsPlaybackFlagSet origIsPlaybackFlagSet = nullptr;

		char __fastcall hkIsPlaybackFlagSet(unsigned flag)
		{
			if (flag == gsig::CLIPFLAG_DISABLE_CAMERA_MOVEMENT &&
			    Config::get().unlockCameraRestrictions)
			{
				static bool s_said = false;
				if (!s_said)
				{
					s_said = true;
					logger::write("info",
						"limits: camera-disabled frame flag suppressed - the director "
						"will honour the marker's camera type");
				}
				return 0;
			}
			return origIsPlaybackFlagSet(flag);
		}

		// The virtual is reached through the live marker storage, so it cannot be
		// hooked until a clip is open. Doing it this way rather than by pattern is
		// deliberate: the vtable SLOT is the stable thing (offset 0x160 on both
		// builds), whereas the function body is small, generic and exactly the
		// kind of code a compiler shuffles between updates.
		bool s_editableHooked = false;

		void tryHookIsEditable()
		{
			if (s_editableHooked) return;

			void* storage = game::markerStorage();
			if (!storage) return;   // no clip open yet; try again next frame

			void** vtbl = *(void***)storage;
			if (!vtbl) return;

			void* fn = vtbl[gsig::MARKERSTORAGE_VT_ISEDITABLE / sizeof(void*)];

			// A vtable slot is only as trustworthy as the object it came from, and
			// this one is read from a global that other code also writes. Refuse
			// anything that is not executable - hooking a bad address is a crash
			// with no obvious cause, days later.
			MEMORY_BASIC_INFORMATION mbi{};
			if (!fn || !VirtualQuery(fn, &mbi, sizeof(mbi)) ||
			    mbi.State != MEM_COMMIT ||
			    !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
			                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
			{
				s_editableHooked = true;   // do not retry every frame forever
				logger::write("info",
					"limits: marker-storage slot 0x%X is not code (%p) - first-person "
					"clips stay locked", gsig::MARKERSTORAGE_VT_ISEDITABLE, fn);
				return;
			}

			s_editableHooked = true;
			memory((uintptr_t)fn).hook(hkIsEditable, &origIsEditable, "IsMarkerControlEditable");
			logger::write("info",
				"limits: IsMarkerControlEditable hooked at %p (vtable +0x%X)",
				fn, gsig::MARKERSTORAGE_VT_ISEDITABLE);
		}
	}

	void tick()
	{
		tryHookIsEditable();
	}

	// Lift the leash by rewriting the METADATA the check reads, not by hooking
	// the function that reads it.
	//
	// Hooking GetMaxDistanceAllowedFromPlayer is not enough on Enhanced: Clang
	// INLINED it into the free-camera update, so the copy that actually gates
	// the camera never goes through our detour - which is why the leash kept
	// firing while the log cheerfully reported "distance=unlimited". Both the
	// standalone function and the inlined copy read the same
	// camReplayDirectorMetadata field, so overwriting that catches every caller
	// on either build.
	//
	// The original is captured before the first change and put back the moment
	// the option is switched off, so this does not permanently alter metadata
	// that other systems share.
	void applyDistanceLimit(void* director)
	{
		static float s_original = -1.0f;

		if (!director) return;
		auto* meta = *(uint8_t**)((uint8_t*)director + rdirector::OFF_Metadata);
		if (!meta) return;

		float* maxDist = (float*)(meta + rdirector::META_MaxDistance);
		const Config& cfg = Config::get();

		if (cfg.unlimitedCameraDistance)
		{
			if (s_original < 0.0f)
			{
				s_original = *maxDist;
				logger::write("info", "limits: leash %.1f -> %.1f (metadata %p)",
					s_original, cfg.maxCameraDistance, (void*)maxDist);
			}
			if (*maxDist != cfg.maxCameraDistance) *maxDist = cfg.maxCameraDistance;
		}
		else if (s_original >= 0.0f)
		{
			logger::write("info", "limits: leash restored to %.1f", s_original);
			*maxDist   = s_original;
			s_original = -1.0f;
		}
	}

	// =========================================================================
	//  Uncapping the editor's zoom
	// =========================================================================
	//  The whole 0.45x..4.50x range is two floats in the free camera's metadata.
	//  camReplayFreeCamera::UpdateZoom, as disassembled at Legacy 0x2AD21C:
	//
	//      mov   rax, [rcx+0x250]        ; this->m_Metadata
	//      movss xmm2, [rax+0x170]       ; m_MaxFov
	//      divss xmm3, [rcx+0x90]        ; zoomFactor    = MaxFov / m_Frame.fov
	//      divss xmm5, [rax+0x16c]       ; maxZoomFactor = MaxFov / m_MinFov
	//      ...   Clamp(zoomFactor, 1.0f, maxZoomFactor)
	//      divss xmm2, xmm3              ; newFov = MaxFov / zoomFactor
	//      ...   Clamp(newFov, 1.0f, 130.0f)   <- camFrame::SetFov, inlined
	//      movss [rcx+0x90], xmm2
	//
	//  So the reachable range IS MinFov..MaxFov, and widening the pair is the
	//  entire fix. Two consequences worth knowing:
	//
	//    - The view does not jump. Both the zoomFactor going in and the FOV
	//      coming out are scaled by the same MaxFov within one call, so it
	//      cancels; only the reachable ends move.
	//    - 1..130 is the ceiling. That last clamp is camFrame::SetFov and is
	//      shared with every camera in the game, so it is deliberately not
	//      patched.
	//
	//  The offsets are hardcoded because they were read straight out of both
	//  binaries, not guessed:
	//    Legacy   0x2AD21C  UpdateZoom standalone, exactly as listed above.
	//    Enhanced 0x237E80  UpdateZoom inlined, but `divss xmm2,[rax+0x16c]` is
	//                       the ONLY such instruction in the whole 96 MB image,
	//                       paired with `movss xmm0,[rax+0x170]` three
	//                       instructions earlier.
	//  They agree, and they agree with the layout implied by m_CapsuleRadius at
	//  metadata+0x178 (which UpdateCollision reads on both builds).
	// =========================================================================
	namespace
	{
		// camReplayFreeCameraMetadata, from m_MaxPitch through m_CapsuleRadius.
		constexpr uint32_t META_MaxPitch      = 0x168;
		constexpr uint32_t META_MinFov        = 0x16C;
		constexpr uint32_t META_MaxFov        = 0x170;
		constexpr uint32_t META_DefaultFov    = 0x174;
		constexpr uint32_t META_CapsuleRadius = 0x178;

		// The director's m_ActiveCamera is a plain camBaseCamera pointer: it is
		// the free camera most of the time, but it is also the recorded camera
		// during un-edited playback and the fallback camera when the free camera
		// bails. Those carry a DIFFERENT metadata class, where 0x16C/0x170 mean
		// something else entirely.
		//
		// So before writing anything, check that all five neighbouring fields
		// hold values in the range the game itself accepts. A block that is not a
		// camReplayFreeCameraMetadata has no reason to pass all five at exactly
		// these offsets, and a build that moved the fields will fail it too —
		// which is the point. Not sure means leave it alone.
		bool looksLikeFreeCamMetadata(const uint8_t* m)
		{
			const float maxPitch = *(const float*)(m + META_MaxPitch);      // 89.5
			const float minFov   = *(const float*)(m + META_MinFov);        // 10
			const float maxFov   = *(const float*)(m + META_MaxFov);        // 100
			const float defFov   = *(const float*)(m + META_DefaultFov);    // 45
			const float radius   = *(const float*)(m + META_CapsuleRadius);  // 0.5

			return maxPitch >= 45.0f  && maxPitch <= 90.0f
			    && minFov   >= 1.0f   && minFov   <= 130.0f
			    && maxFov   >  minFov && maxFov   <= 130.0f
			    && defFov   >= minFov && defFov   <= maxFov
			    && radius   >  0.0f   && radius   <= 10.0f;
		}
	}

	void applyZoomLimit(void* director)
	{
		// The metadata object we last wrote to, so a camera swap restores the
		// old one instead of leaving it widened, and so the validity check is
		// re-run against whatever took its place.
		static uint8_t* s_meta     = nullptr;
		static float    s_origMin  = 0.0f, s_origMax = 0.0f;
		static uint8_t* s_rejected = nullptr;   // only complain once per object

		uint8_t* meta = nullptr;
		if (director)
		{
			if (auto* cam = *(uint8_t**)((uint8_t*)director + rdirector::OFF_ActiveCamera))
				meta = *(uint8_t**)(cam + rdirector::CAM_Metadata);
		}

		// Put the old values back both when the option is switched off and when
		// the camera changes under us — otherwise a widened block would be left
		// behind for whatever picks it up next.
		const bool want = Config::get().uncapZoom;
		if (s_meta && (meta != s_meta || !want))
		{
			*(float*)(s_meta + META_MinFov) = s_origMin;
			*(float*)(s_meta + META_MaxFov) = s_origMax;
			s_meta = nullptr;
		}

		if (!meta || !want) return;

		if (!s_meta)
		{
			if (!looksLikeFreeCamMetadata(meta))
			{
				// Expected while the recorded camera is driving; only worth a
				// line the first time we see a given block.
				if (meta != s_rejected)
				{
					s_rejected = meta;
					logger::write("info",
						"limits: camera metadata %p is not the free camera's — zoom left alone",
						(void*)meta);
				}
				return;
			}
			s_meta    = meta;
			s_origMin = *(float*)(meta + META_MinFov);
			s_origMax = *(float*)(meta + META_MaxFov);
			logger::write("info", "limits: zoom %.1f..%.1f -> %.1f..%.1f (metadata %p)",
				s_origMin, s_origMax, Config::get().zoomMinFov, Config::get().zoomMaxFov,
				(void*)meta);
		}

		// Rewritten every frame rather than once: the ini is re-readable in
		// place, so the values can change under us.
		const Config& cfg = Config::get();
		float* minFov = (float*)(meta + META_MinFov);
		float* maxFov = (float*)(meta + META_MaxFov);
		if (*minFov != cfg.zoomMinFov) *minFov = cfg.zoomMinFov;
		if (*maxFov != cfg.zoomMaxFov) *maxFov = cfg.zoomMaxFov;
	}

	void install()
	{
		// Hooks are installed unconditionally so the in-editor menu can toggle
		// them live; each one checks the config on entry.
		if (game::addr_GetMaxDistanceFromPlayer)
			memory(game::addr_GetMaxDistanceFromPlayer).hook(hkGetMaxDist, &origGetMaxDist, "GetMaxDistanceFromPlayer");
		else
			logger::write("info", "limits: distance getter unresolved — leash unchanged");

		if (game::addr_UpdateCollision)
			memory(game::addr_UpdateCollision).hook(hkUpdateCollision, &origUpdateCollision, "UpdateCollision");
		else
			logger::write("info", "limits: UpdateCollision unresolved — world collision unchanged");

		if (game::addr_ComputeSafePosition)
			memory(game::addr_ComputeSafePosition).hook(hkComputeSafePosition, &origComputeSafePos, "ComputeSafePosition");
		else
			logger::write("info", "limits: ComputeSafePosition unresolved — attach collision unchanged");

		if (game::addr_ProfanityGetStatus)
			memory(game::addr_ProfanityGetStatus).hook(hkProfanityStatus, &origProfanityStatus, "ProfanityGetStatus");
		else
			logger::write("info", "limits: profanity poll unresolved - filter unchanged");

		if (game::addr_IsPlaybackFlagSet)
			memory(game::addr_IsPlaybackFlagSet).hook(hkIsPlaybackFlagSet, &origIsPlaybackFlagSet, "IsPlaybackFlagSet");
		else
			logger::write("info",
				"limits: IsPlaybackFlagSet unresolved - a first-person clip keeps "
				"its recorded camera even with the menu unlocked");

		// THE actual lift: write the count the recording path reads.
		//
		// Hooking SetupReplayBuffer was not enough on its own - both recording
		// call sites load this global, and whether our detour is in place by the
		// time they run is not something we control. Writing the value has no
		// such race: whenever the game next sets the buffer up, it reads ours.
		// The hook stays as a backstop and as the trace that tells us which of
		// the two actually did the work.
		const int want = Config::get().replayBlocks;
		if (game::addr_g_ReplayBlocks)
		{
			auto* blocks = (unsigned short*)game::addr_g_ReplayBlocks;
			const unsigned short before = *blocks;

			if (want != (int)before)
			{
				*blocks = (unsigned short)want;
				logger::write("info",
					"limits: recording blocks %u -> %d (%d MB). Takes effect at the next "
					"recording setup; a clip already rolling keeps the old budget.",
					before, want, (want * gsig::REPLAY_BLOCK_BYTES) / (1024 * 1024));
			}
			else
			{
				logger::write("info", "limits: recording blocks already %u (%d MB)",
					before, (want * gsig::REPLAY_BLOCK_BYTES) / (1024 * 1024));
			}
		}
		else
		{
			logger::write("info",
				"limits: replay block count unresolved - recording length unchanged");
		}

		// =====================================================================
		//  Widen the replay heap, if we are early enough and it is wanted.
		// =====================================================================
		//  36 blocks is not a policy limit - it is the largest count that fits a
		//  172 MB pool reserved once at rva 0x1020. Widening that reservation is
		//  the only way past it, and it can only be done before it happens.
		//
		//  Sized from what a block ACTUALLY costs: 4 MB for the block plus a
		//  384 KB thumbnail, with one spare thumbnail on top. The first attempt
		//  at this used R*'s own (n+1)*4MB and crashed, because their sizing
		//  ignores the thumbnails - which is also why their own stated maximum
		//  of 42 blocks does not fit their own heap. Do not copy their formula.
		//
		//  Anything that is not certain here CLAMPS instead of guessing, because
		//  the failure mode is not a refusal - the game never checks the null it
		//  gets back from a short pool, so an optimistic write crashes it.
		// =====================================================================
		{
			Config& cfg  = const_cast<Config&>(Config::get());
			const int want = cfg.replayBlocks;

			if (want > gsig::REPLAY_BLOCKS_SAFE_MAX)
			{
				// The allocator object's vtable slot. It is a static, so this is
				// zero until its constructor runs - which is the question:
				// has the 172 MB pool been committed yet?
				const void* built = game::addr_g_ReplayAllocator
					? *(void* const*)game::addr_g_ReplayAllocator
					: (const void*)1;   // unresolved: assume the worst

				const bool sites = game::addr_ReplayHeapAllocImm &&
				                   game::addr_ReplayHeapCtorImm;

				const unsigned bytes = gsig::replayHeapBytesFor(want);

				// How big the pool actually IS, rather than how big we assume it
				// is. This matters because "already built" no longer implies
				// "still stock": under FiveM the size immediate is patched at DLL
				// attach, ~20-35 seconds before ScriptHookV hands us this thread,
				// so by the time we get here the heap is both BUILT and WIDE.
				// Clamping on builtness alone threw that away and put the user
				// back on stock recording length.
				const unsigned pool = sites
					? *(const unsigned*)game::addr_ReplayHeapAllocImm
					: gsig::REPLAY_HEAP_STOCK_BYTES;

				// Can this machine actually give the game that reservation? Ask
				// for it ourselves and hand it straight back. The game's own
				// attempt has no failure path we survive, so the only safe place
				// to find out is here, before it tries.
				bool room = false;
				if (sites && !built)
				{
					if (void* probe = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS))
					{
						VirtualFree(probe, 0, MEM_RELEASE);
						room = true;
					}
				}

				if (sites && !built && room)
				{
					memory(game::addr_ReplayHeapAllocImm).put<unsigned>(bytes);
					memory(game::addr_ReplayHeapCtorImm).put<unsigned>(bytes);
					logger::write("info",
						"limits: replay heap %u -> %u MB for %d+%d blocks. Above the "
						"engine's own 36 - if the game dies on the first recording, "
						"lower ReplayBlocks.",
						gsig::REPLAY_HEAP_STOCK_BYTES / (1024u * 1024u),
						bytes / (1024u * 1024u), want, gsig::REPLAY_TEMP_BLOCKS);
				}
				else if (built && pool >= bytes)
				{
					// Built, but already big enough - the early FiveM patch got
					// there first. Nothing to do, and specifically nothing to
					// clamp: the pool in front of us genuinely holds what was
					// asked for.
					logger::write("info",
						"limits: replay heap is already %u MB - wide enough for %d+%d "
						"blocks, keeping ReplayBlocks=%d",
						pool / (1024u * 1024u), want, gsig::REPLAY_TEMP_BLOCKS, want);
				}
				else
				{
					cfg.replayBlocks = gsig::REPLAY_BLOCKS_SAFE_MAX;
					logger::write("info",
						"limits: cannot widen the replay heap (%s) - ReplayBlocks %d -> %d. "
						"The %u MB pool holds %d blocks and no more.",
						!sites ? "heap size sites unresolved"
						       : built ? "heap already built - we installed too late"
						               : "this process cannot reserve that much address space",
						want, gsig::REPLAY_BLOCKS_SAFE_MAX,
						pool / (1024u * 1024u), gsig::REPLAY_BLOCKS_SAFE_MAX);
				}
			}
		}

		// Is the replay heap already built?
		//
		// THE question for ever getting past 36. That ceiling is one 172 MB
		// allocation made at rva 0x1020, and 36 is simply the largest block
		// count that fits it - so the only way further is to widen that
		// allocation, which can only be done BEFORE it happens.
		//
		// m_pReplayAllocator is null until the heap is built, so this one read
		// decides whether that is even reachable from where we install.
		if (game::addr_g_ReplayAllocator)
		{
			const void* alloc = *(void* const*)game::addr_g_ReplayAllocator;
			logger::write("info",
				"limits: replay heap %s at install (allocator=%p). %s",
				alloc ? "ALREADY BUILT" : "not built yet", alloc,
				alloc
					? "Too late to resize it from here - the 172 MB pool is already "
					  "committed, so 36 blocks is the ceiling for this injection point."
					: "We are early enough to widen it - the pool has not been "
					  "committed yet.");
		}

		// Backstop. The hook itself re-reads the ini on entry, so ReplayBlocks
		// can be edited without a rebuild; at the stock 7 it is a pass-through.
		if (game::addr_SetupReplayBuffer)
			memory(game::addr_SetupReplayBuffer).hook(hkSetupReplayBuffer, &origSetupReplayBuffer, "SetupReplayBuffer");
		else
			logger::write("info",
				"limits: SetupReplayBuffer unresolved - no backstop on the block count");

		// Report what is ACTUALLY in effect, not what the ini asked for. These
		// hooks are optional and simply do not resolve on a build we have no
		// pattern for; echoing the config there claims a feature is on when the
		// hook was never installed.
		logger::write("info", "limits: profanity=%s",
			!game::addr_ProfanityGetStatus     ? "stock (unresolved)"
			: Config::get().bypassProfanityFilter ? "bypassed" : "stock");

		// Distance no longer depends on the getter resolving - the metadata
		// override in applyDistanceLimit() works even where the getter is
		// inlined, which is the case on Enhanced.
		logger::write("info", "limits: distance=%s",
			Config::get().unlimitedCameraDistance ? "unlimited (metadata)" : "stock");

		// Deliberately says "pending": the hook it describes cannot be placed
		// until a clip is open, so claiming it here would be reporting an
		// intention as a fact. tick() logs the address once it really lands.
		// TWO locks, reported separately. The menu one lands late (it needs an
		// open clip); the director one is a plain hook. Lumping them together
		// would report success when only half the job is done - which is exactly
		// the state that produces a free camera that will not move.
		logger::write("info", "limits: camera restrictions=%s, director gate=%s",
			Config::get().unlockCameraRestrictions ? "unlocked (pending a clip)" : "stock",
			!game::addr_IsPlaybackFlagSet          ? "stock (unresolved)"
			: Config::get().unlockCameraRestrictions ? "lifted" : "stock");

		// Report the two collision hooks SEPARATELY. Lumping them together said
		// "stock (unresolved)" whenever ComputeSafePosition was missing, even
		// though UpdateCollision was hooked and world geometry really was being
		// passed through - the log was denying a feature that was working.
		logger::write("info", "limits: collision world=%s attach=%s",
			!game::addr_UpdateCollision        ? "stock (unresolved)"
			: Config::get().disableCameraCollision ? "disabled" : "stock",
			!game::addr_ComputeSafePosition    ? "stock (unresolved)"
			: Config::get().disableCameraCollision ? "disabled" : "stock");
	}
}
