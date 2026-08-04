// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "capture/render.h"
#include "capture/fxcapture.h"
#include "capture/exporthook.h"
#include "capture/videoout.h"
#include "capture/audioout.h"
#include "capture/dofsession.h"
#include "game/signatures.h"

#include <cstdio>

namespace render
{
	namespace
	{
		enum class Step
		{
			Idle,
			Settle,   // waiting for the game to render at the time we jumped to
			Ack,      // waiting for the addon to acknowledge our capture request
			Audio,    // playing at normal speed, recording sound, no frames
			Slide,    // the clip is PLAYING; accumulate whatever is presented
			ClipJump, // waiting for a clip step we asked the engine for
			Rewind,   // audio done, going back to clip 1 to start the frames
			Done,
		};

		Settings s_cfg;

		Step  s_step   = Step::Idle;
		int   s_frame  = 0;      // output frame index
		int   s_frames = 0;      // total output frames
		int   s_sample = 0;      // sub-sample within the current output frame
		int   s_wait   = 0;      // settle frames remaining
		int   s_guard  = 0;      // frames spent waiting for one ack

		float s_start = 0.0f;    // clip time of frame 0, ms
		float s_dt    = 0.0f;    // output frame interval, ms

		bool  s_openEnded = false;  // stop when the clock stops following us
		int   s_shortRuns = 0;      // consecutive seeks that fell short
		float s_lastClock  = -1.0f; // clock at the previous check, to spot a clip restart

		// --- sliding renderer ------------------------------------------------
		//
		// Clip time accumulated so far, summed from the replay clock rather than
		// taken from it. The clock RESTARTS at every clip, so it is a position
		// inside the clip on screen and not a position in the project - summing
		// the per-present deltas and refusing the negative ones is what turns it
		// into a project-long timeline that output frames can be cut from.
		double s_slideTime   = 0.0;
		int    s_slideSample = 0;    // samples accumulated into the current frame
		bool   s_slideFlush  = false;// a flush was posted; collect it next present

		// Live playback speed, steered to hit RenderSamples.
		//
		// Sample count in a sliding render is P/(F*S) - present rate over output
		// rate times speed - and P is not knowable in advance: it depends on the
		// scene, the resolution and how expensive the capture turns out to be. So
		// asking the user for a SPEED means asking them to predict their own frame
		// rate, which is no way to request "16 samples".
		//
		// So it is DERIVED, never configured: measured once from the first
		// frame's advance phase, then held against drift by the controller in
		// the flush below. There is deliberately no ini key - the only value a
		// person could supply is a worse one than the measurement.
		float s_slideSpeed   = 0.05f;
		bool  s_slideCapture = false;// false = advancing to the mark, true = exposing
		double s_slideMark   = 0.0;  // clip time at which this frame's exposure opened
		int   s_slideLogged  = 0;    // last speed reported, to keep the log quiet
		double s_slideStep   = 0.0;  // clip ms one present covers, smoothed
		bool  s_slideCalib   = false;// AUTO has taken its measurement
		int   s_slideWarm    = 0;    // presents spent measuring before frame 0

		// sampleCount handed to the addon for a sample that is NOT the last one.
		// It only has to exceed the index, since the addon averages and writes on
		// `sampleIndex >= sampleCount - 1` and merely accumulates otherwise - so
		// this is "do not flush yet" rather than a real count. The true divisor
		// is sent with the final sample, once it is known.
		constexpr uint32_t kSlideOpen = 1u << 24;

		// Where AUTO begins before it has measured anything. Any value the
		// clip visibly advances at will do - it survives exactly one frame's
		// advance phase.
		// Deliberately far too SLOW rather than a best guess.
		//
		// The first frame is exposed before anything has been measured, and the
		// two failure directions are not symmetric: too slow costs a couple of
		// under-blurred frames at the head, too fast bakes a smear. At 0.1x the
		// first frame came out spanning 1373ms of a 33ms shutter - over a second
		// of the clip crushed into one image.
		constexpr float kSlideSeed = 0.005f;

		// Presents to spend measuring before the first frame is exposed.
		//
		// Frame 0 has no advance phase to measure during - its mark is zero, so
		// the capture starts on the very first present - which is exactly how the
		// seed reached the shutter unchallenged. These are pre-roll: the clip
		// moves a fraction of a millisecond at the seed speed and the output
		// timeline is rebased afterwards, so nothing is lost from the head.
		constexpr int kSlideWarm = 12;

		// Warm-up presents are CHEAP - nothing is being captured - while exposure
		// presents carry a full read-back and accumulate, and ran 2-6x slower in
		// practice. So a speed derived from warm-up alone comes out too high.
		// Bias it down and let the controller climb: the climb costs a few soft
		// frames, the alternative costs a smeared one.
		constexpr float kSlideCalibBias = 0.35f;

		// --- position steering -------------------------------------------------
		// How much of the measured drift is taken out per cycle. The controller
		// gets exactly one correction per output frame, and the measurement is
		// quantised by the present that carried it, so a full-gain response
		// chases quantisation noise and rings. Half settles in about two frames.
		constexpr double kSlideDriftGain = 0.5;

		// Bounds on the corrected target span, as a fraction of the wanted one.
		// Deliberately lopsided - see the note at the controller. Behind is free,
		// ahead is not, so slowing down is allowed almost without limit and
		// catching up is rationed.
		constexpr double kSlideTargetLo = 0.05;
		constexpr double kSlideTargetHi = 1.75;

		// Project time is OURS; the game seeks in the current clip's own time.
		// s_clipBase is where the live clip starts in its timeline, s_clipProjAt
		// is the project time that lands there, and everything between them is
		// the same distance in both. Re-based whenever the editor changes clip.
		float s_clipBase   = 0.0f;
		float s_clipProjAt = 0.0f;

		// The first clip's own span, and a flag for having come back to it.
		// A preview does not stop at the end of the last clip - it wraps to the
		// first - so returning to this range is what 'the project ended' actually
		// looks like from in here.
		//
		// Only the FALLBACK now. Two clips trimmed out of the same recording, or
		// simply two clips that both start at zero, report the same span - and
		// this then called the second one a wrap and ended the render one clip
		// in. Anything that can see the clip table uses s_clipAt below instead,
		// where "clip 3 became clip 1" is a fact rather than an inference.
		float s_firstLo = -1.0f;
		float s_firstHi = -1.0f;
		bool  s_looped  = false;

		// Where we are in the project's clip list, and the step we have asked
		// the engine for. s_clipN is 0 when the clip table could not be read, and
		// every clip-aware path below is written to fall back when it is.
		int   s_clipAt     = 0;
		int   s_clipN      = 0;
		int   s_wantClip   = -1;    // clip index requested, -1 = nothing pending
		float s_wantClipAt = 0.0f;  // time inside it our project clock lands on
		int   s_clipWait   = 0;     // frames spent waiting for one clip step

		int   s_pendWait  = 0;      // frames spent waiting for a diverted export
		unsigned long s_pendStart = 0; // tick when that wait began; 0 = not waiting
		unsigned long s_pendLog   = 0; // tick of the last "still waiting" line
		int   s_startMode = -1;     // replay mode the render began in
		int   s_busyWait  = 0;      // frames spent waiting out a clip load
		int   s_modeWait  = 0;      // frames spent in a transient replay mode

		// The real-time audio pass. Survives between renders on purpose: it is
		// what tells the NEXT Export press that a wav is waiting for it.
		bool        s_audioPass  = false;
		int         s_audioStall = 0;
		int         s_autoOpen   = 0;   // ticks until the frame pass is started for us
		std::string s_pendingWav;

		// True when this render came from the Export button rather than the
		// menu. Only an export should close playback afterwards - doing it to a
		// menu-started render would eject you from the clip you were editing.
		bool  s_fromExport = false;

		// Set by the spinner hook when the game tried to draw a loading spinner.
		// Sampled and cleared by the pump, so it means "the last presented frame
		// was not a clean one".
		volatile bool s_spinnerSeen = false;

		// How many times the spinner hook actually fired during a render. If a
		// spinner is visible in the output but this stays 0, the function we
		// hooked is not the one drawing it - the game has more than one spinner
		// system (CPauseMenu::RenderAnimatedSpinner vs CBusySpinner::Render).
		volatile unsigned s_spinnerHits = 0;

		using FnSpinner = void(__fastcall*)(float, int, int);
		FnSpinner origSpinner = nullptr;

		using FnPointer = void(__fastcall*)();
		FnPointer origPointer = nullptr;

		// Forcing the flag from the pump was not enough: the input code rewrites
		// it every frame, and our write landed before this function consumed it.
		// Setting it HERE, at the point of use, is the only ordering that holds -
		// the same reasoning as suppressing the spinner in its own draw call.
		void __fastcall hkPointer()
		{
			if (s_step != Step::Idle && Config::get().renderHideHud)
				game::setCursorVisible(false);

			// The frame pass is started from HERE, not from the render pump.
			//
			// pump() is driven by the replay camera's update, which stops the
			// moment playback closes - and the audio pass ends with the game
			// closing playback and returning to the export menu. So the countdown
			// sat there never ticking and the second pass never began. This
			// function runs every frame in the frontend, which is exactly where
			// we are by then.
			//
			// Opening playback from a frontend update is what the Export button
			// itself does - TriggerExport runs out of the menu's input handling -
			// so this is the same context the game uses, not a new one.
			if (s_step == Step::Idle && s_autoOpen > 0 && --s_autoOpen == 0)
			{
				if (!s_pendingWav.empty())
				{
					logger::write("info", "render: audio captured - starting the frame pass");
					game::openPlayback(gsig::PLAYBACK_TYPE_BAKE);
				}
			}
			origPointer();
		}

		void __fastcall hkSpinner(float a, int b, int cc)
		{
			if (s_step != Step::Idle)
			{
				// Suppress AND remember. Suppressing alone would leave a frame
				// that the game itself considered unready; remembering alone
				// would still composite the spinner into it.
				s_spinnerSeen = true;
				++s_spinnerHits;
				return;
			}
			origSpinner(a, b, cc);
		}

		// -------------------------------------------------------------------------
		// The spinner neither hook above catches - the SEEK spinner.
		//
		// It shows as a bare segmented ring dead centre with NO body text, and it
		// was baked into output frames 0-3 of an Enhanced render. Both hooks below
		// were installed and the pump's hold-on-spinner path never fired, so it
		// goes through neither.
		//
		// What it actually is: the editor raises a busy spinner for ANY seek -
		// scrubbing the timeline by hand does it too. A render seeks constantly,
		// and the big one is the jump to the start, so it lands on the first
		// frames and clears once the small per-frame steps take over.
		//
		// It is stopped at its SOURCE instead - see hkSpinnerOn below. Blocking
		// the Scaleform call that carries it was tried and removed: it did land,
		// and the ring was still drawn, because by then the spinner had been
		// handed to a movie that animates it on its own with no further native
		// calls.
		// -------------------------------------------------------------------------

