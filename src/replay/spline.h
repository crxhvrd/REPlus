// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cmath>
#include <cstdint>

// =============================================================================
//  The actual fix: a real spline to replace Rockstar's lerp+lag-filter.
// =============================================================================
//  Stock "Smooth Blend" is a two-point Lerp between markers with a damped
//  spring chasing it. A spring is a causal
//  low-pass filter, so it trails the target and undershoots every corner —
//  which is why the camera visibly misses markers and feels laggy.
//
//  Catmull-Rom fixes both properties at once: it passes exactly through its
//  control points and is a pure function of t (no state, so scrubbing
//  backwards gives the identical path — stock disables smoothing entirely
//  when rewinding precisely because a spring cannot run in reverse).
//
//  Alpha (the menu's "Spline Tension") picks the knot spacing: 0 uniform,
//  0.5 centripetal, 1.0 chordal. It DEFAULTS TO 0, which is not the textbook
//  answer and is deliberate.
//
//  Centripetal is normally recommended because it provably never cusps or
//  self-intersects (Yuksel et al., "Parameterization and Applications of
//  Catmull-Rom Curves") - but that theorem is about a curve within ONE segment,
//  and the failure it prevents is overshoot between well-spaced control points.
//  It buys that by shortening the tangent wherever a neighbouring chord is
//  short, and at a direction REVERSAL that is exactly backwards: a short
//  tangent leaves the curve no momentum to sweep around, so it pokes past the
//  marker and hooks back across its own path. Measured on a real shot with a
//  160-degree reversal, arc length over chord around the marker:
//
//      alpha 0.00 -> 1.11       alpha 0.50 -> 1.56       alpha 1.00 -> 3.59
//
//  i.e. the guarantee holds and is protecting against the wrong thing. Hand
//  placed camera markers reverse direction constantly; they are rarely the
//  smooth monotone polylines the theorem has in mind.
//
//  0 also matches the sibling Simple Camera project, whose Hermite is
//  parameterised by TIME - with roughly even marker spacing that IS uniform
//  Catmull-Rom. Raise alpha per marker where a shot wants to be held tight to
//  its marks rather than flowing between them.
// =============================================================================
namespace spline
{
	struct Vec3
	{
		float x = 0, y = 0, z = 0;

		Vec3() = default;
		Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
		explicit Vec3(const float* p) : x(p[0]), y(p[1]), z(p[2]) {}

		Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		Vec3 operator*(float s)       const { return { x * s,   y * s,   z * s }; }

		float lengthSq() const { return x * x + y * y + z * z; }
		float length()   const { return sqrtf(lengthSq()); }

		void store(float* p) const { p[0] = x; p[1] = y; p[2] = z; }
	};

	// -------------------------------------------------------------------------
	// Non-uniform Catmull-Rom via the Barry-Goldman pyramid.
	//
	// Evaluating this way (rather than the uniform basis-matrix form) is what
	// lets alpha do its job: knot spacing follows |dP|^alpha, so alpha=0 gives
	// uniform, 0.5 centripetal, 1.0 chordal.
    //
	// t is in [0,1] across the P1 -> P2 segment.
	// -------------------------------------------------------------------------
	inline Vec3 catmullRom(const Vec3& p0, const Vec3& p1,
	                       const Vec3& p2, const Vec3& p3,
	                       float t, float alpha)
	{
		auto knot = [alpha](float ti, const Vec3& a, const Vec3& b) {
			const float d = (b - a).length();
			// Guard coincident control points: a zero interval would divide by
			// zero below. Nudge instead of bailing so the curve stays defined.
			return ti + powf(d < 1e-6f ? 1e-6f : d, alpha);
		};

		const float t0 = 0.0f;
		const float t1 = knot(t0, p0, p1);
		const float t2 = knot(t1, p1, p2);
		const float t3 = knot(t2, p2, p3);

		// Map the caller's normalised t onto the real [t1,t2] knot interval.
		const float tt = t1 + (t2 - t1) * t;

		const Vec3 a1 = p0 * ((t1 - tt) / (t1 - t0)) + p1 * ((tt - t0) / (t1 - t0));
		const Vec3 a2 = p1 * ((t2 - tt) / (t2 - t1)) + p2 * ((tt - t1) / (t2 - t1));
		const Vec3 a3 = p2 * ((t3 - tt) / (t3 - t2)) + p3 * ((tt - t2) / (t3 - t2));

		const Vec3 b1 = a1 * ((t2 - tt) / (t2 - t0)) + a2 * ((tt - t0) / (t2 - t0));
		const Vec3 b2 = a2 * ((t3 - tt) / (t3 - t1)) + a3 * ((tt - t1) / (t3 - t1));

		return b1 * ((t2 - tt) / (t2 - t1)) + b2 * ((tt - t1) / (t2 - t1));
	}

