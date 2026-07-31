// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdint>

// =============================================================================
//  sReplayMarkerInfo  —  the per-marker record (sMarkerData_Current)
// =============================================================================
//  m_data sits at offset 0 (plain struct, no vtable). Every offset below was
//  confirmed against the shipped binary at eight independent fields, so the
//  mapping is exact.
//
//  Beware the Id/Index pairs: the *Index members are UI-display values only —
//  the Video Editor menu reads those, but anything doing real work wants the
//  *Id. We want the Id.
// =============================================================================
namespace rmarker
{
	enum : uint32_t
	{
		OFF_EditedPropertyFlags   = 0x00, // u64
		OFF_FreeCamQuaternion     = 0x08, // u64, smallest-3 / 20-bit  (see quat.h)
		OFF_AttachEntityQuat      = 0x10, // u64
		OFF_AttachEntityPosition  = 0x18, // float[3]
		OFF_FreeCamPosition       = 0x24, // float[3]   <- the curve control point
		OFF_FreeCamPosRelAttach   = 0x30, // float[3]
		OFF_FreeCamRelHeading     = 0x3C, // float
		OFF_FreeCamRelPitch       = 0x40, // float
		OFF_FreeCamRelRoll        = 0x44, // float
		OFF_VerticalLookAtOffset  = 0x64, // float
		OFF_HorizontalLookAtOff   = 0x68, // float
		OFF_LookAtRoll            = 0x6C, // float
		OFF_TimeMs                = 0x70, // float  (non-dilated)
		OFF_TransitionDurationMs  = 0x74, // float
		OFF_AttachTargetId        = 0x78, // s32, -1 = none
		OFF_AttachTargetIndex     = 0x7C, // s32, UI display only
		OFF_LookAtTargetId        = 0x80, // s32, -1 = none
		OFF_LookAtTargetIndex     = 0x84, // s32, UI display only
		OFF_DofFocusTargetId      = 0x88, // s32, -1 = none
		OFF_DofFocusTargetIndex   = 0x8C, // s32, UI display only
		OFF_Speed                 = 0x90, // u32
		OFF_ShakeSpeed            = 0x94, // u32
		OFF_MarkerType            = 0xA0, // u8, 2 = anchor
		OFF_CamType               = 0xA1, // u8, 6 = RPLYCAM_FREE_CAMERA
		OFF_TransitionType        = 0xA2, // u8
		OFF_Fov                   = 0xA3, // u8
		OFF_CameraBlendType       = 0xA4, // u8, 0 NONE / 1 LINEAR / 2 SMOOTH
		OFF_ShakeType             = 0xA5, // u8
		OFF_ShakeIntensity        = 0xA6, // u8
		OFF_AttachType            = 0xA7, // u8
		OFF_CamMatrixValid        = 0xB2, // bool
		// Is the stored attach quaternion/position at +0x10/+0x18 trustworthy?
		// Cleared when the attach target changes, and by the game's own loader
		// when the packed quaternion does not decode to a unit length. Confirmed
		// as the `marker[0xB3] = 0` write in ResetForNewAttachEntity.
		OFF_AttachMatrixValid     = 0xB3, // bool
		OFF_CameraEasing          = 0xB5, // bool
		SIZE                      = 0xB8,
	};

	// eMarkerCameraBlendType
	enum : uint8_t { BLEND_NONE = 0, BLEND_LINEAR = 1, BLEND_SMOOTH = 2 };
	// eMarkerCameraAttachType. Note that picking an attach target from the menu
	// writes POSITION_ONLY as a side effect, so it is the common case rather
	// than the exotic one - the stepper does
	//     if (aux != -1 && lookAtId != newAttachId) marker[0xA7] = 2;
	enum : uint8_t
	{
		ATTACH_FULL                = 0,  // rotates fully with the parent
		ATTACH_POSITION_AND_HEADING = 1, // parent's heading, own pitch/roll
		ATTACH_POSITION_ONLY       = 2,  // translates only; rotation frozen
	};
	// ReplayCameraType
	enum : uint8_t { CAM_FREE = 6 };
	// eMarkerType
	enum : uint8_t { MARKERTYPE_ANCHOR = 2 };

