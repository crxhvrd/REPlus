// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "replay/precache.h"
#include "capture/render.h"
#include "game/signatures.h"

// =============================================================================
//  The editor stalling between every action
// =============================================================================
//  The single worst problem with the Rockstar Editor on a modded install, and
//  one the community has attacked for years from the wrong end.
//
//  After every seek, pause and marker jump, CReplayMgrInternal::Validate()
//  enters a precache and refuses to advance the replay until
//  IsWaitingOnWorldStreaming() has answered false for ten CONSECUTIVE frames.
//  That function's first three tests are:
//
//      CStreaming::GetNumberObjectsRequested() > 0
//      strStreamingEngine::GetIsLoadingPriorityObjects()
//      GetInfo().GetNumberRealObjectsRequested()
//
//  All three are the GLOBAL, whole-game streaming counters. So the editor waits
//  for the entire streaming system to fall completely idle - and a modded game
//  never is. The scene streamer re-scores the PVS every frame, LOD and HD-txd
//  transitions churn continuously, and the replay preloader issues fresh
//  requests inside the very loop that is waiting on them.
//
//  THAT is why no capacity fix has ever worked. It is an idle requirement, not
//  a memory one; a bigger heap simply lets the streamer keep more in flight.
//  The only two workarounds anyone ever found - draw distance at 0%, and
//  unzooming the camera - both work by removing work from the streamer rather
//  than by giving it more room. (Zoom because LOD scale is
//  tan(22.5deg)/tan(fov/2), so a tight FOV multiplies every entity's LOD
//  trigger distance and the streamed set grows with the cube of it.)
//
//  Failing to settle, the precache runs to its give-up timer every single time.
//  And that timer accumulates at MIN(33, frameMs), so 6600 is 200 FRAMES rather
//  than 6.6 seconds: ~6.7 s at 30 fps, ~13 s at 15, ~20 s at 10. Editor input
//  is dead for the whole of it, because CVideoEditorPlayback::UpdateInput gates
//  its editing block on !CReplayMgr::IsPreCachingScene().
//
//  ---------------------------------------------------------------------------
//  Why answering false is a fix and not a shortcut
//  ---------------------------------------------------------------------------
//  The code immediately AFTER this gate still runs the replay's own completion
//  check:
//
//      readerResult = sm_pAdvanceReader->HandleResults(PreloaderScannerType, ...)
//      if (!readerResult) return eValidationOther;
//
//  So the +/-4 s window of entity and event preloads is still waited on. We only
//  stop waiting for the whole map streamer to go quiet. The set we still wait
//  for is finite and therefore converges; the one we stop waiting for is
//  refilled every frame by design and never would.
//
//  It is also a path the engine already ships and already takes - the 6600
//  give-up returns false from exactly here. We take it on frame one instead of
//  frame two hundred.
//
//  The RENDER is untouched, and deliberately so. render.cpp gates frame capture
//  on game::replayBusy(), which reads ms_bPreCaching - so shortening the
//  precache would let it capture frames the stock code would have waited for.
//  The hook suspends itself while a render is active, and the game's own bake
//  blocks per frame through a different route entirely
//  (CSceneStreamerMgr::Process -> BlockAndLoad), which this never touched.
// =============================================================================
namespace precache
{
	namespace
	{
		// =====================================================================
		//  Nothing here may touch the game outside the editor
		// =====================================================================
		//  Every one of these hooks sits on a function the engine only reaches
		//  from CReplayMgrInternal::Validate(), and BOTH of Validate's call
		//  sites are already wrapped in `if (IsEditModeActive())`. So in theory
		//  none of this can fire during normal play.
		//
		//  In theory is not good enough for something that changes streaming
		//  behaviour. A wrong pattern on a future build, an inlined copy reached
		//  by another path, or simply a wrong assumption on my part, and we
		//  would be altering when the streamer is allowed to settle during
		//  ordinary gameplay - which is exactly where an allocator failure would
		//  show up and be near-impossible to attribute.
		//
		//  Note this deliberately does NOT use game::isEditModeActive(), which
		//  answers TRUE when the mode global is unresolved. That default is right
		//  for "should the overlay hide itself" and wrong for a safety gate:
		//  here, not knowing must mean leave the game alone.
		//
		//  EDIT(2) is the exact scope the engine itself uses for Validate, so
		//  this cannot under-cover: RECORD, DISABLED, LOADCLIP and
		//  WAITINGFORSAVE never run a precache in the first place.
		unsigned long long s_outsideEditor = 0;