	// -------------------------------------------------------------------------
	// Phantom endpoints.
	//
	// At the first/last marker there is no neighbour to take a tangent from.
	// Reflecting the segment (p1 + (p1 - p2)) keeps the end tangent finite and
	// matching the segment direction, so the curve leaves the first marker
	// heading at the second rather than kinking.
	//
	// Fallback only - naturalEnd below is what the clip's ends actually use.
	// Reflection is the RIGHT shape when there is nothing else to go on (a
	// two-marker clip, where a straight line is the only defined answer) and
	// the wrong one whenever there is.
	// -------------------------------------------------------------------------
	inline Vec3 phantom(const Vec3& inner, const Vec3& outer)
	{
		return inner + (inner - outer);
	}

	// -------------------------------------------------------------------------
	// Natural end condition - the phantom that lets a clip's FIRST segment know
	// where it is going.
	//
	// The reflected phantom above has a property that is easy to miss and very
	// visible on screen: it makes the Catmull-Rom tangent at the endpoint come
	// out as EXACTLY the chord.
	//
	//     c1 = 2*P0 - P1   =>   m0 = (P1 - c1)/2 = P1 - P0
	//
	// So the curve leaves the first marker in a straight line aimed at the
	// second, and only begins to turn once the far control point starts to
	// weigh in - the move looks like it has not yet noticed the rest of the
	// shot. Every interior marker gets a tangent that straddles its neighbours;
	// only the two ends are blind, which is why this reads as "the spline works
	// in the middle of the clip but not at the start".
	//
	// The standard cure is the natural end condition, C''= 0 at the endpoint,
	// which is what a draughtsman's spline does when it is not clamped. Solving
	// the Hermite second derivative at u=0 against the known interior tangent at
	// the far end (m1 = (P2 - P0)/2) gives
	//
	//     m0 = 1.5*(P1 - P0) - 0.5*m1        ->        c1 = 2.5*P0 - 2*P1 + 0.5*P2
	//
	// so the start now leans on P2 and arcs into the turn instead of aiming at
	// P1 and swinging late. It stays a bounded combination of three real points,
	// so it cannot fling the curve the way an unbounded extrapolation could, and
	// it degenerates back to the plain reflection exactly when the three points
	// are collinear and evenly spaced - i.e. it only changes what it should.
	//
	// The algebra is the uniform one; with alpha != 0 the knot spacing shifts it
	// slightly, so the result is very nearly natural rather than exactly so.
	// That is a fair trade for keeping one evaluator.
	//
	// Pass the endpoint first, then the two markers heading away from it - so
	// the clip's tail calls it with (last, secondLast, thirdLast).
	// -------------------------------------------------------------------------
	inline Vec3 naturalEnd(const Vec3& end, const Vec3& next1, const Vec3& next2)
	{
		return end * 2.5f - next1 * 2.0f + next2 * 0.5f;
	}

