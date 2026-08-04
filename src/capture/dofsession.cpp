// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "capture/dofsession.h"
#include "capture/render.h"
#include "replay/marker.h"
#include "replay/smoothblend.h"
#include "utils/config.h"
#include "utils/log.h"

#include <atomic>

namespace dofsession
{
	namespace
	{
		// Mirrors ScreenshotSessionStartReturnCode in the add-on. Kept as plain
		// constants rather than an enum so the ABI is unmistakable: the exported
		// function returns an int and the add-on reads it as its own enum.
		constexpr int RC_OK                  = 0;
		constexpr int RC_CAMERA_NOT_ENABLED  = 1;
		constexpr int RC_PATH_PLAYING        = 2;
		constexpr int RC_ALREADY_ACTIVE      = 3;
		constexpr int RC_FEATURE_UNAVAILABLE = 4;

		// A session left running because the add-on never called end - ReShade
		// reloaded, the overlay was closed mid-session, the user alt-F4'd the
		// preset - would leave the editor with no HUD, a pinned camera and a
		// moved playhead, and no obvious way back. So the pairing is never
		// trusted; this is the backstop.
		//
		// Deliberately generous. The controller's SETUP phase moves the camera
		// once and then waits for the user to dial the focus in, which can take
		// as long as it takes. This only has to fire eventually, not promptly -
		// the fast paths out are leaving the editor or starting a render, both
		// checked every pump.
		constexpr uint32_t kIdleTimeoutMs = 120000;

		// --- written on ReShade's thread, read on the game's ------------------
		std::atomic<bool>     s_active{ false };
		std::atomic<uint32_t> s_startSeq{ 0 };
		std::atomic<uint32_t> s_endSeq{ 0 };
		std::atomic<uint32_t> s_moveSeq{ 0 };
		std::atomic<float>    s_reqX{ 0.0f };
		std::atomic<float>    s_reqY{ 0.0f };
		std::atomic<float>    s_reqFov{ 0.0f };
		std::atomic<bool>     s_reqAbsolute{ true };

		// --- the shutter, which belongs entirely to the add-on ----------------
		// Set only by the TIMED entry point. The add-on is the only side that
		// CAN own this: it knows the sample count and which aperture ring each
		// point came from, so it can stratify the exposure and then break the
		// correlation between time and radius. Neither is knowable here - the
		// sample total is never reported over the interface - so there is no
		// fallback and no setting. An add-on that does not call the timed entry
		// point gets a frozen instant, which is the behaviour it was written
		// against.
		std::atomic<float>    s_reqTimeMs{ 0.0f };
		std::atomic<bool>     s_reqTimed{ false };

		// --- the handshake ----------------------------------------------------
		// s_servicedSeq is the move the game thread has actually acted on, and
		// s_pumpsSinceSeek counts frames since. Read by IGCS_QuerySampleReady on
		// ReShade's thread, hence atomic.
		std::atomic<uint32_t> s_servicedSeq{ 0 };
		std::atomic<uint32_t> s_pumpsSinceSeek{ 0 };
		std::atomic<uint32_t> s_lastActivity{ 0 };

		// Is the clip running under us?
		//
		// There is no playback-state accessor resolved, and requestStart is on
		// ReShade's thread anyway - so this is sampled on the game thread by
		// watching whether the replay clock moves on its own, and published for
		// the other side to read. Scrubbing counts as running, which is what we
		// want: starting a session mid-drag is not meaningful either.
		std::atomic<bool>     s_playbackRunning{ false };

		// --- main thread only --------------------------------------------------
		uint32_t s_startSeen = 0;
		uint32_t s_endSeen   = 0;
		uint32_t s_moveSeen  = 0;

		bool  s_running    = false;   // the session as the GAME sees it
		float s_offX       = 0.0f;    // aperture offset, camera-right, world units
		float s_offY       = 0.0f;    // aperture offset, camera-up
		float s_fovOverride = 0.0f;   // 0 = leave the frame's own FOV alone
		uint32_t s_sample  = 0;       // samples this session, for the log only
		bool  s_hudHidden  = false;
		bool  s_collisionSaved = false;
		bool  s_collisionWas   = false;

