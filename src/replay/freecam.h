// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdint>
#include <cmath>
#include <initializer_list>
#include <Windows.h>
#include "replay/spline.h"

// =============================================================================
//  camReplayFreeCamera, CEntity, and the look-at solve
// =============================================================================
//  Everything the spline needs that does NOT live in the marker record: the
//  live attach-entity matrix, the look-at target, and a reimplementation of the
//  game's look-at framing maths.
//
//  Both builds share every offset below - they were read off the decompile of
//  the SAME two functions on each binary and matched exactly, so there is no
//  per-build branch here. Where a field was proven on only one build the
//  comment says so.
// =============================================================================

namespace rentity
{
	// CEntity's cached world matrix. Rows at a 16-byte stride, row d (+0x90) is
	// the world position.
	//
	// Confirmed on BOTH builds inside camReplayFreeCamera::ComputeCollisionRootPosition
	// (leg 0x7ff76ef7970c, enh 0x7ff7581bfad0), which reads the attach entity's
	// matrix as the fallback when m_AttachEntity is null - Legacy reads all four
	// rows (+0x60/+0x70/+0x80/+0x90), Enhanced only the two the maths needs
	// (+0x80/+0x90), at the same offsets. ComputeSafePosition reads +0x90..+0x9C
	// as the position on both builds as well, which is the same field again from
	// a second, independent call site.
	enum : uint32_t
	{
		OFF_MatrixA  = 0x60,
		OFF_MatrixB  = 0x70,
		OFF_MatrixC  = 0x80,
		OFF_Position = 0x90,   // Matrix34 row d
	};

	inline spline::Vec3 position(const void* e)
	{
		return spline::Vec3((const float*)((const uint8_t*)e + OFF_Position));
	}
}

namespace rfreecam
{
	// =========================================================================
	//  camReplayFreeCamera field offsets
	// =========================================================================
	//  m_Frame        +0x20   - the SaveMarkerCameraInfo vcall passes `this+0x20`
	//                           as its frame argument on both builds.
	//  frame flags    +0xF0   - `|= 0x80` (Flag_HasCutOrientation), both builds.
	//  m_MarkerIndex  +0x238  - GetMarker(cam[0x47]) in ResetForNewLookAtEntity,
	//                           and the `cam+0x238 == markerIndex` test the
	//                           director uses to pick between its two cameras.
	//  m_Metadata     +0x250  - already relied on by FREECAM_UPDATECOLLISION.
	//  m_AttachEntityMatrix
	//                 +0x3E0  - Matrix34, 16-byte row stride. See below.
	//  m_LookAtOffset +0x4A0  - Vector2, `cam[0x94] = 0` when the look-at target
	//                           changes, both builds. x = HORIZONTAL, y = VERTICAL.
	//  m_AttachEntity +0x4C0  - the `== 0` test that selects between the attach
	//                           matrix and the entity matrix, both builds.
	//  m_LookAtEntity +0x4C8  - `cam[0x99]`, the slot ResetForNewLookAtEntity
	//                           registers the new target into, both builds.
	//
	//  The two entity slots are RegdConstEnt, which is a bare pointer plus an
	//  intrusive back-link the game maintains; reading the pointer is safe, and
	//  we never write either.
	// =========================================================================
	enum : uint32_t
	{
		OFF_Frame          = 0x20,
		OFF_MarkerIndex    = 0x238,
		OFF_Metadata       = 0x250,

		// Matrix34. Legacy reads all four rows explicitly at +0x3E0/+0x3F0/
		// +0x400/+0x410; Enhanced's Clang build folded the UnTransform down to
		// its Z component and so only reads rows c and d, at +0x400/+0x410. The
		// 16-byte stride is therefore proven on both builds and the base follows
		// from it - but rows a and b are only DIRECTLY observed on Legacy, which
		// is why validMatrix() below checks orthonormality before we trust it.
		OFF_AttachMatrixA  = 0x3E0,
		OFF_AttachMatrixB  = 0x3F0,
		OFF_AttachMatrixC  = 0x400,
		OFF_AttachMatrixD  = 0x410,

