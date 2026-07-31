// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cmath>
#include <cstdint>
#include "replay/spline.h"

// =============================================================================
//  Marker orientation
// =============================================================================
//  Markers store camera rotation as a 64-bit "smallest three" quaternion.
//  decode() reimplements the game's dequantiser as read off the decompile, so
//  it must stay bit-exact with the encoder: 2-bit largest-component index in
//  the top bits, then three 20-bit components each mapped over
//  [-1/sqrt2, +1/sqrt2]. Verified by round-tripping real marker values.
//
//  Orientation is interpolated with a TIME-AWARE CUBIC HERMITE through the four
//  marker quaternions - hermite() below. Slerp is only C0 at the control
//  points, which puts a direction-change tick at every marker; squad fixes that
//  for evenly spaced keyframes and is kept below for reference, but its control
//  quaternions assume uniform knot spacing, and hand-placed markers are never
//  uniform. Hermite takes its tangents from the real marker TIMES, so the rate
//  arriving at a marker and the rate leaving it agree whatever the spacing.
// =============================================================================
namespace rquat
{
	struct Quat
	{
		float x = 0, y = 0, z = 0, w = 1;

		Quat() = default;
		Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

		Quat operator*(float s) const { return { x * s, y * s, z * s, w * s }; }
		Quat operator+(const Quat& o) const { return { x + o.x, y + o.y, z + o.z, w + o.w }; }

		Quat operator*(const Quat& o) const // Hamilton product
		{
			return {
				w * o.x + x * o.w + y * o.z - z * o.y,
				w * o.y - x * o.z + y * o.w + z * o.x,
				w * o.z + x * o.y - y * o.x + z * o.w,
				w * o.w - x * o.x - y * o.y - z * o.z
			};
		}

		Quat conjugate() const { return { -x, -y, -z, w }; }

		float dot(const Quat& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }

		Quat normalized() const
		{
			const float n = sqrtf(dot(*this));
			if (n < 1e-8f) return {};
			const float inv = 1.0f / n;
			return { x * inv, y * inv, z * inv, w * inv };
		}
	};

	// -------------------------------------------------------------------------
	// The game's dequantiser, reimplemented from the decompile. Layout, bit
	// widths and the [-1/sqrt2, 1/sqrt2] range all mirror its encoder exactly.
	// -------------------------------------------------------------------------
	inline Quat decode(uint64_t packed)
	{
		constexpr uint64_t MASK20 = (1ull << 20) - 1;
		const float oneOverSqrt2 = 1.0f / sqrtf(2.0f);

		auto deq = [&](uint64_t v) {
			// DeQuantize<u64>(v, -k, +k, 20)
			float f = (float)v / (float)((1u << 20) - 1u);
			f *= (oneOverSqrt2 - -oneOverSqrt2);
			f += -oneOverSqrt2;
			return f;
		};

		const uint64_t largestIdx = packed >> 60;

		float o[3];
		o[0] = deq((packed >> 40) & MASK20);
		o[1] = deq((packed >> 20) & MASK20);
		o[2] = deq(packed & MASK20);

		const float total2 = o[0] * o[0] + o[1] * o[1] + o[2] * o[2];
		// The encoder flips the quaternion so the dropped component is positive,
		// which is what makes reconstructing it from the unit-norm constraint
		// unambiguous. Clamp: quantisation error can push total2 just past 1.
		const float largest = sqrtf(total2 >= 1.0f ? 0.0f : 1.0f - total2);

		switch (largestIdx)
		{
		case 0:  return { largest, o[0],    o[1],    o[2]    };
		case 1:  return { o[0],    largest, o[1],    o[2]    };
		case 2:  return { o[0],    o[1],    largest, o[2]    };
		default: return { o[0],    o[1],    o[2],    largest };
		}
	}