	inline const uint8_t* base(const void* m) { return (const uint8_t*)m; }

	template <typename T>
	inline T get(const void* m, uint32_t off) { return *(const T*)(base(m) + off); }

	inline uint8_t  camType(const void* m)   { return get<uint8_t>(m, OFF_CamType); }
	inline uint8_t  blendType(const void* m) { return get<uint8_t>(m, OFF_CameraBlendType); }
	inline uint8_t  markerType(const void* m){ return get<uint8_t>(m, OFF_MarkerType); }
	inline float    timeMs(const void* m)    { return get<float>(m, OFF_TimeMs); }
	inline int32_t  attachId(const void* m)  { return get<int32_t>(m, OFF_AttachTargetId); }
	inline int32_t  lookAtId(const void* m)  { return get<int32_t>(m, OFF_LookAtTargetId); }
	inline uint64_t camQuat(const void* m)   { return get<uint64_t>(m, OFF_FreeCamQuaternion); }

	// The parent's orientation AT THE MOMENT THIS MARKER WAS AUTHORED, packed
	// the same way as the camera quaternion.
	//
	// This is what makes a mounted segment interpolable. Except in FULL mode the
	// game never refreshes the attach matrix's rotation, so each marker carries
	// its own frozen frame - and two markers made while the car pointed
	// different ways store their offsets against different bases.
	inline uint64_t attachQuat(const void* m) { return get<uint64_t>(m, OFF_AttachEntityQuat); }

	// Whether attachQuat() above means anything for this marker.
	//
	// It has to be asked, and asking is not optional the way it looks: decoding
	// a smallest-three quaternion ALWAYS yields a unit quaternion, and building
	// a basis from one always yields an orthonormal matrix - so a stale or never
	// written field produces a perfectly well-formed frame that happens to point
	// the wrong way. There is no way to detect that from the value itself; this
	// flag is the only signal. The game gates on it too, falling back to the
	// live entity matrix when it is clear.
	inline bool attachMatrixValid(const void* m)
	{
		return get<uint8_t>(m, OFF_AttachMatrixValid) != 0;
	}
	inline bool     easing(const void* m)    { return get<uint8_t>(m, OFF_CameraEasing) != 0; }
	inline uint8_t  attachType(const void* m){ return get<uint8_t>(m, OFF_AttachType); }

	// Look-at framing, as authored on the marker. LoadMarkerCameraInfo copies
	// these three straight into the camera's live m_LookAtOffset / m_LookAtRoll,
	// so during playback the marker IS the authoritative copy - which is why the
	// re-aim reads them here rather than reaching into the camera for values it
	// would then have to keep in step with a blend.
	inline float lookAtVertical(const void* m)   { return get<float>(m, OFF_VerticalLookAtOffset); }
	inline float lookAtHorizontal(const void* m) { return get<float>(m, OFF_HorizontalLookAtOff); }
	inline float lookAtRoll(const void* m)       { return get<float>(m, OFF_LookAtRoll); }

	// Parent-relative pose. Meaningful only when attachId != -1.
	inline float relHeading(const void* m) { return get<float>(m, OFF_FreeCamRelHeading); }
	inline float relPitch(const void* m)   { return get<float>(m, OFF_FreeCamRelPitch); }
	inline float relRoll(const void* m)    { return get<float>(m, OFF_FreeCamRelRoll); }

	inline const float* position(const void* m)
	{
		return (const float*)(base(m) + OFF_FreeCamPosition);
	}

	// The control point for an ATTACHED marker.
	//
	// +0x24 (position() above) is the world position at authoring time, and is
	// simply the wrong space once the parent has moved - the game does not use
	// it either. What it loads is this, into m_DesiredPosition, and transforms
	// by the parent's live matrix every frame (UpdatePosition).
	inline const float* positionRelative(const void* m)
	{
		return (const float*)(base(m) + OFF_FreeCamPosRelAttach);
	}