		OFF_LookAtOffset   = 0x4A0,   // float[2]: x horizontal, y vertical
		OFF_AttachEntity   = 0x4C0,
		OFF_LookAtEntity   = 0x4C8,
	};

	inline int32_t markerIndex(const void* cam)
	{
		return *(const int32_t*)((const uint8_t*)cam + OFF_MarkerIndex);
	}

	inline void* attachEntity(const void* cam)
	{
		return *(void* const*)((const uint8_t*)cam + OFF_AttachEntity);
	}

	inline void* lookAtEntity(const void* cam)
	{
		return *(void* const*)((const uint8_t*)cam + OFF_LookAtEntity);
	}

	// -------------------------------------------------------------------------
	// A Matrix34 in the game's row layout: a = right, b = forward, c = up,
	// d = translation. Same convention the spline's orientation path already
	// uses (see the note on Config::splineOrientation).
	// -------------------------------------------------------------------------
	struct Mat34
	{
		spline::Vec3 a, b, c, d;

		spline::Vec3 transform(const spline::Vec3& v) const
		{
			return a * v.x + b * v.y + c * v.z + d;
		}

		spline::Vec3 transform3x3(const spline::Vec3& v) const
		{
			return a * v.x + b * v.y + c * v.z;
		}
	};

	inline Mat34 attachMatrix(const void* cam)
	{
		const uint8_t* p = (const uint8_t*)cam;
		Mat34 m;
		m.a = spline::Vec3((const float*)(p + OFF_AttachMatrixA));
		m.b = spline::Vec3((const float*)(p + OFF_AttachMatrixB));
		m.c = spline::Vec3((const float*)(p + OFF_AttachMatrixC));
		m.d = spline::Vec3((const float*)(p + OFF_AttachMatrixD));
		return m;
	}

	// -------------------------------------------------------------------------
	// Is this really a rotation matrix?
	//
	// Not paranoia about the game's data - it is a guard on OUR offset. The
	// attach matrix is read by a hard-coded offset that a future patch can move,
	// and the failure mode of getting it wrong is not a slightly-off curve: it
	// is the camera being flung to wherever the garbage transform maps to. An
	// orthonormality test costs nine multiplies and turns that into a silent
	// fall back to stock.
	//
	// It also catches the genuinely-stale case: a camera the game is not
	// currently updating whose matrix was never initialised.
	// -------------------------------------------------------------------------
	inline bool validMatrix(const Mat34& m)
	{
		auto finite3 = [](const spline::Vec3& v) {
			return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
		};
		if (!finite3(m.a) || !finite3(m.b) || !finite3(m.c) || !finite3(m.d))
			return false;

		// Unit rows. A real basis is exact to float precision; 1e-2 is loose
		// enough never to reject a legitimate matrix and tight enough that
		// arbitrary memory essentially never passes all three.
		auto unitish = [](const spline::Vec3& v) {
			const float l2 = v.lengthSq();
			return l2 > 0.98f && l2 < 1.02f;
		};
		if (!unitish(m.a) || !unitish(m.b) || !unitish(m.c)) return false;

		// Mutually perpendicular.
		auto dot = [](const spline::Vec3& u, const spline::Vec3& v) {
			return u.x * v.x + u.y * v.y + u.z * v.z;
		};
		if (fabsf(dot(m.a, m.b)) > 0.02f) return false;
		if (fabsf(dot(m.b, m.c)) > 0.02f) return false;
		if (fabsf(dot(m.a, m.c)) > 0.02f) return false;

		return true;
	}