	// -------------------------------------------------------------------------
	// The inverse of decode() above, needed to write a pose back into a marker.
	//
	// The encoder flips the quaternion so the dropped (largest) component is
	// positive; that is what lets the decoder reconstruct it from the unit-norm
	// constraint without a sign bit.
	//
	// Truncates rather than rounds, matching the game's Quantize<> helper. The
	// resulting error is under one part in 2^20 either way.
	// -------------------------------------------------------------------------
	inline uint64_t encode(const Quat& in)
	{
		const Quat q = in.normalized();
		const float c[4] = { q.x, q.y, q.z, q.w };

		int largest = 0;
		float largestAbs = fabsf(c[0]);
		for (int i = 1; i < 4; ++i)
		{
			const float a = fabsf(c[i]);
			if (a > largestAbs) { largestAbs = a; largest = i; }
		}

		// Flip so the dropped component is positive.
		const float sign = c[largest] < 0.0f ? -1.0f : 1.0f;

		const float oneOverSqrt2 = 1.0f / sqrtf(2.0f);
		auto quant = [&](float v) -> uint64_t {
			float f = (v * sign - -oneOverSqrt2) / (oneOverSqrt2 - -oneOverSqrt2);
			f *= (float)((1u << 20) - 1u);
			if (f < 0.0f) f = 0.0f;
			if (f > (float)((1u << 20) - 1u)) f = (float)((1u << 20) - 1u);
			return (uint64_t)f;
		};

		float others[3];
		for (int i = 0, k = 0; i < 4; ++i)
			if (i != largest) others[k++] = c[i];

		uint64_t out = (uint64_t)largest << 60;
		out |= quant(others[0]) << 40;
		out |= quant(others[1]) << 20;
		out |= quant(others[2]);
		return out;
	}

	inline Quat fromAxisAngle(float ax, float ay, float az, float radians)
	{
		const float h = radians * 0.5f;
		const float s = sinf(h);
		return { ax * s, ay * s, az * s, cosf(h) };
	}