	// =========================================================================
	//  Editing markers directly
	// =========================================================================
	//  The editor's menu dispatcher never just writes a field — every case
	//  pairs the write with a BTS/OR into m_editedPropertyFlags (+0x00). That
	//  bit is what marks the value as authored; without it the editor does not
	//  treat the marker as edited and the change will not stick.
	//
	//  Bit numbers below were read straight out of the dispatcher's switch, one
	//  case at a time, so they are observed rather than guessed.
	// =========================================================================
	enum EditedBit : int
	{
		BIT_MarkerType       = 0,
		// +0xA8 m_activePostFx: `OR qword ptr [rdi],0x10` -> bit 4.
		BIT_ActivePostFx     = 4,
		BIT_PostFxIntensity  = 5,
		BIT_Saturation       = 6,
		BIT_Contrast         = 7,
		BIT_Vignette         = 8,
		BIT_Brightness       = 9,
		BIT_CamType          = 10,
		BIT_ShakeType        = 11,
		BIT_ShakeIntensity   = 12,
		BIT_ShakeSpeed       = 13,
		BIT_CameraEasing     = 14,
		BIT_CameraBlendType  = 20,
		BIT_DofMode          = 26,
		BIT_DofFocusMode     = 27,
		BIT_DofFocalDistance = 28,
		BIT_DofIntensity     = 30,
		BIT_SFXVolume        = 31,
		BIT_DialogVolume     = 32,
		BIT_MusicVolume      = 33,
		BIT_ScoreIntensity   = 34,
		BIT_MicrophoneType   = 35,
		BIT_OneshotVolume    = 38,
		BIT_AmbientVolume    = 39,
	};

	inline void markEdited(void* m, int bit)
	{
		*(uint64_t*)m |= (1ull << bit);
	}

	template <typename T>
	inline void setField(void* m, uint32_t off, T value, int editedBit)
	{
		*(T*)((uint8_t*)m + off) = value;
		markEdited(m, editedBit);
	}

	// Extra marker fields the editor exposes, beyond the camera ones above.
	enum : uint32_t
	{
		OFF_PostFxIntensity  = 0x48,
		OFF_Saturation       = 0x4C,
		OFF_Contrast         = 0x50,
		OFF_Vignette         = 0x54,
		OFF_Brightness       = 0x58,
		OFF_DofFocalDistance = 0x5C,
		OFF_DofIntensity     = 0x60,
		OFF_ActivePostFx     = 0xA8,
		OFF_SFXVolume        = 0xA9,
		OFF_MusicVolume      = 0xAA,
		OFF_DialogVolume     = 0xAB,
		OFF_AmbientVolume    = 0xAC,
		OFF_OneshotVolume    = 0xAD,
		OFF_ScoreIntensity   = 0xAE,
		OFF_DofMode          = 0xAF,
		OFF_DofFocusMode     = 0xB0,
		OFF_MicrophoneType   = 0xB1,
	};