		// The game's SECOND spinner. CPauseMenu::RenderAnimatedSpinner above and
		// CBusySpinner::Render are unrelated systems with their own movies, and
		// suppressing only the first still left one composited into Enhanced
		// renders. Runs on the RENDER thread, so this stays trivial.
		using FnBusySpinner = void(__fastcall*)();
		FnBusySpinner origBusySpinner = nullptr;

		void __fastcall hkBusySpinner()
		{
			if (s_step != Step::Idle)
			{
				s_spinnerSeen = true;
				++s_spinnerHits;
				return;
			}
			origBusySpinner();
		}

		// -------------------------------------------------------------------------
		// Where the spinner is actually RAISED.
		//
		// CBusySpinner::On(bodyText, icon, sourceIndex). The editor calls this for
		// every seek, which is why scrubbing the timeline flashes a ring in the
		// middle of the screen at all, and why a render - which seeks for a living
		// - gets one baked into its frames.
		//
		// Dropping the call for the VIDEO EDITOR source alone is the whole fix.
		// Nothing downstream then has a spinner to show: no list entry is written,
		// no movie is told to display one, and the matching Off() for that source
		// becomes a harmless no-op. Every other source - savegame, script, cloud,
		// profanity - is untouched.
		//
		// Deliberately NOT gated on a render being active. The user wants this gone
		// while scrubbing in the clip editor too, and suppressing it everywhere is
		// both what was asked for and the more predictable behaviour: a spinner
		// that only sometimes appears is worse than one that never does.
		// -------------------------------------------------------------------------
		using FnSpinnerOn = void(__fastcall*)(const char*, int, int);
		FnSpinnerOn origSpinnerOn = nullptr;

		void __fastcall hkSpinnerOn(const char* bodyText, int icon, int sourceIndex)
		{
			if (sourceIndex == gsig::SPINNER_SOURCE_VIDEO_EDITOR &&
			    Config::get().hideEditorSpinner)
			{
				// Only mark the frame unclean if we are mid-render. Outside one
				// this is just the editor being quiet, not a frame to reject.
				if (s_step != Step::Idle) { s_spinnerSeen = true; ++s_spinnerHits; }
				return;
			}
			origSpinnerOn(bodyText, icon, sourceIndex);
		}

		// -------------------------------------------------------------------------
		// The spinner's REAL draw - the one that finally works on Enhanced.
		//
		// hkSpinner above hooks CPauseMenu::RenderAnimatedSpinner, which is only a
		// wrapper. That is sufficient on Legacy, where MSVC kept the call. Clang
		// inlined it into most of its eleven callers on Enhanced - including the
		// editor's Render, which reaches the draw four separate times - so the
		// editor's spinner never passed through the wrapper and the detour on it
		// never fired. Hooking the callee cannot be bypassed: every route,
		// wrapper included, ends here.
		//
		// Suppressed during a capture (a rendered frame carries no UI), and while
		// the editor is up if HideEditorSpinner is set - which is what removes the
		// ring when scrubbing the timeline, not just in renders.
		// -------------------------------------------------------------------------
		using FnDrawSpinner = void(__fastcall*)(void*, void*, int, int, int);
		FnDrawSpinner origDrawSpinner = nullptr;

		void __fastcall hkDrawSpinner(void* pos, void* size, int a, int b, int c)
		{
			const Config& cfg = Config::get();
			if (s_step != Step::Idle)
			{
				// Mid-capture: suppress and mark the frame unclean, exactly as the
				// other spinner hooks do.
				s_spinnerSeen = true;
				++s_spinnerHits;
				return;
			}
			if (cfg.hideEditorSpinner && game::isEditModeActive()) return;

			origDrawSpinner(pos, size, a, b, c);
		}

		// -------------------------------------------------------------------------
		// Every Scaleform movie's draw.
		//
		// Last layer standing. The ring in the centre of the screen survived, in
		// order: RenderAnimatedSpinner suppressed, CBusySpinner::Render suppressed,
		// SET_SAVING_TEXT refused (confirmed landing on movies 1 and 17),
		// CBusySpinner::On dropped at source, and the editor HUD and cursor flags
		// forced down. An always-on log of BeginMethod then showed ZERO Scaleform
		// method calls for an entire render with the ring visible - so whatever
		// draws it was configured before the render began and animates itself.
		//
		// A movie like that can only be stopped where it DRAWS. Skipping every
		// movie for the duration of a capture is also just correct: a rendered
		// frame should carry no UI whatsoever, and this is the one place that
		// guarantees it regardless of which movie or which system put it there.
		//
		// Gated on renderHideHud so it stays opt-out, and on s_step so normal
		// editor use is untouched. Runs on the RENDER thread - keep it trivial.
		// -------------------------------------------------------------------------
		using FnRenderMovie = void(__fastcall*)(int, void*, void*, int, int, int, int);
		FnRenderMovie origRenderMovie = nullptr;

		void __fastcall hkRenderMovie(int movieId, void* pos, void* scale,
		                              int a, int b, int c, int d)
		{
			// A captured frame should carry no UI, whichever movie or system put
			// it there. This is the one place that guarantees it.
			//
			// Covers a depth-of-field session as well as a render. That blend
			// runs during ReShade's effect pass, i.e. after the game has drawn
			// its own frame - so without this the timeline and transport bar are
			// accumulated into every sample.
			const bool capturing = (s_step != Step::Idle) || dofsession::active();
			if (capturing && Config::get().renderHideHud) return;

			origRenderMovie(movieId, pos, scale, a, b, c, d);
		}

		uint32_t s_lastBeat = 0;

		// Progress reporting. A 64-sample render is minutes per second of
		// footage, and until now it logged "export started" and then nothing at
		// all until it finished - so a slow render and a hung one looked
		// identical, and there was no way to judge whether to wait. The output
		// file is no help either: mp4 writes its index at the end, so it sits at
		// 48 bytes throughout regardless of progress.
		uint32_t s_startTick   = 0;
		uint32_t s_lastReport  = 0;
		constexpr uint32_t kReportEveryMs = 15000;
		char     s_folder[MAX_PATH]{};
	}

	namespace
	{

		// An ack that never arrives means the addon died mid-render. Bail rather
		// than freezing the editor forever.
		constexpr int kAckGuard = 600;

		// A clip load is slow but finite. Well past the worst case, so this
		// only fires if something is genuinely wedged.
		constexpr int kBusyGuard = 5000;

		// Loading the next clip of a project is a real disk load, not a seek.
		// Generous on purpose: the failure this protects against (a mode that
		// never returns) is rare, while giving up too early throws away the
		// whole render.
		constexpr int kModeGuard = 5000;

		// DISABLED is also how a genuine exit looks, so it waits seconds, not a
		// minute - long enough to ride out a transition that passes through it.
		constexpr int kExitGuard = 240;

		// Pump ticks of a motionless clock before the audio pass calls it the end
		// of the project. Roughly a second and a half - long enough not to trip on
		// a hitch, short enough not to record silence onto the tail.
		constexpr int kAudioStall = 90;

		// Time of the current sub-sample. Samples are spread across the OPEN
		// part of the frame interval only, which is what shutter angle means:
		// shutter 0.5 exposes the first half of the interval, so the blur trail
		// covers half a frame of motion and the rest is dark time.
		float sampleTime()
		{
			const float base = s_start + (float)s_frame * s_dt;

			// One sample means no blur at all, and the shutter is not consulted -
			// which is what makes "render high fps, blend in post" a clean path
			// through this code rather than a special case.
			if (s_cfg.samples <= 1) return base;

			// Samples sit at the CENTRE of each slice, not its leading edge.
			// Using k/N would put the first sample exactly on the frame time and
			// the last one a slice short of the shutter close, biasing every
			// blurred frame backwards by half a slice. (k+0.5)/N is the midpoint
			// rule and has no such bias.
			const float open = s_dt * s_cfg.shutter;
			return base + open * (((float)s_sample + 0.5f) / (float)s_cfg.samples);
		}

		// -------------------------------------------------------------------------
		// sampleTime() in the units the game actually seeks in.
		//
		// CReplayMgr::JumpTo takes an ABSOLUTE time inside the clip on screen,
		// not a position in the project, and a clip trimmed out of the middle of
		// a recording starts nowhere near zero. Clip one usually begins at ~0,
		// which is why passing project time straight through worked for exactly
		// as long as a render stayed in the first clip: on clip two the request
		// fell below the clip's start, the engine clamped it there, and every
		// seek for the rest of the render landed on the same first frame - a
		// picture frozen while the frame counter climbed.
		// -------------------------------------------------------------------------
		float seekTime()
		{
			return s_clipBase + (sampleTime() - s_clipProjAt);
		}

		// Point the mapping at whatever clip the editor is showing now, keeping
		// the project position we are at. Called at the start of a render and
		// after every clip transition.
		void rebaseClip(const char* why)
		{
			float lo = 0.0f, hi = 0.0f;
			if (!game::clipRange(lo, hi)) return;   // Legacy: identity mapping

			if (s_firstLo < 0.0f) { s_firstLo = lo; s_firstHi = hi; }
			else if (lo == s_firstLo && hi == s_firstHi) s_looped = true;

			s_clipBase   = lo;
			s_clipProjAt = sampleTime();
			s_lastClock  = -1.0f;
			logger::write("info", "render: clip %s - project %.0f now maps to clip time %.0f..%.0f",
				why, s_clipProjAt, lo, hi);
		}

		// Put the playhead back at the very start of the project.
		//
		// False means it cannot be done from here - no clip table, or playback
		// is already gone - and the caller should fall back to re-opening.
		bool rewindToProjectStart()
		{
			float lo = 0.0f, hi = 0.0f;
			if (!game::clipRangeAt(0, lo, hi)) return false;

			// Pause first in both cases. The audio pass leaves playback ROLLING,
			// and a preview that keeps rolling while the engine gets around to
			// our request can wrap to clip one on its own - which lands us at the
			// right clip by the wrong route, with playback still running.
			game::playbackPause();

			// Already on clip one: there is no clip to step to, and asking for
			// the one we are on is a no-op the engine drops, so seek instead.
			if (game::clipIndex() == 0)
			{
				game::jumpProjectTo(lo, 0);
				return true;
			}
			return game::jumpToClip(0, lo);
		}


		void buildPath(char* out, int cap)
		{
			snprintf(out, cap, "%s\\frame_%06d.%s", s_folder, s_frame,
				s_cfg.jpeg ? "jpg" : "png");
		}

