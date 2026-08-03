// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include <fstream>
#include "main.h"
#include "utils/paths.h"
#include "replay/smoothblend.h"
#include "replay/marker.h"
#include "replay/spline.h"
#include "replay/quat.h"
#include "replay/freecam.h"
#include "replay/settings.h"
#include "replay/shake.h"
#include "capture/render.h"
#include "capture/dofsession.h"

using spline::Vec3;

namespace smoothblend
{
	static void(__fastcall* origUpdateSmoothing)(void*) = nullptr;

	// Written on the sim thread, read on the render thread by the overlay.
	// A single aligned pointer, so a volatile write is enough here: we only
	// null-check it and then read fields the director owns for its whole
	// lifetime. It saves resolving the director global separately.
	static void* volatile s_lastDirector = nullptr;

	void* lastDirector() { return (void*)s_lastDirector; }

	// A marker is only usable as a control point if it lives in the same
	// coordinate space as the rest of the curve: free camera, same attach
	// target. Anything else is reported absent so the caller mirrors a phantom
	// instead.
	static void* controlMarker(void* storage, int index, int32_t segmentAttach)
	{
		void* m = rstorage::tryGetMarker(storage, index);
		return rmarker::isSplineable(m, segmentAttach) ? m : nullptr;
	}

	// -------------------------------------------------------------------------
	// The marker physically before or after `from`, for the curve's outer
	// control points.
	//
	// Deliberately NOT the director's GetNextMarkerIndex. That helper encodes
	// editor policy which has nothing to do with tangents: it skips anchors,
	// and it drops the FINAL marker entirely whenever the one before it is not
	// blending -
	//
	//     if (!isSecondFromEndMarkerBlending) --numMarkers;
	//
	// so on any project containing a cut it stops handing back the last marker.
	// Used for a window, that silently removes an outer control point, and a
	// Catmull-Rom segment missing BOTH of its outer points is not merely
	// flatter - it is exactly the straight line between its endpoints, because
	// the reflected phantoms give both ends the same tangent. A curve that
	// quietly degenerates into the lerp it was meant to replace.
	//
	// Anchors are still skipped: they carry no camera of their own. A neighbour
	// in a different space ends the chain rather than being skipped past, since
	// interpolating across it would be meaningless.
	// -------------------------------------------------------------------------
	static int neighbourIndex(void* storage, int from, int dir, int32_t segmentAttach)
	{
		const int count = rstorage::markerCount(storage);
		for (int i = from + dir; i >= 0 && i < count; i += dir)
		{
			void* m = rstorage::tryGetMarker(storage, i);
			if (!m) continue;
			if (rmarker::markerType(m) == rmarker::MARKERTYPE_ANCHOR) continue;
			return rmarker::isSplineable(m, segmentAttach) ? i : -1;
		}
		return -1;
	}

	// -------------------------------------------------------------------------
	// A mounted marker's offset, resolved into a WORLD-SPACE DISPLACEMENT from
	// the parent's origin.
	//
	// This is what makes the three attach modes interpolable together, and it is
	// not the obvious approach - the obvious one is to interpolate the stored
	// offsets in the parent's frame and rotate once at the end. That is only
	// correct for FULL, where the rotation is the live entity matrix and so is
	// shared by every marker in the segment.
	//
	// In the other two modes the game never refreshes the attach matrix's
	// rotation, so each marker keeps whatever the parent was pointing at when it
	// was created (+0x10). Two markers authored either side of a corner store
	// their offsets against bases a quarter turn apart, and rotating both by
	// one of them puts the other metres away. Resolving per marker first and
	// interpolating the results afterwards avoids the question entirely:
	//
	//     world = parentOrigin + interpolate( R_i * offset_i )
	//
	// and for FULL that is algebraically identical to the old path, because
	// R is then the same for every i.
	// -------------------------------------------------------------------------
	// THIS marker's attach frame, resolved. Split out of attachDisplacement
	// because the ORIENTATION needs exactly the same thing and getting it from
	// somewhere else is what made the camera snap at every keyframe.
	static rfreecam::Mat34 attachFrame(const void* m, const rfreecam::Mat34& live)
	{
		const uint8_t type = rmarker::attachType(m);

		// FULL tracks the parent completely, so the live rotation IS this
		// marker's rotation - no need to reconstruct anything. This is also why
		// only the OTHER two modes could ever teleport: they are the only ones
		// that read a stored frame.
		if (type == rmarker::ATTACH_FULL) return live;

		// The stored frame has to be declared trustworthy before it is used, and
		// an orthonormality check does NOT establish that - decoding always
		// yields a unit quaternion and a clean basis, so a stale field passes
		// every structural test while pointing somewhere else entirely, and the
		// camera jumps by whatever that rotation does to the offset. The game
		// gates on the same flag and falls back the same way.
		if (!rmarker::attachMatrixValid(m)) return live;

		rfreecam::Mat34 R;
		{
			float a[3], b[3], c[3];
			rquat::toMatrixRows(rquat::decode(rmarker::attachQuat(m)), a, b, c);
			R.a = Vec3(a); R.b = Vec3(b); R.c = Vec3(c);
			R.d = Vec3(0.0f, 0.0f, 0.0f);
		}
		if (!rfreecam::validMatrix(R)) return live;

		if (type == rmarker::ATTACH_POSITION_AND_HEADING)
		{
			// Heading follows the parent; pitch and roll stay as authored. Same
			// decomposition the game uses, so the same three angles come out.
			const float pitch = asinf(R.b.z < -1.0f ? -1.0f : (R.b.z > 1.0f ? 1.0f : R.b.z));
			const float roll  = atan2f(-R.a.z, R.c.z);
			const float liveHeading = atan2f(-live.b.x, live.b.y);
			lookat::basisFromHeadingPitchRoll(liveHeading, pitch, roll, R);
		}

		return R;
	}

	static Vec3 attachDisplacement(const void* m, const rfreecam::Mat34& live)
	{
		return attachFrame(m, live).transform3x3(Vec3(rmarker::positionRelative(m)));
	}

	// -------------------------------------------------------------------------
	// A mounted marker's WORLD orientation.
	//
	// The same argument attachDisplacement makes for position, applied to
	// rotation - and not applying it here is what put a snap on every interior
	// keyframe of a moving-vehicle shot.
	//
	// An attached marker authors heading/pitch/roll in its parent's frame, and
	// outside FULL the game never refreshes that frame, so each marker keeps
	// whatever the vehicle was pointing at when it was created. Interpolating
	// the three angles and then applying ONE parent frame therefore asks which
	// marker's frame to use - and the answer changed at every marker crossing,
	// because the director rebuilds its cameras there and we read the new one.
	// Measured on a driving shot: the frame rotated 4.9 degrees between two
	// markers, and the camera's aim snapped by exactly that at the join.
	//
	// Resolving each marker through its OWN frame first removes the question,
	// exactly as it did for position. For FULL the result is algebraically
	// unchanged: every marker then shares the live rotation, and
	// left-multiplying by a common rotation commutes with the component-wise
	// Hermite used below.
	// -------------------------------------------------------------------------
	static rquat::Quat attachWorldQuat(const void* m, const rfreecam::Mat34& live)
	{
		const rfreecam::Mat34 R = attachFrame(m, live);

		rfreecam::Mat34 local;
		lookat::basisFromHeadingPitchRoll(rmarker::relHeading(m),
		                                  rmarker::relPitch(m),
		                                  rmarker::relRoll(m), local);

		// Front AND up carried through the parent - the from-front helper would
		// re-derive a roll-free up and silently discard the authored roll. This
		// is UpdateRotation's own pair of Transform3x3 calls followed by its
		// FromFrontAndUp.
		rfreecam::Mat34 world;
		lookat::worldMatrixFromFrontAndUp(R.transform3x3(local.b),
		                                  R.transform3x3(local.c), world);

		return rquat::fromBasis(world.a, world.b, world.c);
	}

	// Bring `a` into the same turn as `ref`, so a set of angles can be
	// interpolated without a 2*pi step in the middle of the curve. Heading and
	// roll both wrap; pitch is clamped well inside +-pi and never needs it.
	// What the spline actually decided this frame, once a second.
	//
	// The new paths are close to unfalsifiable from the chair: on a shot mounted
	// to a car, "riding a curve" and "riding stock" look alike until you know
	// what to compare, and a segment that DECLINED looks identical to one that
	// was never eligible. Same lesson the shake log exists for - "I turned it on
	// and nothing happened" needs to be answerable without a debugger.
	//
	// Rate-limited as a whole rather than per site, so each second reports the
	// one path that frame took.
	static void splineDebug(const char* fmt, ...)
	{
		if (!Config::get().splineDebugLog) return;

		static uint32_t s_last = 0;
		const uint32_t now32 = GetTickCount();
		if (now32 - s_last < 1000) return;
		s_last = now32;

		va_list args;
		va_start(args, fmt);
		logger::vwrite("info", fmt, args);
		va_end(args);
	}