	// -------------------------------------------------------------------------
	// Writing a camera pose into a marker.
	//
	// Mirrors camReplayBaseCamera::SaveMarkerCameraInfo exactly: position,
	// packed quaternion, FOV, and m_camMatrixValid. Note there is NO
	// edited-property bit for the camera pose - m_camMatrixValid is the flag
	// LoadMarkerCameraInfo gates on, so setting it is what makes the pose stick.
	// -------------------------------------------------------------------------
	inline void setCameraPosition(void* m, const float p[3])
	{
		float* dst = (float*)((uint8_t*)m + OFF_FreeCamPosition);
		dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2];
		*(uint8_t*)((uint8_t*)m + OFF_CamMatrixValid) = 1;
	}

	inline void setCameraQuat(void* m, uint64_t packed)
	{
		*(uint64_t*)((uint8_t*)m + OFF_FreeCamQuaternion) = packed;
		*(uint8_t*)((uint8_t*)m + OFF_CamMatrixValid) = 1;
	}

	inline void setFov(void* m, uint8_t fov)
	{
		*(uint8_t*)((uint8_t*)m + OFF_Fov) = fov;
		*(uint8_t*)((uint8_t*)m + OFF_CamMatrixValid) = 1;
	}

	inline uint8_t fov(const void* m) { return get<uint8_t>(m, OFF_Fov); }

	// A marker this mod is willing to build a curve through.
	//
	// `segmentAttach` is the attach target of the segment being splined: -1 for
	// an ordinary world-space shot, or an entity id when the shot is mounted on
	// something. A control point has to live in the SAME space as the rest of
	// the curve, so a marker qualifies only if it is a free camera AND its own
	// attach target matches.
	//
	// The attach condition is doing real work in both directions. It keeps
	// world-space curves free of attached markers, as before - and it keeps an
	// attached curve free of world-space ones, and of markers mounted on a
	// DIFFERENT entity. Two different parents are two different coordinate
	// spaces; there is no meaningful interpolation between them, and the game
	// does not attempt one either (it blends the two live camera frames, whose
	// endpoints are both moving).
	//
	// Attach TYPE deliberately does NOT have to match across a segment. It did
	// once, when the curve was solved in one shared parent frame - full,
	// position-and-heading and position-only each make the game build a
	// different attach matrix, so markers that disagreed were storing offsets
	// against different bases. The curve now resolves each marker through its
	// OWN frame into a world-space displacement before interpolating, which
	// makes a mixed-mode segment well defined rather than merely tolerated.
	inline bool isSplineable(const void* m, int32_t segmentAttach)
	{
		return m && camType(m) == CAM_FREE && attachId(m) == segmentAttach;
	}
}

// =============================================================================
//  IReplayMarkerStorage
// =============================================================================
//  A pure interface, so the vtable is one flat run with no bases folded in -
//  EXCEPT that MSVC emits an overload SET in reverse. See the note on the slot
//  enum below; getting that wrong is what made "add marker" call AddMarker's
//  pointer-taking overload with a bool, and crash.
//
//  Slot 21 (TryGetMarker) is confirmed by the +0xA8 call in UpdateSmoothing and
//  slot 33 (GetMarkerCount) by the +0x108 call in GetNextMarkerIndex, which pins
//  the enumeration at both ends.
// =============================================================================
namespace rstorage
{
	// Slot numbers are positions in the vtable. Four of them are confirmed
	// against real call sites rather than inferred from their neighbours:
	//   +0xA8  TryGetMarker    - the call in UpdateSmoothing
	//   +0x108 GetMarkerCount  - the call in GetNextMarkerIndex
	//   +0x70  IncrememtMarkerSpeed          } both in the menu dispatcher's
	//   +0x88  RecalcPositionsOnSpeedChange  } speed case, back to back
	// which pins the enumeration at several points along its length.
	// !! MSVC emits an OVERLOAD SET in reverse. Where a name is overloaded,
	// stepping through the slots in the obvious order gives the wrong one.
	// Four overload sets sit in this vtable:
	//     AddMarker   x3  (slots 2-4)
	//     GetMarker   x2  (19-20)   TryGetMarker x2 (21-22)
	//     GetFirstMarker x2 (23-24) GetLastMarker x2 (25-26)
	// The const/non-const pairs are harmless either way - both members do the
	// same thing - which is why TryGetMarker at 0xA8 worked despite this.
	// AddMarker is not harmless: its three overloads take different arguments.
	//
	// We deliberately use the MIDDLE AddMarker overload. Reversing a 3-element
	// set leaves the middle element fixed, so slot 3 is AddMarker(bool,bool,s32&)
	// whichever way the compiler ordered them. It adds at the current playback
	// time, which is what "add at playhead" means anyway.
	enum : uint32_t
	{
		VT_AddMarkerHere     = 0x18,  // slot  3 - bool (bool sort, bool inherit, s32& outIdx)
		VT_RemoveMarker      = 0x28,  // slot  5 - void (u32 index)
		VT_ResetMarker       = 0x30,  // slot  6 - void (u32 index)
		VT_CanPlaceMarkerAt  = 0x38,  // slot  7 - bool (float timeMs, bool ignoreBoundary)
		VT_SetMarkerTimeMs   = 0x60,  // slot 12 - void (u32 idx, float newTimeMs)
		VT_IncrementSpeed    = 0x70,  // slot 14 - void (u32 idx, s32 delta)
		VT_RecalcOnSpeed     = 0x88,  // slot 17 - void (u32 startingIdx)
		VT_TryGetMarker      = 0xA8,  // slot 21 - sReplayMarkerInfo* (u32 index)
		VT_GetMarkerCount    = 0x108, // slot 33 - s32 ()
		VT_GetMaxMarkerCount = 0x110, // slot 34 - s32 ()
		VT_BlendTypeUpdated  = 0x168, // slot 45 - void (u32 index)
	};