		// Drop an ffmpeg cheat-sheet next to the frames.
		//
		// The intended workflow is to render with no in-engine blur at a
		// multiple of the target rate and synthesise the blur in post, which
		// gives a shutter angle you can change afterwards without re-rendering.
		// Getting the tmix/fps pair right is fiddly and easy to get subtly
		// wrong, so the numbers are worked out here where the rendered rate is
		// actually known.
		void writeAssembleHelp()
		{
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s\\assemble.txt", s_folder);

			FILE* f = nullptr;
			if (fopen_s(&f, path, "w") != 0 || !f) return;

			const char* ext = s_cfg.jpeg ? "jpg" : "png";
			const float fps = s_cfg.fps;

			fprintf(f, "Rendered %d frames at %g fps (%s).\n\n", s_frame, fps, ext);

			fprintf(f, "Straight conform, no added blur:\n");
			fprintf(f, "  ffmpeg -framerate %g -i frame_%%06d.%s"
			           " -c:v libx264 -crf 16 -pix_fmt yuv420p out.mp4\n\n", fps, ext);

			if (s_cfg.samples <= 1)
			{
				// Blend M source frames down to one output frame. Averaging ALL
				// of them is a 360-degree shutter; averaging half is 180. So to
				// land on 180 you need the source rate to be twice the multiple
				// you are blending, which is why the 180 line below wants 4x.
				const float half    = fps * 0.5f;
				const float quarter = fps * 0.25f;

				fprintf(f, "Synthesised motion blur (this is the intended route -\n"
				           "shutter is chosen here, not at render time):\n\n");

				fprintf(f, "  %g fps, 360-degree shutter (blend every 2, halve the rate):\n", half);
				fprintf(f, "    ffmpeg -framerate %g -i frame_%%06d.%s"
				           " -vf \"tmix=frames=2:weights='1 1',fps=%g\""
				           " -c:v libx264 -crf 16 -pix_fmt yuv420p out.mp4\n\n", fps, ext, half);

				fprintf(f, "  %g fps, 180-degree shutter (blend 2 of every 4):\n", quarter);
				fprintf(f, "    ffmpeg -framerate %g -i frame_%%06d.%s"
				           " -vf \"tmix=frames=2:weights='1 1',fps=%g\""
				           " -c:v libx264 -crf 16 -pix_fmt yuv420p out.mp4\n\n", fps, ext, quarter);

				fprintf(f, "  General rule: render at target*M, then blend"
				           " round(M * shutter) frames\n"
				           "  and decimate to the target rate. Blending all M is 360 degrees.\n");
			}
			else
			{
				fprintf(f, "These frames already contain %d-sample motion blur at a"
				           " %.0f-degree\nshutter, baked in at render time.\n",
				           s_cfg.samples, s_cfg.shutter * 360.0f);
			}

			// Audio is an instruction rather than a rewritten command line: the
			// blur variants above differ in their filters and rates, and adding
			// the same three arguments to each would triple the file for no
			// gain. Appended once, applies to whichever line you pick.
			//
			// The wav THIS render just captured wins over AudioFromFile.
			//
			// In Frames mode the audio pass still runs - a full real-time
			// playthrough - but there is no encoder for it to be muxed into, so
			// without naming it here that pass was paid for and then silently
			// wasted: the wav sat in the folder and the only file telling you
			// how to assemble anything pointed somewhere else.
			const std::string& au = !s_pendingWav.empty() ? s_pendingWav
			                                              : Config::get().audioFromFile;
			if (!au.empty())
			{
				fprintf(f, "\nAudio: insert this before the output filename on"
				           " whichever line you use.\n");
				fprintf(f, "  -i \"%s\" -map 0:v -map 1:a -c:a aac -b:a 320k -shortest\n",
					au.c_str());
				fprintf(f, "\n-shortest is not optional: the frame count sets the video"
				           " length and the\nborrowed track is however long the game's"
				           " export ran. They agree only\napproximately.\n");
			}

			fclose(f);
		}

		void finish(const char* why, bool complete)
		{
			s_step = Step::Idle;

			// Unconditional: an aborted render must not leave the editor with a
			// permanently invisible HUD and no obvious way to get it back.
			if (Config::get().renderHideHud)
			{
				game::setEditorHudVisible(true);
				game::setCursorVisible(true);
			}
			// An audio pass has no encoder and no frames. Everything below is
			// for the frame pass.
			if (s_audioPass)
			{
				const double secs = audioout::seconds();
				audioout::end();
				s_pendingWav = audioout::path();
				s_audioPass  = false;

				// Straight on to the frames, in this same playback.
				//
				// The two passes used to be two Export presses, because the
				// second one has to start at the beginning of the project and a
				// seek cannot get there - it clamps to the clip on screen, and
				// the audio pass ends on the LAST clip. Re-pressing Export was
				// the only thing that reliably rewound.
				//
				// A clip step can, so it does. Staying inside the playback we
				// are already in also removes the fragile part of the old route:
				// waiting out a close, counting frames in the frontend, and
				// hoping the re-open lands - none of which has to happen now.
				//
				// An audio pass that did NOT finish leaves a partial wav, and
				// leaving the path set hands it to the next Export - which then
				// skips its own audio pass, adopts a truncated take, and reuses
				// this render's folder on top of it. Drop it here so the next
				// press starts clean.
				if (!complete) s_pendingWav.clear();

				// `complete` gates it: a CANCELLED audio pass must not go on to
				// render frames the user just asked to stop.
				if (complete && !s_pendingWav.empty() && rewindToProjectStart())
				{
					s_step     = Step::Rewind;
					s_clipWait = 0;
					s_modeWait = 0;
					logger::write("info",
						"render: %s (%.2fs of audio) - rewinding to clip 1 for the frame pass",
						why, secs);
					return;
				}

				// Fallback, for a playback that has already closed under us or a
				// build where the clip table did not resolve: the old two-press
				// route. Two seconds is long enough for the editor to finish
				// returning to its menu, short enough not to look like nothing
				// happened.
				s_autoOpen = (complete && !s_pendingWav.empty()) ? 120 : 0;
				logger::write("info", "render: %s (%.2fs of audio) - %s", why, secs,
					s_autoOpen ? "frame pass starts in ~2s"
					           : "nothing recorded, press Export to render silent");
				return;
			}

			// Close the encoder before anything else that can bail out of this
			// function: ffmpeg only finalises the container when its stdin
			// closes, so an early return here would leave an unplayable file.
			// Called unconditionally - end() is a no-op when no video was
			// started, and an aborted render still deserves its partial video.
			videoout::end(complete);

			// The assemble helper is for the frames-only workflow. With a video
			// already written and the frames consumed, it would describe files
			// that are no longer there.
			if (complete && s_frame > 0 && !Config::get().wantsVideo()) writeAssembleHelp();

			// Hand the playback type back before anything closes playback.
			//
			// CVideoEditorPlayback::Close() branches on it to choose where the
			// editor goes next. Left as PREVIEW - which is what our diversion
			// makes it - it takes the preview branch, which shows the OPTIONS
			// menu and rebuilds the timeline. That is why the editor came back
			// on the wrong screen after an export, with Start Export leading
			// into the clip editor: the game was correctly returning us to
			// where a preview ends, having been told a preview is what ran.
			// The bake branch goes to the export menu, which is where the user
			// pressed the button.
			//
			// Done here and nowhere earlier: for the whole render the type has
			// to stay PREVIEW, because that is precisely what keeps the game's
			// own encoder out of the way.
			if (s_fromExport) game::setPlaybackType(gsig::PLAYBACK_TYPE_BAKE);

			// Return to the editor the way a finished bake does. Deferred via the
			// game's own flag, never a direct Close(): this runs inside the
			// camera update, where tearing playback down is not survivable.
			if (s_fromExport && Config::get().exportCloseWhenDone)
			{
				if (!game::requestPlaybackClose())
					logger::write("info", "export: close-when-done unresolved - staying in playback");
			}
			// One wav per pair of presses. Leaving it set would silently attach
			// the previous project's audio to the next render.
			//
			// UNCONDITIONAL, including on an abort. It used to be gated on
			// `complete`, so every failed render - addon gone, stuck loading,
			// clip step never landed - left the path set. The next Export then
			// did three wrong things at once: skipped its own audio pass, muxed
			// the stale wav, and took the folder-REUSE branch in begin(), writing
			// its frames and video.mp4 straight over the previous render's
			// output. The overwrite is the serious one - it is silent data loss
			// from a render that merely failed.
			if (!s_pendingWav.empty())
			{
				s_pendingWav.clear();
				videoout::setAudio("");
			}

			s_fromExport = false;
			s_wantClip   = -1;   // never let a half-finished step reach the next render
			logger::write("info", "render: %s (%d/%d frames, %d clip(s), spinnerHits=%u, %s)",
				why, s_frame, s_frames, s_clipN ? s_clipAt + 1 : 0, s_spinnerHits, s_folder);
			s_spinnerHits = 0;
		}

		// A requested clip step has arrived. Re-establish the render's own
		// playback state on the far side of it and carry on.
		//
		// The pause matters: a clip transition runs the engine's state machine,
		// which restores ITS saved state on the way out. Left alone, the clock
		// still follows our seeks while the world no longer redraws for them -
		// the frame counter climbs and the picture stays frozen.
		void landClip()
		{
			game::playbackPause();

			s_clipAt     = s_wantClip;
			s_clipBase   = s_wantClipAt;
			s_clipProjAt = sampleTime();
			s_wantClip   = -1;
			s_lastClock  = -1.0f;
			s_shortRuns  = 0;
			s_busyWait   = 0;

			game::jumpProjectTo(seekTime(), 0);
			s_wait = s_cfg.settleFrames;
			s_step = Step::Settle;

			logger::write("info",
				"render: clip %d/%d at frame %d - project %.0f maps to clip time %.0f",
				s_clipAt + 1, s_clipN, s_frame, s_clipProjAt, s_clipBase);
		}

		// Step to the next clip, carrying the project clock across the seam.
		void advanceClip(float clipEnd)
		{
			const int next = s_clipAt + 1;

			float lo = 0.0f, hi = 0.0f;
			if (!game::clipRangeAt(next, lo, hi))
			{
				finish("finished - the next clip has no range", true);
				return;
			}

			// The clip boundary almost never falls on a frame boundary, so the
			// frame that crosses it is owed the remainder. rebaseClip() snaps to
			// the new clip's first image instead, throwing away up to a frame of
			// project time - once per boundary, all of it drift against the
			// audio.
			float over = seekTime() - clipEnd;
			if (over < 0.0f)    over = 0.0f;
			if (over > hi - lo) over = 0.0f;   // a clip shorter than one frame

			s_wantClip   = next;
			s_wantClipAt = lo + over;
			s_clipWait   = 0;

			if (!game::jumpToClip(next, s_wantClipAt))
			{
				finish("finished - could not step to the next clip", true);
				return;
			}

			logger::write("info", "render: frame %d - stepping to clip %d/%d at %.0f",
				s_frame, next + 1, s_clipN, s_wantClipAt);
			s_step = Step::ClipJump;
		}

	}

	Settings& settings() { return s_cfg; }