		// --- the shutter ------------------------------------------------------
		// The instant the session opened. Every sample time is measured from it
		// and the playhead is put back to it at the end: a session that finishes
		// somewhere else has scrubbed the user's clip behind their back.
		float s_t0        = 0.0f;
		bool  s_haveT0    = false;
		float s_spanLo    = 0.0f;   // clip bounds, so a shutter cannot run off
		float s_spanHi    = 0.0f;   // the end and pile samples onto a clamp
		bool  s_haveSpan  = false;
		bool  s_warnedSpan  = false;
		bool  s_warnedLate  = false;
		bool  s_loggedTimed = false;

		// What the previous sample asked for, so the next one can tell whether
		// the seek had landed by the time the add-on blended. Session state, not
		// a function-local static: a fresh session must not compare against the
		// last one's target.
		float s_prevTarget     = 0.0f;
		bool  s_havePrevTarget = false;

		void touch() { s_lastActivity.store(GetTickCount(), std::memory_order_relaxed); }

		// Both halves of the integration have to walk the loaded modules - one
		// to find the add-on, the other to find a rival camera tool - so these
		// are shared. K32EnumProcessModules is declared rather than included so
		// this does not pull in psapi; it lives in kernel32 on anything we run on.
		extern "C" BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE*, DWORD, LPDWORD);

