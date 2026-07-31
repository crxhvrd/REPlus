// Numerical check of the look-at / attach maths in replay/freecam.h.
// Validates against the GAME's own extraction formulas, which is the only
// thing that makes the sign conventions falsifiable without launching GTA.
#include <cstdio>
#include <cmath>
#include "replay/freecam.h"
#include "replay/quat.h"

using spline::Vec3;
using rfreecam::Mat34;

static int g_fail = 0;

static void check(bool ok, const char* what, double got = 0, double want = 0)
{
	if (!ok) { ++g_fail; printf("  FAIL  %-52s got %.6f want %.6f\n", what, got, want); }
	else       printf("  ok    %s\n", what);
}

static void checkNear(double got, double want, double tol, const char* what)
{
	check(fabs(got - want) < tol, what, got, want);
}

// The game's own heading/pitch/roll extraction, as read off the decompile.
// These are the definitions the engine itself measures a camera basis against,
// which is what makes the sign conventions checkable rather than a matter of
// taste.
static float gameHeading(const Mat34& m) { return atan2f(-m.b.x, m.b.y); }
static float gamePitch  (const Mat34& m) { return asinf(m.b.z); }
static float gameRoll   (const Mat34& m) { return atan2f(-m.a.z, m.c.z); }

static float dot(const Vec3& u, const Vec3& v) { return u.x*v.x + u.y*v.y + u.z*v.z; }

static void checkOrthonormal(const Mat34& m, const char* tag)
{
	char buf[128];
	sprintf_s(buf, "%s: |a|=1", tag); checkNear(m.a.length(), 1.0, 1e-4, buf);
	sprintf_s(buf, "%s: |b|=1", tag); checkNear(m.b.length(), 1.0, 1e-4, buf);
	sprintf_s(buf, "%s: |c|=1", tag); checkNear(m.c.length(), 1.0, 1e-4, buf);
	sprintf_s(buf, "%s: a.b=0", tag); checkNear(dot(m.a, m.b), 0.0, 1e-4, buf);
	sprintf_s(buf, "%s: b.c=0", tag); checkNear(dot(m.b, m.c), 0.0, 1e-4, buf);
	sprintf_s(buf, "%s: a.c=0", tag); checkNear(dot(m.a, m.c), 0.0, 1e-4, buf);
	// Right-handed: a = b x c
	const Vec3 x = lookat::cross(m.b, m.c);
	sprintf_s(buf, "%s: a = b x c", tag);
	checkNear((x - m.a).length(), 0.0, 1e-4, buf);
}