	// Currently unused: the attached orientation path moved to world
	// quaternions, where rquat::align does the equivalent job on the double
	// cover. Kept because any future EULER channel needs exactly this, and the
	// seam bug it prevents is silent - a shot crossing +-pi interpolating the
	// long way round looks like a deliberate whip pan.
	static float unwrapNear(float a, float ref)
	{
		constexpr float kPi = 3.14159265358979f;
		constexpr float kTwoPi = 6.28318530717959f;
		while (a - ref >  kPi) a -= kTwoPi;
		while (a - ref < -kPi) a += kTwoPi;
		return a;
	}

	static bool finite3(const Vec3& v)
	{
		return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
	}

	// =========================================================================
	//  The frame as the director was ABOUT to smooth it.
	// =========================================================================
	//  Captured at the top of the detour, before origUpdateSmoothing runs, so it
	//  holds the pose the director WANTED before its own springs touched it -
	//  which is also the straight line the stock blend would have drawn.
	//
	//  Diagnostic only. The trace prints it as `des`/`desfwd` next to our own
	//  output, and that pairing is what separates "our curve is wrong" from
	//  "our curve is right and something after us moved the camera" - it is how
	//  the marker-crossing snap was eventually pinned.
	//
	//  It once fed a feature that added the springs' contribution back on top of
	//  the curve on mounted shots. That was removed: what the springs contribute
	//  is not smoothing but LAG, measured at 1.373 m on a 3.4 m segment, and it
	//  vanishes in a single frame whenever the spring is cut. The full write-up
	//  is in ENHANCED_PORT.md; there is deliberately no switch to bring it back.
	// =========================================================================
	static rfreecam::Mat34 s_preSmooth;
	static bool            s_havePreSmooth = false;

	static void readFrame(const void* self, rfreecam::Mat34& out)
	{
		const auto* base = (const uint8_t*)self;
		out.a = Vec3((const float*)(base + rdirector::OFF_FrameMatrixA));
		out.b = Vec3((const float*)(base + rdirector::OFF_FrameMatrixB));
		out.c = Vec3((const float*)(base + rdirector::OFF_FrameMatrixC));
		out.d = Vec3((const float*)(base + rdirector::OFF_FramePosition));
	}

	static void capturePreSmooth(void* self)
	{
		s_havePreSmooth = false;
		if (!self) return;

		readFrame(self, s_preSmooth);

		// Orthonormality, not just finiteness: the rotational delta below
		// inverts this basis by transposing it, which is only the inverse if it
		// really is a rotation. A frame the director has not filled in yet
		// fails here and we simply fall back to overwriting, as before.
		s_havePreSmooth = rfreecam::validMatrix(s_preSmooth);
	}

	// -------------------------------------------------------------------------
	// Six-marker window centred on the active segment.
	//
	// We need three consecutive segment LENGTHS to build a speed profile that
	// is continuous across the markers bounding the active segment, and each
	// Catmull-Rom segment needs one control point either side. Hence indices
	// -2..+3 around the current marker.
	// -------------------------------------------------------------------------
	struct Window
	{
		void*  m[6]{};   // [0]=-2 [1]=-1 [2]=curr [3]=next [4]=+2 [5]=+3
		Vec3   p[6];
		bool   have[6]{};
	};

	static Vec3 pointAt(const Window& w, int i)
	{
		if (w.have[i]) return w.p[i];

		// The two control points that BOUND the active segment (1 and 4) decide
		// its end tangents, so when one of them is missing - which is the clip's
		// first and last segment, every time - a natural end condition is worth
		// the extra real point it costs. A reflection there aims the curve
		// straight at the next marker and the shot visibly does not anticipate
		// the turn; see spline::naturalEnd.
		//
		// Indices 0 and 5 never contribute a tangent to the segment being drawn -
		// they only measure the NEIGHBOURING segments' lengths for the speed
		// profile. That sounds like it licenses a cheaper phantom, and it does
		// not: those lengths become the pchip's distance knots, and the
		// neighbour computes its own length from its own window. If the two
		// disagree, the two segments disagree about how far apart the markers
		// are, and Fritsch-Carlson's harmonic mean - which is dominated by the
		// smaller of the two secants - stalls the camera at the join. Measured
		// as a 55% speed dip at the first and last crossings of a clip, and only
		// there, because those are the only joins where one window invents an
		// end point and the other does not.
		//
		// So each of these has to reproduce EXACTLY what its neighbour uses.
		// have[0] is false precisely when the previous segment has no
		// predecessor of its own - i.e. when it invents its c1 the same way -
		// and likewise for have[5]. The conditions line up because they are the
		// same question asked from one marker further along.
		if (i < 2)
		{
			if (i == 1 && w.have[2] && w.have[3] && w.have[4])
				return spline::naturalEnd(w.p[2], w.p[3], w.p[4]);
			// What the PREVIOUS segment (w.p[1] -> w.p[2]) uses for its own c1.
			if (i == 0 && w.have[1] && w.have[2] && w.have[3])
				return spline::naturalEnd(w.p[1], w.p[2], w.p[3]);
			if (w.have[i + 1] && w.have[i + 2])
				return spline::phantom(w.p[i + 1], w.p[i + 2]);
			if (w.have[i + 1]) return w.p[i + 1];
		}
		else
		{
			if (i == 4 && w.have[3] && w.have[2] && w.have[1])
				return spline::naturalEnd(w.p[3], w.p[2], w.p[1]);
			// What the NEXT segment (w.p[3] -> w.p[4]) uses for its own c4.
			if (i == 5 && w.have[4] && w.have[3] && w.have[2])
				return spline::naturalEnd(w.p[4], w.p[3], w.p[2]);
			if (w.have[i - 1] && w.have[i - 2])
				return spline::phantom(w.p[i - 1], w.p[i - 2]);
			if (w.have[i - 1]) return w.p[i - 1];
		}
		return w.p[2];
	}

	// =========================================================================
	//  Per-frame trace (SplineTraceLog).
	// =========================================================================
	//  What the camera actually did this frame, and what it was built from.
	//
	//  splineDebugLog answers "which path did this frame take" once a second,
	//  which is the right granularity for a decision and useless for motion: a
	//  teleport, a lag and a wrong transform all look the same in a summary. The
	//  trace prints a line per frame with the terms separated - parent origin,
	//  our displacement from it, the smoothing delta, the frame the director
	//  wanted, and where the camera actually ended up after shake and DOF - so
	//  the three can be told apart by reading down a column.
	//
	//  Filled in by applySpline, printed at the end of the detour once every
	//  writer has had its turn.
	// =========================================================================
	struct Trace
	{
		bool  valid = false;
		bool  attached = false;
		float now = 0.0f;
		int   seg0 = -1, seg1 = -1;
		float u = 0.0f;
		Vec3  parentOrigin;   // vehicle origin this frame, or 0 when unattached
		Vec3  ourRel;         // the curve's output, before the origin is added

		// Orientation, decomposed. A stutter in the finished forward vector can
		// come from three places and they need opposite fixes: the PARENT's
		// rotation arriving in steps (upstream, the replay's entity transform),
		// our interpolated parent-relative angles (ours), or something after us.
		// `fwd` alone cannot tell them apart, so all three terms are logged and
		// the one that steps is the answer.
		Vec3  parentFwd;      // the attach matrix's forward row
		float h = 0, p = 0, r = 0;   // our interpolated relative angles, degrees
		char  orient = '-';   // 'e' euler (attached) / 'q' quaternion / 'l' look-at / '-' none
	};
	static Trace s_trace;

	// Why the curve declined this frame, for the STOCK trace line. A summary
	// rate-limited to once a second cannot answer this: the dropouts that
	// matter are six frames long, so the odds of a sample landing inside one
	// are what decides whether you ever see the reason. Per frame, it is
	// simply there.
	static const char* s_declineReason = "";