		// The plain test. Silent, because most callers here are POLLS rather than
		// hooks and being outside the editor is their normal state - the audio
		// pump runs every frame, and LoadModel/HandleResults legitimately fire
		// during a clip load. Counting those produced
		// "declined 9374 call(s) made outside the editor" after a few minutes of
		// ordinary play, under a message telling the reader a pattern was probably
		// on the wrong function. Alarming, and untrue.
		bool inEditor()
		{
			if (!game::addr_g_ReplayMode) return false;
			return *(const int*)game::addr_g_ReplayMode == gsig::REPLAYMODE_EDIT;
		}

		// The same test for the ONE caller where being outside the editor really
		// would be news. IsWaitingOnWorldStreaming is reachable only from
		// CReplayMgrInternal::Validate, and both of Validate's call sites are
		// already wrapped in `if (IsEditModeActive())` - so if this ever counts
		// anything, either that is untrue or the pattern has drifted onto another
		// function, and both are worth knowing.
		bool inEditorStrict()
		{
			if (inEditor()) return true;
			if (s_outsideEditor++ == 0)
				logger::write("info",
					"precache: IsWaitingOnWorldStreaming reached outside the editor "
					"(replay mode %d = %s) - declining. Validate is edit-gated, so this "
					"should be impossible; suspect the pattern.",
					game::replayMode(), gsig::replayModeName(game::replayMode()));
			return false;
		}

		// ---------------------------------------------------------------------
		//  The gate
		// ---------------------------------------------------------------------
		using FnIsWaiting = char(__fastcall*)();
		FnIsWaiting origIsWaiting = nullptr;

		// Counters, for the one honest line we log. Split deliberately: "we
		// short-circuited N times" says nothing on its own, because most of
		// those calls would have returned false anyway on a light install. The
		// interesting number is how often the stock answer was "still waiting",
		// i.e. how many frames of stall were actually removed.
		unsigned long long s_calls    = 0;   // times the hook decided
		unsigned long long s_wouldWait = 0;  // ...and stock would have blocked
		bool               s_suspended = false;

		// When the current run of precaching began, in real milliseconds, and
		// when the gate was last queried. Used to bound the preload wait below.
		//
		// The gate is only reachable from inside Validate's IsPreCachingScene()
		// branch, so "was it called microseconds ago" is an exact test for
		// "we are inside a precache right now", and a gap means a new one.
		double s_gateLastMs  = 0.0;
		double s_runStartMs  = 0.0;

		double nowMs();   // defined with the model-load hook below

		char __fastcall hkIsWaitingOnWorldStreaming()
		{
			if (!inEditorStrict()) return origIsWaiting();

			const Config& cfg = Config::get();

			if (!cfg.fastPrecache) return origIsWaiting();

			// Leave captured frames alone. A render wants exactly the guarantee
			// this hook removes, and it can afford the wait - it is not
			// interactive.
			if (!cfg.fastPrecacheDuringRender && render::active())
			{
				s_suspended = true;
				return origIsWaiting();
			}
			s_suspended = false;

			// Mark that a precache is live, and start a new run's clock when
			// the last query was long enough ago to be a different one.
			const double t = nowMs();
			if (s_gateLastMs == 0.0 || t - s_gateLastMs > 500.0) s_runStartMs = t;
			s_gateLastMs = t;

			// Ask stock what it WOULD have said, purely so the log can report
			// how much stalling this actually removed.
			//
			// Calling through is safe and side-effect-free for our purposes:
			// the function reads counters and flags and returns a verdict. The
			// one thing it does mutate is Enhanced's inlined vehicle-HD sweep,
			// which calls Update_HD_Models on vehicles that are mid-upgrade -
			// and that is work the game wants done either way. Letting it run
			// keeps the HD pipeline ticking while we stop BLOCKING on it.
			const char stock = origIsWaiting();

			++s_calls;
			if (stock) ++s_wouldWait;

			return 0;
		}

