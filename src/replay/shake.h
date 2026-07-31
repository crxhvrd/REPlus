// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdint>
#include <cmath>

// =============================================================================
//  Procedural camera shake
// =============================================================================
//  Two independent layers, because one fractal sum cannot express what camera
//  shake actually looks like:
//
//    SWAY   - low frequency, larger amplitude. The operator breathing, weight
//             shifting, a boom settling. This is what sells "handheld".
//    JITTER - high frequency, small amplitude. Engine buzz, footfall, rattle.
//
//  Octaves add detail *within* a layer at a fixed half-amplitude falloff, which
//  is not the same thing: sway and jitter need independent amplitude AND rate,
//  and their ratio is most of the character of a shake.
//
//  Every term is directly settable, per axis and per marker. There are no
//  presets - a preset is just a set of numbers you happened to like.
//
//  THE important property: it is a pure function of replay time. No running
//  RNG, no frame counter, no accumulated state. That gives:
//
//    * preview matches export, exactly
//    * scrubbing back and forth reproduces the same shake every pass
//    * a re-render reproduces the take you approved
//    * pausing freezes the shake instead of leaving a still frame jittering
//    * playback speed and fine-scrubbing stay correct with no special cases
//
//  Seed exists so you can reroll a take without changing its character.
// =============================================================================
namespace shake
{
	struct Layer
	{
		float posAmp = 0.0f;  // metres
		float rotAmp = 0.0f;  // degrees
		float freq   = 1.0f;  // oscillations per second, roughly
	};

	// -------------------------------------------------------------------------
	// SIMPLE mode.
	//
	// Four amplitudes in two units is the honest model, and it is the wrong
	// first experience: the mod shipped with all four at zero, so shake did
	// nothing until you found and set several numbers, and the pos:rot ratio -
	// the thing that actually decides whether a shake reads as an operator or
	// as an effect - was yours to get wrong.
	//
	// Simple mode collapses that to ONE intensity, expanded through a fixed
	// ratio below. The ratio is the opinion: 0.05 m of translation per degree
	// of rotation, and a 70/30 split between the slow and fast layers. Those
	// numbers are not derived from anything - they are what handheld looks
	// like, chosen once so the user does not have to choose them.
	//
	// Complex mode is untouched and remains the full model.
	// -------------------------------------------------------------------------
	namespace simple
	{
		// UNITS, and they are the sibling Simple Camera project's, deliberately:
		// amplitude 1.0 means ONE DEGREE of rotation and five centimetres of
		// translation. Its shake evaluates
		//
		//     posMag = amp * 0.05      rotMag = amp * 1.0
		//
		// so a value carried across from a shot authored there means the same
		// thing here. The four constants below therefore sum to exactly 1.0 deg
		// and 0.05 m, and the 70/30 split between the slow and fast layers is
		// what divides them.
		//
		// The split is the opinion - it is what handheld looks like, chosen once
		// so the user does not have to. The SCALE is not an opinion any more; it
		// is a unit, and "amplitude 2.0" now means two degrees rather than
		// whatever 2.0 used to expand to.
		inline constexpr float kSwayPos   = 0.0350f;  // metres   } sum = 0.05 m
		inline constexpr float kJitterPos = 0.0150f;  //          }
		inline constexpr float kSwayRot   = 0.700f;   // degrees  } sum = 1.0 deg
		inline constexpr float kJitterRot = 0.300f;   //          }

		// Sway is SLOW - body drift, not a wobble - and its rate is now the
		// frequency the user types, in HERTZ, rather than a multiplier on a
		// hidden constant. 0.35 Hz is a three-second wander; 0.20 is slower
		// still and is what the shot this was rebuilt for uses.
		//
		// Jitter rides above it at a fixed ratio. Real handheld is a sub-hertz
		// wander with rattle on top, so the two layers want to be an order of
		// magnitude apart - a factor of five puts them in the same busy mid-band
		// and is most of why an even-sounding shake reads as fake.
		inline constexpr float kJitterFreqRatio = 22.9f;   // 8.0 / 0.35, kept
	}

	struct Params
	{
		Layer sway  { 0.0f, 0.0f, 1.8f };
		Layer jitter{ 0.0f, 0.0f, 9.0f };

		int octaves = 3;   // detail within each layer, 1-6
		int seed    = 0;   // reroll without changing character

		// How unevenly the shake plays out over time. 0 is the old behaviour -
		// statistically identical second after second, which is exactly what
		// "too uniform" was. Higher detunes the six channels against each other
		// and adds a slow swell/lull envelope over the whole thing.
		float variation = 0.7f;