	template <typename Fn>
	inline Fn vfn(void* storage, uint32_t off)
	{
		auto vtbl = *(uintptr_t**)storage;
		return (Fn)(*(uintptr_t*)((uint8_t*)vtbl + off));
	}

	inline void* tryGetMarker(void* storage, int index)
	{
		if (!storage || index < 0) return nullptr;
		auto vtbl = *(uintptr_t**)storage;
		using Fn = void* (__fastcall*)(void*, uint32_t);
		return ((Fn)(*(uintptr_t*)((uint8_t*)vtbl + VT_TryGetMarker)))(storage, (uint32_t)index);
	}

	// The editor calls this after a blend change so the director can re-evaluate
	// which cameras need to exist. Skipping it leaves the blend visually stale
	// until something else forces a rebuild.
	inline void blendTypeUpdated(void* storage, int index)
	{
		if (!storage || index < 0) return;
		auto vtbl = *(uintptr_t**)storage;
		using Fn = void(__fastcall*)(void*, uint32_t);
		((Fn)(*(uintptr_t*)((uint8_t*)vtbl + VT_BlendTypeUpdated)))(storage, (uint32_t)index);
	}

	inline int markerCount(void* storage)
	{
		if (!storage) return 0;
		return vfn<int(__fastcall*)(void*)>(storage, VT_GetMarkerCount)(storage);
	}

	inline int maxMarkerCount(void* storage)
	{
		if (!storage) return 0;
		return vfn<int(__fastcall*)(void*)>(storage, VT_GetMaxMarkerCount)(storage);
	}