		// ---------------------------------------------------------------------
		//  The blocking urgent load
		// ---------------------------------------------------------------------
		//  A separate freeze, and a harder one: this spins
		//  CStreaming::LoadAllRequestedObjects() on the main thread, so nothing
		//  renders at all. Five seconds for the first entity whose model cannot
		//  be streamed, one second for each afterwards.
		//
		//  We clamp the manager's own timeout rather than forcing
		//  createUrgent=false. Both stop the hang, but clearing createUrgent
		//  changes which branch the function returns through, and the timeout is
		//  a value the engine already writes to itself (5000 -> 1000 on failure)
		//  - so a smaller number is a value it is already built to hold.
		using FnLoadModel = bool(__fastcall*)(void*, unsigned, int, char, char,
		                                      int*, int*, unsigned);
		FnLoadModel origLoadModel = nullptr;

		// How many urgent loads actually SPUN, and for how long in total.
		//
		// The first version of this counted every urgent call and logged it as
		// "urgent model loads capped: 639", which reads as 639 hangs prevented
		// and is nothing of the sort - almost all of those complete instantly
		// because the model is already resident. The number that matters is how
		// many entered the blocking loop at all, which is measurable: time the
		// original. Anything past a frame spun.
		unsigned long long s_urgentCalls = 0;   // urgent loads seen
		unsigned long long s_urgentSpun  = 0;   // ...that blocked >16ms
		double             s_urgentMs    = 0.0; // total time they blocked for

		double nowMs()
		{
			LARGE_INTEGER f, t;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&t);
			return f.QuadPart ? (double)t.QuadPart * 1000.0 / (double)f.QuadPart : 0.0;
		}

		bool __fastcall hkLoadModel(void* self, unsigned modelHash, int mapTypeDef,
		                            char oldVersion, char createUrgent,
		                            int* modelReq, int* req, unsigned flags)
		{
			// Straight through outside the editor - no write, no timing, nothing.
			// This one is the most important of the four to scope: the model
			// manager belongs to the replay interfaces, and if any path other
			// than editor playback ever reaches it, lowering the timeout would
			// change entity creation during ordinary gameplay.
			if (!inEditor())
				return origLoadModel(self, modelHash, mapTypeDef, oldVersion,
				                     createUrgent, modelReq, req, flags);

			const float want = Config::get().urgentModelLoadMs;

			// Only the urgent path blocks, and only a positive setting overrides.
			// Zero is the documented "leave it stock" value.
			if (self && createUrgent && want > 0.0f)
			{
				auto* timeout = (float*)((unsigned char*)self + gsig::MODELMGR_LOADTIMEOUT_OFF);

				// Refuse to write anything that is not the value we expect to
				// find. `self` is a pointer we were handed rather than one we
				// resolved, and +0x20 on the wrong object is someone else's
				// field. The engine only ever holds 5000 or 1000 here (or our
				// own value from a previous call), so anything else means the
				// layout moved and we should keep our hands off it.
				const float cur = *timeout;
				const bool  known = cur == gsig::MODELMGR_TIMEOUT_STOCK
				                 || cur == gsig::MODELMGR_TIMEOUT_FALLBACK
				                 || cur == want;

				if (known)
				{
					if (cur != want) *timeout = want;
					++s_urgentCalls;
				}
				else
				{
					static bool s_warned = false;
					if (!s_warned)
					{
						s_warned = true;
						logger::write("info",
							"precache: model-manager timeout at +0x%X reads %.1f, which is "
							"neither 5000 nor 1000 - layout has moved, leaving it stock",
							gsig::MODELMGR_LOADTIMEOUT_OFF, cur);
					}
				}
			}

			if (!createUrgent)
				return origLoadModel(self, modelHash, mapTypeDef, oldVersion,
				                     createUrgent, modelReq, req, flags);

			const double t0 = nowMs();
			const bool ok = origLoadModel(self, modelHash, mapTypeDef, oldVersion,
			                              createUrgent, modelReq, req, flags);
			const double dt = nowMs() - t0;

			if (dt > 16.0) { ++s_urgentSpun; s_urgentMs += dt; }
			return ok;
		}