	// -------------------------------------------------------------------------
	// The director keeps TWO free cameras and blends between them, so "the free
	// camera" is only meaningful with respect to a marker. Both slots are walked
	// and matched on m_MarkerIndex, exactly as the game's own target-steppers do
	// before calling ResetForNewLookAtEntity.
	//
	// director +0x4D0 / +0x4D8, confirmed on both builds in LookAtTargetNext.
	// -------------------------------------------------------------------------
	enum : uint32_t
	{
		DIR_FreeCamera0 = 0x4D0,
		DIR_FreeCamera1 = 0x4D8,
	};

	inline void* forMarker(void* director, int markerIdx)
	{
		if (!director || markerIdx < 0) return nullptr;
		const uint8_t* d = (const uint8_t*)director;

		for (uint32_t off : { (uint32_t)DIR_FreeCamera0, (uint32_t)DIR_FreeCamera1 })
		{
			void* cam = *(void* const*)(d + off);
			if (cam && markerIndex(cam) == markerIdx) return cam;
		}
		return nullptr;
	}

	// -------------------------------------------------------------------------
	// Any live camera that is currently mounted on something.
	//
	// The marker-index match above is the precise question and it is the right
	// one to ask first, but it is not always answerable: the director rebuilds
	// its two cameras when the playhead crosses a marker, and for a handful of
	// frames afterwards neither slot carries the index the segment is asking
	// about. Declining for those frames drops the camera back to stock and then
	// picks it up again, which is a visible snap in and out of the curve - a
	// dropout that measured six to seven frames on a driving shot.
	//
	// Falling back to whichever camera IS mounted is sound here because the
	// segment has already required both its markers to share one attach target,
	// so any live attach matrix belongs to that same entity. Only its ORIGIN is
	// used for the curve, and every camera on one entity agrees about that -
	// UpdateAttachEntityMatrix writes `.d` from the entity transform in all
	// three attach modes.
	// -------------------------------------------------------------------------
	inline void* anyAttached(void* director)
	{
		if (!director) return nullptr;
		const uint8_t* d = (const uint8_t*)director;

		for (uint32_t off : { (uint32_t)DIR_FreeCamera0, (uint32_t)DIR_FreeCamera1 })
		{
			void* cam = *(void* const*)(d + off);
			if (cam && attachEntity(cam)) return cam;
		}
		return nullptr;
	}
}

// =============================================================================
//  Look-at framing
// =============================================================================
//  The free camera's look-at framing solve, reimplemented from the decompile.
//
//  WHY this has to exist here. For a look-at marker the game does not use the
//  marker's stored quaternion at all - its rotation update throws the authored
//  angles away and re-solves the orientation from the target every frame. It
//  re-solves from ITS camera position. We then move the camera along the spline,
//  after UpdateSmoothing, which is the last writer - so nothing re-aims and the
//  subject drifts off its intended framing by roughly (our offset / target
//  distance) radians.
//
//  The game hits exactly this in its own marker-to-marker blend and handles it:
//  the blend re-solves the look-at from the BLENDED position rather than
//  slerping the two endpoint orientations, whenever both markers share a
//  target. This is the same
//  step, done from the splined position.
// =============================================================================
namespace lookat
{
	using spline::Vec3;
	using rfreecam::Mat34;

	inline Vec3 cross(const Vec3& u, const Vec3& v)
	{
		return { u.y * v.z - u.z * v.y,
		         u.z * v.x - u.x * v.z,
		         u.x * v.y - u.y * v.x };
	}

	inline float dot(const Vec3& u, const Vec3& v)
	{
		return u.x * v.x + u.y * v.y + u.z * v.z;
	}

	inline Vec3 normalize(const Vec3& v)
	{
		const float l = v.length();
		return l > 1e-8f ? v * (1.0f / l) : Vec3(0.0f, 1.0f, 0.0f);
	}