	void installHooks()
	{
		if (game::addr_RenderSpinner)
		{
			memory(game::addr_RenderSpinner).hook(hkSpinner, &origSpinner, "RenderSpinner");
			logger::write("info", "render: spinner suppression hooked");
		}
		else
		{
			logger::write("info", "render: spinner unresolved - loading spinners may be captured");
		}

		if (game::addr_DrawSpinner)
		{
			memory(game::addr_DrawSpinner).hook(hkDrawSpinner, &origDrawSpinner, "DrawSpinner");
			logger::write("info", "render: spinner draw suppressed (the inlined-past one)");
		}
		else
		{
			logger::write("info",
				"render: spinner draw unresolved - the editor's spinner stays");
		}

		if (game::addr_ScaleformRenderMovie)
		{
			memory(game::addr_ScaleformRenderMovie).hook(hkRenderMovie, &origRenderMovie, "ScaleformRenderMovie");
			logger::write("info", "render: scaleform movie rendering suppressed during capture");
		}
		else
		{
			logger::write("info",
				"render: CScaleformMgr::RenderMovie unresolved - UI movies may be captured");
		}

		if (game::addr_BusySpinnerOn)
		{
			memory(game::addr_BusySpinnerOn).hook(hkSpinnerOn, &origSpinnerOn, "BusySpinnerOn");
			logger::write("info", "render: editor seek spinner suppressed at source");
		}
		else
		{
			logger::write("info",
				"render: CBusySpinner::On unresolved - the editor's seek spinner stays");
		}

		if (game::addr_BusySpinnerRender)
		{
			memory(game::addr_BusySpinnerRender).hook(hkBusySpinner, &origBusySpinner, "BusySpinnerRender");
			logger::write("info", "render: busy spinner suppression hooked");
		}
		else
		{
			logger::write("info", "render: busy spinner unresolved - it may be captured");
		}

		if (game::addr_MousePointerUpdate && game::addr_g_CursorVisible)
		{
			memory(game::addr_MousePointerUpdate).hook(hkPointer, &origPointer, "MousePointerUpdate");
			logger::write("info", "render: cursor suppression hooked");
		}
		else
		{
			logger::write("info", "render: cursor update unresolved - cursor may be captured");
		}
	}

	void applyConfig()
	{
		const Config& c = Config::get();
		s_cfg.fps          = c.renderFps;
		s_cfg.samples      = c.renderSamples;
		s_cfg.shutter      = c.renderShutter;
		s_cfg.settleFrames = c.renderSettleFrames;
		s_cfg.settleSubFrames = c.renderSettleSubFrames;
		s_cfg.jpeg         = c.renderJpeg;
		s_cfg.quality      = c.renderQuality;
		s_cfg.highlight    = c.renderHighlight;
		s_cfg.channelOrder = c.renderChannelOrder;
		s_cfg.captureMode  = c.renderCaptureMode;

		// State the destination at startup, not only when a render begins.
		//
		// It was previously only in the "render: started ... -> <folder>" line,
		// which is no use to anyone whose render never started - and that is
		// exactly the person asking where the output went. It also moved with the
		// path fix: under FiveM it now resolves beside the .asi in plugins\,
		// rather than the subprocess cache older builds used, so even a working
		// setup can have output in an unexpected place after an update.
		{
			static std::string s_lastReported;
			const std::string dir =
				fxcapture::captureBaseDir(Config::get().renderOutputFolder.c_str());
			if (dir != s_lastReported)
			{
				s_lastReported = dir;
				logger::write("info", "render: output -> %s\\render_NNNN%s",
					dir.c_str(),
					Config::get().renderOutputFolder.empty()
						? "   (RenderOutputFolder is empty, so this is the default)"
						: "   (from RenderOutputFolder)");
			}
		}
	}

	bool active()      { return s_step != Step::Idle; }
	int  frameCount()  { return s_frames; }
	int  frameDone()   { return s_frame; }
	const char* outputFolder() { return s_folder; }

	namespace
	{
		// Everything the render needs once a range is known.
		//
		// This used to take a `walkClips` flag, because there were two entry
		// points: an export renders the PROJECT and must step through its clips,
		// while a render started from the camera menu covered a marker range
		// inside the clip being edited and had no business leaving it. That
		// second entry point is gone with the menu row that reached it, so the
		// flag could only ever be true and the clip walk is unconditional.
		bool begin(float startMs, int frames, bool openEnded, const char** reason)
		{
			static const char* kNoFolder = "could not create the output folder";

			// The frame pass of a two-pass render writes into the folder the
			// audio pass already made, so the wav and the video end up together
			// instead of one folder apart.
			const size_t folderLen = strlen(s_folder);
			const bool   reuse     = folderLen > 0 && !s_pendingWav.empty() &&
			                         _strnicmp(s_pendingWav.c_str(), s_folder, folderLen) == 0;

			if (!reuse &&
			    !fxcapture::newSequenceFolder(Config::get().renderOutputFolder.c_str(),
			                                  s_folder, sizeof(s_folder)))
			{
				if (reason) *reason = kNoFolder;
				return false;
			}

			if (s_cfg.fps     < 1.0f) s_cfg.fps = 1.0f;
			if (s_cfg.samples < 1)    s_cfg.samples = 1;

			s_dt        = 1000.0f / s_cfg.fps;
			s_start     = startMs;
			s_frames    = frames;
			s_frame     = 0;
			s_sample    = 0;
			s_openEnded = openEnded;
			s_shortRuns = 0;
			s_lastClock  = -1.0f;
			s_modeWait  = 0;
			s_busyWait  = 0;
			s_startMode = game::replayMode();

			// The project's clips. Zero means we could not read them and every
			// clip-aware path below falls back to the old guesswork - which is
			// what a build with an unresolved playback controller gets.
			s_clipAt   = 0;
			s_clipN    = 0;
			s_wantClip = -1;
			s_clipWait = 0;
			{
				const int at = game::clipIndex();
				if (at >= 0)
				{
					s_clipAt = at;
					s_clipN  = game::clipCount();
				}
			}

			// Audio pass. Plays the project through at normal speed and records
			// it; no seeking, no frames, and deliberately no dependency on the
			// capture addon, so sound works without ReShade installed.
			if (Config::get().renderAudio && s_pendingWav.empty())
			{
				if (audioout::begin(s_folder))
				{
					s_audioPass  = true;
					s_audioStall = 0;
					s_lastClock  = -1.0f;
					game::playbackSetSpeed(1.0f);   // ONLY 1.0 - the audio engine
					game::playbackPlay();           // misbehaves at any other rate
					s_step = Step::Audio;
					logger::write("info",
						"render: AUDIO pass - playing the project through at normal speed, "
						"the frame pass follows on its own");
					return true;
				}
				logger::write("info", "render: audio capture unavailable - rendering silent");
			}
			if (!s_pendingWav.empty()) videoout::setAudio(s_pendingWav.c_str());

			fxcapture::setQuality(s_cfg.quality);
			fxcapture::setHighlightBoost(s_cfg.highlight);
			fxcapture::setChannelOrder(s_cfg.channelOrder);

			// Pause first: every frame time from here is one we chose. If playback
			// kept running, the clock would move between our jump and the addon's
			// grab and the shutter would no longer mean anything.
			game::playbackPause();
			s_clipBase = s_clipProjAt = 0.0f;   // identity until the clip is known
			s_firstLo  = s_firstHi = -1.0f;
			s_looped   = false;
			rebaseClip("start");
			game::jumpProjectTo(seekTime(), 0);

			s_slideTime   = 0.0;
			s_slideSample = 0;
			s_slideFlush  = false;
			s_audioStall  = 0;   // doubles as the sliding clock-stall counter

			// Take the editor's HUD down for the duration. Same flag the
			// hide-HUD key uses, so this is the game's own path - and Open()
			// sets it back to true on the next playback either way.
			if (Config::get().renderHideHud)
			{
				game::setEditorHudVisible(false);
				game::setCursorVisible(false);
			}

			// Started here, once s_folder and the settings are final, so the
			// encoder's frame rate is the one actually being rendered at.
			videoout::begin(s_folder, s_cfg.fps);

			s_startTick  = GetTickCount();
			s_lastReport = s_startTick;
			s_lastBeat = fxcapture::heartbeat();

			// Sliding: let the clip PLAY, slowly, and take whatever is presented.
			//
			// The seek above still ran, and is still wanted - it puts the playhead
			// at the start before anything rolls. From here the engine drives the
			// clock, including stepping clips on its own, exactly as it does for
			// the audio pass.
			if (s_cfg.captureMode == 1)
			{
				// A seed, not a setting. The advance phase of the first frame
				// measures what one present is worth and solves for the real
				// speed before anything is exposed, so this only has to be
				// somewhere the clip visibly moves and nothing more.
				s_slideSpeed  = kSlideSeed;
				s_slideCalib  = false;
				s_slideWarm   = 0;
				s_slideStep   = 0.0;
				s_slideCapture = false;
				s_slideLogged = 0;
				game::playbackSetSpeed(s_slideSpeed);
				game::playbackPlay();
				s_lastClock = -1.0f;
				s_step      = Step::Slide;
				logger::write("info",
					"render: SLIDING - %d sample(s) per frame at a %.0f-degree shutter, "
					"starting at %.3gx. The clip plays, so particles and any temporal "
					"accumulation stay live; the speed self-tunes so the samples span "
					"the shutter.",
					s_cfg.samples < 1 ? 1 : s_cfg.samples, s_cfg.shutter * 360.0f,
					s_slideSpeed);
				return true;
			}

			s_wait     = s_cfg.settleFrames;
			s_step     = Step::Settle;
			return true;
		}
	}