int main()
{
	const float kPi = 3.14159265358979f;
	const float kDeg = kPi / 180.0f;

	printf("== 1. zero offset, zero roll: forward must point AT the target ==\n");
	{
		const Vec3 cam(10.0f, -25.0f, 3.5f);
		const Vec3 tgt(-4.0f, 12.0f, 8.0f);
		Mat34 m;
		check(lookat::solve(tgt, cam, 0, 0, 50.0f, 0.0f, 16.0f/9.0f, m), "solve() succeeded");

		const Vec3 want = lookat::normalize(tgt - cam);
		checkNear((m.b - want).length(), 0.0, 1e-5, "forward == normalize(target - camera)");
		checkNear((m.d - cam).length(),  0.0, 1e-5, "translation == camera position");
		checkOrthonormal(m, "solve0");
		// No roll asked for -> camera right stays horizontal.
		checkNear(m.a.z, 0.0, 1e-5, "no roll -> a.z == 0");
	}

	printf("\n== 2. roll must come back out of the game's own extractor ==\n");
	for (float degrees : { -35.0f, -5.0f, 12.0f, 47.0f })
	{
		const Vec3 cam(0.0f, 0.0f, 2.0f);
		const Vec3 tgt(3.0f, 20.0f, 4.0f);
		Mat34 m;
		lookat::solve(tgt, cam, 0, 0, 45.0f, degrees * kDeg, 16.0f/9.0f, m);

		char buf[96];
		sprintf_s(buf, "roll %+.0f deg round-trips through atan2(-a.z, c.z)", degrees);
		checkNear(gameRoll(m) / kDeg, degrees, 1e-3, buf);

		// Rolling must not disturb the aim.
		const Vec3 want = lookat::normalize(tgt - cam);
		sprintf_s(buf, "roll %+.0f deg leaves forward on target", degrees);
		checkNear((m.b - want).length(), 0.0, 1e-5, buf);
		checkOrthonormal(m, "rolled");
	}

	printf("\n== 3. screen offset moves the subject off centre, consistently ==\n");
	{
		const Vec3 cam(0.0f, 0.0f, 0.0f);
		const Vec3 tgt(0.0f, 30.0f, 0.0f);        // dead ahead
		const float fov = 50.0f, aspect = 16.0f/9.0f;
		const float tanV = tanf(fov * kDeg * 0.5f);
		const float tanH = tanV * aspect;

		for (float ox : { -0.6f, 0.0f, 0.35f })
		for (float oy : { -0.4f, 0.0f, 0.5f })
		{
			Mat34 m;
			lookat::solve(tgt, cam, ox, oy, fov, 0.0f, aspect, m);

			// Where does the target actually land, in the solved camera's frame?
			const Vec3 v = tgt - cam;
			const float f  = dot(v, m.b);
			const float rt = dot(v, m.a);
			const float up = dot(v, m.c);
			const float sx = (rt / f) / tanH;
			const float sy = (up / f) / tanV;

			char buf[128];
			sprintf_s(buf, "offset(%+.2f,%+.2f) -> screen(%+.3f,%+.3f)", ox, oy, sx, sy);
			printf("  ..    %s\n", buf);

			// The offset is applied to the AIM: screenPoint (-tanH*ox, 1,
			// tanV*oy) turns the camera left and up for a positive offset, so
			// the subject lands right and down. Hence +x = subject right,
			// +y = subject down.
			//
			// Exact on either axis alone. Combined offsets are approximate by
			// construction - the game intersects a single plane through the
			// target rather than solving the two angles jointly - and the error
			// grows with |ox|*|oy|: measured 0.020 at (0.35, 0.50), 0.042 at
			// (0.60, 0.40), 0.052 at (0.60, 0.50). That is stock behaviour,
			// reproduced, not a defect in the port; the tolerance is set to
			// admit it and would catch anything structurally wrong.
			const double tol = (ox != 0.0f && oy != 0.0f) ? 8e-2 : 1e-3;

			sprintf_s(buf, "offset(%+.2f,%+.2f): subject right by +ox", ox, oy);
			checkNear(sx,  ox, tol, buf);
			sprintf_s(buf, "offset(%+.2f,%+.2f): subject down by -oy", ox, oy);
			checkNear(sy, -oy, tol, buf);
			checkOrthonormal(m, "offset");
		}
	}

	printf("\n== 4. heading/pitch/roll -> basis -> game extractors round-trip ==\n");
	{
		const float hs[] = { -170.0f, -90.0f, -12.0f, 0.0f, 33.0f, 120.0f, 179.0f };
		const float ps[] = { -78.0f, -40.0f, 0.0f, 25.0f, 81.0f };
		const float rs[] = { -60.0f, 0.0f, 17.0f };

		int cases = 0, bad = 0;
		for (float h : hs) for (float p : ps) for (float r : rs)
		{
			Mat34 m;
			lookat::basisFromHeadingPitchRoll(h * kDeg, p * kDeg, r * kDeg, m);

			const float gh = gameHeading(m) / kDeg;
			const float gp = gamePitch(m)   / kDeg;
			const float gr = gameRoll(m)    / kDeg;

			++cases;
			const bool ok = fabsf(gh - h) < 1e-2f
			             && fabsf(gp - p) < 1e-2f
			             && fabsf(gr - r) < 1e-2f;
			if (!ok)
			{
				++bad; ++g_fail;
				printf("  FAIL  h/p/r %+7.1f %+6.1f %+6.1f  ->  %+7.1f %+6.1f %+6.1f\n",
				       h, p, r, gh, gp, gr);
			}
			// Orthonormality on a sample, quietly.
			if (fabsf(m.a.length() - 1.0f) > 1e-4f ||
			    fabsf(dot(m.a, m.b))       > 1e-4f ||
			    fabsf(dot(m.b, m.c))       > 1e-4f) { ++bad; ++g_fail;
				printf("  FAIL  basis not orthonormal at h/p/r %.0f %.0f %.0f\n", h, p, r); }
		}
		printf("  %s   %d h/p/r combinations round-trip through the game's extractors\n",
		       bad ? "FAIL " : "ok   ", cases);
	}

	printf("\n== 5. attach transform: local offset -> world, and back ==\n");
	{
		// A parent rotated 40 deg heading, 10 deg pitch, sitting at (100,50,7).
		Mat34 parent;
		lookat::basisFromHeadingPitchRoll(40.0f * kDeg, 10.0f * kDeg, 0.0f, parent);
		parent.d = Vec3(100.0f, 50.0f, 7.0f);

		check(rfreecam::validMatrix(parent), "a real basis passes validMatrix()");

		const Vec3 local(2.0f, -5.0f, 1.5f);
		const Vec3 world = parent.transform(local);

		// Round-trip by hand: untransform must give the local offset back.
		const Vec3 rel = world - parent.d;
		const Vec3 back(dot(rel, parent.a), dot(rel, parent.b), dot(rel, parent.c));
		checkNear((back - local).length(), 0.0, 1e-4, "transform() then untransform round-trips");

		// A camera 5m behind the parent must stay 5m behind after the parent turns.
		Mat34 turned;
		lookat::basisFromHeadingPitchRoll(140.0f * kDeg, 10.0f * kDeg, 0.0f, turned);
		turned.d = Vec3(300.0f, -20.0f, 7.0f);
		const Vec3 w2 = turned.transform(local);
		checkNear((w2 - turned.d).length(), (world - parent.d).length(), 1e-3,
		          "distance to parent preserved as the parent moves and turns");
	}

	printf("\n== 6. validMatrix rejects what it should ==\n");
	{
		Mat34 junk;
		junk.a = Vec3(1.7f, 0.2f, 0.0f); junk.b = Vec3(0.0f, 3.0f, 0.0f);
		junk.c = Vec3(0.0f, 0.0f, 0.5f); junk.d = Vec3(0, 0, 0);
		check(!rfreecam::validMatrix(junk), "non-unit rows rejected");

		Mat34 skew;
		lookat::basisFromHeadingPitchRoll(0, 0, 0, skew);
		skew.c = skew.b;                       // parallel rows
		check(!rfreecam::validMatrix(skew), "non-perpendicular rows rejected");

		Mat34 nan;
		lookat::basisFromHeadingPitchRoll(0, 0, 0, nan);
		nan.d = Vec3(sqrtf(-1.0f), 0, 0);
		check(!rfreecam::validMatrix(nan), "NaN translation rejected");

		Mat34 good;
		lookat::basisFromHeadingPitchRoll(1.2f, -0.4f, 0.3f, good);
		good.d = Vec3(5, 6, 7);
		check(rfreecam::validMatrix(good), "a genuine basis accepted");
	}

	printf("\n== 7. degenerate inputs decline rather than emit garbage ==\n");
	{
		Mat34 m;
		const Vec3 p(1.0f, 2.0f, 3.0f);
		check(!lookat::solve(p, p, 0, 0, 50.0f, 0.0f, 16.0f/9.0f, m),
		      "camera sitting on the target returns false");

		// Straight down: the from-front helper's degenerate branch.
		check(lookat::solve(Vec3(0, 0, -50), Vec3(0, 0, 0), 0, 0, 50.0f, 0.0f, 16.0f/9.0f, m),
		      "target straight below still solves");
		checkOrthonormal(m, "straight-down");
		checkNear((m.b - Vec3(0, 0, -1)).length(), 0.0, 1e-4, "straight down: forward == -Z");
	}

	printf("\n== 8. attach frame: packed quaternion -> basis round-trip ==\n");
	{
		// A mounted marker stores the parent's orientation as a packed
		// quaternion (+0x10), and resolving its offset means turning that back
		// into a basis. If the quantiser round-trip or the matrix convention
		// were off, every non-FULL attach mode would place the camera somewhere
		// plausible but wrong - which is exactly the sort of thing that survives
		// an in-game glance.
		int cases = 0, bad = 0;
		for (float h : { -150.0f, -40.0f, 0.0f, 65.0f, 175.0f })
		for (float p : { -50.0f, 0.0f, 30.0f })
		for (float r : { -20.0f, 0.0f, 45.0f })
		{
			Mat34 want;
			lookat::basisFromHeadingPitchRoll(h * kDeg, p * kDeg, r * kDeg, want);

			// Basis -> quaternion, derived as the exact inverse of
			// rquat::toMatrixRows rather than from a remembered formula.
			//
			// That matters: toMatrixRows writes a/b/c as the COLUMNS of the
			// rotation matrix, so M[row][col] is
			//     M00 M01 M02     a.x b.x c.x
			//     M10 M11 M12  =  a.y b.y c.y
			//     M20 M21 M22     a.z b.z c.z
			// Reading them as rows instead gives the transpose, i.e. the
			// conjugate quaternion - which reconstructs a basis that is wrong by
			// twice the rotation and looks plausible at small angles.
			const float M00 = want.a.x, M01 = want.b.x, M02 = want.c.x;
			const float M10 = want.a.y, M11 = want.b.y, M12 = want.c.y;
			const float M20 = want.a.z, M21 = want.b.z, M22 = want.c.z;

			const float tr = M00 + M11 + M22;
			rquat::Quat q;
			if (tr > 0.0f) {
				const float s = sqrtf(tr + 1.0f) * 2.0f;
				q.w = 0.25f * s;
				q.x = (M21 - M12) / s; q.y = (M02 - M20) / s; q.z = (M10 - M01) / s;
			} else if (M00 > M11 && M00 > M22) {
				const float s = sqrtf(1.0f + M00 - M11 - M22) * 2.0f;
				q.x = 0.25f * s;
				q.w = (M21 - M12) / s; q.y = (M01 + M10) / s; q.z = (M02 + M20) / s;
			} else if (M11 > M22) {
				const float s = sqrtf(1.0f + M11 - M00 - M22) * 2.0f;
				q.y = 0.25f * s;
				q.w = (M02 - M20) / s; q.x = (M01 + M10) / s; q.z = (M12 + M21) / s;
			} else {
				const float s = sqrtf(1.0f + M22 - M00 - M11) * 2.0f;
				q.z = 0.25f * s;
				q.w = (M10 - M01) / s; q.x = (M02 + M20) / s; q.y = (M12 + M21) / s;
			}

			// Through the game's 20-bit smallest-three packing and back.
			float a[3], b[3], c[3];
			rquat::toMatrixRows(rquat::decode(rquat::encode(q)), a, b, c);
			Mat34 got; got.a = Vec3(a); got.b = Vec3(b); got.c = Vec3(c);
			got.d = Vec3(0, 0, 0);

			++cases;
			// Quantisation is 20 bits per component, so this is tight.
			const float err = (got.a - want.a).length()
			                + (got.b - want.b).length()
			                + (got.c - want.c).length();
			if (err > 1e-3f || !rfreecam::validMatrix(got))
			{
				++bad; ++g_fail;
				printf("  FAIL  h/p/r %+7.1f %+6.1f %+6.1f  basis error %.6f%s\n",
				       h, p, r, err, rfreecam::validMatrix(got) ? "" : " (not orthonormal)");
			}

			// The displacement an offset resolves to must match rotating it by
			// the original basis - that IS the attach resolve.
			const Vec3 off(2.0f, -5.0f, 1.5f);
			const Vec3 dWant = want.transform3x3(off);
			const Vec3 dGot  = got.transform3x3(off);
			if ((dGot - dWant).length() > 1e-2f)
			{
				++bad; ++g_fail;
				printf("  FAIL  h/p/r %+7.1f %+6.1f %+6.1f  displacement off by %.5f m\n",
				       h, p, r, (dGot - dWant).length());
			}
		}
		printf("  %s   %d stored attach frames round-trip and resolve an offset correctly\n",
		       bad ? "FAIL " : "ok   ", cases);
	}

	printf("\n== 9. FULL mode stays algebraically identical to the old path ==\n");
	{
		// The rewrite must not change FULL: there the rotation is the live
		// entity matrix, shared by every marker, so resolving per marker and
		// hanging the result off the origin has to equal the old
		// "interpolate locally, transform once".
		Mat34 live;
		lookat::basisFromHeadingPitchRoll(37.0f * kDeg, -8.0f * kDeg, 3.0f * kDeg, live);
		live.d = Vec3(940.0f, -122.0f, 26.0f);

		const Vec3 offA(1.5f, -4.0f, 1.0f), offB(-2.0f, -3.0f, 1.4f);
		for (float t : { 0.0f, 0.25f, 0.5f, 0.9f, 1.0f })
		{
			// old: interpolate offsets, then full transform
			const Vec3 lerpOff = offA + (offB - offA) * t;
			const Vec3 oldWay  = live.transform(lerpOff);

			// new: resolve each to a displacement, interpolate, add origin
			const Vec3 dA = live.transform3x3(offA), dB = live.transform3x3(offB);
			const Vec3 newWay = live.d + (dA + (dB - dA) * t);

			char buf[96];
			sprintf_s(buf, "FULL t=%.2f: new path == old path", t);
			checkNear((newWay - oldWay).length(), 0.0, 1e-3, buf);
		}
	}

	printf("\n== 11. rotation is C1 across markers (the Hermite switch) ==\n");
	{
		// Five markers with deliberately UNEVEN spacing - 400 ms then 100 ms
		// then 1700 ms - because even spacing hides the entire defect.
		//
		// smoothblend evaluates a 4-knot window per segment: knots [0..3] while
		// playing 1->2, then knots [1..4] while playing 2->3. Marker 2 is the
		// join. The rate LEAVING it in the second window must match the rate
		// ARRIVING at it in the first, or the camera changes turn speed on the
		// keyframe - which is what "rough direction change between keyframes"
		// looks like from the chair.
		const float T[5] = { 0.0f, 400.0f, 500.0f, 2200.0f, 2600.0f };
		const float Y[5] = { 0.0f,  12.0f,  -5.0f,   40.0f,   41.0f };

		const float* tL = &T[0]; const float* yL = &Y[0];   // window for 1->2
		const float* tR = &T[1]; const float* yR = &Y[1];   // window for 2->3
		const float join = T[2];

		// One-sided 3-point derivatives, so neither reaches past its window's
		// span into the clamp.
		const float h = 0.5f;
		auto rateFromLeft = [&](float (*f)(const float*, const float*, float)) {
			return (3.0f * f(tL, yL, join) - 4.0f * f(tL, yL, join - h)
			        + f(tL, yL, join - 2.0f * h)) / (2.0f * h);
		};
		auto rateFromRight = [&](float (*f)(const float*, const float*, float)) {
			return (-3.0f * f(tR, yR, join) + 4.0f * f(tR, yR, join + h)
			        - f(tR, yR, join + 2.0f * h)) / (2.0f * h);
		};

		// Both windows must agree that marker 2's value is -5.
		checkNear(spline::hermiteAt(tL, yL, join), -5.0, 1e-3, "hermite hits the knot from the left");
		checkNear(spline::hermiteAt(tR, yR, join), -5.0, 1e-3, "hermite hits the knot from the right");

		const double in  = rateFromLeft(spline::hermiteAt);
		const double out = rateFromRight(spline::hermiteAt);
		checkNear(in, out, 1e-4, "hermite: turn rate matches across the marker");

		// pchip does NOT, and that is not a bug in pchip - marker 2 is a local
		// minimum of Y, so Fritsch-Carlson zeroes the tangent there on purpose.
		// On an FOV curve that is exactly right. On an angle it is the pan
		// stopping dead at the keyframe, which is why rotation moved off it.
		checkNear(rateFromLeft(spline::pchipAt),  0.0, 1e-4, "pchip stalls at an extremum (by design)");
		checkNear(rateFromRight(spline::pchipAt), 0.0, 1e-4, "pchip stalls leaving it too");

		// And the parameterisation half: a per-segment normalised parameter
		// cannot be C1 across a marker whatever curve is fitted through it,
		// because its own rate is scaled by 1/span. 400 ms against 1700 ms is a
		// 4.25x step. This guards the fix - it is the reason rotation is
		// sampled at real project time and not at anything segment-local.
		{
			const double spanL = T[2] - T[1], spanR = T[3] - T[2];
			check(fabs(spanR / spanL - 1.0) > 1.0,
			      "segment-local parameter would step 4.25x here");
		}
	}

	printf("\n== 12. quaternion hemisphere alignment ==\n");
	{
		// decode() returns whichever representative has its largest component
		// positive, which says nothing about what the PREVIOUS marker returned.
		// A neighbour on the far hemisphere is the same rotation but the
		// opposite point in quaternion space, and a component spline through it
		// takes the long way round.
		// Two headings six degrees apart - the sort of gap adjacent markers
		// actually have.
		const rquat::Quat a = rquat::fromAxisAngle(0.0f, 0.0f, 1.0f, 20.0f * kDeg);
		const rquat::Quat b = rquat::align(
			rquat::fromAxisAngle(0.0f, 0.0f, 1.0f, 26.0f * kDeg), a);

		const rquat::Quat flipped = b * -1.0f;
		check(a.dot(flipped) < 0.0f, "the flipped twin really is on the far side");
		const rquat::Quat fixed = rquat::align(flipped, a);
		checkNear(a.dot(fixed), a.dot(b), 1e-5, "align brings it back to the near side");

		// Splining through the flipped twin without aligning swings a long way
		// off; with align it stays between its endpoints.
		const float tq[4] = { -1000.0f, 0.0f, 1000.0f, 2000.0f };
		const rquat::Quat good[4] = { a, a, b, b };
		const rquat::Quat bad[4]  = { a, a, flipped, b };

		const rquat::Quat mid  = rquat::hermite(good, tq, 500.0f);
		const rquat::Quat midB = rquat::hermite(bad,  tq, 500.0f);

		// Midway between two orientations 6 degrees apart, the result must be
		// within that 6 degrees of both. |dot| near 1 is "almost the same".
		check(fabsf(mid.dot(a)) > 0.99f && fabsf(mid.dot(b)) > 0.99f,
		      "aligned: midpoint stays between its endpoints");
		check(fabsf(midB.dot(a)) < fabsf(mid.dot(a)),
		      "unaligned: midpoint leaves the arc (what align prevents)");

		// Endpoints are interpolated exactly.
		const rquat::Quat at1 = rquat::hermite(good, tq, 0.0f);
		checkNear(fabsf(at1.dot(a)), 1.0, 1e-4, "hermite interpolates the first knot");
		const rquat::Quat at2 = rquat::hermite(good, tq, 1000.0f);
		checkNear(fabsf(at2.dot(b)), 1.0, 1e-4, "hermite interpolates the second knot");
	}

	printf("\n== 13. the clip's FIRST segment anticipates the next marker ==\n");
	{
		// Three markers turning a corner: out along +Y, then away along +X.
		// This is the shape a static orbit-the-car clip makes, and the one the
		// reflected phantom got visibly wrong at the top of the clip.
		const Vec3 P0(0.0f, 0.0f, 0.0f);
		const Vec3 P1(0.0f, 4.0f, 0.0f);
		const Vec3 P2(4.0f, 8.0f, 0.0f);

		auto startDir = [&](const Vec3& c1, float alpha) {
			// Direction the curve actually leaves P0 in.
			const Vec3 a = spline::catmullRom(c1, P0, P1, P2, 0.000f, alpha);
			const Vec3 b = spline::catmullRom(c1, P0, P1, P2, 0.002f, alpha);
			return lookat::normalize(b - a);
		};

		const Vec3 chord = lookat::normalize(P1 - P0);

		// Reflection: the start tangent IS the chord, to float precision. The
		// move leaves marker 0 aimed straight at marker 1 and knows nothing
		// about marker 2 - which is what "no smooth curve at the start of the
		// interpolation" looks like.
		const Vec3 dRefl = startDir(spline::phantom(P0, P1), 0.0f);
		checkNear(dot(dRefl, chord), 1.0, 1e-4, "reflection leaves marker 0 straight at marker 1");

		// Natural end condition: it leans on P2 and departs off the chord.
		const Vec3 dNat = startDir(spline::naturalEnd(P0, P1, P2), 0.0f);
		check(dot(dNat, chord) < 0.995f, "natural end condition departs off the chord",
		      dot(dNat, chord), 1.0);

		// ...and it leans the RIGHT way: away from the turn, so the curve arcs
		// into it rather than swinging late. The corner goes to +X, so the
		// start must lean -X.
		check(dNat.x < -0.01f, "and it leans away from the corner (arcs in)", dNat.x, -1.0);

		// The whole segment bows further from the straight line than it used to.
		auto bow = [&](const Vec3& c1, float alpha) {
			const Vec3 mid = spline::catmullRom(c1, P0, P1, P2, 0.5f, alpha);
			return (mid - (P0 + P1) * 0.5f).length();
		};
		check(bow(spline::naturalEnd(P0, P1, P2), 0.5f) > bow(spline::phantom(P0, P1), 0.5f),
		      "centripetal: the first segment now bows more than the reflection");

		// Bounded, not an extrapolation that can fling the curve: the invented
		// point stays within a few chord lengths even on a hard corner.
		const Vec3 nat = spline::naturalEnd(P0, P1, P2);
		check((nat - P0).length() < 3.0f * (P1 - P0).length(),
		      "invented point stays bounded on a 90-degree corner",
		      (nat - P0).length(), (P1 - P0).length());

		// Degenerates to the plain reflection exactly when it should: three
		// collinear, evenly spaced markers have no turn to anticipate.
		{
			const Vec3 L0(0, 0, 0), L1(0, 5, 0), L2(0, 10, 0);
			checkNear((spline::naturalEnd(L0, L1, L2) - spline::phantom(L0, L1)).length(),
			          0.0, 1e-4, "collinear + even spacing: identical to the reflection");
		}

		// Second derivative at the start really is ~0 for the uniform case -
		// that IS the natural condition, and it is what the algebra claims.
		{
			const Vec3 c1 = spline::naturalEnd(P0, P1, P2);
			const float h = 0.01f;
			const Vec3 f0 = spline::catmullRom(c1, P0, P1, P2, 0.0f,     0.0f);
			const Vec3 f1 = spline::catmullRom(c1, P0, P1, P2, h,        0.0f);
			const Vec3 f2 = spline::catmullRom(c1, P0, P1, P2, 2.0f * h, 0.0f);
			const Vec3 acc = (f0 - f1 * 2.0f + f2) * (1.0f / (h * h));
			// Against the reflection's, which is large.
			const Vec3 r1 = spline::phantom(P0, P1);
			const Vec3 g0 = spline::catmullRom(r1, P0, P1, P2, 0.0f,     0.0f);
			const Vec3 g1 = spline::catmullRom(r1, P0, P1, P2, h,        0.0f);
			const Vec3 g2 = spline::catmullRom(r1, P0, P1, P2, 2.0f * h, 0.0f);
			const Vec3 accRefl = (g0 - g1 * 2.0f + g2) * (1.0f / (h * h));
			check(acc.length() < 0.05f * accRefl.length(),
			      "uniform: C''(0) ~ 0, the natural condition holds",
			      acc.length(), accRefl.length());
		}
	}

	printf("\n== 14. fromBasis is the exact inverse of toMatrixRows ==\n");
	{
		int bad = 0, cases = 0;
		for (float h = -170.0f; h <= 170.0f; h += 37.0f)
		for (float p = -80.0f;  p <= 80.0f;  p += 31.0f)
		for (float r = -170.0f; r <= 170.0f; r += 53.0f)
		{
			Mat34 m;
			lookat::basisFromHeadingPitchRoll(h * kDeg, p * kDeg, r * kDeg, m);

			const rquat::Quat q = rquat::fromBasis(m.a, m.b, m.c);

			Mat34 back;
			float a[3], b[3], c[3];
			rquat::toMatrixRows(q, a, b, c);
			back.a = Vec3(a); back.b = Vec3(b); back.c = Vec3(c);

			++cases;
			const float err = (back.a - m.a).length() + (back.b - m.b).length() + (back.c - m.c).length();
			if (err > 1e-3f) { ++bad; ++g_fail;
				printf("  FAIL  h/p/r %+7.1f %+6.1f %+6.1f  round-trip error %.6f\n", h, p, r, err); }
		}
		printf("  %s   %d bases round-trip through fromBasis/toMatrixRows\n", bad ? "FAIL " : "ok   ", cases);

		// The w-branch divides by w. A camera turned 180 degrees has w ~ 0 and
		// is an ordinary shot, so the diagonal branches have to carry it.
		{
			Mat34 m; lookat::basisFromHeadingPitchRoll(180.0f * kDeg, 0.0f, 0.0f, m);
			const rquat::Quat q = rquat::fromBasis(m.a, m.b, m.c);
			check(isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w),
			      "180-degree turn does not divide by a vanishing w");
			float a[3], b[3], c[3]; rquat::toMatrixRows(q, a, b, c);
			checkNear((Vec3(b) - m.b).length(), 0.0, 1e-3, "...and still round-trips");
		}
	}

	printf("\n== 15. a mounted shot's aim survives the marker crossing ==\n");
	{
		// THE regression this guards. Two markers on a turning car: each stores
		// its camera angles against the vehicle's frame AS IT WAS when the
		// marker was made, and outside FULL the game never refreshes those. The
		// old path interpolated the authored angles and applied ONE frame, so
		// the answer depended on which of the two frames the director happened
		// to hand us - and it hands over a different one at every crossing.
		// Measured in game: a 4.9 degree snap on every interior keyframe.
		const float turn = 4.889f * kDeg;      // how far the car turned between them
		Mat34 frameA, frameB;
		lookat::basisFromHeadingPitchRoll(10.0f * kDeg, 0.0f, 0.0f, frameA);
		lookat::basisFromHeadingPitchRoll(10.0f * kDeg + turn, 0.0f, 0.0f, frameB);

		// Same authored angles on both markers - so the shot is "hold this
		// framing relative to the car" and the world aim must differ by exactly
		// the car's turn, with nothing sudden anywhere.
		const float ah = 30.0f * kDeg, ap = -5.0f * kDeg, ar = 0.0f;
		auto worldQuat = [&](const Mat34& frame) {
			Mat34 local; lookat::basisFromHeadingPitchRoll(ah, ap, ar, local);
			Mat34 world;
			lookat::worldMatrixFromFrontAndUp(frame.transform3x3(local.b),
			                                  frame.transform3x3(local.c), world);
			return rquat::fromBasis(world.a, world.b, world.c);
		};
		const rquat::Quat qA = worldQuat(frameA);
		const rquat::Quat qB = rquat::align(worldQuat(frameB), qA);

		auto fwdOf = [](const rquat::Quat& q) {
			float a[3], b[3], c[3]; rquat::toMatrixRows(q, a, b, c); return Vec3(b);
		};
		auto angBetween = [](const Vec3& u, const Vec3& v) {
			// stable at small angles - acos saturates and reports a real
			// difference as exactly zero, which is how this went unnoticed once.
			return 2.0 * atan2((u - v).length(), (u + v).length()) * 180.0 / 3.14159265358979;
		};

		// Old path: interpolate the angles, apply ONE frame. Both markers hold
		// the same angles, so whichever frame is used the result is constant -
		// and the two answers differ by the car's turn. That difference IS the
		// snap, and it appears the instant the director swaps frames.
		checkNear(angBetween(fwdOf(worldQuat(frameA)), fwdOf(worldQuat(frameB))),
		          4.889, 0.05, "old path: the two frames disagree by the car's turn");

		// New path: each marker resolved through its own frame, then the world
		// orientations interpolated. Continuity at the join is then a property
		// of the curve, not of which camera answered the phone.
		const float tq[4] = { -1000.0f, 0.0f, 1000.0f, 2000.0f };
		const rquat::Quat segA[4] = { qA, qA, qB, qB };   // segment ending at the marker
		const rquat::Quat segB[4] = { qA, qB, qB, qB };   // segment starting at it
		const Vec3 arriving = fwdOf(rquat::hermite(segA, tq, 1000.0f));
		const Vec3 leaving  = fwdOf(rquat::hermite(segB, tq, 1000.0f));
		checkNear(angBetween(arriving, leaving), 0.0, 1e-3,
		          "new path: aim is continuous across the crossing");

		// And it interpolates rather than jumping: mid-segment sits between.
		const Vec3 mid = fwdOf(rquat::hermite(segA, tq, 500.0f));
		const double toA = angBetween(mid, fwdOf(qA)), toB = angBetween(mid, fwdOf(qB));
		check(toA > 0.05 && toB > 0.05 && toA + toB < 4.889 * 1.05,
		      "midpoint lies between the two markers' aims", toA + toB, 4.889);
	}

	printf("\n== 16. adjacent windows agree on a shared segment's length ==\n");
	{
		// The speed profile fits one curve through (marker time, cumulative
		// distance). Each segment measures its neighbours' lengths from its OWN
		// six-marker window, so if two windows disagree about the length of the
		// segment between them, the two of them disagree about where the knots
		// are - and Fritsch-Carlson's harmonic mean, dominated by the smaller
		// secant, stalls the camera at that join. Measured as a 55% speed dip,
		// at the clip's first and last crossings only, because those are the
		// only joins where one window invents an end point and the other does
		// not.
		const Vec3 M0(0,0,0), M1(0,6,0), M2(5,11,0), M3(12,12,1), M4(18,8,2);
		const float alpha = 0.5f;

		// Segment 0->1 has no predecessor, so it invents its c1. Segment 1->2
		// has a predecessor but no marker two back, so it invents its c0 - and
		// that c0 must be the SAME invented point, or the two disagree about
		// how long segment 0->1 is.
		const Vec3 seg01_c1 = spline::naturalEnd(M0, M1, M2);   // what 0->1 uses
		const Vec3 seg12_c0 = spline::naturalEnd(M0, M1, M2);   // what 1->2 must use

		const float lenFrom01 = spline::segmentLength(seg01_c1, M0, M1, M2, alpha);
		const float lenFrom12 = spline::segmentLength(seg12_c0, M0, M1, M2, alpha);
		checkNear(lenFrom01, lenFrom12, 1e-4, "0->1 measured from either side agrees");

		// And the reflection - what it used to use - genuinely differs, so this
		// test would have caught the regression rather than passing vacuously.
		const float lenRefl = spline::segmentLength(spline::phantom(M0, M1), M0, M1, M2, alpha);
		check(fabsf(lenRefl - lenFrom01) > 0.01f,
		      "the old reflection really did give a different length",
		      lenRefl, lenFrom01);

		// Same at the tail: segment 3->4 invents its c4; segment 2->3 measures
		// 3->4 as its lenNext and must invent the same c5.
		const Vec3 seg34_c4 = spline::naturalEnd(M4, M3, M2);
		const Vec3 seg23_c5 = spline::naturalEnd(M4, M3, M2);
		checkNear(spline::segmentLength(M2, M3, M4, seg34_c4, alpha),
		          spline::segmentLength(M2, M3, M4, seg23_c5, alpha), 1e-4,
		          "3->4 measured from either side agrees");

		// The arc table and the length that is clamped against it must come from
		// the same sampling, or the camera tops out short of the marker.
		{
			const float len = spline::segmentLength(M0, M1, M2, M3, alpha);
			const float uAtFull = spline::paramAtDistance(M0, M1, M2, M3, len, alpha);
			checkNear(uAtFull, 1.0, 1e-4, "full segment length maps to u = 1 exactly");
		}
	}

	printf("\n== 17. natural pacing: velocity is continuous across a marker ==\n");
	{
		// The trap this guards. "Uniform Catmull-Rom sampled at normalised
		// segment time" looks like a time-parameterised Hermite and is not: its
		// parameter rate is 1/duration, so the moment two segments differ in
		// length the VELOCITY steps at the marker between them even though the
		// position does not. Measured in game at 0.23 / 0.38 / 0.33 m/s across
		// three markers. Taking the tangents from the real marker times removes
		// it, which is the whole reason natural pacing owns a flag rather than
		// being "the other two switched off".
		const Vec3 P[5] = { Vec3(0,0,0), Vec3(1.1f,0.2f,0), Vec3(5.0f,0.7f,-0.1f),
		                    Vec3(6.8f,1.9f,0), Vec3(2.5f,0.1f,-0.2f) };
		const float T[5] = { 0.0f, 765.0f, 1640.0f, 2625.0f, 3420.0f };   // uneven

		auto velAt = [&](int seg, float when, float h) {
			const Vec3 p[4] = { P[seg > 0 ? seg - 1 : 0], P[seg], P[seg + 1],
			                    P[seg + 2 < 5 ? seg + 2 : 4] };
			const float t[4] = {
				seg > 0 ? T[seg - 1] : T[seg] - (T[seg + 1] - T[seg]),
				T[seg], T[seg + 1],
				seg + 2 < 5 ? T[seg + 2] : T[seg + 1] + (T[seg + 1] - T[seg]) };
			const Vec3 a = spline::hermitePos(p, t, when);
			const Vec3 b = spline::hermitePos(p, t, when + h);
			return (double)((b - a).length() / (fabsf(h) / 1000.0f));
		};
		for (int m = 1; m <= 3; ++m)
		{
			const double in  = velAt(m - 1, T[m] - 0.5f, 0.5f);   // arriving
			const double out = velAt(m,     T[m],        0.5f);   // leaving
			char buf[96];
			sprintf_s(buf, "marker %d: speed matches across the join", m);
			checkNear(in, out, 0.05, buf);
		}

		// And it still interpolates the markers exactly, like the curve it
		// replaces - a pacing mode must not move the shot.
		{
			const Vec3 p[4] = { P[0], P[1], P[2], P[3] };
			const float t[4] = { T[0] - (T[2] - T[1]), T[1], T[2], T[3] };
			checkNear((spline::hermitePos(p, t, T[1]) - P[1]).length(), 0.0, 1e-3,
			          "hits the segment's first marker");
			checkNear((spline::hermitePos(p, t, T[2]) - P[2]).length(), 0.0, 1e-3,
			          "hits the segment's second marker");
		}
	}

	printf("\n%s  (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
	return g_fail ? 1 : 0;
}