	// -------------------------------------------------------------------------
	// Build a basis from a front vector, matching what the game's own helper
	// produces: right = front x Z, up = right x front, then a = front x up.
	//
	// The XAXIS fallback mirrors its behaviour too - a front vector alone cannot
	// define a roll-free basis when it is aligned with world up or down, so one
	// is picked arbitrarily rather than producing a degenerate matrix.
	// -------------------------------------------------------------------------
	// Front AND up given explicitly: b = front, c = up, a = front x up, all
	// normalised. Keeps an authored roll that the from-front form would discard.
	inline void worldMatrixFromFrontAndUp(const Vec3& front, const Vec3& up, Mat34& out)
	{
		out.b = normalize(front);
		out.c = normalize(up);
		out.a = normalize(cross(out.b, out.c));
		out.d = Vec3(0.0f, 0.0f, 0.0f);
	}

	inline void worldMatrixFromFront(const Vec3& front, Mat34& out)
	{
		const Vec3 f = normalize(front);

		Vec3 right = cross(f, Vec3(0.0f, 0.0f, 1.0f));
		if (fabsf(f.z) > 0.99999f) right = Vec3(1.0f, 0.0f, 0.0f);

		worldMatrixFromFrontAndUp(f, cross(right, f), out);
	}

	// -------------------------------------------------------------------------
	// Roll about the camera's own forward axis (Matrix34::RotateLocalY).
	//
	// The SIGN is derived rather than guessed, because getting it backwards
	// tilts an authored roll the wrong way. The game defines roll by its own
	// extractor, camFrame::ComputeRollFromMatrix = atan2(-a.z, c.z). Take a
	// level camera (a.z = 0, c.z = 1) and require that extractor to return +r:
	//
	//     a' = a*cos r - c*sin r   ->   a'.z = -sin r
	//     c' = a*sin r + c*cos r   ->   c'.z =  cos r
	//     atan2(-a'.z, c'.z) = atan2(sin r, cos r) = r
	//
	// That this is the same quantity as the marker's LookAtRoll is not an
	// assumption either: the game's own blend lerps m_LookAtRoll against
	// camFrame::ComputeRoll() in the two arms of one expression, so it treats
	// them as interchangeable.
	// -------------------------------------------------------------------------
	inline void rollAboutFront(Mat34& m, float radians)
	{
		if (fabsf(radians) < 1e-6f) return;
		const float s = sinf(radians), c = cosf(radians);
		const Vec3 a = m.a, u = m.c;
		m.a = a * c - u * s;
		m.c = a * s + u * c;
	}

	// -------------------------------------------------------------------------
	// The aspect ratio the game would have used.
	//
	// It scales the HORIZONTAL screen offset only, so it is irrelevant whenever
	// the marker's horizontal look-at offset is zero - which is the default and
	// the overwhelmingly common case. Rather than resolve gVpMan for a value
	// that usually cannot matter, take the display's, and let the ini pin it for
	// anyone running a stretched or multi-monitor setup. The game's own code
	// falls back to 16:9 when it has no viewport, so that is the last resort.
	// -------------------------------------------------------------------------
	inline float displayAspect()
	{
		static float s_cached = 0.0f;
		if (s_cached > 0.0f) return s_cached;

		const int w = GetSystemMetrics(SM_CXSCREEN);
		const int h = GetSystemMetrics(SM_CYSCREEN);
		s_cached = (w > 0 && h > 0) ? (float)w / (float)h : (16.0f / 9.0f);
		return s_cached;
	}