	// The editor refuses markers too close to an existing one or to the clip
	// bounds; ask before adding so we surface a disabled button rather than a
	// silent no-op.
	inline bool canPlaceMarkerAt(void* storage, float timeMs)
	{
		if (!storage) return false;
		__try { return vfn<bool(__fastcall*)(void*, float, bool)>(
			storage, VT_CanPlaceMarkerAt)(storage, timeMs, false); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Adds a marker at the current playback time. sortAfterAdd keeps the array
	// time-ordered (every consumer assumes that); inheritProperties copies the
	// previous marker's settings, which is what the editor's own add does.
	//
	// Wrapped in SEH: this is a counted vtable slot, and a wrong guess here
	// means an arbitrary call rather than a clean failure. Better to log and
	// survive than take the game down mid-clip.
	inline int addMarkerHere(void* storage)
	{
		if (!storage) return -1;
		int outIdx = -1;
		bool ok = false;
		__try
		{
			ok = vfn<bool(__fastcall*)(void*, bool, bool, int*)>(
				storage, VT_AddMarkerHere)(storage, true, true, &outIdx);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
		return ok ? outIdx : -1;
	}

	// The mutators below all sit on COUNTED vtable slots. None of their names is
	// overloaded, so the reversal quirk above does not apply to them - but after
	// being wrong once, a fault here should be a log line, not a dead game.
	inline bool removeMarker(void* storage, int index)
	{
		if (!storage || index < 0) return false;
		__try { vfn<void(__fastcall*)(void*, uint32_t)>(
			storage, VT_RemoveMarker)(storage, (uint32_t)index); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return true;
	}

	inline bool resetMarker(void* storage, int index)
	{
		if (!storage || index < 0) return false;
		__try { vfn<void(__fastcall*)(void*, uint32_t)>(
			storage, VT_ResetMarker)(storage, (uint32_t)index); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return true;
	}

	inline bool setMarkerTimeMs(void* storage, int index, float timeMs)
	{
		if (!storage || index < 0) return false;
		__try { vfn<void(__fastcall*)(void*, uint32_t, float)>(
			storage, VT_SetMarkerTimeMs)(storage, (uint32_t)index, timeMs); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return true;
	}

	// Speed is stepped, not set: the editor exposes it as a 9-entry list and
	// drives it with +/-1 deltas, then has to recompute every later marker's
	// position because playback speed changes how time maps onto the clip.
	inline bool incrementSpeed(void* storage, int index, int delta)
	{
		if (!storage || index < 0) return false;
		__try
		{
			vfn<void(__fastcall*)(void*, uint32_t, int)>(
				storage, VT_IncrementSpeed)(storage, (uint32_t)index, delta);
			vfn<void(__fastcall*)(void*, uint32_t)>(
				storage, VT_RecalcOnSpeed)(storage, (uint32_t)index);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return true;
	}
}

// =============================================================================
//  camReplayDirector  —  the `this` we are handed
// =============================================================================
namespace rdirector
{
	enum : uint32_t
	{
		OFF_Frame              = 0x1E0, // camFrame
		OFF_FrameMatrixA       = 0x1F0, // float[3] right   (Matrix34 row a)
		OFF_FrameMatrixB       = 0x200, // float[3] forward (row b)
		OFF_FrameMatrixC       = 0x210, // float[3] up      (row c)
		OFF_FramePosition      = 0x220, // float[3]         (row d)
		OFF_FrameFov           = 0x250, // float, clamped 1..130 by the game

		// camFrame DOF block. Offsets follow the camFrame member order; the
		// layout self-validates because m_Fov falls on +0x250, which we had
		// already confirmed independently, and the struct ends just before
		// m_ActiveCamera at +0x2C0.
		// camBaseDirector declares m_Frame, m_CachedInitialDestinationFrame,
		// m_PostEffectFrame in that order, so the authoring frame sits exactly
		// two camFrames before the one we render: 0x1E0 - 2*0xE0 = 0x20. The
		// arithmetic closes on itself, which is what makes it safe to use - the
		// gap between two independently confirmed offsets is exactly two frames
		// of the size the field layout already implied.
		//
		// It matters because it is the frame the editor AUTHORS from.
		// camReplayBaseCamera::GetDefaultFrame clones GetFrameNoPostEffects(),
		// i.e. m_Frame, and a marker created without saved camera info is
		// written from it - so a new marker lands where the game would have put
		// the camera, not where the curve actually put it. See
		// Config::authorFromCurve.
		OFF_AuthorFrame        = 0x20,
		OFF_AuthorMatrixA      = 0x30,
		OFF_AuthorMatrixB      = 0x40,
		OFF_AuthorMatrixC      = 0x50,
		OFF_AuthorPosition     = 0x60,
		OFF_AuthorFov          = 0x90,

		OFF_ActiveCamera       = 0x2C0,
		OFF_Metadata           = 0x2F0, // camReplayDirectorMetadata*
		// Inside that metadata: the free camera's leash from the player, and the
		// amount subtracted from it while editing a marker. Confirmed by
		// GetMaxDistanceAllowedFromPlayer, which is exactly
		//   f = meta[0x38];  if (editing this marker) f -= meta[0x3C];
		META_MaxDistance       = 0x38, // float
		META_EditDistanceDrop  = 0x3C, // float

		// Inside the ACTIVE CAMERA (OFF_ActiveCamera above), not the director:
		// its camBaseCameraMetadata pointer. Confirmed by UpdateCollision, which
		// reads the capsule radius from *(cam+0x250)+0x178.
		CAM_Metadata           = 0x250,
		OFF_CurrentMarkerIndex = 0x630, // s32
		OFF_ShouldResetSmooth  = 0x66A, // bool
	};

	inline float* framePosition(void* self)
	{
		return (float*)((uint8_t*)self + OFF_FramePosition);
	}
	inline int currentMarkerIndex(void* self)
	{
		return *(int*)((uint8_t*)self + OFF_CurrentMarkerIndex);
	}
}