	// -------------------------------------------------------------------------
	// Trapezoidal ease.
	//
	// Shapes the *distance* profile along the segment: accelerate over the
	// first `easeIn` of the segment, coast, decelerate over the last `easeOut`.
	// Returns the fraction of arc length that should have been covered at t,
	// so it must be applied BEFORE arcLengthRemap (which maps distance ->
	// curve parameter).
	//
	// Peak speed is solved so the areas still integrate to exactly 1, which is
	// what keeps the camera arriving at the next marker precisely on time
	// regardless of the ease amounts.
	// -------------------------------------------------------------------------
	inline float ease(float t, float easeIn, float easeOut)
	{
		if (easeIn <= 0.0f && easeOut <= 0.0f) return t;

		// Degenerate request (ramps longer than the segment): scale both down
		// rather than refusing, so the UI can't produce an unusable curve.
		float in = easeIn < 0.0f ? 0.0f : easeIn;
		float out = easeOut < 0.0f ? 0.0f : easeOut;
		const float sum = in + out;
		if (sum > 1.0f) { in /= sum; out /= sum; }

		const float vMax = 1.0f / (1.0f - (in + out) * 0.5f);

		if (t < in && in > 0.0f)
			return 0.5f * vMax * (t * t) / in;

		const float sIn = 0.5f * vMax * in;
		if (t <= 1.0f - out)
			return sIn + vMax * (t - in);

		const float r = 1.0f - t;
		return out > 0.0f ? 1.0f - 0.5f * vMax * (r * r) / out : 1.0f;
	}

	// -------------------------------------------------------------------------
	// Monotone cubic (Fritsch-Carlson / PCHIP) time -> distance.
	//
	// THIS is what makes motion smooth across keyframes, and it is the piece a
	// per-segment arc-length remap cannot give you.
	//
	// Normalising each segment on its own forces speed to len/duration inside
	// that segment, so at a marker it steps straight to the next segment's
	// ratio — position stays continuous but velocity does not, which is felt as
	// a kick at every keyframe. Fitting one monotone curve through the
	// (marker time, cumulative distance) knots instead gives:
	//   * exact marker timing        — the curve interpolates the knots
	//   * C1 speed across markers    — no kick
	//   * monotone by construction   — the camera can never track backwards,
	//                                  which a plain cubic through the same
	//                                  knots absolutely can.
	//
	// t[] / s[] are 4 knots; we evaluate on the [t1,t2] span and return the
	// distance travelled. Tangents at t1 and t2 look at their neighbours, which
	// is where the cross-marker continuity comes from.
	// -------------------------------------------------------------------------
	// The curve above is described in terms of distance because that is what it
	// was written for, but nothing in it is distance-specific: it is a monotone
	// cubic through four (x, y) knots, evaluated on the [x1, x2] span. FOV wants
	// exactly the same curve over (marker time -> degrees), so the generic name
	// is the real one and pchipDistance is kept as the distance-flavoured alias.
	inline float pchipAt(const float* t, const float* s, float now)
	{
		auto h = [&](int k) { return t[k + 1] - t[k]; };
		auto d = [&](int k) {
			const float hk = h(k);
			return hk > 1e-6f ? (s[k + 1] - s[k]) / hk : 0.0f;
		};

		// Fritsch-Carlson tangent: zero at a local extremum (that is what
		// enforces monotonicity), otherwise a weighted harmonic mean of the
		// neighbouring secants.
		auto tangent = [&](int k) {
			const float dPrev = d(k - 1), dNext = d(k);
			if (dPrev * dNext <= 0.0f) return 0.0f;
			const float hPrev = h(k - 1), hNext = h(k);
			const float w1 = 2.0f * hNext + hPrev;
			const float w2 = hNext + 2.0f * hPrev;
			return (w1 + w2) / (w1 / dPrev + w2 / dNext);
		};

		const float m1 = tangent(1);
		const float m2 = tangent(2);

		const float hh = h(1);
		if (hh <= 1e-6f) return s[1];

		float u = (now - t[1]) / hh;
		if (!(u > 0.0f)) u = 0.0f;
		if (u > 1.0f)    u = 1.0f;

		const float u2 = u * u, u3 = u2 * u;
		return (2.0f * u3 - 3.0f * u2 + 1.0f) * s[1]
		     + (u3 - 2.0f * u2 + u)     * hh * m1
		     + (-2.0f * u3 + 3.0f * u2) * s[2]
		     + (u3 - u2)                * hh * m2;
	}