	// -------------------------------------------------------------------------
	// The look-at framing solve, reimplemented from the decompile.
	//
	// Produces the camera basis that puts `target` at screen position `offset`
	// (x horizontal, y vertical, each -1..1 over the half-frame) as seen from
	// `cameraPos` at `fovDegrees`, with `rollRadians` about the view axis.
	//
	// The game's version scales the screen point by the near clip and then
	// normalises it, so nearZ cancels exactly - which is the one input we would
	// otherwise have to go and find. Dropping it is an algebraic simplification,
	// not an approximation.
	//
	// Returns false if the geometry is degenerate (camera sitting on the
	// target); the caller then leaves the stock orientation alone.
	// -------------------------------------------------------------------------
	inline bool solve(const Vec3& target, const Vec3& cameraPos,
	                  float offsetX, float offsetY,
	                  float fovDegrees, float rollRadians,
	                  float aspect, Mat34& out)
	{
		const Vec3 toTarget = target - cameraPos;
		if (toTarget.lengthSq() < 1e-6f) return false;

		// Orbit frame: forward runs camera -> target, origin AT the target.
		Mat34 orbit;
		worldMatrixFromFront(toTarget, orbit);
		orbit.d = target;

		// Camera position expressed in orbit space. Its y is -distance.
		const Vec3 rel = cameraPos - orbit.d;
		const Vec3 orbitCam(dot(rel, orbit.a), dot(rel, orbit.b), dot(rel, orbit.c));

		// The view ray that should land on the target, in orbit space.
		constexpr float kDeg2Rad = 3.14159265358979f / 180.0f;
		const float tanV = tanf(fovDegrees * kDeg2Rad * 0.5f);
		const float tanH = tanV * aspect;
		const Vec3 ray = normalize(Vec3(tanH * -offsetX, 1.0f, tanV * offsetY));

		// Intersect it with the plane y = 0 (the plane through the target,
		// perpendicular to the view axis). ray.y is positive by construction
		// and orbitCam.y negative, so this always hits in front of the camera.
		if (ray.y < 1e-6f) return false;
		const float t = -orbitCam.y / ray.y;
		const Vec3 hit = orbitCam + ray * t;

		Mat34 local;
		worldMatrixFromFront(hit - orbitCam, local);

		// Roll pivots the orbit basis about the view axis before composing.
		rollAboutFront(orbit, rollRadians);

		// local (orbit space) -> world.
		out.a = orbit.transform3x3(local.a);
		out.b = orbit.transform3x3(local.b);
		out.c = orbit.transform3x3(local.c);
		out.d = cameraPos;

		return isfinite(out.b.x) && isfinite(out.b.y) && isfinite(out.b.z);
	}

	// -------------------------------------------------------------------------
	// Camera basis from heading / pitch / roll, matching
	// camFrame::ComputeWorldMatrixFromHeadingPitchAndRoll (FromEulersYXZ).
	//
	// Written out directly instead of porting RAGE's Euler builder, and verified
	// against the game's own extractors rather than against a remembered
	// convention. For heading h, pitch p, roll r the game reads back
	//   heading = atan2(-b.x, b.y)   pitch = asin(b.z)   roll = atan2(-a.z, c.z)
	// and the construction below satisfies all three identically:
	//   b  = (-sin h cos p,  cos h cos p,  sin p)
	//   a0 = ( cos h,        sin h,        0    )
	//   c0 = ( sin h sin p, -cos h sin p,  cos p)
	// gives cross(b, c0) = a0, atan2(-b.x, b.y) = h and asin(b.z) = p; applying
	// the roll rotation above then yields atan2(-a.z, c.z) = r.
	//
	// This is what an ATTACHED marker stores (m_AttachEntityRelativeHeading /
	// Pitch / Roll, marker +0x3C/+0x40/+0x44) - parent-relative, so the result
	// still has to be rotated by the attach matrix.
	// -------------------------------------------------------------------------
	inline void basisFromHeadingPitchRoll(float h, float p, float r, Mat34& out)
	{
		const float sh = sinf(h), ch = cosf(h);
		const float sp = sinf(p), cp = cosf(p);

		out.b = Vec3(-sh * cp, ch * cp, sp);
		const Vec3 a0(ch, sh, 0.0f);
		const Vec3 c0(sh * sp, -ch * sp, cp);

		const float sr = sinf(r), cr = cosf(r);
		out.a = a0 * cr - c0 * sr;
		out.c = a0 * sr + c0 * cr;
		out.d = Vec3(0.0f, 0.0f, 0.0f);
	}
}