		// Per-axis weights on top of the amplitudes.
		// axisPos = { lateral, forward, vertical }
		// axisRot = { pitch,   roll,    yaw      }
		//
		// Rotations are EQUAL by default. Roll used to ship at 0.5 and that was
		// a mistake twice over: it is the axis a handheld rig moves in most
		// obviously, and it is also the one that reads WEAKEST per degree -
		// pitch and yaw shift the whole frame, while roll leaves the centre
		// still and only moves the corners. Damping it was backwards.
		//
		// Forward translation keeps its 0.6: unlike the others it reads as a
		// zoom pump rather than as camera movement.
		float axisPos[3] = { 1.0f, 0.6f, 1.0f };
		float axisRot[3] = { 1.0f, 1.0f, 1.0f };

		// --- Simple mode ---
		bool  simpleMode = true;   // the default a new user meets
		float amplitude  = 1.0f;   // DEGREES of rotation (and 0.05 m per unit)
		float frequency  = 0.35f;  // HERTZ, the slow layer's rate

		// --- Speed coupling (BOTH modes) ---
		//
		// A rig shakes harder when it is moving, and a camera that shakes the
		// same amount standing still is the single thing that reads as "effect"
		// rather than "operator".
		//
		// The speed fed in is the camera's speed along its own path at this
		// project time - a property of the marker layout and the speed profile,
		// NOT a frame-to-frame measurement. So this stays a pure function of
		// time: scrub to t, get the same speed, get the same shake, and a
		// re-render still matches the take you approved.
		float speedAmp     = 0.0f;   // extra amplitude at reference speed
		float speedFreq    = 0.0f;   // extra frequency at reference speed
		float speedRef     = 30.0f;  // m/s that counts as "fully moving"
		bool  stopWhenStill = false; // fade out when the camera is parked

		bool active() const
		{
			if (simpleMode) return amplitude > 0.0f;
			return sway.posAmp   > 0.0f || sway.rotAmp   > 0.0f ||
			       jitter.posAmp > 0.0f || jitter.rotAmp > 0.0f;
		}

		// Expand simple mode into the layer model everything downstream uses.
		// A no-op in complex mode, so callers can apply it unconditionally.
		void resolve()
		{
			if (!simpleMode) return;
			sway.posAmp   = amplitude * simple::kSwayPos;
			sway.rotAmp   = amplitude * simple::kSwayRot;
			jitter.posAmp = amplitude * simple::kJitterPos;
			jitter.rotAmp = amplitude * simple::kJitterRot;

			// The typed frequency IS the sway's rate in Hz; jitter tracks it.
			sway.freq     = frequency;
			jitter.freq   = frequency * simple::kJitterFreqRatio;
		}
	};

	namespace detail
	{
		// Integer hash -> [-1,1]. Cheap, and reproducible on every machine and
		// every run, which is the entire point.
		inline float hash11(uint32_t n)
		{
			n = (n << 13) ^ n;
			n = n * (n * n * 15731u + 789221u) + 1376312589u;
			return (float)(n & 0x7FFFFFFFu) / (float)0x7FFFFFFF * 2.0f - 1.0f;
		}

		// Smooth value noise: hashed lattice, smoothstep between samples.
		// Smoothstep rather than linear so the derivative is continuous -
		// linear leaves a visible tick at every lattice crossing.
		inline float valueNoise(uint32_t seed, float x)
		{
			const float fl = floorf(x);
			const int   i  = (int)fl;
			const float f  = x - fl;

			const float a = hash11((uint32_t)(i)     * 374761393u + seed);
			const float b = hash11((uint32_t)(i + 1) * 374761393u + seed);

			const float u = f * f * (3.0f - 2.0f * f);
			return a + (b - a) * u;
		}