	// -------------------------------------------------------------------------
	// The trace gets its own file and its own stream, for two reasons that both
	// bite at a line per frame.
	//
	// logger::vwrite formats into a 512-byte buffer and REOPENS the log on every
	// call. A full trace line is close enough to 512 to truncate on world
	// coordinates, and sixty file opens a second is enough to perturb the very
	// timing a motion trace exists to measure - a diagnostic that changes what
	// it is measuring is worse than none. A kept-open stream costs nothing per
	// frame and keeps a few hundred trace lines out of the readable log.
	// -------------------------------------------------------------------------
	static void traceWrite(const char* line)
	{
		static std::ofstream s_out(paths::file("spline-trace.log").c_str(),
		                           std::ofstream::out | std::ofstream::trunc);
		if (!s_out) return;
		s_out << line << '\n';

		// Flushed per line on purpose: this is used to diagnose a crash or a
		// visual fault the operator then alt-tabs away from, and a buffered
		// tail that never reaches disk is exactly the part you wanted.
		s_out.flush();
	}

	static void traceFrame(void* self)
	{
		if (!Config::get().splineTraceLog) return;

		const auto* base = (const uint8_t*)self;
		const Vec3 pos(rdirector::framePosition(self));
		const Vec3 fwd((const float*)(base + rdirector::OFF_FrameMatrixB));
		const float fov = *(const float*)(base + rdirector::OFF_FrameFov);

		char line[512];

		if (!s_trace.valid)
		{
			// The spline declined this frame. Still worth a line - it is how you
			// see the camera being driven by stock, and by how much that differs
			// from the frames either side of it.
			snprintf(line, sizeof(line),
				"STOCK [%-42s] pos=(%.2f %.2f %.2f) fwd=(%.3f %.3f %.3f) fov=%.1f",
				s_declineReason, pos.x, pos.y, pos.z, fwd.x, fwd.y, fwd.z, fov);
			traceWrite(line);
			return;
		}

		// desired = the frame the director had before its springs ran, which is
		// also the position stock would have blended to. Printing it next to
		// ours is what separates "our curve is wrong" from "our curve is right
		// and something after us moved the camera".
		const Vec3 desired = s_havePreSmooth ? s_preSmooth.d : Vec3(0.0f, 0.0f, 0.0f);
		const Vec3 desFwd  = s_havePreSmooth ? s_preSmooth.b : Vec3(0.0f, 0.0f, 0.0f);

		if (s_trace.attached)
		{
			snprintf(line, sizeof(line),
				"t=%8.1f %d->%d u=%.4f ATT par=(%.3f %.3f %.3f) rel=(%.3f %.3f %.3f) "
				"pos=(%.3f %.3f %.3f) des=(%.3f %.3f %.3f) dev=%.4f "
				"fwd=(%.4f %.4f %.4f) desfwd=(%.4f %.4f %.4f) "
				"parfwd=(%.4f %.4f %.4f) o=%c hpr=(%.3f %.3f %.3f) fov=%.1f",
				s_trace.now, s_trace.seg0, s_trace.seg1, s_trace.u,
				s_trace.parentOrigin.x, s_trace.parentOrigin.y, s_trace.parentOrigin.z,
				s_trace.ourRel.x, s_trace.ourRel.y, s_trace.ourRel.z,
				pos.x, pos.y, pos.z,
				desired.x, desired.y, desired.z, (pos - desired).length(),
				fwd.x, fwd.y, fwd.z,
				desFwd.x, desFwd.y, desFwd.z,
				s_trace.parentFwd.x, s_trace.parentFwd.y, s_trace.parentFwd.z,
				s_trace.orient, s_trace.h, s_trace.p, s_trace.r, fov);
		}
		else
		{
			snprintf(line, sizeof(line),
				"t=%8.1f %d->%d u=%.3f WORLD                                        "
				"          pos=(%.2f %.2f %.2f) des=(%.2f %.2f %.2f) dev=%.3f "
				"fwd=(%.3f %.3f %.3f) fov=%.1f",
				s_trace.now, s_trace.seg0, s_trace.seg1, s_trace.u,
				pos.x, pos.y, pos.z,
				desired.x, desired.y, desired.z, (pos - desired).length(),
				fwd.x, fwd.y, fwd.z, fov);
		}
		traceWrite(line);
	}

	// Camera speed along the path at the current project time, in m/s. Written
	// by applySpline, consumed by applyShake later in the same tick.
	//
	// Not valid every frame: a marker the spline declined, or a segment with no
	// usable window, leaves nothing to differentiate. Speed coupling then
	// contributes nothing rather than guessing - a wrong speed would modulate
	// the shake with noise, which is worse than not modulating it at all.
	static float s_pathSpeed      = 0.0f;
	static bool  s_pathSpeedValid = false;

