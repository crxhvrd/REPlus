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
			memory(game::addr_GetMaxDistanceFromPlayer).hook(hkGetMaxDist, &origGetMaxDist);
		else
			logger::write("info", "limits: distance getter unresolved — leash unchanged");

		if (game::addr_UpdateCollision)
			memory(game::addr_UpdateCollision).hook(hkUpdateCollision, &origUpdateCollision);
		else
			logger::write("info", "limits: UpdateCollision unresolved — world collision unchanged");

		if (game::addr_ComputeSafePosition)
			memory(game::addr_ComputeSafePosition).hook(hkComputeSafePosition, &origComputeSafePos);
		else
			logger::write("info", "limits: ComputeSafePosition unresolved — attach collision unchanged");

		if (game::addr_ProfanityGetStatus)
			memory(game::addr_ProfanityGetStatus).hook(hkProfanityStatus, &origProfanityStatus);
		else
			logger::write("info", "limits: profanity poll unresolved - filter unchanged");

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