		// ---------------------------------------------------------------------
		//  The preload wait - the last one, and the only one with no give-up
		// ---------------------------------------------------------------------
		//  With both stall gates bypassed this is what remains, and unlike them
		//  it has no timer at all:
		//
		//      bool readerResult = sm_pAdvanceReader->HandleResults(...);
		//      if (!readerResult) return eValidationOther;
		//
		//  HandleResults answers `m_reachedExtent && requests.size() == 0`, and a
		//  request leaves that array only by being satisfied or by ageing out
		//  after an internal 10-second timeout - REAL time, not the
		//  frame-quantised units the stall timers count in. So one entity whose
		//  model cannot stream pins the precache for ten seconds; and if a dense
		//  frame's requests will not fit the array, m_reachedExtent never becomes
		//  true and nothing ends the wait at all.
		//
		//  We always call the original, so the scanner threads are still joined
		//  and the preloading still happens - only the ANSWER is overridden, and
		//  only after a wall-clock budget. What that costs is entities arriving
		//  through the urgent path instead of the preloaded one, which is already
		//  capped above.
		//
		//  Distinguishing the three call sites matters. Only Validate's blocks on
		//  the result; the jump-prepare state machine is a different mechanism
		//  left alone, and the playback-path call discards its answer entirely.
		//  Validate's is the only one that runs immediately after the streaming
		//  gate within the same call, so requiring that the gate was queried
		//  microseconds ago identifies it exactly - no argument sniffing, no
		//  guessing from scannerTypes.
		using FnHandleResults = char(__fastcall*)(void*, unsigned, void*, unsigned,
		                                          char, unsigned);
		FnHandleResults origHandleResults = nullptr;

		unsigned long long s_preloadForced = 0;
		double             s_preloadWorstMs = 0.0;

		char __fastcall hkHandleResults(void* self, unsigned scannerTypes, void* flags,
		                                unsigned replayTime, char forceLoading,
		                                unsigned mask)
		{
			// Stamped BEFORE the original runs, and that matters: the first thing
			// HandleResults does is WaitForAllScanners, which blocks on the two
			// scanner threads. Measuring the gap to the streaming gate afterwards
			// would fold that wait into it, so a scan that took longer than the
			// window would make this look like a different call site and quietly
			// disable the cap exactly when it is most needed.
			const double tEntry = nowMs();

			// The original ALWAYS runs: it joins the scanner threads and does the
			// preloading. We are overriding a verdict, not skipping work.
			const char stock = origHandleResults(self, scannerTypes, flags,
			                                    replayTime, forceLoading, mask);
			if (stock) return stock;

			if (!inEditor()) return stock;

			const Config& cfg = Config::get();
			if (!cfg.fastPrecache || cfg.precacheMaxMs <= 0) return stock;
			if (!cfg.fastPrecacheDuringRender && render::active()) return stock;

			// Only the precache's own call. Anything not immediately preceded by
			// the streaming gate is a different call site and is left stock.
			if (s_gateLastMs == 0.0 || tEntry - s_gateLastMs > 50.0) return stock;

			const double elapsed = tEntry - s_runStartMs;
			if (elapsed < (double)cfg.precacheMaxMs) return stock;

			if (elapsed > s_preloadWorstMs) s_preloadWorstMs = elapsed;
			++s_preloadForced;
			return 1;
		}