	static void applySpline(void* self)
	{
		s_pathSpeedValid = false;   // this tick, until a branch below sets it
		s_trace.valid    = false;   // ditto - every early return below is a
		                            // frame the camera ran on stock
		s_declineReason  = "";
		s_trace.orient   = '-';

		const Config& cfg = Config::get();
		if (!cfg.splinePosition && !cfg.splineOrientation && !cfg.splineFov)
		{
			s_declineReason = "toggles all off";
			splineDebug("spline: off - Position/Orientation/Fov all 0");
			return;
		}

		void* storage = game::markerStorage();
		if (!storage) { s_declineReason = "no marker storage";
		                splineDebug("spline: no marker storage"); return; }

		const int currIdx = rdirector::currentMarkerIndex(self);
		if (currIdx < 0) { s_declineReason = "no current marker";
		                   splineDebug("spline: no current marker"); return; }

		void* curr = rstorage::tryGetMarker(storage, currIdx);
		if (!curr) { s_declineReason = "marker unreadable";
		             splineDebug("spline: marker %d unreadable", currIdx); return; }

		const uint8_t blend = rmarker::blendType(curr);
		if (blend == rmarker::BLEND_NONE)
		{
			s_declineReason = "marker is a CUT (blend=None)";
			splineDebug("spline: marker %d is a CUT (blend=None) - left alone", currIdx);
			return; // a cut must stay a cut
		}

		const rsettings::MarkerSettings ms = rsettings::get(rmarker::timeMs(curr));
		if (ms.path == rsettings::PathMode::Stock)
		{
			s_declineReason = "per-marker Path=Stock";
			splineDebug("spline: marker %d set to Stock path per-marker", currIdx);
			return;
		}
		// The likeliest reason a shot is not being splined at all, and it used to
		// be silent: OnlySmoothBlend ships ON, so a marker whose Transition is
		// Linear rather than Smooth is skipped by design and looks identical to
		// a bug from the chair.
		if (ms.path == rsettings::PathMode::Inherit &&
		    cfg.onlySmoothBlend && blend != rmarker::BLEND_SMOOTH)
		{
			s_declineReason = "transition is not Smooth (OnlySmoothBlend)";
			splineDebug("spline: marker %d transition is %s, not Smooth - skipped "
				"(set it to Smooth, or OnlySmoothBlend=0)",
				currIdx, blend == rmarker::BLEND_LINEAR ? "Linear" : "None");
			return;
		}

		const float alpha = ms.has(rsettings::P_ALPHA) ? ms.v[rsettings::P_ALPHA] : cfg.alpha;

		const int nextIdx = game::nextMarkerIndex(currIdx);
		void* next = rstorage::tryGetMarker(storage, nextIdx);
		if (!next)
		{
			s_declineReason = "no next marker (last in clip)";
			splineDebug("spline: marker %d has no next marker (last in clip)", currIdx);
			return;
		}

		// --- what space is this segment in? -----------------------------------
		// -1 is an ordinary world-space shot. Anything else is mounted on an
		// entity, and the whole curve then lives in that entity's frame.
		const int32_t segAttach = rmarker::attachId(curr);
		if (segAttach != -1 && !cfg.splineAttached)
		{
			s_declineReason = "SplineAttached=0";
			splineDebug("spline: attached seg (id=%d) declined - SplineAttached=0", segAttach);
			return;
		}

		if (!rmarker::isSplineable(curr, segAttach) ||
		    !rmarker::isSplineable(next, segAttach))
		{
			s_declineReason = "segment ends disagree";
			splineDebug("spline: declined - segment ends disagree "
				"(curr attach=%d cam=%u / next attach=%d cam=%u)",
				segAttach, rmarker::camType(curr),
				rmarker::attachId(next), rmarker::camType(next));
			return;
		}

		// --- the parent transform, for a mounted segment ----------------------
		// m_AttachEntityMatrix rather than the entity's own matrix, deliberately.
		// The game builds it in UpdateAttachEntityMatrix with a ped's facing
		// direction substituted for its heading, the root bone substituted for a
		// ragdoll's origin, and the marker's ATTACH TYPE already applied - full,
		// position-and-heading, or position-only all produce a different matrix.
		// Rebuilding any of that from the raw entity transform would both
		// reintroduce the jitter Rockstar removed and quietly ignore the attach
		// type; reading their result gets all of it for free.
		// EITHER end of the segment will do, and both have to be tried. The
		// director keeps exactly two cameras, m_ActiveCamera and
		// m_PreviousCamera, and during a blend the ACTIVE one is bound to the
		// marker being blended TO - so the camera holding currIdx is usually the
		// previous one, and outside a blend it may be the only one there is.
		// Falling back to stock because we looked in one slot would show up as
		// the camera popping between splined and unsplined mid-shot, which is
		// worse than never splining at all.
		//
		// Taking either is sound because both markers are already required to
		// share an attach target AND an attach type, so the game built both
		// cameras' matrices from the same entity by the same rules. Both are
		// live: m_PreviousCamera is BaseUpdate'd whenever it exists, and deleted
		// when it stops being needed.
		rfreecam::Mat34 parent;
		bool haveParent = false;
		if (segAttach != -1)
		{
			void* cam = rfreecam::forMarker(self, currIdx);
			if (!cam || !rfreecam::attachEntity(cam))
				cam = rfreecam::forMarker(self, nextIdx);

			// Neither slot carries the index we asked for. That is not
			// necessarily an absent entity - the director rebuilds both cameras
			// when the playhead crosses a marker, and for a few frames after it
			// the indices simply do not line up. Take any mounted camera rather
			// than dropping the segment; see rfreecam::anyAttached for why that
			// is the same entity's origin either way.
			if (!cam || !rfreecam::attachEntity(cam))
				cam = rfreecam::anyAttached(self);

			// An attach entity that has genuinely gone - streamed out, or
			// removed from the clip - leaves no live space to put the curve in.
			// Stock copes with that; we decline.
			if (!cam || !rfreecam::attachEntity(cam))
			{
				s_declineReason = "no live attach entity";
				splineDebug("spline: attached seg (id=%d) declined - "
					"no live camera/attach entity for marker %d or %d",
					segAttach, currIdx, nextIdx);
				return;
			}

			parent = rfreecam::attachMatrix(cam);
			if (!rfreecam::validMatrix(parent))
			{
				// If this ever fires, the offset moved: the matrix passed a
				// null check but is not a rotation.
				s_declineReason = "attach matrix not orthonormal";
				splineDebug("spline: attached seg (id=%d) declined - "
					"attach matrix failed the orthonormality check", segAttach);
				return;
			}
			haveParent = true;
		}

		// --- build the window -------------------------------------------------
		// The two ENDPOINTS stay as the director sees them - they define the
		// segment it is actually blending, and the timing has to agree with it.
		// The outer control points are walked geometrically; see neighbourIndex.
		const int im1 = neighbourIndex(storage, currIdx, -1, segAttach);
		const int im2 = im1 >= 0 ? neighbourIndex(storage, im1, -1, segAttach) : -1;
		const int ip2 = neighbourIndex(storage, nextIdx, +1, segAttach);
		const int ip3 = ip2 >= 0 ? neighbourIndex(storage, ip2, +1, segAttach) : -1;
		const int idx[6] = { im2, im1, currIdx, nextIdx, ip2, ip3 };

		Window w;
		for (int i = 0; i < 6; ++i)
		{
			w.m[i] = (i == 2 || i == 3) ? (i == 2 ? curr : next)
			                            : controlMarker(storage, idx[i], segAttach);
			w.have[i] = w.m[i] != nullptr;
			if (w.have[i])
			{
				// Unattached: the authored world position, as always.
				//
				// Mounted: a world-space DISPLACEMENT from the parent's origin,
				// each marker resolved through its own attach frame first. The
				// world position stored beside the offset is a stale snapshot
				// from authoring time and is never the right answer here.
				w.p[i] = haveParent
					? attachDisplacement(w.m[i], parent)
					: Vec3(rmarker::position(w.m[i]));
			}
		}

		// --- timing -----------------------------------------------------------
		// Use the clock UNTRUNCATED. The game truncates to whole milliseconds
		// for its own lerp, but matching that here just quantises our curve into
		// ~1 ms stair-steps, which on a fast pan is centimetres of visible
		// jitter. Nothing downstream requires us to agree with it.
		const float now = *(float*)game::addr_g_ReplayTimeMs;

		const float tCurr = rmarker::timeMs(curr);
		const float tNext = rmarker::timeMs(next);
		const float span  = tNext - tCurr;
		if (span < 1e-3f) { s_declineReason = "zero-length segment in time"; return; }

		// --- segment lengths around the active segment ------------------------
		// prev  = p[1] -> p[2]   (controls p[0..3])
		// curr  = p[2] -> p[3]   (controls p[1..4])
		// next  = p[3] -> p[4]   (controls p[2..5])
		const Vec3 c0 = pointAt(w, 0), c1 = pointAt(w, 1), c2 = pointAt(w, 2);
		const Vec3 c3 = pointAt(w, 3), c4 = pointAt(w, 4), c5 = pointAt(w, 5);

		const float lenCurr = spline::segmentLength(c1, c2, c3, c4, alpha);

		// A zero-length path has nothing to interpolate. In WORLD space that is
		// two markers in the same place and stock handles it fine.
		//
		// On a MOUNTED segment it is the most ordinary shot there is - a camera
		// bolted to the car at one offset, which is a straight line in the
		// world and a single POINT in the parent's frame. Bailing here would
		// hand it straight back to the smoothing spring, i.e. back to the lag
		// this whole path exists to remove, and on the shot most likely to show
		// it. So hold the relative position instead of declining.
		const bool degenerate = (lenCurr < 1e-5f);
		if (degenerate && !haveParent) { s_declineReason = "zero-length path, world space"; return; }

		// How far the curve departs from the straight line between the two
		// markers, at the midpoint. THIS is the number that says whether the
		// spline is doing anything you could see: if it is near zero the curve
		// and the lerp are the same path, and no amount of splining will look
		// different from linear.
		const Vec3 mid  = spline::catmullRom(c1, c2, c3, c4, 0.5f, alpha);
		const Vec3 lerp = (c2 + c3) * 0.5f;
		const float bow = (mid - lerp).length();

		// Which end tangents this segment actually got. The clip's first and
		// last segments are structurally different from the ones in the middle
		// and always will be - naming that in the log stops a perfectly healthy
		// end segment from being read as a broken spline, which is exactly how
		// this one was misread.
		const char* ends =
			(w.have[1] && w.have[4]) ? "" :
			(!w.have[1] && !w.have[4]) ? "  <-- both ends invented: this IS a straight line" :
			!w.have[1] ? "  <-- clip start, natural end condition"
			           : "  <-- clip end, natural end condition";

		if (haveParent)
		{
			// A segment whose two ends disagree about whether their stored attach
			// frame is usable resolves them through DIFFERENT rotations, and the
			// camera steps at the join. Worth seeing.
			//
			// `path` is measured in the PARENT's frame, so on a mounted shot it
			// is how far the camera moves relative to the vehicle - not how far
			// it travels through the world. A path of a metre against a bow of
			// four centimetres is the whole reason a mounted shot cannot look as
			// obviously curved as a world-space one, and reading those two
			// numbers together is what says so.
			splineDebug("spline: ATTACHED seg %d->%d win=[%d %d|%d %d] of %d "
				"type=%u/%u frameOk=%d/%d path=%.3fm bow=%.3fm%s%s",
				currIdx, nextIdx, im1, im2, ip2, ip3, rstorage::markerCount(storage),
				rmarker::attachType(curr), rmarker::attachType(next),
				(int)rmarker::attachMatrixValid(curr), (int)rmarker::attachMatrixValid(next),
				lenCurr, bow,
				degenerate ? " (locked-off)" : "", ends);
		}
		else
		{
			splineDebug("spline: world seg %d->%d win=[%d %d|%d %d] of %d "
				"path=%.3fm bow=%.3fm%s",
				currIdx, nextIdx, im1, im2, ip2, ip3, rstorage::markerCount(storage),
				lenCurr, bow, ends);
		}

		// Time knots shared by every channel that is a function of the CLOCK:
		// the position when natural pacing is on, the FOV, and the rotation.
		// Mirroring the interval when a neighbour is missing keeps the end
		// tangents finite, the same way the position window mirrors a phantom
		// control point.
		const float tk[4] = {
			w.have[1] ? rmarker::timeMs(w.m[1]) : tCurr - span,
			tCurr, tNext,
			w.have[4] ? rmarker::timeMs(w.m[4]) : tNext + span,
		};

		float u = 0.0f; // curve parameter on the active segment

		// Natural pacing replaces the whole distance pipeline: the position is
		// a Hermite through the four markers over their real TIMES, sampled at
		// the clock. No arc-length remap and no distance profile, so the curve's
		// own parameter speed drives the camera - it eases into tight bends and
		// flows through open ones, which is the Simple Camera feel.
		//
		// u is still computed, from time, because the channels below are
		// expressed in it - but it no longer decides where the camera IS.
		const bool naturalPath = cfg.naturalPacing && !degenerate;

		if (degenerate)
		{
			// Position is the same everywhere on this segment, so the parameter
			// only matters to the orientation and FOV channels below - drive it
			// by time so those still interpolate instead of freezing at the
			// marker.
			float t = (now - tCurr) / span;
			if (!(t > 0.0f)) t = 0.0f;
			if (t > 1.0f)    t = 1.0f;
			u = t;
		}
		else if (naturalPath)
		{
			float t = (now - tCurr) / span;
			if (!(t > 0.0f)) t = 0.0f;
			if (t > 1.0f)    t = 1.0f;
			u = t;

			// Camera speed at this instant, for shake coupling.
			//
			// Differentiated from spline::hermitePos - the curve this branch
			// actually drives the camera with - rather than from a distance
			// profile, which natural pacing never builds. Without it
			// s_pathSpeedValid stayed false on the SHIPPING DEFAULT, so the
			// whole motion-coupling block was silently inert: ShakeSpeedAmp,
			// ShakeSpeedFreq, ShakeStopWhenStill and the menu's Shake: Motion
			// group all read as "speed not known this tick" and contributed
			// nothing. Turning any of them up did visibly nothing unless you
			// also moved the Speed Profile off Natural, which nothing said.
			//
			// A CENTRED difference on a function of project time, exactly like
			// the two branches below: scrub to t, get the same speed, and a
			// re-render still matches the take that was approved.
			constexpr float kH = 8.0f;   // ms
			const Vec3 ctrl[4] = { c1, c2, c3, c4 };
			const Vec3 pA = spline::hermitePos(ctrl, tk, now - kH);
			const Vec3 pB = spline::hermitePos(ctrl, tk, now + kH);
			s_pathSpeed      = (pB - pA).length() / (2.0f * kH * 0.001f);
			s_pathSpeedValid = true;
		}
		else if (cfg.smoothSpeedProfile && !ms.has(rsettings::P_EASE_IN) && !ms.has(rsettings::P_EASE_OUT))
		{
			const float lenPrev = w.have[1] ? spline::segmentLength(c0, c1, c2, c3, alpha) : lenCurr;
			const float lenNext = w.have[4] ? spline::segmentLength(c2, c3, c4, c5, alpha) : lenCurr;

			// Knot times. Mirror the interval when a neighbour is missing so the
			// end tangents stay sane instead of collapsing to zero.
			const float tPrev = w.have[1] ? rmarker::timeMs(w.m[1]) : tCurr - span;
			const float tP2   = w.have[4] ? rmarker::timeMs(w.m[4]) : tNext + span;

			const float tk[4] = { tPrev, tCurr, tNext, tP2 };
			const float sk[4] = { 0.0f, lenPrev, lenPrev + lenCurr, lenPrev + lenCurr + lenNext };

			float dist = spline::pchipDistance(tk, sk, now) - lenPrev;
			if (!(dist > 0.0f)) dist = 0.0f;
			if (dist > lenCurr)  dist = lenCurr;

			u = spline::paramAtDistance(c1, c2, c3, c4, dist, alpha);

			// Camera speed at this instant, for shake coupling.
			//
			// A CENTRED difference on the same time->distance curve, not a
			// frame-to-frame measurement. That distinction is the whole reason
			// the coupling can exist at all here: this is a function of project
			// time, so scrubbing to t always yields the same speed, and the
			// shake stays exactly reproducible across a re-render.
			constexpr float kH = 8.0f;   // ms
			const float d1 = spline::pchipDistance(tk, sk, now - kH);
			const float d2 = spline::pchipDistance(tk, sk, now + kH);
			s_pathSpeed = fabsf(d2 - d1) / (2.0f * kH * 0.001f);   // m/s
			s_pathSpeedValid = true;
		}
		else
		{
			// Explicit per-marker ease, or the smooth profile switched off:
			// fall back to shaping this segment on its own. Ease deliberately
			// overrides the cross-marker speed continuity вЂ” that is the point
			// of asking for a hold at the keyframe.
			float t = (now - tCurr) / span;
			if (!(t > 0.0f)) t = 0.0f;
			if (t > 1.0f)    t = 1.0f;

			const float frac = spline::ease(t,
				ms.has(rsettings::P_EASE_IN)  ? ms.v[rsettings::P_EASE_IN]  : 0.0f,
				ms.has(rsettings::P_EASE_OUT) ? ms.v[rsettings::P_EASE_OUT] : 0.0f);

			u = cfg.arcLengthRemap
				? spline::paramAtDistance(c1, c2, c3, c4, frac * lenCurr, alpha)
				: frac;

			// Same idea on the eased path: differentiate the ease curve rather
			// than the pchip, so an ease-in genuinely ramps the shake up with
			// the move instead of the shake arriving at full strength while the
			// camera is still accelerating.
			constexpr float kH = 8.0f;   // ms
			auto fracAt = [&](float when) {
				float x = (when - tCurr) / span;
				if (!(x > 0.0f)) x = 0.0f;
				if (x > 1.0f)    x = 1.0f;
				return spline::ease(x,
					ms.has(rsettings::P_EASE_IN)  ? ms.v[rsettings::P_EASE_IN]  : 0.0f,
					ms.has(rsettings::P_EASE_OUT) ? ms.v[rsettings::P_EASE_OUT] : 0.0f);
			};
			const float f1 = fracAt(now - kH), f2 = fracAt(now + kH);
			s_pathSpeed = fabsf(f2 - f1) * lenCurr / (2.0f * kH * 0.001f);
			s_pathSpeedValid = true;
		}

		// On a mounted segment every length above is measured in the PARENT's
		// frame, so what came out is speed relative to the car - which is near
		// zero for a camera locked to it, however fast it is actually travelling.
		// Report it unknown rather than hand the shake's motion gate a number
		// that means something else; -1 is already the "not known this tick"
		// contract applyShake expects, and it costs only the speed coupling.
		if (haveParent) s_pathSpeedValid = false;

		if (cfg.splinePosition)
		{
			// tk is the same four marker times the scalar channels use, with a
			// mirrored interval where a neighbour is missing - exactly what a
			// time-parameterised Hermite needs.
			const Vec3 ctrl[4] = { c1, c2, c3, c4 };
			Vec3 pos = naturalPath ? spline::hermitePos(ctrl, tk, now)
			                       : spline::catmullRom(c1, c2, c3, c4, u, alpha);

			// DIAGNOSTIC: exaggerate how far the curve departs from the straight
			// line, without moving either endpoint.
			//
			// "The curve is not reaching the camera" and "the curve is reaching
			// the camera and is too subtle to see" look identical from the
			// chair, and they need opposite fixes - one is a plumbing bug, the
			// other is a tuning question. Scaling the deviation answers it in one
			// launch: at a gain of 10 the camera either swings obviously wide of
			// the straight line, or it does not move at all and the write is
			// being discarded somewhere downstream.
			//
			// The deviation is zero at u=0 and u=1, so the markers are still hit
			// exactly however large the gain is - it cannot desync the timing or
			// send the camera somewhere unrecoverable.
			if (cfg.splineDebugGain != 1.0f)
			{
				const Vec3 chord = c2 + (c3 - c2) * u;
				pos = chord + (pos - chord) * cfg.splineDebugGain;
			}

			// The curve is a displacement on a mounted segment; hang it off the
			// parent's live origin. This lands after the game's own attach
			// transform and after its collision push-off, and overrides both -
			// the same trade the world-space path has always made.
			s_trace.valid        = true;
			s_trace.attached     = haveParent;
			s_trace.now          = now;
			s_trace.seg0         = currIdx;
			s_trace.seg1         = nextIdx;
			s_trace.u            = u;
			s_trace.parentOrigin = haveParent ? parent.d : Vec3(0.0f, 0.0f, 0.0f);
			s_trace.parentFwd    = haveParent ? parent.b : Vec3(0.0f, 0.0f, 0.0f);
			s_trace.ourRel       = pos;

			if (haveParent)
			{
				pos = parent.d + pos;

			}

			if (finite3(pos))
				pos.store(rdirector::framePosition(self));
		}

		// --- field of view ----------------------------------------------------
		// Stock blends FOV with a two-point lerp - `fov += (target - fov) * t`,
		// written at the very end of UpdateSmoothing (leg 0x2A19F8, the
		// `movss [rbx+0x250]` there). That is the same defect the path had:
		// continuous value, discontinuous rate, so a push-in kicks at every
		// keyframe even once the path and rotation glide through it.
		//
		// Placed BEFORE the orientation block on purpose - that one returns early
		// on look-at markers, and a look-at shot can still zoom.
		//
		// MONOTONE rather than Catmull-Rom, and this is the one channel where the
		// difference is worth seeing: a Catmull-Rom through 50 -> 30 degrees can
		// dip below 30 and come back, which on a zoom reads as a wobble at the end
		// of the move. pchip cannot overshoot by construction, and it still
		// interpolates the knots exactly, so the FOV is the authored value at each
		// keyframe at the right moment.
		//
		// Marker FOV is a u8 in WHOLE DEGREES (+0xA3, cast straight to float and
		// clamped 1..130 by the game - confirmed in the marker->frame setup at leg
		// 0x254890), so this de-quantises as well as smooths: a slow push-in
		// currently steps one degree at a time.

		// --- when the ROTATION channels are sampled ---------------------------
		// Real project time, NOT the position curve's parameter u. Getting this
		// wrong put a direction kick at every single keyframe.
		//
		// u is an arc-length parameter normalised per segment, so du/dt is
		// roughly (speed / segment length). Position does not care - the
		// |dC/du| cancels, which is the entire point of reparameterising by
		// arc length - but anything ELSE driven by the same u inherits the
		// ratio directly. Between a 10 m segment and a 40 m one, du/dt steps by
		// 4x at the marker, so the camera turns four times faster on one side
		// of the keyframe than the other. A driving shot hides it behind the
		// vehicle's own motion; a static one does not, which is exactly where
		// it was caught.
		//
		// An explicit ease is the one case where following the motion rather
		// than the clock IS wanted - the camera accelerates out of the keyframe
		// and the framing should go with it - and it does not bring the kick
		// back, because an ease ramp has zero derivative at both ends, so two
		// eased segments meet at a matched rate instead of a mismatched one.
		float rotAt = now;
		{
			const bool hasEase = ms.has(rsettings::P_EASE_IN) || ms.has(rsettings::P_EASE_OUT);
			if (hasEase)
			{
				float t = (now - tCurr) / span;
				if (!(t > 0.0f)) t = 0.0f;
				if (t > 1.0f)    t = 1.0f;
				rotAt = tCurr + spline::ease(t,
					ms.has(rsettings::P_EASE_IN)  ? ms.v[rsettings::P_EASE_IN]  : 0.0f,
					ms.has(rsettings::P_EASE_OUT) ? ms.v[rsettings::P_EASE_OUT] : 0.0f) * span;
			}
		}
		// The same instant as a 0..1 fraction of the segment, for the handful of
		// scalar framing terms that are only meaningful within one segment.
		const float rotFrac = span > 1e-6f ? (rotAt - tCurr) / span : 0.0f;

		if (cfg.splineFov)
		{
			const float fCurr = (float)rmarker::fov(curr);
			const float fNext = (float)rmarker::fov(next);

			// A marker with no usable FOV means the shot never authored one; do
			// not invent a value for it, and do not clamp 0 up to 1 - that would
			// be a hard zoom to a 1-degree lens.
			if (fCurr >= 1.0f && fNext >= 1.0f)
			{
				// Missing neighbour: repeat the end value instead of reflecting
				// it. A reflected scalar invents zoom the shot never asked for and
				// can leave the legal range; repeating gives a zero end tangent,
				// i.e. the move eases in at the first marker.
				const float fk[4] = {
					w.have[1] ? (float)rmarker::fov(w.m[1]) : fCurr,
					fCurr, fNext,
					w.have[4] ? (float)rmarker::fov(w.m[4]) : fNext,
				};

				float fov = spline::pchipAt(tk, fk, now);
				if (isfinite(fov))
				{
					// The game's own bounds, the same two constants SetFov and
					// UpdateZoom clamp against.
					if (fov < 1.0f)   fov = 1.0f;
					if (fov > 130.0f) fov = 130.0f;
					*(float*)((uint8_t*)self + rdirector::OFF_FrameFov) = fov;
				}
			}
		}

		// --- orientation ------------------------------------------------------
		// Three cases, and only the last is a spline. A look-at marker's
		// orientation is DERIVED, and an attached marker's is authored in the
		// parent's frame rather than the world's, so each needs its own path.
		const bool orientStock = (ms.orient == rsettings::OrientMode::Stock);

		// --- look-at: re-aim, do not interpolate -------------------------------
		// The stored quaternion is not what the game renders for these markers -
		// UpdateRotation discards the authored angles and re-solves the framing
		// from the target every frame. Splining the quaternion would fight a
		// derived value. What actually needs fixing is that the game solved it
		// from ITS position and we then moved the camera, with nothing left to
		// re-aim: our write lands after UpdateSmoothing, the last writer.
		//
		// Only when both ends share a target. When they differ the game slerps
		// the two look-at orientations rather than re-solving (PostUpdate's
		// shouldBlendOrientation), and aiming at one of the two targets through
		// a transition to the other would be wrong in a much more visible way
		// than the drift this fixes.
		if (rmarker::lookAtId(curr) != -1 || rmarker::lookAtId(next) != -1)
		{
			const int32_t target = rmarker::lookAtId(curr);
			if (orientStock || !cfg.lookAtReaim || !cfg.splinePosition)
			{
				splineDebug("spline: look-at seg - no re-aim "
					"(markerStock=%d LookAtReaim=%d SplinePosition=%d)",
					(int)orientStock, (int)cfg.lookAtReaim, (int)cfg.splinePosition);
				return;
			}
			if (target == -1 || rmarker::lookAtId(next) != target)
			{
				splineDebug("spline: look-at seg - no re-aim, ends differ "
					"(curr=%d next=%d); the game cross-fades these",
					target, rmarker::lookAtId(next));
				return;
			}

			// Either camera, for the same reason the attach lookup takes either:
			// during a blend the one bound to currIdx is m_PreviousCamera, and
			// both markers are already required to share this target.
			void* cam = rfreecam::forMarker(self, currIdx);
			void* ent = cam ? rfreecam::lookAtEntity(cam) : nullptr;
			if (!ent)
			{
				cam = rfreecam::forMarker(self, nextIdx);
				ent = cam ? rfreecam::lookAtEntity(cam) : nullptr;
			}
			if (!ent) return;

			auto* base = (uint8_t*)self;

			// Read the position and FOV back rather than recomputing them: this
			// is the pose the camera actually ends the frame at, whether it came
			// from the spline above, the FOV block, or the game.
			const Vec3  camPos(rdirector::framePosition(self));
			const float fov = *(const float*)(base + rdirector::OFF_FrameFov);

			// Framing offsets track the same instant the rotation channels do,
			// for the same reason: driven by the position curve's parameter they
			// would inherit its per-segment normalisation and drift at a rate
			// that steps at every marker. Almost always a no-op anyway - both
			// ends are zero unless the shot deliberately puts the subject
			// off-centre.
			auto mix = [rotFrac](float a, float b) { return a + (b - a) * rotFrac; };

			const float offX = mix(rmarker::lookAtHorizontal(curr), rmarker::lookAtHorizontal(next));
			const float offY = mix(rmarker::lookAtVertical(curr),   rmarker::lookAtVertical(next));
			const float roll = mix(rmarker::lookAtRoll(curr),       rmarker::lookAtRoll(next));

			const float aspect = cfg.lookAtAspect > 0.0f
				? cfg.lookAtAspect : lookat::displayAspect();

			const Vec3 tgt = rentity::position(ent);
			splineDebug("spline: LOOK-AT re-aim target=%d at (%.1f %.1f %.1f) "
				"dist=%.1fm off=%.2f/%.2f fov=%.1f",
				target, tgt.x, tgt.y, tgt.z, (tgt - camPos).length(), offX, offY, fov);

			s_trace.orient = 'l';
			rfreecam::Mat34 aimed;
			if (lookat::solve(tgt, camPos,
			                  offX, offY, fov, roll, aspect, aimed))
			{
				aimed.a.store((float*)(base + rdirector::OFF_FrameMatrixA));
				aimed.b.store((float*)(base + rdirector::OFF_FrameMatrixB));
				aimed.c.store((float*)(base + rdirector::OFF_FrameMatrixC));
			}
			return;
		}

		const bool wantOrient =
			ms.orient == rsettings::OrientMode::Squad ? true :
			ms.orient == rsettings::OrientMode::Stock ? false :
			cfg.splineOrientation;

		if (!wantOrient) return;

		// --- attached: interpolate WORLD orientations, one per marker ----------
		// Each marker resolved through its OWN attach frame first, then the four
		// results interpolated - the same construction attachDisplacement uses for
		// position, and for the same reason.
		//
		// The obvious alternative, and what this used to do, is to interpolate the
		// authored heading/pitch/roll and apply one parent frame at the end. That
		// asks WHICH marker's frame, and outside FULL they disagree - the game
		// never refreshes a frozen attach frame, so each marker keeps whatever the
		// vehicle was pointing at when it was made. The director rebuilds its two
		// cameras at every marker crossing, so the frame we read changed there and
		// the aim snapped by however far the car had turned between the two
		// markers: measured at 4.9 degrees in one frame on a driving shot, on every
		// interior keyframe of the clip. Resolving per marker removes the question.
		//
		// FULL is unaffected either way - there every marker shares the live
		// rotation, and left-multiplying by a common rotation commutes with the
		// component-wise Hermite below.
		if (haveParent)
		{
			const rquat::Quat q1 = attachWorldQuat(curr, parent);
			const rquat::Quat q2 = rquat::align(attachWorldQuat(next, parent), q1);
			const rquat::Quat q0 = w.have[1]
				? rquat::align(attachWorldQuat(w.m[1], parent), q1) : q1;
			const rquat::Quat q3 = w.have[4]
				? rquat::align(attachWorldQuat(w.m[4], parent), q2) : q2;

			const rquat::Quat qk[4] = { q0, q1, q2, q3 };
			const rquat::Quat q = rquat::hermite(qk, tk, rotAt);
			s_trace.orient = 'e';

			if (isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w))
			{
				rfreecam::Mat34 world;
				{
					float a[3], b[3], c[3];
					rquat::toMatrixRows(q, a, b, c);
					world.a = Vec3(a); world.b = Vec3(b); world.c = Vec3(c);
					world.d = Vec3(0.0f, 0.0f, 0.0f);
				}

				// Report the angles we ended up at rather than the ones we fed in -
				// the trace's job is to say where the camera is pointing, and after
				// the change there is no single authored triple to quote.
				constexpr float kRad2Deg = 57.2957795f;
				s_trace.h = atan2f(-world.b.x, world.b.y) * kRad2Deg;
				s_trace.p = asinf(world.b.z < -1.0f ? -1.0f : (world.b.z > 1.0f ? 1.0f : world.b.z)) * kRad2Deg;
				s_trace.r = atan2f(-world.a.z, world.c.z) * kRad2Deg;

				if (finite3(world.a) && finite3(world.b) && finite3(world.c))
				{
					auto* base = (uint8_t*)self;
					world.a.store((float*)(base + rdirector::OFF_FrameMatrixA));
					world.b.store((float*)(base + rdirector::OFF_FrameMatrixB));
					world.c.store((float*)(base + rdirector::OFF_FrameMatrixC));
				}
			}
			return;
		}