	bool startOpenEnded(const char** reason)
	{
		static const char* kNoAddon = "capture addon not loaded";
		static const char* kBusy    = "already rendering";

		if (s_step != Step::Idle) { if (reason) *reason = kBusy; return false; }

		fxcapture::init();
		if (!fxcapture::addonPresent()) { if (reason) *reason = kNoAddon; return false; }

		const float now = game::addr_g_ReplayTimeMs ? *(float*)game::addr_g_ReplayTimeMs : 0.0f;
		if (s_cfg.fps < 1.0f) s_cfg.fps = 1.0f;

		// How long the project is, measured off its clip table.
		//
		// Every clip's own span, added up, in exactly the units the render steps
		// through - so the count below is the project, not an estimate of it.
		//
		// The wav used to be the only measurement available and it is the wrong
		// one twice over. It is REAL elapsed time, so it carries whatever loading
		// the gate did not catch; and if the audio pass ended early - which it
		// did, on every multi-clip project, because a looping preview never
		// stopped the clock - the frame pass inherited the truncation and stopped
		// mid-project. It is kept below only for builds that cannot read clips.
		//
		// The count is a bound, not the stopping rule: the render ends when it
		// runs off the end of the LAST clip, which is why there is slack on it.
		const char* source    = nullptr;
		int         frames    = 0x7FFFFFFF;
		bool        openEnded = true;
		float       totalMs   = 0.0f;

		const int clips = game::clipCount();
		for (int i = 0; i < clips; ++i)
		{
			float lo = 0.0f, hi = 0.0f;
			if (game::clipRangeAt(i, lo, hi)) totalMs += hi - lo;
		}

		if (totalMs > 1.0f)
		{
			frames    = (int)(totalMs / (1000.0f / s_cfg.fps) + 0.5f) + 2;
			openEnded = false;
			source    = "from the clip table";
		}
		else if (!s_pendingWav.empty() && audioout::seconds() > 0.05)
		{
			frames    = (int)(audioout::seconds() * s_cfg.fps + 0.5);
			totalMs   = (float)(audioout::seconds() * 1000.0);
			openEnded = false;
			source    = "from the audio pass";
		}

		// An unreadable clip table is a QUIET failure, and it is the one that
		// produces "the render doesn't work" reports.
		//
		// clipCount() answers through the playback controller, whose montage
		// pointer is only populated once the project has actually been loaded
		// for playback. Read it too early and the project honestly reports zero
		// clips - at which point everything downstream degrades instead of
		// stopping: the frame count becomes unbounded, s_clipN is 0, so
		// JumpToClip is never issued and the render ends at the first clip
		// boundary. On a single-clip project that is invisible; on a multi-clip
		// one you get clip one and nothing else, with no line anywhere saying so.
		//
		// It stays a fallback rather than becoming an abort - a render that
		// covers one clip beats one that refused - but it says what happened,
		// which is the whole difference between this and a bug report.
		if (clips <= 0 || totalMs <= 1.0f)
			logger::write("info",
				"render: !! the project's clip table read %d clip(s)/%.0fms - rendering "
				"OPEN-ENDED from the playhead. Multi-clip stepping is OFF, so this will "
				"stop at the end of the current clip. This usually means the project was "
				"not fully loaded for playback; open it in the editor first, then Export.",
				clips, totalMs);

		if (!begin(now, frames, openEnded, reason)) return false;
		s_fromExport = true;

		if (openEnded)
			logger::write("info",
				"render: export started at %.2fs @ %.3g fps, %d sample(s), replayMode=%d -> %s",
				now * 0.001f, s_cfg.fps, s_cfg.samples, s_startMode, s_folder);
		else
			logger::write("info",
				"render: export started at %.2fs @ %.3g fps, %d sample(s), %d clip(s), "
				"%d frames (%.2fs, %s), replayMode=%d -> %s",
				now * 0.001f, s_cfg.fps, s_cfg.samples, s_clipN, frames,
				totalMs * 0.001f, source, s_startMode, s_folder);
		return true;
	}

	void cancel()
	{
		if (s_step == Step::Idle) return;
		finish("cancelled", false);
	}