	inline Quat slerp(Quat a, Quat b, float t)
	{
		float d = a.dot(b);
		// Take the short way round; -q is the same rotation.
		if (d < 0.0f) { b = b * -1.0f; d = -d; }

		if (d > 0.9995f) // nearly parallel — lerp avoids the sin(0) blowup
			return (a + (Quat{ b.x - a.x, b.y - a.y, b.z - a.z, b.w - a.w } * t)).normalized();

		const float theta = acosf(d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d));
		const float sinTheta = sinf(theta);
		const float wa = sinf((1.0f - t) * theta) / sinTheta;
		const float wb = sinf(t * theta) / sinTheta;
		return (a * wa + b * wb).normalized();
	}

	inline Quat log(const Quat& q)
	{
		const float vLen = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
		if (vLen < 1e-8f) return { 0, 0, 0, 0 };
		const float a = atan2f(vLen, q.w) / vLen;
		return { q.x * a, q.y * a, q.z * a, 0.0f };
	}

	inline Quat exp(const Quat& q)
	{
		const float vLen = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
		if (vLen < 1e-8f) return { 0, 0, 0, 1 };
		const float s = sinf(vLen) / vLen;
		return { q.x * s, q.y * s, q.z * s, cosf(vLen) };
	}

	// -------------------------------------------------------------------------
	// Bring `q` onto the same hemisphere as `ref`.
	//
	// NOT optional, and its absence was a real bug. q and -q are the same
	// rotation, and decode() returns whichever representative has its LARGEST
	// component positive - a choice that has nothing to do with the previous
	// marker's. Two nearly identical camera angles can therefore come back as
	// q and -q, and any spline through the raw components then runs a full turn
	// between them. slerp copes on its own (it flips internally); a component
	// spline and squad's log() both do not.
	// -------------------------------------------------------------------------
	inline Quat align(Quat q, const Quat& ref)
	{
		if (q.dot(ref) < 0.0f) q = q * -1.0f;
		return q;
	}

	// -------------------------------------------------------------------------
	// Cubic Hermite through four orientations at four marker TIMES, evaluated
	// at `now`. This is the orientation curve.
	//
	// Component-wise, then renormalised. squad below gives exactly uniform
	// angular speed inside a segment and is the textbook answer, but its
	// control quaternions are built assuming uniformly spaced knots - so with
	// hand-placed markers it is not C1 ACROSS them, and the across-markers
	// discontinuity is the one you can actually see. This is C1 across markers
	// by construction, and its within-segment speed error is a fraction of a
	// percent at the angles adjacent keyframes actually differ by.
	//
	// The four inputs must already share a hemisphere - see align().
	// -------------------------------------------------------------------------
	inline Quat hermite(const Quat q[4], const float t[4], float now)
	{
		const float x[4] = { q[0].x, q[1].x, q[2].x, q[3].x };
		const float y[4] = { q[0].y, q[1].y, q[2].y, q[3].y };
		const float z[4] = { q[0].z, q[1].z, q[2].z, q[3].z };
		const float w[4] = { q[0].w, q[1].w, q[2].w, q[3].w };

		return Quat(spline::hermiteAt(t, x, now),
		            spline::hermiteAt(t, y, now),
		            spline::hermiteAt(t, z, now),
		            spline::hermiteAt(t, w, now)).normalized();
	}

	// -------------------------------------------------------------------------
	// squad, kept for reference. Superseded by hermite() above, which handles
	// non-uniform marker spacing; see the note at the top of this file.
	// -------------------------------------------------------------------------
	// Intermediate ("inner") control quaternion for squad at q1, given its
	// neighbours. This is what buys C1 continuity through the control point.
	inline Quat squadControl(const Quat& q0, const Quat& q1, const Quat& q2)
	{
		const Quat inv = q1.conjugate();
		const Quat l0 = log(inv * q0);
		const Quat l2 = log(inv * q2);
		const Quat sum{ -(l0.x + l2.x) * 0.25f,
		                -(l0.y + l2.y) * 0.25f,
		                -(l0.z + l2.z) * 0.25f, 0.0f };
		return (q1 * exp(sum)).normalized();
	}

	inline Quat squad(const Quat& q1, const Quat& q2,
	                  const Quat& s1, const Quat& s2, float t)
	{
		return slerp(slerp(q1, q2, t), slerp(s1, s2, t), 2.0f * t * (1.0f - t));
	}

	// -------------------------------------------------------------------------
	// Basis rows -> quaternion. The exact inverse of toMatrixRows below, and
	// derived from it rather than copied from a reference, so the two cannot
	// drift apart: the trace identity a.x + b.y + c.z = 4w^2 - 1 gives w, and
	// the three antisymmetric pairs then give x, y, z.
	//
	// The branch on the largest diagonal element is not decoration - taking the
	// w route when w is near zero divides by it, and a camera turned 180 degrees
	// from identity is an ordinary shot, not a corner case.
	//
	// Needed because an ATTACHED marker's world orientation has to be built per
	// marker (its own frozen attach frame times its own authored angles) before
	// anything can be interpolated. See attachWorldQuat in smoothblend.cpp.
	// -------------------------------------------------------------------------
	inline Quat fromBasis(const spline::Vec3& a, const spline::Vec3& b, const spline::Vec3& c)
	{
		const float trace = a.x + b.y + c.z;

		if (trace > 0.0f)
		{
			const float s = 2.0f * sqrtf(trace + 1.0f);   // = 4w
			return Quat((b.z - c.y) / s, (c.x - a.z) / s, (a.y - b.x) / s, 0.25f * s).normalized();
		}
		if (a.x > b.y && a.x > c.z)
		{
			const float s = 2.0f * sqrtf(1.0f + a.x - b.y - c.z);   // = 4x
			return Quat(0.25f * s, (a.y + b.x) / s, (a.z + c.x) / s, (b.z - c.y) / s).normalized();
		}
		if (b.y > c.z)
		{
			const float s = 2.0f * sqrtf(1.0f - a.x + b.y - c.z);   // = 4y
			return Quat((a.y + b.x) / s, 0.25f * s, (b.z + c.y) / s, (c.x - a.z) / s).normalized();
		}
		const float s = 2.0f * sqrtf(1.0f - a.x - b.y + c.z);       // = 4z
		return Quat((a.z + c.x) / s, (b.z + c.y) / s, 0.25f * s, (a.y - b.x) / s).normalized();
	}

	// -------------------------------------------------------------------------
	// Quaternion -> RAGE Matrix34 basis rows (a = right, b = forward, c = up).
	// Rows are written at 16-byte stride; the caller supplies row pointers.
	// -------------------------------------------------------------------------
	inline void toMatrixRows(const Quat& q, float* a, float* b, float* c)
	{
		const Quat n = q.normalized();
		const float xx = n.x * n.x, yy = n.y * n.y, zz = n.z * n.z;
		const float xy = n.x * n.y, xz = n.x * n.z, yz = n.y * n.z;
		const float wx = n.w * n.x, wy = n.w * n.y, wz = n.w * n.z;

		a[0] = 1.0f - 2.0f * (yy + zz); a[1] = 2.0f * (xy + wz);        a[2] = 2.0f * (xz - wy);
		b[0] = 2.0f * (xy - wz);        b[1] = 1.0f - 2.0f * (xx + zz); b[2] = 2.0f * (yz + wx);
		c[0] = 2.0f * (xz + wy);        c[1] = 2.0f * (yz - wx);        c[2] = 1.0f - 2.0f * (xx + yy);
	}
}