		// ---------------------------------------------------------------------
		//  The audio gate
		// ---------------------------------------------------------------------
		//  A SECOND 6600-unit budget, immediately after the streaming one and
		//  entirely independent of it:
		//
		//      if ((!IsReplayMusicTrackPrepared() || !AreAllStreamingSoundsPrepared())
		//          && sm_uAudioStallTimer < 6600)
		//      {
		//          sm_uAudioStallTimer += MIN(33, step);
		//          return eValidationOther;
		//      }
		//
		//  So a clip carrying radio music can burn its own 200 frames even with
		//  the streaming gate gone - and the precache STOPS replay music when it
		//  starts, then waits for it to re-prepare, which on a saturated install
		//  is exactly the slow thing.
		//
		//  Done by writing the counter rather than by hooking either audio
		//  predicate: the branch already exists and already has a defined
		//  meaning at 6600 ("stop waiting"), so we are choosing an outcome the
		//  engine ships rather than lying to a caller about whether audio is
		//  ready. It also needs no third and fourth pattern.
		//
		//  The address is DERIVED (stall timer + 4), so it is guarded twice:
		//  only written while ms_bPreCaching is actually set, and only when what
		//  is there now is a plausible value for this counter. A wrong derive
		//  then writes nothing and says so once.
		bool s_audioAddrRejected = false;
		unsigned long long s_audioSkips = 0;