		// Fractal sum. Each octave doubles rate and halves amplitude; 2.03
		// rather than exactly 2 stops the octaves periodically re-aligning into
		// a visible pulse.
		//
		// Normalised by RMS, not by the sum of the amplitudes.
		//
		// Dividing by the sum (1.75 for three octaves) is what everyone writes
		// and it is wrong for this: three quasi-independent values almost never
		// align, so the result clustered around +-0.26 of its stated range and
		// the extremes were effectively unreachable. That is the "grey mush"
		// fBm is known for, and on a camera it reads as a shake with no
		// dynamics - always present, never going anywhere.
		//
		// sqrt(sum of squares) is the standard deviation of the sum, so this
		// puts the output in roughly the same statistical range as a single
		// octave. Peaks now happen. They can exceed 1 occasionally, which is
		// correct - a real shake has the odd big one - and callers clamp
		// nothing, so an amplitude is a typical excursion rather than a hard
		// ceiling.
		inline float fbm(uint32_t seed, float x, int octaves)
		{
			if (octaves < 1) octaves = 1;
			float sum = 0.0f, amp = 1.0f, sumSq = 0.0f;
			for (int o = 0; o < octaves; ++o)
			{
				sum   += valueNoise(seed + (uint32_t)o * 7919u, x) * amp;
				sumSq += amp * amp;
				x     *= 2.03f;
				amp   *= 0.5f;
			}
			return sumSq > 0.0f ? sum / sqrtf(sumSq) : 0.0f;
		}

		// -------------------------------------------------------------------
		// Summed sines - the "handheld" model. Ported from Simple Camera.
		//
		// Same spectrum as the fBm above: 2.03 between octaves and a 0.5
		// amplitude falloff, which at three octaves is 0.571 / 0.286 / 0.143 -
		// exactly Simple Camera's 4/7, 2/7, 1/7. The octave STRUCTURE was never
		// the difference between them.
		//
		// What differs is the waveform. A sine oscillates: it swings out, slows,
		// reverses, follows through. Value noise wanders - a random walk with no
		// commitment to where it was going. An operator's sway is a damped mass
		// with momentum, so it behaves like the former, and that is the whole of
		// why one reads as handheld and the other as drift.
		//
		// Normalised by the SUM here, not the RMS. The opposite of the noise
		// version, and correct for the same underlying reason: sines are
		// deterministic and periodically do all line up, so the sum is a real
		// peak that gets reached. Independent noise octaves almost never align,
		// which is why dividing those by the sum buried them near zero.
		//
		// Phase offsets from the seed rather than rand(), so it stays exactly
		// reproducible - Simple Camera can afford randomness because it never
		// has to render the same take twice.
		// -------------------------------------------------------------------
		inline float sineFbm(uint32_t seed, float x, int octaves)
		{
			if (octaves < 1) octaves = 1;
			if (octaves > 6) octaves = 6;

			// Simple Camera's first three, continued with values that keep the
			// octaves from sharing a rational ratio.
			static const float kPhaseMul[6] = { 1.0f, 1.7f, 2.3f, 3.1f, 4.3f, 5.9f };

			// 0..1000 radians, matching the scale its rand() seeds produced.
			const float ph = (hash11(seed * 2246822519u) * 0.5f + 0.5f) * 1000.0f;

			float sum = 0.0f, amp = 1.0f, total = 0.0f, rate = 1.0f;
			for (int o = 0; o < octaves; ++o)
			{
				sum   += sinf(x * rate + ph * kPhaseMul[o]) * amp;
				total += amp;
				rate  *= 2.03f;
				amp   *= 0.5f;
			}
			return total > 0.0f ? sum / total : 0.0f;
		}

		// Deterministic per-channel frequency spread.
		//
		// The one thing SimpleCamera does that REP did not: run each degree of
		// freedom at a slightly different rate. Six channels at identical rates
		// stay statistically identical forever - different numbers, same
		// texture. Detuned, they beat against each other over tens of seconds,
		// so the composite swells and settles on its own without anything
		// modulating it.
		//
		// Derived from the seed rather than rand(), so it is still exactly
		// reproducible - which is the whole reason REP can render.
		inline float channelRate(uint32_t seed, float spread)
		{
			return 1.0f + hash11(seed * 2654435761u) * spread;
		}

		inline void addLayer(const Layer& L, int octaves,
		                     uint32_t seedBase, float timeMs, float variation,
		                     const float axP[3], const float axR[3],
		                     float outPos[3], float outRot[3])
		{
			if (L.posAmp <= 0.0f && L.rotAmp <= 0.0f) return;

			// sineFbm wants radians; the layer rate is in Hz.
			constexpr float kTwoPi = 6.28318531f;
			const float secs = timeMs / 1000.0f;
			const float t    = secs * (L.freq > 0.0f ? L.freq : 1.0f) * kTwoPi;

			// +-22.5%, unconditionally rather than scaled by variation: the six
			// channels beating against each other IS the character, not an
			// embellishment on top of it.
			constexpr float kSpread = 0.225f;
			(void)variation;

			for (int i = 0; i < 3; ++i)
			{
				const uint32_t sp = seedBase + 0x01u + (uint32_t)i;
				const uint32_t sr = seedBase + 0x11u + (uint32_t)i;

				outPos[i] += sineFbm(sp, t * channelRate(sp, kSpread), octaves)
				           * L.posAmp * axP[i];
				outRot[i] += sineFbm(sr, t * channelRate(sr, kSpread), octaves)
				           * L.rotAmp * axR[i];
			}
		}