	void pump()
	{
		// --- export diagnostics ------------------------------------------
		//
		// "I pressed Export, it rendered the vanilla watermarked video, and the
		// log says nothing" was unanswerable: every interesting fact was either
		// logged once at startup or not at all. These three cover the ways it
		// can fail without anyone noticing.
		{
			// 1. The addon coming and going. Reported once at init as "not seen
			//    yet" and never again, so an addon that loads later - or dies
			//    mid-session - left no trace at all.
			static int  s_addonWas  = -1;
			static bool s_saidInEd  = false;
			const int   addonNow    = fxcapture::addonPresent() ? 1 : 0;
			if (addonNow != s_addonWas)
			{
				if (s_addonWas != -1 || !addonNow)
					logger::write("info",
						"capture: addon %s (heartbeat=%u). Rendering needs this alive.",
						addonNow ? "is now PRESENT" : "is NOT presenting",
						fxcapture::heartbeat());
				s_addonWas = addonNow;
				s_saidInEd = false;   // re-state it once we are in the editor
			}

			// Say it AGAIN, once, when the editor is actually open.
			//
			// The startup report is ~1s into the process, long before ReShade
			// necessarily has a device, so "not presenting" there means little -
			// and a user reading their log sees one stale line and concludes
			// nothing. Restating it at the moment rendering could be requested is
			// the one that answers "why did Export fall back".
			if (!s_saidInEd && game::isEditModeActive() && Config::get().enableRenderer)
			{
				s_saidInEd = true;

				// Re-check WHICH ReShade, for the same reason this whole block
				// exists. The startup scan runs before ReShade is necessarily in
				// the module list, so it can report "no ReShade in this process"
				// at a machine that plainly has one - and that line then stands
				// unchallenged for the rest of the session. hostState() logs only
				// when the answer changes, so a correct startup guess costs
				// nothing here and a wrong one gets superseded.
				fxcapture::hostState();

				if (fxcapture::addonPresent())
					logger::write("info",
						"capture: editor open, addon PRESENT (heartbeat=%u) - Export will "
						"render with RE+", fxcapture::heartbeat());
				else
					logger::write("info",
						"capture: editor open but the addon is NOT presenting (channel=%s, "
						"heartbeat=0). Export WILL fall back to the game's own encoder. "
						"Rendering needs ReShade WITH FULL ADD-ON SUPPORT plus the "
						"IgcsConnector.addon64 BUNDLED WITH THIS MOD - any other one lacks "
						"the capture channel and will present without ever grabbing a "
						"frame. If both are right, check the add-on is enabled and that "
						"nothing else (ENB) owns present.",
						fxcapture::available() ? "mapped" : "NOT MAPPED");
			}

			// 2. Someone patching over our Open detour. Silent today: MinHook
			//    reported success, the log said "hooked", and the bytes are gone.
			static bool s_saidClobbered = false;
			if (!s_saidClobbered && game::addr_PlaybackOpen && !exporthook::hookIntact())
			{
				s_saidClobbered = true;
				logger::write("info",
					"export: !! our Open detour is no longer at %p - another mod has "
					"patched over it. Export will not reach RE+.",
					(void*)game::addr_PlaybackOpen);
			}

			// 3. THE symptom, stated outright. If the game enters a bake that did
			//    not come through our hook, the vanilla encoder is running and we
			//    never saw the press - which is exactly the report we could not
			//    diagnose. Say so, with every precondition, at the moment it
			//    happens.
			static int s_lastType = -2;
			if (game::addr_g_PlaybackType)
			{
				const int t = *(int*)game::addr_g_PlaybackType;

				// SEED on the first observation, never judge it.
				//
				// The first version evaluated the bake test on the very first
				// read, and this global holds 1 at startup on at least some
				// builds - so it fired one millisecond after init, eight seconds
				// before the user had even opened the editor, and announced that
				// the hook was on the wrong function. It was not; Export simply
				// had not been pressed. Only a TRANSITION into bake means
				// anything.
				if (s_lastType == -2)
				{
					s_lastType = t;
				}
				else if (t != s_lastType)
				{
					const int prev = s_lastType;
					s_lastType = t;
					logger::write("info", "export: playback type %d -> %d", prev, t);

					if (t == gsig::PLAYBACK_TYPE_BAKE && !exporthook::pending() &&
					    s_step == Step::Idle)
					{
						logger::write("info",
							"export: !! entered a BAKE we did not divert - this is the "
							"vanilla encoder. Open intercepted %u time(s), detour %s, "
							"renderer=%s, addon=%s, heartbeat=%u, channel=%s.",
							exporthook::openCount(),
							exporthook::hookIntact() ? "intact" : "OVERWRITTEN",
							Config::get().enableRenderer ? "on" : "off",
							fxcapture::addonPresent() ? "present" : "not presenting",
							fxcapture::heartbeat(),
							fxcapture::available() ? "mapped" : "NOT MAPPED");

						if (!fxcapture::addonPresent())
							logger::write("info",
								"export:    the addon is not presenting, so this fallback is "
								"BY DESIGN - see the capture line above. Fix ReShade/IGCS, "
								"not the renderer.");
						else if (exporthook::openCount() == 0)
							logger::write("info",
								"export:    Open was never intercepted, so the hook is on the "
								"wrong function for this build.");
					}
				}
			}
		}

		// A diverted Export lands here: Open() has returned, but playback needs
		// a few frames before the replay clock is usable, so we retry rather
		// than starting from inside the hook.
		if (s_step == Step::Idle && exporthook::pending())
		{
			// The seek we rely on refuses to move unless the replay mode is EDIT,
			// and Open() leaves it mid-transition for a while. Starting during
			// that window is what made the first attempt abort instantly.
			//
			// Bounded by WALL CLOCK, not by a frame count. It used to give up
			// after 900 pump() calls, described in a comment as "~15s at 60fps" -
			// which is only true at 60fps. A user reported this failing after
			// 4.8 seconds, because their machine was running the editor fast
			// enough to burn 900 frames in that time. The thing being waited on is
			// a savegame-queue commit (REPLAYMODE_WAITINGFORSAVE, which is what
			// TriggerPlayback sets on the way into playback) and that takes as
			// long as the disk takes, entirely unrelated to frame rate.
			// WAIT, do not race a timer.
			//
			// A short bound here is simply wrong. Loading and precaching take as
			// long as the install takes - minutes on a heavily modded setup - and
			// giving up does not fall back to anything: the bake was already
			// diverted, so an abort costs the user the export entirely and they
			// have to press Export again. The people most likely to be cut off
			// are exactly the ones with the most content to stream.
			//
			// So wait for as long as the editor is still somewhere a render makes
			// sense, and report progress instead of sitting silent. The backstop
			// exists only to stop a genuine hang pending forever; it is not a
			// judgement about how long loading is allowed to take.
			if (game::replayMode() != gsig::REPLAYMODE_EDIT || game::replayBusy())
			{
				const unsigned long now = GetTickCount();
				if (s_pendStart == 0) { s_pendStart = now; s_pendLog = now; }

				const int   mode = game::replayMode();
				const char* busy = game::replayBusyReason();

				// The editor closed under us - nothing left to render into.
				if (mode == gsig::REPLAYMODE_DISABLED)
				{
					exporthook::clearPending();
					s_pendWait = 0; s_pendStart = 0;
					logger::write("info",
						"export: not rendering - the editor closed while waiting to start");
					return;
				}

				// Say what we are waiting on, every 5s, so a long wait looks like
				// a long wait rather than a hang.
				if (now - s_pendLog >= 5000)
				{
					s_pendLog = now;
					logger::write("info",
						"export: waiting to start - mode=%s(%d)%s%s, %us elapsed",
						gsig::replayModeName(mode), mode,
						busy ? ", busy on " : "", busy ? busy : "",
						(unsigned)((now - s_pendStart) / 1000));
				}

				if (now - s_pendStart > 300000)   // 5 min backstop
				{
					exporthook::clearPending();
					s_pendWait = 0; s_pendStart = 0;
					logger::write("info",
						"export: giving up after 5 minutes - mode=%s(%d)%s%s. This is a "
						"backstop against a hang, not a loading limit; if the editor was "
						"still legitimately streaming, press Export again once it settles.",
						gsig::replayModeName(mode), mode,
						busy ? ", busy on " : "", busy ? busy : "");
				}
				return;
			}
			if (s_pendStart != 0)
				logger::write("info", "export: replay ready after %us - starting",
					(unsigned)((GetTickCount() - s_pendStart) / 1000));
			s_pendStart = 0;

			// Do not start until the playhead is at the beginning of the PROJECT.
			//
			// A reopened playback reports the clock wherever the previous pass
			// left it - after an audio pass that is the END of a clip - and the
			// engine then advances to the NEXT clip on its own before our first
			// seek lands. The render came out starting at clip two, ran to the
			// wrap, and stopped one clip short. Waiting here for the playhead to
			// come home costs a few frames and removes the whole class of
			// problem.
			//
			// Clip ONE, not just the start of whatever clip is on screen. That
			// was all this could ask for before - a seek clamps to the current
			// clip - so pressing Export while editing clip three rendered clips
			// three onwards and called it the project.
			{
				float lo = 0.0f, hi = 0.0f;
				if (game::clipRangeAt(0, lo, hi) && game::addr_g_ReplayTimeMs)
				{
					const float now = *(float*)game::addr_g_ReplayTimeMs;
					const int   at  = game::clipIndex();

					if (at > 0 || now > lo + 250.0f)
					{
						if (s_pendWait == 0 || (s_pendWait % 30) == 0)
							rewindToProjectStart();
						if (++s_pendWait <= 900) return;

						logger::write("info",
							"export: playhead would not return to the project start "
							"(clip %d, clock %.0f, clip 1 starts %.0f) - rendering from here anyway",
							at, now, lo);
					}
				}
			}

			const char* why = nullptr;
			if (startOpenEnded(&why))
			{
				exporthook::clearPending();
				s_pendWait = 0;
			}
			else if (++s_pendWait > 900) // ~15s at 60fps
			{
				exporthook::clearPending();
				s_pendWait = 0;
				logger::write("info", "export: could not start the render - %s", why ? why : "?");
			}
			return;
		}

		if (s_step == Step::Idle) return;

		// Re-assert every frame: the editor's own update writes both of these
		// flags too, so setting them once at start is not enough to keep them
		// down - the cursor especially, which the input code rewrites whenever
		// the mouse moves.
		//
		// Hoisted to the top of the active path so it covers the AUDIO pass as
		// well. It used to sit inside the frame-pass switch, which the audio
		// case returns before ever reaching, so a pass that records sound left
		// the whole editor HUD on screen - and since that pass plays the project
		// at normal speed, it was the one you actually sat and watched.
		//
		// It also has to be above the mode-hold and transition returns below,
		// or the HUD would blink back on at every clip boundary.
		if (Config::get().renderHideHud)
		{
			game::setEditorHudVisible(false);
			game::setCursorVisible(false);
		}

		// Leaving the editor mid-render invalidates everything below.
		//
		// Compared against the mode we STARTED in, not against REPLAYMODE_EDIT:
		// an export runs as a full-project preview, which is a different mode,
		// so testing for EDIT killed every export render on its first frame.
		// A clip boundary is a MODE CHANGE, not an exit. Aborting on any change
		// ended every multi-clip render at the first boundary - after a full,
		// correct first clip, which is what made it read as an end-of-project
		// problem rather than a transition one.
		//
		// The rule is "it has to come back", applied to EVERY value including
		// DISABLED. Keying on specific mode numbers would be guesswork: the
		// engine drives clip transitions through its replay STATE bits
		// (CLIP_TRANSITION_REQUEST / _LOAD), and sm_uMode is only along for the
		// ride, so which value it lands on mid-transition is not something we
		// can assume. Waiting for the mode to return is true regardless.
		//
		// DISABLED gets a much shorter leash than the rest, because it is the
		// one value that is also the genuine "user left the editor" signal, and
		// there we want to stop promptly rather than sit for a minute.
		const int mode = game::replayMode();
		if (mode != s_startMode)
		{
			const int guard = (mode == gsig::REPLAYMODE_DISABLED) ? kExitGuard : kModeGuard;
			if (++s_modeWait <= guard)
			{
				// Stop recording before returning: this branch runs BEFORE the
				// audio case below, so without it the load would be captured as
				// however many seconds of silence and everything after the first
				// clip would sit that far behind the picture.
				if (s_step == Step::Audio) audioout::gate(false);

				// Logged on the first frame of every change: when this aborts a
				// render, the mode VALUE is the whole diagnosis, and the earlier
				// version threw it away and just said "left the editor".
				if (s_modeWait == 1)
					logger::write("info",
						"render: replay mode %d -> %d at frame %d - holding (up to %d frames)",
						s_startMode, mode, s_frame, guard);
				s_wait = s_cfg.settleFrames;   // never composite a loading screen
				return;
			}
			logger::write("info", "render: replay mode stuck at %d for %d frames - giving up",
				mode, s_modeWait);
			finish("aborted - left the editor", false);
			return;
		}

		// Back from a transition: re-assert the render's playback state.
		//
		// The pause set at the start of the render belongs to the clip we
		// started in. A clip transition runs the engine's own state machine,
		// which restores ITS saved state on the way out, and what is left is a
		// replay whose clock still follows our seeks while the world no longer
		// redraws for them - the frame counter climbs and the picture stays
		// frozen on the new clip's first image. Nothing in the seek path can
		// detect that, because from its point of view every seek lands exactly.
		//
		// So re-issue the pause and the seek for the clip we are now in, which
		// is the same reasoning as the HUD flags further down: once something
		// else has rewritten the state, setting it again at the point of use is
		// the only ordering that holds.
		if (s_modeWait)
		{
			logger::write("info",
				"render: replay mode back to %d after %d frames (%s at frame %d)",
				mode, s_modeWait,
				s_step == Step::Audio ? "still recording" : "re-arming", s_frame);
			s_modeWait = 0;

			// The audio pass is a plain real-time playthrough - the engine has
			// just moved itself to the next clip and there is nothing to re-seek.
			// Everything below is frame-pass repair, and running it here paused
			// playback and dropped the state machine into Settle, so the second
			// clip came out as rendered frames instead of sound.
			if (s_step == Step::Audio)
			{
				game::playbackSetSpeed(1.0f);
				game::playbackPlay();
				s_lastClock  = -1.0f;   // the new clip's clock has its own base
				s_audioStall = 0;
				return;
			}

			// Sliding is a playthrough too, so it needs the same treatment and
			// none of the frame-pass repair below. Running that here would pause
			// the clip and drop the state machine into Settle - which is exactly
			// the bug the audio case above exists to avoid, and it would strand a
			// sliding render on the first clip boundary.
			//
			// The engine restores ITS saved speed across a transition, so the
			// slow motion has to be re-asserted rather than assumed.
			if (s_step == Step::Slide)
			{
				game::playbackSetSpeed(s_slideSpeed);
				game::playbackPlay();
				s_lastClock  = -1.0f;   // the new clip's clock has its own base
				s_audioStall = 0;
				return;
			}

			// The rewind between the two passes goes through a transition too.
			// Let its own waiter below decide when it has landed - there is no
			// frame-pass state to repair yet.
			if (s_step == Step::Rewind) return;

			game::playbackPause();

			const int at = game::clipIndex();

			// A clip step we ASKED for, now landed. Put the project clock where
			// we planned rather than where rebaseClip would guess.
			if (s_wantClip >= 0 && at == s_wantClip) { landClip(); return; }

			// Still waiting on one. The mode changed for some other reason -
			// a stream-in, a transient - and this is not the arrival. Go back to
			// waiting; deliberately WITHOUT resetting s_clipWait, or a mode that
			// blips repeatedly would keep the guard from ever running out.
			if (s_wantClip >= 0) { s_step = Step::ClipJump; return; }

			if (s_clipN > 0 && at >= 0)
			{
				// The engine changed clip by itself. It does that whenever the
				// clock reaches the end of a clip, which our own seeks can
				// cause, so this is an ordinary way to arrive at the next clip
				// and not an error - adopt it and carry on from there.
				if (at > s_clipAt)
				{
					float lo = 0.0f, hi = 0.0f;
					s_wantClip   = at;
					s_wantClipAt = game::clipRangeAt(at, lo, hi) ? lo : 0.0f;
					landClip();
					return;
				}

				// Backwards is the preview wrapping to clip one, which is the
				// end of the project: everything from here would be a second
				// copy of footage already rendered. Note this is an INDEX
				// comparison now. It used to compare clip time SPANS, and two
				// clips cut from the same recording report the same span - so a
				// perfectly ordinary project ended one clip in.
				if (at < s_clipAt)
				{
					finish("finished - playback wrapped back to clip 1", true);
					return;
				}

				// Same clip. Whatever moved the mode was not a transition, so
				// there is nothing to rebase - re-seek where we were and carry
				// on. Reading this as a wrap, which an earlier version of this
				// block did, would have ended the render on any mode blip.
				game::jumpProjectTo(seekTime(), 0);
				s_wait = s_cfg.settleFrames;
				s_step = Step::Settle;
				return;
			}

			// No clip table: the old behaviour, span comparison and all.
			rebaseClip("changed");
			if (s_looped)
			{
				finish("finished - project looped back to the first clip", true);
				return;
			}
			game::jumpProjectTo(seekTime(), 0);
			s_wait = s_cfg.settleFrames;
			s_step = Step::Settle;   // abandon any ack we were waiting on
			return;
		}

		// Between the passes: the audio is recorded and we have asked the editor
		// to go back to clip one. Wait for it to land, then start the frame pass
		// in this same playback.
		//
		// Deliberately above the heartbeat gate. The rewind needs nothing from
		// the capture addon, and a render that could not get home because the
		// addon happened to stall would be a bad trade.
		if (s_step == Step::Rewind)
		{
			float lo = 0.0f, hi = 0.0f;
			const bool haveClip = game::clipRangeAt(0, lo, hi);
			const float clock   = game::addr_g_ReplayTimeMs
			                    ? *(float*)game::addr_g_ReplayTimeMs : lo;

			const bool home = haveClip && game::clipIndex() == 0 &&
			                  !game::replayBusy() && clock <= lo + 250.0f;

			if (!home)
			{
				// Re-ask periodically. The request is a single field the engine
				// reads on its own schedule and clears when a transition starts,
				// so one that arrives mid-transition is simply dropped.
				if ((s_clipWait % 60) == 0) rewindToProjectStart();

				if (++s_clipWait <= kBusyGuard) return;

				logger::write("info",
					"render: could not get back to clip 1 (clip %d, clock %.0f) - "
					"press Export again to render the frames",
					game::clipIndex(), clock);
				s_step     = Step::Idle;
				s_autoOpen = 120;
				return;
			}

			// begin() insists on being entered from Idle, and startOpenEnded
			// re-arms everything - including s_fromExport, so the editor still
			// returns to its menus when the frames are done.
			logger::write("info", "render: back at clip 1 - starting the frame pass");
			s_step = Step::Idle;

			const char* why = nullptr;
			if (!startOpenEnded(&why))
			{
				logger::write("info",
					"render: could not start the frame pass - %s (press Export again)",
					why ? why : "?");
				s_autoOpen = 120;
			}
			return;
		}

		// Audio pass, handled before the addon heartbeat below: recording sound
		// needs nothing from the capture addon, so it must not be gated on one.
		if (s_step == Step::Audio)
		{
			// Drop samples while the editor is loading. The frame pass holds
			// through a clip transition and the video has no gap there, so
			// recording those seconds would push everything after the first clip
			// out of sync by however long the load took.
			// s_audioStall in the gate as well as the end test: once the clock
			// has stopped there is nothing left to record, and waiting out the
			// full stall count before closing put an extra second and a half of
			// dead air on the end of every take - which then became a second and
			// a half of extra frames, since the frame count comes from this.
			const bool clean = !game::replayBusy() && mode == s_startMode;
			audioout::gate(clean && s_audioStall == 0);

			// Follow the clip index, because on a multi-clip project it is the
			// only reliable end there is.
			//
			// A full-project preview does not stop when it runs out of project -
			// it WRAPS to clip one. The clock therefore never goes quiet, the
			// stall test below never fires, and the pass records the project over
			// and over until something else stops it: the audio pass simply did
			// not end, so the frame pass never began. The index going backwards
			// is that wrap, and it is unambiguous.
			const int at = game::clipIndex();
			bool clipChanged = false;
			if (s_clipN > 0 && at >= 0)
			{
				if (at < s_clipAt)
				{
					finish("audio pass finished - project wrapped", true);
					return;
				}
				clipChanged = at > s_clipAt;
				if (clipChanged)
					logger::write("info", "render: audio pass on clip %d/%d", at + 1, s_clipN);
				s_clipAt = at;
			}

			// A project that instead PAUSES on the last clip ends with the clock
			// going quiet while nothing is loading. Off the last clip that is a
			// hitch rather than an ending, so it gets a much longer leash - long
			// enough to ride out a stall, short enough that a genuinely wedged
			// replay still lets go of the render eventually.
			if (game::addr_g_ReplayTimeMs && clean)
			{
				const bool last  = s_clipN <= 0 || s_clipAt >= s_clipN - 1;
				const int  limit = last ? kAudioStall : kAudioStall * 8;

				const float now = *(float*)game::addr_g_ReplayTimeMs;

				// The clock jumping BACKWARDS is the preview restarting. On a
				// one-clip project that is the only form the wrap takes - the
				// index cannot show it, because there is only ever clip one -
				// and without this such a project records itself forever.
				// Excluded when the clip just changed: a new clip's timeline
				// legitimately starts lower than the last one ended.
				if (!clipChanged && s_lastClock >= 0.0f && now < s_lastClock - 250.0f)
				{
					finish("audio pass finished - playback restarted", true);
					return;
				}

				if (s_lastClock >= 0.0f && now <= s_lastClock + 0.001f)
				{
					// Everything the end of an audio pass involves lives in
					// finish(), including arming the frame pass. Closing the
					// recorder here as well looked harmless and was not: it
					// cleared s_audioPass first, so finish() took the frame-pass
					// branch instead and the second pass was never queued.
					if (++s_audioStall >= limit)
					{
						finish(last ? "audio pass finished"
						            : "audio pass finished - the clock stopped mid-project",
						       true);
						return;
					}
				}
				else s_audioStall = 0;
				s_lastClock = now;
			}
			return;
		}

		// One step per PRESENTED frame, not per call. Without this the whole
		// state machine would spin through in a single frame and every capture
		// would grab the same stale image.
		const uint32_t beat = fxcapture::heartbeat();
		if (beat == s_lastBeat) return;
		if (beat == 0) { finish("aborted - capture addon went away", false); return; }
		s_lastBeat = beat;

		// ---------------------------------------------------------------------
		//  SLIDING: the clip PLAYS; each output frame is advance-then-capture.
		// ---------------------------------------------------------------------
		//  Two phases per output frame, which is what makes the sample count exact
		//  rather than emergent:
		//
		//    ADVANCE - let the clip run until the accumulated clock reaches this
		//              frame's mark. Nothing is captured; these presents are the
		//              cost of moving the world forward.
		//    CAPTURE - take exactly RenderSamples consecutive presents, indices
		//              0..N-1, and let the addon average them. The world keeps
		//              simulating between them, which is the entire point of this
		//              mode - particles step, and TAA/SSR/RT history stays warm.
		//
		//  The first version of this took "whatever presents happened to land in
		//  the interval", so RenderSamples did nothing and the count moved with the
		//  frame rate. Separating the two phases fixes that: the count is asked for
		//  and delivered, and what varies instead is how much CLIP TIME those N
		//  samples span - which is the shutter, and is what the speed now steers.
		//
		//  One present per sample. The addon captures inside its present handler
		//  and acks there, and this runs from the camera update earlier in the same
		//  frame, so posting sample j+1 on the next tick never races sample j. The
		//  ack is still checked before posting - if the addon ever misses a present
		//  we would otherwise skip an index and hand it a short average.
		// ---------------------------------------------------------------------
		if (s_step == Step::Slide)
		{
			if (s_slideFlush)
			{
				if (!fxcapture::lastDone()) return;

				if (videoout::active())
				{
					char done[MAX_PATH];
					buildPath(done, sizeof(done));
					videoout::pushFrame(done);
				}
				s_slideFlush   = false;
				s_slideCapture = false;
				s_slideSample  = 0;

				// Steer on POSITION, not on rate.
				//
				// The obvious controller here asks "did N samples span the shutter"
				// and nudges the speed until they do. That regulates the exposure
				// LENGTH and says nothing about where on the output timeline the
				// exposure sat, and the difference is what decides whether the
				// finished video plays at a constant speed.
				//
				// A player assumes frame f covers exactly f*dt onwards. Nothing in a
				// rate controller anchors it there, and it is blind by construction: a
				// clock running at exactly the right RATE but a few milliseconds behind
				// measures its own span as perfect and corrects nothing, so the offset
				// stays forever. Cruise control holds the speed and has no opinion
				// about being three miles from where you meant to be.
				//
				// The advance phase below looks like it already covers that - it waits
				// for the absolute grid, `s_slideTime >= s_frame * s_dt`, so an error
				// cannot compound. True for a short shutter, and FALSE at the default
				// 360 degrees, which is the case that matters: there the wanted span IS
				// the frame interval, so no advance slack is left to absorb anything.
				// The test only enforces "not earlier than" - it can wait, it cannot
				// rewind - so an exposure that ran long pushes the next mark late by the
				// overshoot and every later frame inherits it. Hitches only ever add.
				// A one-way ratchet, and exactly the "the video drifts in time" symptom.
				//
				// So compare against where the exposure SHOULD have closed and fold the
				// difference into the next cycle. Absolute, so it does not matter how
				// the error arose, and self-correcting rather than merely
				// non-compounding.
				if (s_cfg.samples > 1)
				{
					const double want     = (double)s_dt * (double)s_cfg.shutter;
					const double consumed = s_slideTime - s_slideMark;

					// s_frame is still the frame just exposed - it is incremented below -
					// so this is that frame's own slot on the grid.
					const double idealEnd = (double)s_frame * (double)s_dt + want;
					const double drift    = s_slideTime - idealEnd;   // + = running ahead

					// Half the drift, not all of it. One frame's clock reading is
					// quantised by the present that carried it, so this is a noisy
					// measurement; correcting it in full chases the noise and rings.
					double target = want - drift * kSlideDriftGain;

					// Asymmetric on purpose. The two directions are not equivalent:
					// running BEHIND is absorbed by the advance wait and costs nothing,
					// while running AHEAD cannot be undone without a seek - and a seek is
					// the zero-delta frame this whole mode exists to avoid. So the loop
					// may slow almost to a stop to let the grid catch up, and may only
					// hurry back gently. It settles fractionally short, the harmless side.
					const double lo = want * kSlideTargetLo;
					const double hi = want * kSlideTargetHi;
					if (target < lo) target = lo;
					if (target > hi) target = hi;

					// Proportional, and it replaces the old two-regime nudge outright.
					// That version corrected nothing at all inside a +/-15% band, which is
					// precisely where a standing offset lives, and needed a separate fast
					// path for large errors because a fixed 10% step took fourteen frames
					// to walk back from a bad start. One clamped ratio does both jobs.
					if (consumed > 0.0)
					{
						double k = target / consumed;
						if (k < 0.1) k = 0.1;
						if (k > 4.0) k = 4.0;
						s_slideSpeed = (float)((double)s_slideSpeed * k);
					}

					if (s_slideSpeed < 0.001f) s_slideSpeed = 0.001f;
					if (s_slideSpeed > 1.0f)   s_slideSpeed = 1.0f;
					game::playbackSetSpeed(s_slideSpeed);

					const int shown = (int)(s_slideSpeed * 10000.0f);
					if (shown != s_slideLogged)
					{
						s_slideLogged = shown;
						logger::write("info",
							"render: sliding - %d sample(s) spanned %.1fms of a %.1fms "
							"shutter, drift %+.1fms; speed now %.4gx",
							s_cfg.samples, consumed, want, drift, s_slideSpeed);
					}
				}

				if (++s_frame >= s_frames) { finish("finished", true); return; }

				const uint32_t now = GetTickCount();
				if (now - s_lastReport >= kReportEveryMs)
				{
					s_lastReport = now;
					const float elapsed = (now - s_startTick) / 1000.0f;
					logger::write("info",
						"render: %d/%d frames, %.0fs elapsed, clip time %.1fs",
						s_frame, s_frames, elapsed, s_slideTime * 0.001);
				}
			}

			if (!game::addr_g_ReplayTimeMs) { finish("aborted - no replay clock", false); return; }

			// Follow the engine across clips, and stop when it wraps - the same
			// rule the audio pass uses, because a full-project preview does not
			// stop at the end, it returns to clip one.
			const int at = game::clipIndex();
			bool clipChanged = false;
			if (s_clipN > 0 && at >= 0)
			{
				if (at < s_clipAt) { finish("finished - playback wrapped back to clip 1", true); return; }
				clipChanged = at > s_clipAt;
				if (clipChanged)
					logger::write("info", "render: sliding onto clip %d/%d at frame %d",
						at + 1, s_clipN, s_frame);
				s_clipAt = at;
			}

			// Nothing usable is on screen during a load, and the clock is not
			// moving either, so contribute neither a sample nor any time.
			if (game::replayBusy() || s_spinnerSeen)
			{
				s_spinnerSeen = false;
				s_lastClock   = -1.0f;
				return;
			}

			const float clock = *(float*)game::addr_g_ReplayTimeMs;

			// Accumulate clip time, refusing the deltas that are not playback:
			// negative is a new clip or a restart, and an implausibly large jump is
			// a hitch or a seek. Crediting either slides the output timeline
			// against the picture.
			if (s_lastClock >= 0.0f && !clipChanged)
			{
				const float delta = clock - s_lastClock;
				if (delta > 0.0f && delta < s_dt * 4.0f)
				{
					s_slideTime += delta;
					// How much clip time one present covers, smoothed. This is the
					// whole measurement AUTO needs: it already folds in the present
					// rate and the current speed, so no assumption about either.
					s_slideStep = (s_slideStep <= 0.0) ? delta : s_slideStep * 0.8 + delta * 0.2;
				}

				// A clock that stops while nothing is loading is the project having
				// ended on its last clip.
				if (delta <= 0.0f)
				{
					if (++s_audioStall >= kAudioStall)
					{
						finish("finished - the clock stopped", true);
						return;
					}
				}
				else s_audioStall = 0;
			}
			s_lastClock = clock;

			// --- WARM-UP ------------------------------------------------------
			//
			// Measure before exposing anything. Frame 0's mark is zero, so without
			// this the capture begins on the first present and the seed speed goes
			// straight into the shutter - which is how a 33ms exposure came out
			// spanning 1373ms of clip.
			if (!s_slideCalib)
			{
				++s_slideWarm;

				// Throw away the first half of the measurement.
				//
				// playbackSetSpeed does not take effect on the present that
				// issues it, so the earliest deltas are still at whatever speed
				// the clip was running at before - which for the frame pass is
				// 1.0x. Averaging those in reads the OLD speed: the log showed
				// "one present covers 9.213ms of clip at 0.005x", implying 1.8
				// seconds per present, when the true figure was ~0.4ms. The
				// calibration came out ~23x too slow and the controller needed
				// five frames to climb back - five visibly under-blurred frames,
				// in the output, at the head of every render.
				//
				// Resetting the average here means only deltas measured at the
				// speed we actually set reach it.
				if (s_slideWarm == kSlideWarm / 2) s_slideStep = 0.0;

				if (s_slideWarm < kSlideWarm || s_slideStep <= 0.0) return;

				s_slideCalib = true;

				const int    n    = s_cfg.samples < 1 ? 1 : s_cfg.samples;
				const double want = (n > 1) ? (double)s_dt * (double)s_cfg.shutter
				                            : (double)s_dt * 0.1;
				const double have = s_slideStep * (double)n;
				if (have > 0.0)
				{
					float ns = (float)((double)s_slideSpeed * want / have) * kSlideCalibBias;
					if (ns < 0.001f) ns = 0.001f;
					if (ns > 1.0f)   ns = 1.0f;
					logger::write("info",
						"render: sliding AUTO - one present covers %.3fms of clip at "
						"%.4gx, so %d sample(s) start at %.4gx for a %.1fms shutter",
						s_slideStep, s_slideSpeed, n, ns, want);
					s_slideSpeed  = ns;
					s_slideLogged = (int)(ns * 10000.0f);
					game::playbackSetSpeed(s_slideSpeed);
				}

				// The warm-up was pre-roll, not output time. Rebasing here means
				// frame 0 starts from wherever the measurement left the playhead
				// instead of carrying a head offset for the whole render.
				s_slideTime = 0.0;
				s_lastClock = -1.0f;   // the next delta belongs to the new speed
				return;
			}

			// --- ADVANCE ------------------------------------------------------
			if (!s_slideCapture)
			{
				if (s_slideTime < (double)s_frame * (double)s_dt) return;
				s_slideCapture = true;
				s_slideSample  = 0;
				s_slideMark    = s_slideTime;   // where this frame's exposure opened
			}

			// --- CAPTURE ------------------------------------------------------
			if (!fxcapture::lastDone()) return;   // the addon missed a present

			const int want = s_cfg.samples < 1 ? 1 : s_cfg.samples;

			char path[MAX_PATH];
			buildPath(path, sizeof(path));
			fxcapture::requestSample(path, want, s_slideSample);

			if (++s_slideSample >= want) s_slideFlush = true;
			return;
		}

		switch (s_step)
		{
		case Step::Settle:
		{
			if (--s_wait > 0) return;

			// LOADING IS CHECKED FIRST, and that ordering is the whole fix for
			// multi-clip renders. A seek across a clip boundary sends the editor
			// into a load, during which the clock legitimately stops moving. The
			// end-of-timeline test below cannot tell that apart from running out
			// of project, so if it runs first it declares the render finished at
			// the first clip boundary and returns to the menu - which is exactly
			// what happened. A clock that is not advancing because a clip is
			// loading is not the end of anything.
			if (game::replayBusy() || s_spinnerSeen)
			{
				s_spinnerSeen = false;
				if (++s_busyWait > kBusyGuard)
				{
					finish("aborted - stuck loading", false);
					return;
				}
				// The clock is meaningless mid-load; do not let this count
				// towards the end-of-timeline decision.
				s_shortRuns = 0;
				s_wait = s_cfg.settleFrames; // resettle once the clip is back
				return;
			}
			s_busyWait = 0;

			// ---------------------------------------------------------------
			// Off the end of the clip on screen: step to the next one, or stop.
			//
			// This is the multi-clip pump, and the thing that was missing. A
			// seek CLAMPS to the clip it is already in, so once the project
			// clock runs past this clip's end every seek from here lands on the
			// same last image - which is what "it renders one clip" actually
			// was: clip one, correct, then that clip's final frame repeated for
			// however many frames were left in the count.
			//
			// The old code waited for the ENGINE to change clip. It does do that
			// when the clock reaches a clip end, and the handler above still
			// takes it when it happens - but it is not something to depend on
			// while paused, and there is no reason to wait for it when the clip
			// list is right there. Asking outright also turns the end of the
			// project into a fact - there is no clip after this one - instead of
			// the ten-strikes-and-assume below.
			// ---------------------------------------------------------------
			// Whether the clip table answered THIS time, not merely whether it
			// answered at the start.
			//
			// These accessors go through the playback controller and can decline
			// - they need the replay mode to be right, and a render spends time
			// in transitions where it is not. Keyed on s_clipN alone, a decline
			// took the clip branch, found nothing to do, and SKIPPED the
			// end-of-timeline fallback below because that was an `else if`. On an
			// open-ended render (s_frames = INT_MAX) nothing else stops it, so
			// the render simply never ended.
			bool clipEndKnown = false;
			if (s_clipN > 0)
			{
				float lo = 0.0f, hi = 0.0f;

				// Half a millisecond of tolerance: seekTime() lands exactly on
				// hi at the last frame of a clip whose length divides evenly,
				// and that frame is this clip's, not the next one's.
				if (game::clipRange(lo, hi))
				{
					clipEndKnown = true;
					if (seekTime() > hi + 0.5f)
					{
						if (s_clipAt + 1 < s_clipN) { advanceClip(hi); return; }
						finish("finished - end of the last clip", true);
						return;
					}
				}
			}

			// End of the project, for open-ended renders with no clip table -
			// and for the clip-aware ones whenever the table declined above.
			//
			// Both sides are now in the CLIP's own time - seekTime() is what we
			// actually asked the engine for, and the clock reports the same
			// space - so this is a straight "did the seek land" test with no
			// offset bookkeeping. The clip-restart guesswork that used to live
			// here existed only because project time was being compared against
			// clip time, and re-basing on transition removed the mismatch it was
			// trying to paper over.
			if (!clipEndKnown && s_openEnded && game::addr_g_ReplayTimeMs)
			{
				const float want = seekTime();
				const float have = *(float*)game::addr_g_ReplayTimeMs;
				s_lastClock = have;

				if (want - have > s_dt * 1.5f)
				{
					logger::write("info",
						"render: seek fell short at frame %d - want %.1f, clock %.1f, lag %.1f [strike %d]",
						s_frame, want, have, want - have, s_shortRuns + 1);

					// Several strikes, not two: the cost is asymmetric. Too eager
					// ends a long render early and silently; too patient wastes a
					// few seconds of seeks at the real end. Running off the end of
					// the LAST clip lands here, which is correct - running off the
					// end of any other clip makes the editor change clip first,
					// and the re-base resets this.
					if (++s_shortRuns >= 10) { finish("finished - reached the end", true); return; }
					s_wait = s_cfg.settleFrames;
					return;
				}
				s_shortRuns = 0;
			}

			char path[MAX_PATH];
			buildPath(path, sizeof(path));
			if (!fxcapture::requestSample(path, s_cfg.samples, s_sample))
			{
				finish("aborted - capture channel lost", false);
				return;
			}
			s_guard = 0;
			s_step  = Step::Ack;
			return;
		}

		case Step::ClipJump:
		{
			// Waiting on a clip step we asked for.
			//
			// Only reached when the engine did NOT drop the replay mode while it
			// loaded; when it does, the mode-hold block above absorbs the wait
			// and lands the jump itself. Both routes end in landClip(), so
			// whichever gets there first is fine.
			if (game::clipIndex() != s_wantClip || game::replayBusy() || s_spinnerSeen)
			{
				s_spinnerSeen = false;
				if (++s_clipWait > kBusyGuard)
				{
					finish("aborted - clip step never landed", false);
					return;
				}

				// Re-ask now and then, for the same reason the rewind does: the
				// request is one field the engine reads on its own schedule and
				// clears at a transition, so one that arrives mid-transition is
				// dropped without trace.
				if ((s_clipWait % 120) == 0)
					game::jumpToClip(s_wantClip, s_wantClipAt);
				return;
			}
			landClip();
			return;
		}

		case Step::Ack:
		{
			if (!fxcapture::lastDone())
			{
				if (++s_guard > kAckGuard) finish("aborted - addon stopped responding", false);
				return;
			}

			// Next sub-sample, or next output frame.
			if (++s_sample >= s_cfg.samples)
			{
				s_sample = 0;

				// Every sub-sample of this output frame has been accumulated and
				// written, so the file is final. Hand it over BEFORE advancing -
				// buildPath keys off s_frame, and the last frame has to go out
				// before the finish() below returns.
				if (videoout::active())
				{
					char done[MAX_PATH];
					buildPath(done, sizeof(done));
					videoout::pushFrame(done);
				}

				if (++s_frame >= s_frames) { finish("finished", true); return; }

				// Time-based rather than every Nth frame: at these speeds a
				// fixed frame interval is either a wall of text or one line
				// every few minutes, depending on the sample count.
				const uint32_t now = GetTickCount();
				if (now - s_lastReport >= kReportEveryMs)
				{
					s_lastReport = now;
					const float elapsed = (now - s_startTick) / 1000.0f;
					const float perFrame = elapsed / (float)s_frame;
					if (s_openEnded)
					{
						logger::write("info",
							"render: %d frames, %.0fs elapsed (%.1fs/frame)",
							s_frame, elapsed, perFrame);
					}
					else
					{
						logger::write("info",
							"render: %d/%d frames (%.0f%%), %.0fs elapsed, ~%.0fs left",
							s_frame, s_frames, 100.0f * s_frame / (float)s_frames,
							elapsed, perFrame * (float)(s_frames - s_frame));
					}
				}
			}

			game::jumpProjectTo(seekTime(), 0);

			// Advancing to a new output frame is a real seek - a whole frame
			// interval. Stepping between sub-samples moves the clock by
			// dt*shutter/N, a fraction of a millisecond. Waiting the same for
			// both is what made a render cost minutes per second of footage:
			// at 64 samples that is 192 frames of waiting per output frame, and
			// 189 of them are for seeks far too small to need it.
			//
			// It cannot go to zero. One redraw per sub-sample is precisely what
			// produces the blur - skip it and all 64 samples capture the same
			// image, which averages back to one sharp frame.
			s_wait = (s_sample == 0) ? s_cfg.settleFrames : s_cfg.settleSubFrames;
			s_step = Step::Settle;
			return;
		}

		default:
			s_step = Step::Idle;
			return;
		}
	}
}