		HMODULE selfModule()
		{
			HMODULE h = nullptr;
			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)&selfModule, &h);
			return h;
		}

		// =====================================================================
		//  The other half of the interface: the camera-tools DATA BUFFER
		// =====================================================================
		//  Exporting the four functions is not enough on its own. The add-on
		//  gates its whole screenshot and depth-of-field UI on a `cameraEnabled`
		//  byte in an 8 KB block that the camera tools are expected to fill every
		//  frame - and it checks that BEFORE it would ever call us. With the
		//  block left zeroed the panel just reports
		//  "The camera is currently disabled so no depth-of-field session can be
		//  started", and nothing we return from a session call is ever consulted.
		//
		//  The handshake runs the other way round from the function binding: WE
		//  call into the add-on. connectFromCameraTools() allocates the block and
		//  - usefully - makes the add-on re-scan for camera tools, so calling it
		//  once we are ready fixes the load-order problem in both directions at
		//  once. getDataFromCameraToolsBuffer() then hands back the pointer.
		//
		//  Layout must stay byte-identical to CameraToolsData in the add-on.
		// =====================================================================
		struct CameraToolsData
		{
			uint8_t cameraEnabled;         // THE gate for the whole feature
			uint8_t cameraMovementLocked;
			uint8_t reserved1;
			uint8_t reserved2;
			float   fov;                   // degrees
			float   coordinates[3];
			float   lookQuaternion[4];     // qx qy qz qw
			float   rotationMatrixUp[3];
			float   rotationMatrixRight[3];
			float   rotationMatrixForward[3];
			float   pitch, yaw, roll;      // radians
		};
		static_assert(sizeof(CameraToolsData) == 84, "CameraToolsData layout drifted");

		using FnConnectFromCameraTools = bool  (*)();
		using FnGetDataBuffer          = LPBYTE(*)();

		CameraToolsData* s_toolsData = nullptr;
		uint32_t s_lastConnectTry = 0;

		// The add-on is a ReShade add-on and we are an ASI; neither load order is
		// guaranteed, so this retries rather than assuming. Throttled, because
		// until ReShade is up it will fail every time.
		void ensureConnected()
		{
			if (s_toolsData) return;

			const uint32_t now32 = GetTickCount();
			if (s_lastConnectTry && now32 - s_lastConnectTry < 2000) return;
			s_lastConnectTry = now32;

			const HMODULE self = selfModule();
			HMODULE mods[512];
			DWORD needed = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
				return;

			const int count = (int)(needed / sizeof(HMODULE));
			for (int i = 0; i < count && i < 512; ++i)
			{
				if (mods[i] == self) continue;
				auto connect = (FnConnectFromCameraTools)
					GetProcAddress(mods[i], "connectFromCameraTools");
				auto getBuf = (FnGetDataBuffer)
					GetProcAddress(mods[i], "getDataFromCameraToolsBuffer");
				if (!connect || !getBuf) continue;

				if (!connect()) return;
				s_toolsData = (CameraToolsData*)getBuf();
				if (!s_toolsData) return;

				*s_toolsData = CameraToolsData{};

				char name[MAX_PATH]{};
				GetModuleFileNameA(mods[i], name, MAX_PATH);
				logger::write("info", "dof: connected to the capture add-on (%s) - "
					"depth-of-field sessions available", name);
				return;
			}
		}

		// Publish the editor camera every frame.
		//
		// `cameraEnabled` is deliberately the exact condition under which a
		// session can actually start, so the add-on's own panel greys itself out
		// for the right reason and the answer to "why can I not press this" is
		// visible without reading a log.
		void publishCameraData(bool sessionRunning)
		{
			if (!s_toolsData) return;

			void* dir = smoothblend::lastDirector();
			const bool usable = game::isEditModeActive()
				&& dir != nullptr
				&& !s_playbackRunning.load(std::memory_order_relaxed);

			s_toolsData->cameraEnabled        = usable ? 1 : 0;
			s_toolsData->cameraMovementLocked = sessionRunning ? 1 : 0;
			if (!dir) return;

			auto* b = (const uint8_t*)dir;
			const float* a  = (const float*)(b + rdirector::OFF_FrameMatrixA); // right
			const float* fw = (const float*)(b + rdirector::OFF_FrameMatrixB); // forward
			const float* up = (const float*)(b + rdirector::OFF_FrameMatrixC); // up
			const float* p  = (const float*)(b + rdirector::OFF_FramePosition);

			s_toolsData->fov = *(const float*)(b + rdirector::OFF_FrameFov);
			for (int i = 0; i < 3; ++i)
			{
				s_toolsData->coordinates[i]           = p[i];
				s_toolsData->rotationMatrixRight[i]   = a[i];
				s_toolsData->rotationMatrixForward[i] = fw[i];
				s_toolsData->rotationMatrixUp[i]      = up[i];
			}

			// The game's own definitions, so these agree with everything else the
			// mod does with this basis.
			s_toolsData->yaw   = atan2f(-fw[0], fw[1]);
			s_toolsData->pitch = asinf(fw[2] < -1.0f ? -1.0f : (fw[2] > 1.0f ? 1.0f : fw[2]));
			s_toolsData->roll  = atan2f(-a[2], up[2]);

			// Basis -> quaternion. Consumed only by shaders reading the
			// IGCS_cameraLookQuaternion uniform, not by the depth-of-field
			// session, so a handedness disagreement with the add-on's DirectX
			// maths would show up there and nowhere else.
			//
			// The three vectors are the COLUMNS of the rotation matrix, not its
			// rows - reading them the other way round yields the conjugate,
			// which is wrong by twice the rotation while still looking sane near
			// identity.
			const float m00 = a[0], m01 = fw[0], m02 = up[0];
			const float m10 = a[1], m11 = fw[1], m12 = up[1];
			const float m20 = a[2], m21 = fw[2], m22 = up[2];
			const float tr = m00 + m11 + m22;
			float q[4];
			if (tr > 0.0f)
			{
				const float s = sqrtf(tr + 1.0f) * 2.0f;
				q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
			}
			else if (m00 > m11 && m00 > m22)
			{
				const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
				q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
			}
			else if (m11 > m22)
			{
				const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
				q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
			}
			else
			{
				const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
				q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
			}
			for (int i = 0; i < 4; ++i) s_toolsData->lookQuaternion[i] = q[i];
		}

		// =====================================================================
		//  Getting out of another camera tool's way
		// =====================================================================
		//  The add-on binds to the FIRST module it finds exporting
		//  IGCS_StartScreenshotSession and then talks only to that one. Merely by
		//  exporting these we can therefore win the binding away from a real
		//  camera tool - Simple Camera, say - and its own screenshot sessions
		//  would stop working with no error anywhere, because the add-on is
		//  happily calling us and we are declining.
		//
		//  That would be a regression we caused, so declining is not enough: when
		//  the Rockstar Editor is not up, the call is FORWARDED to whichever
		//  other module implements the interface. Whoever the add-on happened to
		//  bind to, the request reaches the tool that can actually service it.
		//
		//  Once a session has been delegated every later call in it goes the same
		//  way, or the two tools would each own half of one session.
		// =====================================================================
		using FnStart     = int  (*)(uint8_t);
		using FnMultishot = void (*)(float, float, float, bool);
		using FnPanorama  = void (*)(float);
		using FnEnd       = void (*)();

		struct Peer
		{
			FnStart     start     = nullptr;
			FnMultishot multishot = nullptr;
			FnPanorama  panorama  = nullptr;
			FnEnd       end       = nullptr;
			bool ok() const { return start && multishot && end; }
		};

		Peer s_peer;
		bool s_peerSearched = false;
		std::atomic<bool> s_delegated{ false };

		const Peer& peer()
		{
			if (s_peerSearched) return s_peer;
			s_peerSearched = true;

			const HMODULE self = selfModule();
			HMODULE mods[512];
			DWORD needed = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
				return s_peer;

			const int count = (int)(needed / sizeof(HMODULE));
			for (int i = 0; i < count && i < 512; ++i)
			{
				if (mods[i] == self) continue;
				auto start = (FnStart)GetProcAddress(mods[i], "IGCS_StartScreenshotSession");
				if (!start) continue;

				s_peer.start     = start;
				s_peer.multishot = (FnMultishot)GetProcAddress(mods[i], "IGCS_MoveCameraMultishot");
				s_peer.panorama  = (FnPanorama)GetProcAddress(mods[i], "IGCS_MoveCameraPanorama");
				s_peer.end       = (FnEnd)GetProcAddress(mods[i], "IGCS_EndScreenshotSession");

				char name[MAX_PATH]{};
				GetModuleFileNameA(mods[i], name, MAX_PATH);
				logger::write("info", "dof: another IGCS camera tool is present (%s) - "
					"sessions outside the editor will be forwarded to it", name);
				break;
			}
			return s_peer;
		}

		// Hide the editor's UI for the duration, exactly as a render does. The
		// depth-of-field blend runs during ReShade's effect pass, which is AFTER
		// the game has drawn its frame - and the editor's HUD is part of that
		// frame, so without this every sample carries the timeline and transport
		// bar into the accumulation.
		void hideUi(bool hide)
		{
			if (!Config::get().renderHideHud) return;
			if (hide == s_hudHidden) return;
			s_hudHidden = hide;
			game::setEditorHudVisible(!hide);
			game::setCursorVisible(!hide);
		}

		// Camera collision has to be off while the camera walks the aperture.
		// Left on, a sample that lands near geometry gets shoved off the ring by
		// the game's own safe-position push, and the bokeh is quietly wrong - but
		// only near walls, which is the worst kind of bug to be handed later.
		void forceCollisionOff(bool force)
		{
			Config& cfg = const_cast<Config&>(Config::get());
			if (force)
			{
				if (s_collisionSaved) return;
				s_collisionWas = cfg.disableCameraCollision;
				s_collisionSaved = true;
				cfg.disableCameraCollision = true;
			}
			else
			{
				if (!s_collisionSaved) return;
				cfg.disableCameraCollision = s_collisionWas;
				s_collisionSaved = false;
			}
		}

		void beginOnMainThread()
		{
			s_running     = true;
			s_offX        = 0.0f;
			s_offY        = 0.0f;
			s_fovOverride = 0.0f;
			s_sample      = 0;
			s_warnedSpan    = false;
			s_warnedLate    = false;
			s_loggedTimed   = false;
			s_havePrevTarget = false;

			// Park playback. Whatever the shutter turns out to be, the only
			// thing that may move the clock during a session is us - a clip
			// running underneath would smear the whole shot rather than the
			// interval that was asked for.
			game::playbackPause();

			// The instant the session opened. Everything is measured from here
			// and the playhead is returned to it at the end.
			s_haveT0 = game::addr_g_ReplayTimeMs != 0;
			s_t0     = s_haveT0 ? *(float*)game::addr_g_ReplayTimeMs : 0.0f;

			// The clip's own bounds. A shutter running past the end would be
			// clamped by the engine, which does not fail - it quietly hands back
			// the same last frame for every sample after the boundary, and the
			// accumulation ends up weighted towards one instant with nothing
			// anywhere to say so.
			s_haveSpan = game::clipRange(s_spanLo, s_spanHi);

			hideUi(true);
			forceCollisionOff(true);

			// Whether there is a shutter at all is not known yet - it arrives
			// with the first timed step, if the add-on sends one.
			logger::write("info", "dof: session started at t=%.1fms", s_t0);
		}

		void endOnMainThread(const char* why)
		{
			if (!s_running) return;
			s_running = false;

			s_offX = s_offY = 0.0f;
			s_fovOverride = 0.0f;

			// Put the playhead back where the session found it.
			//
			// Unconditional rather than only-when-stepped: a session that ends
			// on a different frame from the one it started on has scrubbed the
			// user's clip behind their back, and whether a shutter was in play
			// depends on an add-on setting we do not track across sessions. A
			// seek to where we already are is a no-op the engine drops.
			if (s_haveT0) game::jumpProjectTo(s_t0, 0);

			forceCollisionOff(false);
			hideUi(false);

			logger::write("info", "dof: session ended (%s) after %u samples", why, s_sample);
		}
	}

	bool active() { return s_active.load(std::memory_order_relaxed); }

	// -------------------------------------------------------------------------
	// ReShade's thread. Validation is read-only - a couple of int loads - so it
	// is safe here, and it HAS to be here: the add-on uses the return value
	// synchronously to decide whether to open a session at all, and it surfaces
	// each code as its own message.
	// -------------------------------------------------------------------------
	int requestStart(uint8_t type)
	{
		if (s_active.load()) return RC_ALREADY_ACTIVE;

		// Not our territory: hand the whole session to a real camera tool if one
		// is loaded, rather than declining and leaving the user with a feature
		// that silently stopped working because we took the binding.
		if (!game::isEditModeActive())
		{
			const Peer& p = peer();
			if (p.ok())
			{
				const int rc = p.start(type);
				if (rc == RC_OK) s_delegated.store(true);
				return rc;
			}
			return RC_CAMERA_NOT_ENABLED;
		}

		// Our own renderer owns the clock and the HUD while it runs; two things
		// seeking the replay would produce neither a render nor a screenshot.
		if (render::active()) return RC_ALREADY_ACTIVE;

		if (!game::addr_g_ReplayTimeMs) return RC_FEATURE_UNAVAILABLE;

		// Playback must be parked. A session integrates ONE instant, so a clip
		// running underneath it would smear the whole shot rather than the
		// shutter interval we asked for.
		if (s_playbackRunning.load()) return RC_PATH_PLAYING;

		s_active.store(true);
		s_startSeq.fetch_add(1);
		touch();
		return RC_OK;
	}

	void requestMove(float stepLeftRight, float stepUpDown, float fovDegrees,
	                 bool fromStartPosition)
	{
		if (s_delegated.load())
		{
			if (s_peer.multishot)
				s_peer.multishot(stepLeftRight, stepUpDown, fovDegrees, fromStartPosition);
			return;
		}
		if (!s_active.load()) return;
		s_reqX.store(stepLeftRight, std::memory_order_relaxed);
		s_reqY.store(stepUpDown, std::memory_order_relaxed);
		s_reqFov.store(fovDegrees, std::memory_order_relaxed);
		s_reqAbsolute.store(fromStartPosition, std::memory_order_relaxed);
		s_reqTimed.store(false, std::memory_order_relaxed);
		s_moveSeq.fetch_add(1);
		touch();
	}

	void requestMoveTimed(float stepLeftRight, float stepUpDown, float fovDegrees,
	                      bool fromStartPosition, float timeOffsetMs)
	{
		if (s_delegated.load())
		{
			// Whoever we handed the session to almost certainly cannot step a
			// clock - nothing outside a replay can - so the time is dropped and
			// the aperture step is forwarded on its own.
			if (s_peer.multishot)
				s_peer.multishot(stepLeftRight, stepUpDown, fovDegrees, fromStartPosition);
			return;
		}
		if (!s_active.load()) return;
		s_reqX.store(stepLeftRight, std::memory_order_relaxed);
		s_reqY.store(stepUpDown, std::memory_order_relaxed);
		s_reqFov.store(fovDegrees, std::memory_order_relaxed);
		s_reqAbsolute.store(fromStartPosition, std::memory_order_relaxed);
		s_reqTimeMs.store(timeOffsetMs, std::memory_order_relaxed);
		s_reqTimed.store(true, std::memory_order_relaxed);
		s_moveSeq.fetch_add(1);
		touch();
	}

	// -------------------------------------------------------------------------
	// Has the frame on screen caught up with the last step we were given?
	//
	// ReShade's thread. Two atomic loads and a compare - it must stay that
	// cheap, because the add-on calls it every frame while it waits.
	//
	// What this can and cannot answer is worth being exact about. It reports
	// that the SEEK has been issued on the game thread and that a frame has been
	// drawn since. It does NOT report that the engine has finished settling -
	// streaming, animation and the replay's own interpolation are not observable
	// from here. So this removes OUR latency from the add-on's guess, which is
	// the part that varies with frame rate and thread timing, and leaves the
	// engine's settle to the panel's frame-wait, which is a property of the
	// scene rather than of us.
	// -------------------------------------------------------------------------
	bool sampleReady()
	{
		// Not ours to answer: a delegated session belongs to another tool, and a
		// session that is not running must never stall the add-on's loop.
		if (s_delegated.load(std::memory_order_relaxed)) return true;
		if (!s_active.load(std::memory_order_relaxed))   return true;

		if (s_servicedSeq.load(std::memory_order_acquire) != s_moveSeq.load(std::memory_order_relaxed))
			return false;

		// One full pump after the seek. pump() runs inside UpdateSmoothing, i.e.
		// during the update for the frame about to be drawn, so the seek reaches
		// that same frame - but only counting a LATER pump proves the frame
		// carrying it was actually finished and presented.
		return s_pumpsSinceSeek.load(std::memory_order_relaxed) >= 1;
	}

	// Panorama sessions rotate rather than translate. Nothing drives them here
	// yet - the depth-of-field controller only ever asks for multishot steps -
	// so this is accepted and ignored rather than half-implemented.
	void requestPanorama(float stepAngle)
	{
		if (s_delegated.load())
		{
			if (s_peer.panorama) s_peer.panorama(stepAngle);
			return;
		}
		if (!s_active.load()) return;
		touch();
	}

	void requestEnd()
	{
		if (s_delegated.load())
		{
			s_delegated.store(false);
			if (s_peer.end) s_peer.end();
			return;
		}
		if (!s_active.load()) return;
		s_endSeq.fetch_add(1);
		s_active.store(false);
		touch();
	}

	// -------------------------------------------------------------------------
	// Main thread.
	// -------------------------------------------------------------------------
	void pump()
	{
		// Sample the transport first and unconditionally. Only meaningful while
		// no session is running - once one is, every seek is ours.
		if (!s_running && game::addr_g_ReplayTimeMs)
		{
			static float s_prevTime = 0.0f;
			static bool  s_havePrev = false;
			const float t = *(float*)game::addr_g_ReplayTimeMs;
			if (s_havePrev)
				s_playbackRunning.store(fabsf(t - s_prevTime) > 0.01f,
				                        std::memory_order_relaxed);
			s_prevTime = t;
			s_havePrev = true;
		}

		// Both halves of the handshake, every frame: without the data buffer the
		// add-on never even offers the feature.
		ensureConnected();
		publishCameraData(s_running);

		const uint32_t endSeq = s_endSeq.load();
		if (endSeq != s_endSeen)
		{
			s_endSeen = endSeq;
			endOnMainThread("add-on closed it");
			return;
		}

		const uint32_t startSeq = s_startSeq.load();
		if (startSeq != s_startSeen)
		{
			s_startSeen = startSeq;
			s_moveSeen  = s_moveSeq.load();   // ignore anything queued before we began

			// Only if the session is STILL open.
			//
			// The end sequence is consumed above and RETURNS, so a start and an end
			// arriving between two pumps leaves the start to be applied a frame
			// later - against a session the add-on has already closed. That took
			// the HUD and the cursor down, paused playback and forced collision
			// off, with s_active already false and nothing to undo any of it until
			// the 120s idle timeout expired.
			if (s_active.load()) beginOnMainThread();
		}

		if (!s_running) return;

		// Fast ways out, checked before the idle timeout because they are
		// unambiguous: the editor closing takes the camera with it, and a render
		// starting means something else now owns the clock.
		if (!game::isEditModeActive()) { s_active.store(false); endOnMainThread("editor closed"); return; }
		if (render::active())          { s_active.store(false); endOnMainThread("render started"); return; }

		if (GetTickCount() - s_lastActivity.load() > kIdleTimeoutMs)
		{
			s_active.store(false);
			endOnMainThread("idle timeout");
			return;
		}

		// Keep asserting these: the game turns the cursor back on by itself, the
		// same reason the renderer re-applies them every pump.
		if (s_hudHidden)
		{
			game::setEditorHudVisible(false);
			game::setCursorVisible(false);
		}

		const uint32_t moveSeq = s_moveSeq.load();
		if (moveSeq == s_moveSeen)
		{
			// Count frames since the last seek. This is the whole answer to the
			// add-on's readiness query, so it has to keep running on the quiet
			// frames rather than only when a step arrives.
			const uint32_t p = s_pumpsSinceSeek.load(std::memory_order_relaxed);
			if (p < 0xFFFFFFFFu) s_pumpsSinceSeek.store(p + 1, std::memory_order_relaxed);
			return;
		}
		s_moveSeen = moveSeq;

		const float x = s_reqX.load(std::memory_order_relaxed);
		const float y = s_reqY.load(std::memory_order_relaxed);
		const float f = s_reqFov.load(std::memory_order_relaxed);

		if (s_reqAbsolute.load(std::memory_order_relaxed))
		{
			s_offX = x;
			s_offY = y;
		}
		else
		{
			s_offX += x;
			s_offY += y;
		}
		s_fovOverride = f > 0.0f ? f : 0.0f;

		// --- the shutter ------------------------------------------------------
		//
		// Step the clock so this sample sees its own instant. The camera is NOT
		// pinned while that happens: the spline re-evaluates at the new time and
		// carries the camera along its path, so a moving shot smears the whole
		// frame the way a real camera translating during an exposure does.
		//
		// That interacts with the add-on's alignment in a way worth stating,
		// because it looks like a bug and is not. Every sample is shifted in
		// screen space before it is summed, and that shift is derived from the
		// APERTURE offset alone. So the aperture's parallax is cancelled and the
		// focus plane stays put, while any displacement the SPLINE contributes
		// is left uncancelled and smears. That is the correct division: the
		// aperture is the lens, the spline is the camera body, and only the
		// first is supposed to leave the focus plane sharp.
		//
		// Where in the exposure this sample belongs comes from the add-on and
		// nowhere else - see s_reqTimed. An untimed step means a frozen instant,
		// and the clock is not touched at all.
		// An offset of exactly zero is the add-on telling us its shutter is off:
		// every sample then wants the same instant, which is the frozen-instant
		// case and wants no seek at all. Treated as untimed rather than as a
		// zero-length step, so a shutter-0 session is byte-for-byte the old
		// behaviour instead of a no-op seek and a misleading log line per sample.
		const float offset = s_reqTimed.load(std::memory_order_relaxed)
			? s_reqTimeMs.load(std::memory_order_relaxed) : 0.0f;

		if (offset != 0.0f && s_haveT0)
		{
			float t = s_t0 + offset;

			if (!s_loggedTimed)
			{
				s_loggedTimed = true;
				logger::write("info",
					"dof: shutter driven by the capture add-on - stepping the clock per "
					"sample, so this pass carries motion blur as well as bokeh");
			}

			// Never leave the clip. The engine clamps rather than fails, which
			// would silently pile several samples onto the last frame.
			if (s_haveSpan && (t > s_spanHi || t < s_spanLo))
			{
				if (!s_warnedSpan)
				{
					s_warnedSpan = true;
					logger::write("info",
						"dof: sample at t=%.0f is outside the clip (%.0f..%.0f) and is being "
						"clamped - shorten the add-on's Shutter or move the playhead off "
						"the clip boundary", t, s_spanLo, s_spanHi);
				}
				if (t > s_spanHi) t = s_spanHi;
				if (t < s_spanLo) t = s_spanLo;
			}

			// Read the clock BEFORE seeking. That value is what the frame the
			// add-on has just blended was drawn under, so comparing it against
			// the target set last time says whether our seek was in place in
			// time for that blend.
			//
			// It is not a settle measurement and must not be read as one. The
			// clock lands the instant the seek returns; what lags is the IMAGE -
			// streaming, animation and the replay's own interpolation - and
			// nothing on this side can observe that. So a mismatch here means
			// "the seek was late", not "the frame was not ready". Warned once
			// rather than logged per sample: the failure is a clean double image
			// with nothing to attribute it to, and one line naming it is worth
			// more than several hundred nobody reads.
			if (s_havePrevTarget && !s_warnedLate)
			{
				const float atBlend = *(float*)game::addr_g_ReplayTimeMs;
				const float err = atBlend - s_prevTarget;
				if (err < -0.01f || err > 0.01f)
				{
					s_warnedLate = true;
					logger::write("info",
						"dof: the frame blended at t=%.2f but %.2f was asked for (%+.2fms) - "
						"steps are being serviced after the blend. Raise Frames To Wait in "
						"the add-on's depth-of-field panel if the result shows doubled images",
						atBlend, s_prevTarget, err);
				}
			}
			s_prevTarget = t;
			s_havePrevTarget = true;

			game::jumpProjectTo(t, 0);
		}

		// The step has been serviced. Publishing this is what lets the add-on
		// stop guessing at our latency: it holds its frame-wait until the
		// sequence it asked for comes back, however long this thread took to get
		// here. Released AFTER the seek so the ordering is unambiguous to the
		// reader.
		//
		// Set even when the clock was not touched. A move with no shutter is
		// still a move, and an add-on waiting on the query must not be left
		// waiting for one.
		s_pumpsSinceSeek.store(0, std::memory_order_relaxed);
		s_servicedSeq.store(moveSeq, std::memory_order_release);

		++s_sample;
	}

	void applyToFrame(void* self)
	{
		if (!s_running || !self) return;

		auto* b = (uint8_t*)self;
		const float* ax = (const float*)(b + rdirector::OFF_FrameMatrixA); // right
		const float* az = (const float*)(b + rdirector::OFF_FrameMatrixC); // up
		float* pos = (float*)(b + rdirector::OFF_FramePosition);

		// The aperture walk is a pure translation in the camera's own plane -
		// the same write the shake makes, from a different source. The add-on's
		// shader realigns the focal plane afterwards, so the camera is not
		// re-aimed here.
		for (int i = 0; i < 3; ++i)
			pos[i] += ax[i] * s_offX + az[i] * s_offY;

		if (s_fovOverride > 0.0f)
			*(float*)(b + rdirector::OFF_FrameFov) = s_fovOverride;
	}
}