		// A slow gain on the whole shake: the operator settling, then shifting,
		// then settling again. ~0.09 Hz, so a swell lasts ten seconds or so.
		//
		// Deliberately shared by both layers and all six channels - one
		// envelope reads as one person breathing, whereas per-channel envelopes
		// just average out to the flat signal this exists to remove.
		//
		// Runs on absolute seconds rather than layer-scaled time so changing
		// Shake Speed does not also speed up the breathing.
		inline float envelope(uint32_t seed, float timeMs, float variation)
		{
			if (variation <= 0.0f) return 1.0f;
			const float n = fbm(seed + 0x5BF0u, (timeMs / 1000.0f) * 0.09f, 2);
			const float g = 1.0f + n * 0.85f * variation;
			return g < 0.05f ? 0.05f : g;
		}
	}

	// Evaluate at a replay time. Outputs are CAMERA-LOCAL:
	//   outPos    = { right, forward, up }   metres
	//   outRotDeg = { pitch, roll,    yaw }  degrees about those axes
	//
	// speedMps is the camera's speed along its path AT THIS TIME. Pass a
	// NEGATIVE value for "not known" - the coupling and the motion gate are
	// then both skipped, and the result is the plain uncoupled shake. Zero is
	// a real speed and means the camera is parked, so the two cannot share a
	// value: a caller that does not know the speed must not be able to
	// accidentally silence the shake.
	//
	// `p` is taken BY VALUE and resolved here, so a caller cannot forget to
	// expand simple mode.
	inline void evaluate(Params p, float timeMs, float speedMps,
	                     float outPos[3], float outRotDeg[3])
	{
		outPos[0] = outPos[1] = outPos[2] = 0.0f;
		outRotDeg[0] = outRotDeg[1] = outRotDeg[2] = 0.0f;
		if (!p.active()) return;
		p.resolve();

		const bool haveSpeed = speedMps >= 0.0f;

		// --- speed coupling ---------------------------------------------
		float sn = 0.0f;
		if (haveSpeed && p.speedRef > 0.0f && speedMps > 0.0f)
		{
			sn = speedMps / p.speedRef;
			if (sn > 1.0f) sn = 1.0f;
		}

		const float ampGain  = 1.0f + p.speedAmp  * sn;
		const float freqGain = 1.0f + p.speedFreq * sn;

		// Motion gate. Smoothstepped over a speed BAND rather than switched at
		// a threshold, which is what removes the pop - and because the input is
		// position on a curve rather than a measurement, no attack/release
		// smoothing is needed. A live rig has to guess where the motion is
		// going; this already knows.
		float gate = 1.0f;
		if (p.stopWhenStill && haveSpeed)
		{
			constexpr float kLo = 0.05f;   // m/s: parked
			constexpr float kHi = 0.50f;   // m/s: definitely moving
			float g = (speedMps - kLo) / (kHi - kLo);
			if (g < 0.0f) g = 0.0f;
			if (g > 1.0f) g = 1.0f;
			gate = g * g * (3.0f - 2.0f * g);
			if (gate <= 0.0f) return;
		}

		// Distinct seed bases so the layers never correlate, and so changing
		// seed rerolls both together.
		const uint32_t s = (uint32_t)p.seed * 1013904223u;

		float var = p.variation;
		if (var < 0.0f) var = 0.0f;
		if (var > 1.0f) var = 1.0f;

		// One envelope over everything, folded in with the speed gain.
		const float aScale = ampGain * gate * detail::envelope(s, timeMs, var);

		p.sway.posAmp   *= aScale;  p.sway.rotAmp   *= aScale;
		p.jitter.posAmp *= aScale;  p.jitter.rotAmp *= aScale;
		p.sway.freq     *= freqGain;
		p.jitter.freq   *= freqGain;

		detail::addLayer(p.sway,   p.octaves, s + 0x0000u, timeMs, var,
			p.axisPos, p.axisRot, outPos, outRotDeg);
		detail::addLayer(p.jitter, p.octaves, s + 0x9E37u, timeMs, var,
			p.axisPos, p.axisRot, outPos, outRotDeg);
	}
}