		void pumpAudioGate()
		{
			if (!inEditor()) return;

			const Config& cfg = Config::get();
			if (!cfg.fastPrecache || !cfg.fastPrecacheAudio) return;
			if (!cfg.fastPrecacheDuringRender && render::active()) return;
			if (!game::addr_g_StreamingStallTimer || s_audioAddrRejected) return;

			// Only while the editor is actually precaching. Outside that window
			// these counters are reset by the engine anyway, and writing to them
			// would be writing for no reason.
			if (!game::addr_g_PreCaching || !*(unsigned char*)game::addr_g_PreCaching) return;

			auto* audio = (unsigned*)(game::addr_g_StreamingStallTimer + gsig::STALL_AUDIO_OFF);

			// Both stall counters are bounded by construction: the engine only
			// ever adds MIN(33, step) and only while below 6600, so nothing in
			// this field can legitimately exceed 6600+33. Anything else means
			// the neighbour assumption is wrong for this build and we must not
			// write here.
			const unsigned cur = *audio;
			if (cur > gsig::STALL_LIMIT_MS + 64)
			{
				s_audioAddrRejected = true;
				logger::write("info",
					"precache: audio stall counter at %p reads %u, which is out of range "
					"for it (max %u) - the +0x%X neighbour assumption does not hold on this "
					"build, so the audio gate is left stock.",
					(void*)audio, cur, gsig::STALL_LIMIT_MS + 64, gsig::STALL_AUDIO_OFF);
				return;
			}

			if (cur < gsig::STALL_LIMIT_MS)
			{
				*audio = gsig::STALL_LIMIT_MS;
				++s_audioSkips;
			}
		}
	}

	// One line when the hook has actually done something, and one when it stops
	// or starts being suspended. Reported from the tick rather than at install
	// because "hooked" is not the interesting fact - "removed N frames of stall"
	// is, and it is the number a tester can paste back.
	void tick()
	{
		// Runs before the early-out: the audio half stands on its own and is
		// worth having even where the streaming hook did not resolve.
		pumpAudioGate();

		if (!game::addr_IsWaitingOnWorldStreaming) return;

		static unsigned long long s_lastReported = 0;
		static bool               s_lastSuspended = false;

		if (s_suspended != s_lastSuspended)
		{
			s_lastSuspended = s_suspended;
			logger::write("info", "precache: %s (render %s)",
				s_suspended ? "suspended - stock precache in force"
				            : "active again",
				s_suspended ? "running" : "finished");
		}

		// Log on the first stall removed, then on a 4x growth, so a long session
		// produces a handful of lines rather than one per frame.
		if (s_wouldWait &&
		    (s_lastReported == 0 || s_wouldWait >= s_lastReported * 4))
		{
			s_lastReported = s_wouldWait;

			// Frames, not checks: the gate is queried twice per frame (Validate
			// runs once with preload=false and once with true, and both reach
			// it), so reporting raw checks doubles the apparent saving.
			logger::write("info",
				"precache: ~%llu frames of editor stall removed (%llu of %llu gate checks "
				"would have blocked). Audio gate skipped %llu times. Preload wait cut short "
				"%llu times (worst %.0f ms). Urgent model loads: %llu seen, %llu actually "
				"spun for %.0f ms total.",
				s_wouldWait / 2, s_wouldWait, s_calls, s_audioSkips,
				s_preloadForced, s_preloadWorstMs,
				s_urgentCalls, s_urgentSpun, s_urgentMs);

			// Should always be zero - see inEditorStrict(). Only the streaming
			// gate feeds this; the polls and the clip-load paths do not, because
			// for them being outside the editor is simply normal.
			if (s_outsideEditor)
				logger::write("info",
					"precache: !! %llu streaming-gate call(s) came from OUTSIDE the editor",
					s_outsideEditor);
		}
	}

	void install()
	{
		const Config& cfg = Config::get();

		// Say it in the log, because "does this touch my actual game" is the
		// first question anyone asks of a streaming change, and the answer
		// should not require reading the code.
		logger::write("info",
			"precache: every hook below is gated on replay mode == EDIT(2), so none of "
			"it can affect normal gameplay, recording, or a clip load. %s",
			game::addr_g_ReplayMode
				? "Mode global resolved."
				: "!! Mode global UNRESOLVED - the gate fails closed, so the whole "
				  "feature stays off rather than run unscoped.");

		if (game::addr_IsWaitingOnWorldStreaming)
		{
			memory(game::addr_IsWaitingOnWorldStreaming)
				.hook(hkIsWaitingOnWorldStreaming, &origIsWaiting, "IsWaitingOnWorldStreaming");
			logger::write("info",
				"precache: global streaming gate %s. The replay's own +/-4s preload is "
				"still waited on; only the whole-game 'streamer must be idle' "
				"requirement is dropped.",
				cfg.fastPrecache ? "BYPASSED" : "stock (FastPrecache=0)");
		}
		else
		{
			logger::write("info",
				"precache: IsWaitingOnWorldStreaming unresolved - the editor keeps its "
				"stock 200-frame wait after every seek. This is THE cause of the "
				"per-action stall on a modded install.");
		}

		if (game::addr_AdvanceReaderHandleResults)
		{
			memory(game::addr_AdvanceReaderHandleResults)
				.hook(hkHandleResults, &origHandleResults, "AdvReader::HandleResults");
			if (cfg.precacheMaxMs > 0)
				logger::write("info",
					"precache: preload wait capped at %d ms of real time. Stock has NO "
					"timer here at all - one entity that cannot stream holds the precache "
					"for %u ms, and a request array that will not fit holds it forever.",
					cfg.precacheMaxMs, gsig::PRELOAD_TIME_MAX_MS);
			else
				logger::write("info",
					"precache: preload wait left stock/unbounded (PrecacheMaxMs=0)");
		}
		else
		{
			logger::write("info",
				"precache: HandleResults unresolved - the preload wait stays unbounded, so "
				"a single un-streamable entity can still cost 10 s per action.");
		}

		if (!game::addr_g_StreamingStallTimer)
			logger::write("info",
				"precache: stall-counter derive failed - the audio gate keeps its own "
				"200-frame budget, so a clip carrying radio music can still stall even "
				"with the streaming gate bypassed.");
		else
			logger::write("info", "precache: audio gate %s (counter at %p)",
				cfg.fastPrecacheAudio ? "BYPASSED" : "stock (FastPrecacheAudio=0)",
				(void*)(game::addr_g_StreamingStallTimer + gsig::STALL_AUDIO_OFF));

		if (game::addr_ModelMgrLoadModel)
		{
			memory(game::addr_ModelMgrLoadModel)
				.hook(hkLoadModel, &origLoadModel, "ModelMgr::LoadModel");
			if (cfg.urgentModelLoadMs > 0.0f)
				logger::write("info",
					"precache: urgent model load capped at %.0f ms (stock 5000 then 1000). "
					"An entity whose model cannot stream now costs a dropped frame "
					"instead of a main-thread freeze.",
					cfg.urgentModelLoadMs);
			else
				logger::write("info",
					"precache: urgent model load left stock (UrgentModelLoadMs=0)");
		}
		else
		{
			logger::write("info",
				"precache: ModelMgr::LoadModel unresolved - an un-streamable entity can "
				"still hang the main thread for up to 5 s");
		}
	}
}