		// --- world-space: cubic Hermite through the marker quaternions ---------
		{
			const rquat::Quat q1 = rquat::decode(rmarker::camQuat(curr));

			// Hemisphere-align every neighbour before splining, chained outwards
			// from the segment. Without this a marker whose packed quaternion
			// happens to decode to the far representative - which depends only
			// on which component came out largest, not on the previous marker -
			// puts a full turn into the curve at that keyframe.
			const rquat::Quat q2 = rquat::align(rquat::decode(rmarker::camQuat(next)), q1);
			const rquat::Quat q0 = w.have[1]
				? rquat::align(rquat::decode(rmarker::camQuat(w.m[1])), q1) : q1;
			const rquat::Quat q3 = w.have[4]
				? rquat::align(rquat::decode(rmarker::camQuat(w.m[4])), q2) : q2;

			// Sampled at the same real time the scalar channels are, over the
			// same knots - so the rate arriving at a marker matches the rate
			// leaving it however unevenly the markers are spaced.
			const rquat::Quat qk[4] = { q0, q1, q2, q3 };
			const rquat::Quat q = rquat::hermite(qk, tk, rotAt);
			s_trace.orient = 'q';

			if (isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w))
			{
				auto* base = (uint8_t*)self;
				rquat::toMatrixRows(q,
					(float*)(base + rdirector::OFF_FrameMatrixA),
					(float*)(base + rdirector::OFF_FrameMatrixB),
					(float*)(base + rdirector::OFF_FrameMatrixC));
			}
		}
	}

	// -------------------------------------------------------------------------
	// Procedural shake, applied last.
	//
	// Deliberately after the spline and in the camera's own axes: shake is the
	// operator moving, not the path changing. Running it here also means it
	// lands on stock Linear/None markers too, not just splined ones.
	// -------------------------------------------------------------------------
	static void rotatePair(float* u, float* v, float rad)
	{
		const float c = cosf(rad), s = sinf(rad);
		for (int i = 0; i < 3; ++i)
		{
			const float a = u[i], b = v[i];
			u[i] = a * c + b * s;
			v[i] = -a * s + b * c;
		}
	}

	static void applyShake(void* self, void* marker)
	{
		// Our shake runs only on markers whose Shake row is set to the
		// "Rockstar Editor+" preset. Without this gate the sway/jitter values
		// would apply on top of whatever the game's own shake was already
		// doing - two shakes at once - and there would be no way to turn ours
		// off from the menu.
		if (!marker) return;
		if (!rsettings::get(rmarker::timeMs(marker)).ourShake) return;

		// Start from the global parameters, then let the marker override each
		// term it actually sets.
		shake::Params p = Config::get().shake;

		{
			using namespace rsettings;
			const MarkerSettings ms = rsettings::get(rmarker::timeMs(marker));
			if (ms.has(P_SWAY_POS))  p.sway.posAmp   = ms.v[P_SWAY_POS];
			if (ms.has(P_SWAY_ROT))  p.sway.rotAmp   = ms.v[P_SWAY_ROT];
			if (ms.has(P_SWAY_FREQ)) p.sway.freq     = ms.v[P_SWAY_FREQ];
			if (ms.has(P_JIT_POS))   p.jitter.posAmp = ms.v[P_JIT_POS];
			if (ms.has(P_JIT_ROT))   p.jitter.rotAmp = ms.v[P_JIT_ROT];
			if (ms.has(P_JIT_FREQ))  p.jitter.freq   = ms.v[P_JIT_FREQ];
			if (ms.has(P_OCTAVES))   p.octaves       = (int)ms.v[P_OCTAVES];
			if (ms.has(P_SEED))      p.seed          = (int)ms.v[P_SEED];
			if (ms.has(P_AX_LAT))    p.axisPos[0]    = ms.v[P_AX_LAT];
			if (ms.has(P_AX_FWD))    p.axisPos[1]    = ms.v[P_AX_FWD];
			if (ms.has(P_AX_VERT))   p.axisPos[2]    = ms.v[P_AX_VERT];
			if (ms.has(P_AX_PITCH))  p.axisRot[0]    = ms.v[P_AX_PITCH];
			if (ms.has(P_AX_ROLL))   p.axisRot[1]    = ms.v[P_AX_ROLL];
			if (ms.has(P_AX_YAW))    p.axisRot[2]    = ms.v[P_AX_YAW];

			if (ms.has(P_SIMPLE))     p.simpleMode    = ms.v[P_SIMPLE]     > 0.5f;
			if (ms.has(P_INTENSITY))  p.amplitude     = ms.v[P_INTENSITY];
			if (ms.has(P_FREQ_MUL))   p.frequency     = ms.v[P_FREQ_MUL];
			if (ms.has(P_SPEED_AMP))  p.speedAmp      = ms.v[P_SPEED_AMP];
			if (ms.has(P_SPEED_FREQ)) p.speedFreq     = ms.v[P_SPEED_FREQ];
			if (ms.has(P_STOP_STILL)) p.stopWhenStill = ms.v[P_STOP_STILL] > 0.5f;
			if (ms.has(P_VARIATION))  p.variation     = ms.v[P_VARIATION];
		}

		if (!p.active()) return;

		const float now = *(float*)game::addr_g_ReplayTimeMs;

		// NEGATIVE means "not known this tick", which is not the same as zero.
		// Zero would tell the motion gate the camera is parked and fade the
		// shake out completely - on a marker the spline simply declined, which
		// says nothing about whether the camera is moving.
		const float speed = s_pathSpeedValid ? s_pathSpeed : -1.0f;

		float dp[3], dr[3];
		shake::evaluate(p, now, speed, dp, dr);

		auto* b = (uint8_t*)self;
		float* ax = (float*)(b + rdirector::OFF_FrameMatrixA); // right
		float* ay = (float*)(b + rdirector::OFF_FrameMatrixB); // forward
		float* az = (float*)(b + rdirector::OFF_FrameMatrixC); // up
		float* fpos = (float*)(b + rdirector::OFF_FramePosition);

		// What the shake ACTUALLY produced, once a second.
		//
		// Worth keeping past the bug it was written for: a per-marker override
		// silently beats the ini, and nothing else in the mod surfaces that, so
		// "I changed the setting and nothing happened" is otherwise unfalsifiable
		// from the outside.
		{
			const Config& c = Config::get();
			if (c.shakeDebugLog)
			{
				static uint32_t s_last = 0;
				const uint32_t now32 = GetTickCount();
				if (now32 - s_last > 1000)
				{
					s_last = now32;

					// Resolve a COPY before printing. p still holds the raw
					// parameters at this point - evaluate() takes it by value
					// and expands simple mode internally - so printing p
					// directly reported the amplitudes simple mode was about to
					// overwrite, not the ones it used. That read as "jitter
					// rotation is zero" when it was 0.12.
					shake::Params shown = p;
					shown.resolve();

					logger::write("info",
						"shake: t=%.0f rot=%.3f/%.3f/%.3f deg pos=%.4f/%.4f/%.4f "
						"| mode=%s axisRot=%.2f/%.2f/%.2f swayRot=%.3f jitRot=%.3f",
						now, dr[0], dr[1], dr[2], dp[0], dp[1], dp[2],
						shown.simpleMode ? "simple" : "complex",
						shown.axisRot[0], shown.axisRot[1], shown.axisRot[2],
						shown.sway.rotAmp, shown.jitter.rotAmp);
				}
			}
		}

		// Translate along the camera's own axes.
		for (int i = 0; i < 3; ++i)
			fpos[i] += ax[i] * dp[0] + ay[i] * dp[1] + az[i] * dp[2];

		// Rotate the basis about its own axes. Small angles, so ordering barely
		// matters; each rotation just spins the two rows it does not act about.
		constexpr float kDeg2Rad = 3.14159265f / 180.0f;
		rotatePair(ay, az, dr[0] * kDeg2Rad); // pitch, about local right
		rotatePair(ax, az, dr[1] * kDeg2Rad); // roll,  about local forward
		rotatePair(ax, ay, dr[2] * kDeg2Rad); // yaw,   about local up
	}

	// -------------------------------------------------------------------------
	// Mirror the pose we just wrote into the frame the EDITOR authors from.
	//
	// camReplayBaseCamera::GetDefaultFrame clones the director's
	// GetFrameNoPostEffects() - m_Frame, one stage earlier than the
	// m_PostEffectFrame we render into - and a marker created without saved
	// camera info is written straight out of it. So adding a marker mid-blend
	// authored it at the game's straight-line pose while the viewport showed
	// ours, and the camera stepped by exactly the gap. That is what makes a
	// rotation-only correction impossible: dropping a marker to fix the aim
	// moves the path too.
	//
	// Safe to write. Only that authoring path and one heading query in
	// CamInterface read this frame on the replay director, and Update() clones
	// it fresh from the active camera at the top of every tick - so nothing we
	// put here survives into the next frame's input, and there is no loop.
	//
	// Called BEFORE the shake on purpose. The shake is a per-frame offset; a
	// marker authored from a shaken frame would carry that lean forever.
	//
	// Unconditional: a marker landing where the camera is not has no upside, so
	// there is nothing here for a setting to choose between.
	// -------------------------------------------------------------------------
	static void mirrorAuthoringFrame(void* self)
	{
		auto* b = (uint8_t*)self;
		auto copy = [&](uint32_t from, uint32_t to) {
			const Vec3 v((const float*)(b + from));
			if (finite3(v)) v.store((float*)(b + to));
		};

		copy(rdirector::OFF_FrameMatrixA,  rdirector::OFF_AuthorMatrixA);
		copy(rdirector::OFF_FrameMatrixB,  rdirector::OFF_AuthorMatrixB);
		copy(rdirector::OFF_FrameMatrixC,  rdirector::OFF_AuthorMatrixC);
		copy(rdirector::OFF_FramePosition, rdirector::OFF_AuthorPosition);

		const float fov = *(const float*)(b + rdirector::OFF_FrameFov);
		if (isfinite(fov)) *(float*)(b + rdirector::OFF_AuthorFov) = fov;
	}

	static void __fastcall hkUpdateSmoothing(void* self)
	{
		// BEFORE the original, deliberately: this is the only moment the
		// director's desired (unsmoothed) frame still exists, and the mounted
		// path needs it to tell how much smoothing it is about to overwrite.
		capturePreSmooth(self);

		origUpdateSmoothing(self);

		if (!self) return;
		s_lastDirector = self;

		// The renderer needs a per-frame tick on the game's main thread, and
		// this detour is the only place we reliably get one - it runs every
		// frame the editor is up, playing or paused. It also has to run before
		// the enabled check: a render in progress must still be able to finish
		// or abort cleanly if the mod is toggled off mid-way.
		render::pump();

		// Follow the editor to whichever project and clip are open, so per-marker
		// settings stop leaking across clips. Must run BEFORE the flush below:
		// a project change saves the outgoing store, and doing that after tick()
		// would let the debounce write the outgoing entries under the incoming
		// project's filename.
		rsettings::syncScope();

		// Deferred flush of per-marker settings. Cheap: a compare until the
		// store has actually been quiet for half a second. See rsettings::tick.
		rsettings::tick();

		// Same argument, and the same thread requirement: a depth-of-field
		// session's requests arrive on ReShade's thread but its seeks have to
		// happen here. Outside the enabled check so a session in flight can
		// always release the HUD and the playhead it took.
		dofsession::pump();

		// Also outside the enabled check, and outside the __try below: it is a
		// plain metadata write with its own null guards, and the leash should
		// behave the same whether or not the spline is switched on.
		limits::applyDistanceLimit(self);
		limits::applyZoomLimit(self);

		// Late hooks. Reaching IsMarkerControlEditable means going through the
		// marker storage, which does not exist until a clip is open - so this
		// cannot be done from install() and has to be retried until it takes.
		limits::tick();

		if (!Config::get().enabled) return;

		// The replay camera runs on the render/sim path; a throw escaping here
		// would take the game down mid-playback. Anything unexpected just means
		// this frame keeps stock behaviour.
		__try
		{
			applySpline(self);

			// Before the shake, so what the editor saves is the curve's pose
			// rather than the curve plus this frame's random lean.
			mirrorAuthoringFrame(self);

			// Shake runs even when the spline declined this marker - it is not
			// tied to blending. Look up the current marker for its overrides.
			void* storage = game::markerStorage();
			void* marker = storage
				? rstorage::tryGetMarker(storage, rdirector::currentMarkerIndex(self))
				: nullptr;
			applyShake(self, marker);

			// LAST, deliberately. The aperture offset has to survive everything
			// above it, and it is not part of the shot: the spline and the shake
			// describe where the camera is, this describes where on the lens the
			// sample was taken from.
			dofsession::applyToFrame(self);

			// After every writer, so the position it prints is the one that
			// reaches the viewport - not the one the spline proposed.
			traceFrame(self);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	void install()
	{
		if (!game::addr_UpdateSmoothing)
		{
			logger::write("info", "RockstarEditorPlus: nothing resolved вЂ” stock behaviour retained");
			return;
		}

		memory(game::addr_UpdateSmoothing).hook(hkUpdateSmoothing, &origUpdateSmoothing, "UpdateSmoothing");

		const Config& c = Config::get();
		logger::write("info",
			"RockstarEditorPlus: hooked UpdateSmoothing (pos=%d orient=%d alpha=%.2f speedProfile=%d smoothOnly=%d)",
			(int)c.splinePosition, (int)c.splineOrientation, c.alpha,
			(int)c.smoothSpeedProfile, (int)c.onlySmoothBlend);
	}
}