	inline float pchipDistance(const float* t, const float* s, float now)
	{
		return pchipAt(t, s, now);
	}

	// -------------------------------------------------------------------------
	// Time-aware cubic Hermite through the same four knots.
	//
	// Identical job and identical signature to pchipAt, with one deliberate
	// difference: the tangents are plain time-weighted secants rather than
	// Fritsch-Carlson's monotone ones.
	//
	// Which one is right depends entirely on the channel. FOV wants pchip -
	// a zoom that dips past its target and comes back reads as a wobble, and
	// monotonicity is what forbids that. ROTATION wants this one, because
	// pchip's monotonicity is enforced by ZEROING the tangent at every local
	// extremum: a camera that pans right, then left, has an extremum at the
	// marker between them, and pchip brings the pan to a dead stop there
	// before reversing. A plain Hermite turns through it.
	//
	// C1 across markers comes from the tangent at a knot depending only on its
	// two neighbours - the segment arriving at t1 and the segment leaving it
	// compute the same m1 - so the rate matches across the join even when the
	// two segments have very different durations. That last clause is the
	// whole point: a uniform-parameter cubic loses the property the moment
	// durations differ, which is the classic "camera jerks at every keyframe".
	// -------------------------------------------------------------------------
	inline float hermiteAt(const float* t, const float* s, float now)
	{
		const float h = t[2] - t[1];
		if (h <= 1e-6f) return s[1];

		// Central secant across the knot, weighted by the real time either
		// side of it. Degenerate spacing gives a flat tangent rather than a
		// division by zero.
		auto tangent = [&](int k) {
			const float span = t[k + 1] - t[k - 1];
			return span > 1e-6f ? (s[k + 1] - s[k - 1]) / span : 0.0f;
		};

		const float m1 = tangent(1);
		const float m2 = tangent(2);

		float u = (now - t[1]) / h;
		if (!(u > 0.0f)) u = 0.0f;
		if (u > 1.0f)    u = 1.0f;

		const float u2 = u * u, u3 = u2 * u;
		return (2.0f * u3 - 3.0f * u2 + 1.0f) * s[1]
		     + (u3 - 2.0f * u2 + u)     * h * m1
		     + (-2.0f * u3 + 3.0f * u2) * s[2]
		     + (u3 - u2)                * h * m2;
	}
	// -------------------------------------------------------------------------
	// The same time-aware Hermite, on a position rather than a scalar.
	//
	// This IS the sibling Simple Camera project's path construction: tangents
	// are plain secants weighted by the real marker times, so the rate arriving
	// at a marker equals the rate leaving it however unevenly the segments are
	// spaced - and the camera's speed is then whatever the curve's own
	// parameterisation gives, which is what makes a shot ease into tight bends
	// and flow through open ones.
	//
	// Deliberately NOT "uniform Catmull-Rom evaluated at normalised segment
	// time", which looks equivalent and is not: that is continuous in position
	// but steps in VELOCITY as soon as two segments have different durations,
	// because its parameter rate is 1/duration. Measured on a real shot, the
	// velocity jump at three markers was 0.23 / 0.38 / 0.33 m/s that way and
	// 0.01 / 0.00 / 0.01 this way.
	// -------------------------------------------------------------------------
	inline Vec3 hermitePos(const Vec3 p[4], const float t[4], float now)
	{
		const float x[4] = { p[0].x, p[1].x, p[2].x, p[3].x };
		const float y[4] = { p[0].y, p[1].y, p[2].y, p[3].y };
		const float z[4] = { p[0].z, p[1].z, p[2].z, p[3].z };
		return Vec3(hermiteAt(t, x, now), hermiteAt(t, y, now), hermiteAt(t, z, now));
	}


	// Length of one Catmull-Rom segment, by sampling.
	//
	// SAMPLES must match paramAtDistance's, and not approximately. A polyline
	// through N samples UNDERestimates the curve, so a coarser count here than
	// there makes the length the speed profile clamps against smaller than the
	// total of the table that same distance is then looked up in - the camera
	// tops out short of the marker and steps the remainder. Same constant, one
	// reason.
	inline constexpr int kArcSamples = 32;