// =============================================================================
//  The IGCS camera-tools interface
// =============================================================================
//  Four exports, matching the names the add-on looks up with GetProcAddress.
//  extern "C" keeps them unmangled; on x64 there is only one calling
//  convention, so the add-on's unadorned function pointers match.
//
//  Nothing here touches the game - see the threading note in dofsession.h.
// =============================================================================
extern "C" {

__declspec(dllexport) int IGCS_StartScreenshotSession(uint8_t type)
{
	return dofsession::requestStart(type);
}

__declspec(dllexport) void IGCS_MoveCameraMultishot(float stepLeftRight, float stepUpDown,
                                                    float fovDegrees, bool fromStartPosition)
{
	dofsession::requestMove(stepLeftRight, stepUpDown, fovDegrees, fromStartPosition);
}

// --- optional extensions -----------------------------------------------------
//
// Both are resolved by the add-on with GetProcAddress and are absent on every
// other camera tool, so an add-on that knows about them gets the better path and
// one that does not is unaffected. Neither may ever become load-bearing for a
// plain session.

__declspec(dllexport) void IGCS_MoveCameraMultishotTimed(float stepLeftRight, float stepUpDown,
                                                         float fovDegrees, bool fromStartPosition,
                                                         float timeOffsetMs)
{
	dofsession::requestMoveTimed(stepLeftRight, stepUpDown, fovDegrees,
	                             fromStartPosition, timeOffsetMs);
}

__declspec(dllexport) int IGCS_QuerySampleReady()
{
	return dofsession::sampleReady() ? 1 : 0;
}

__declspec(dllexport) void IGCS_MoveCameraPanorama(float stepAngle)
{
	dofsession::requestPanorama(stepAngle);
}

__declspec(dllexport) void IGCS_EndScreenshotSession()
{
	dofsession::requestEnd();
}

} // extern "C"