	inline float segmentLength(const Vec3& p0, const Vec3& p1,
	                           const Vec3& p2, const Vec3& p3, float alpha)
	{
		constexpr int SAMPLES = kArcSamples;
		float total = 0.0f;
		Vec3 prev = catmullRom(p0, p1, p2, p3, 0.0f, alpha);
		for (int i = 1; i <= SAMPLES; ++i)
		{
			const Vec3 cur = catmullRom(p0, p1, p2, p3, (float)i / SAMPLES, alpha);
			total += (cur - prev).length();
			prev = cur;
		}
		return total;
	}

	// Invert "distance along this segment" back to a curve parameter.
	inline float paramAtDistance(const Vec3& p0, const Vec3& p1,
	                             const Vec3& p2, const Vec3& p3,
	                             float dist, float alpha)
	{
		constexpr int SAMPLES = kArcSamples;
		float acc[SAMPLES + 1];
		acc[0] = 0.0f;

		Vec3 prev = catmullRom(p0, p1, p2, p3, 0.0f, alpha);
		for (int i = 1; i <= SAMPLES; ++i)
		{
			const Vec3 cur = catmullRom(p0, p1, p2, p3, (float)i / SAMPLES, alpha);
			acc[i] = acc[i - 1] + (cur - prev).length();
			prev = cur;
		}

		const float total = acc[SAMPLES];
		if (total < 1e-5f) return 0.0f;
		if (dist <= 0.0f)  return 0.0f;
		if (dist >= total) return 1.0f;

		for (int i = 1; i <= SAMPLES; ++i)
		{
			if (acc[i] >= dist)
			{
				const float span = acc[i] - acc[i - 1];
				const float frac = span > 1e-6f ? (dist - acc[i - 1]) / span : 0.0f;
				return ((float)(i - 1) + frac) / SAMPLES;
			}
		}
		return 1.0f;
	}

	// -------------------------------------------------------------------------
	// Arc-length reparameterisation (legacy, per-segment).
	//
	// A Catmull-Rom segment is not traversed at constant speed for uniform t:
	// where the curve bulges, the camera visibly accelerates. We sample the
	// segment, build a cumulative length table, and invert it so equal t means
	// equal distance.
	//
	// The inversion is piecewise LINEAR, so the parameter's rate of change is
	// piecewise constant and steps once per sample - a speed ripple at
	// SAMPLES-per-segment. At 16 that is a handful of steps per second on a
	// slow move, which is inside the range the eye picks up as roughness; 32
	// halves the step size for ~30 extra curve evaluations a frame, which is
	// nothing next to a replay frame.
	//
	// Endpoints are preserved exactly, so marker timing is untouched — this
	// only redistributes *within* the segment.
	// -------------------------------------------------------------------------
	inline float arcLengthRemap(const Vec3& p0, const Vec3& p1,
	                            const Vec3& p2, const Vec3& p3,
	                            float t, float alpha)
	{
		constexpr int SAMPLES = kArcSamples;
		float acc[SAMPLES + 1];
		acc[0] = 0.0f;

		Vec3 prev = catmullRom(p0, p1, p2, p3, 0.0f, alpha);
		for (int i = 1; i <= SAMPLES; ++i)
		{
			const Vec3 cur = catmullRom(p0, p1, p2, p3, (float)i / SAMPLES, alpha);
			acc[i] = acc[i - 1] + (cur - prev).length();
			prev = cur;
		}

		const float total = acc[SAMPLES];
		if (total < 1e-5f) return t; // degenerate segment — nothing to remap

		const float target = t * total;
		for (int i = 1; i <= SAMPLES; ++i)
		{
			if (acc[i] >= target)
			{
				const float span = acc[i] - acc[i - 1];
				const float frac = span > 1e-6f ? (target - acc[i - 1]) / span : 0.0f;
				return ((float)(i - 1) + frac) / SAMPLES;
			}
		}
		return t;
	}
}
