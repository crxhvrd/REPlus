// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "replay/scene.h"
#include "game/signatures.h"

#include <cstring>

// =============================================================================
//  Overriding the clip's recorded time of day and weather
// =============================================================================
//  Both are recorded per frame and both are replayed, which is why setting the
//  clock or forcing weather from outside does not survive a single frame. The
//  replay path does have two override flags of its own, but they only say "do
//  not apply the recording" - something still has to supply a value, and that
//  means finding a clock setter and a weather setter on each build.
//
//  Substituting the packet is cheaper and stays inside the engine's own code
//  path. One hook, one address per build, and every derived piece of state -
//  cloud transition, wind, wetness, the snow VFX flags - is still computed by
//  the game's own weather code exactly as it would be for a real recording.
//
//  Weather: the extract only READS from its packet, so it is handed a patched
//  stack copy and the buffer is never touched. Nothing to restore, and turning
//  the option off is instantly back to the recording.
//
//  Time: the clock has no equivalent choke point - the play pass reads the
//  packet and sets the clock inline - so that one is patched in the buffer. The
//  frame's clock packet sits exactly 20 bytes in front of the weather packet
//  (the recorder emits GameTime, Clock, Weather with nothing between), which is
//  validated rather than assumed. The original three bytes go back at the START
//  of the next call: by then the play pass has consumed them, so the buffer
//  holds the recording again between frames and there is no bookkeeping to
//  grow. That ordering is also correct when the same frame is re-processed
//  while paused - restore, then re-save, then re-patch.
//
//  The preprocess pass DOES run before the play pass, and it matters enough to
//  have been traced rather than assumed: the frame update reaches the replay
//  preprocess (and with it this hook) well before the main update list reaches
//  the play pass. Reversed, the clock override would restore its packet before
//  anything read it and do nothing at all.
// =============================================================================
namespace scene
{
	namespace
	{
		using FnExtract = void(__fastcall*)(const void*);
		FnExtract origExtract = nullptr;
		bool s_installed = false;

		// The clock packet patched on the previous call, and what was in it.
		// Exactly one, because only one frame is ever in flight.
		unsigned char* s_clockPkt = nullptr;
		unsigned char  s_clockWas[3] = { 0, 0, 0 };

		// --- the timecycle's per-variable replay-override flags -------------
		// Cleared while an override is live so the freshly evaluated keyframe
		// survives; the originals are kept so they can be put straight back.
		// See the note on TIMECYCLE_VARINFOS in signatures.h for why this and
		// not the timecycle packet.
		unsigned char s_tcFlagWas[gsig::TCVARINFO_MAX];
		int  s_tcVarCount = -1;      // -1 = not walked yet, 0 = table unusable
		bool s_tcFlagsCleared = false;

		inline unsigned packetSize(const unsigned char* p)
		{
			unsigned word;
			memcpy(&word, p + gsig::PACKET_SIZE_OFF, sizeof(word));
			return word & 0xFFFFFF;
		}

		inline bool isPacket(const unsigned char* p, unsigned short id, int size)
		{
			unsigned short pid;
			unsigned guard;
			memcpy(&pid, p, sizeof(pid));
			memcpy(&guard, p + gsig::PACKET_GUARD_OFF, sizeof(guard));
			return pid == id
			    && guard == gsig::PACKET_GUARD_BASE
			    && packetSize(p) == (unsigned)size;
		}

		// The CPacketClock for the frame this weather packet belongs to, or null
		// if what is in front of the weather packet is not one. A miss is not an
		// error worth spamming about - an alignment packet can in principle land
		// between them - so it is logged once and then the time override simply
		// sits out that frame.
		unsigned char* clockPacketFor(unsigned char* weather)
		{
			unsigned char* clk = weather - gsig::PACKET_CLOCK_SIZE;
			if (isPacket(clk, gsig::PACKETID_CLOCK, gsig::PACKET_CLOCK_SIZE))
				return clk;

			static bool warned = false;
			if (!warned)
			{
				warned = true;
				logger::write("info",
					"scene: no CPacketClock in front of the weather packet at %p - "
					"time of day left as recorded", (void*)weather);
			}
			return nullptr;
		}

		// Undo the previous frame's clock patch.
		//
		// The packet is re-validated first, and that check is not paranoia: the
		// pointer is into the replay buffer, and loading another clip refills
		// those same blocks with different packets. Writing three bytes at an
		// offset that USED to be a clock packet would then land in the middle of
		// whatever now occupies it. Re-checking the id, size and guard costs
		// nothing and turns that into a no-op.
		void putBackClock()
		{
			if (!s_clockPkt) return;

			if (isPacket(s_clockPkt, gsig::PACKETID_CLOCK, gsig::PACKET_CLOCK_SIZE))
			{
				s_clockPkt[gsig::CLOCK_HOURS_OFF]   = s_clockWas[0];
				s_clockPkt[gsig::CLOCK_MINUTES_OFF] = s_clockWas[1];
				s_clockPkt[gsig::CLOCK_SECONDS_OFF] = s_clockWas[2];
			}
			s_clockPkt = nullptr;
		}

		// Count the table by walking while varId stays sequential. The stock
		// table is a little over 400 entries; the count is not a constant we
		// want to hardcode, and varId == index is the table's own invariant.
		int walkVarInfoTable()
		{
			if (!game::addr_g_TcVarInfos) return 0;

			auto* base = (const unsigned char*)game::addr_g_TcVarInfos;
			int n = 0;
			for (; n < gsig::TCVARINFO_MAX; ++n)
			{
				int varId;
				memcpy(&varId, base + n * gsig::TCVARINFO_STRIDE + gsig::TCVARINFO_VARID_OFF,
				       sizeof(varId));
				if (varId != n) break;
			}
			// A table that ends immediately, or runs to the cap, is not the one
			// we think it is. Refuse rather than write into it.
			if (n < 64 || n >= gsig::TCVARINFO_MAX)
			{
				logger::write("info",
					"scene: timecycle table walk gave %d entries - refusing to touch it, "
					"clip lighting stays as recorded", n);
				return 0;
			}
			logger::write("info", "scene: timecycle table has %d variables", n);
			return n;
		}

		// Clear or restore every replay-override flag. Edge-triggered by tick().
		void setReplayOverrideFlags(bool clear)
		{
			if (s_tcVarCount < 0) s_tcVarCount = walkVarInfoTable();
			if (s_tcVarCount <= 0) return;

			auto* base = (unsigned char*)game::addr_g_TcVarInfos;
			for (int i = 0; i < s_tcVarCount; ++i)
			{
				unsigned char* flag = base + i * gsig::TCVARINFO_STRIDE + gsig::TCVARINFO_REPLAY_OFF;
				if (clear)
				{
					s_tcFlagWas[i] = *flag;
					*flag = 0;
				}
				else
				{
					*flag = s_tcFlagWas[i];
				}
			}
			s_tcFlagsCleared = clear;
		}

		void __fastcall hkExtract(const void* packet)
		{
			// Always first, whatever the options now say: if the user turned an
			// override off between frames the buffer still has to be put back,
			// and doing it here means that costs nothing extra.
			putBackClock();

			auto* pkt = (unsigned char*)packet;
			const Config& cfg = Config::get();

			// Master switch. On As Recorded the clip plays exactly as shot -
			// see overridesActive().
			if (!overridesActive())
			{
				origExtract(packet);
				return;
			}

			// A packet that is not the shape we know is left completely alone.
			// This is the guard against a future build growing CPacketWeather:
			// the hook goes inert rather than writing into the wrong offsets.
			if (!pkt || !isPacket(pkt, gsig::PACKETID_WEATHER, gsig::PACKET_WEATHER_SIZE))
			{
				static bool warned = false;
				if (!warned && pkt)
				{
					warned = true;
					logger::write("info",
						"scene: weather packet at %p is not the expected 288-byte v2 "
						"layout (size %u) - overrides disabled", (void*)pkt, packetSize(pkt));
				}
				origExtract(packet);
				return;
			}

			if (cfg.overrideTimeOfDay)
			{
				if (unsigned char* clk = clockPacketFor(pkt))
				{
					s_clockWas[0] = clk[gsig::CLOCK_HOURS_OFF];
					s_clockWas[1] = clk[gsig::CLOCK_MINUTES_OFF];
					s_clockWas[2] = clk[gsig::CLOCK_SECONDS_OFF];
					s_clockPkt    = clk;

					const int mins = cfg.timeOfDay < 0 ? 0
					               : cfg.timeOfDay > 1439 ? 1439 : cfg.timeOfDay;
					clk[gsig::CLOCK_HOURS_OFF]   = (unsigned char)(mins / 60);
					clk[gsig::CLOCK_MINUTES_OFF] = (unsigned char)(mins % 60);
					clk[gsig::CLOCK_SECONDS_OFF] = 0;
				}
			}

			if (!cfg.overrideWeather)
			{
				origExtract(packet);
				return;
			}

			// 16-byte aligned because the packet is read with MOVUPS/MOVSD from
			// arbitrary offsets - unaligned is legal, but there is no reason to
			// hand the game a worse-aligned buffer than the one it expects.
			alignas(16) unsigned char copy[gsig::PACKET_WEATHER_SIZE];
			memcpy(copy, pkt, sizeof(copy));

			const int count = weatherTypeCount();
			int from = cfg.weatherType;
			if (from < 0) from = 0;
			if (from >= count) from = count - 1;

			// Blend To is optional. Off means "just this type", which is a blend
			// of the type with itself - the engine is perfectly happy with that
			// and it keeps one code path instead of two.
			int to = cfg.weatherBlendTo;
			if (to < 0 || to >= count) to = from;

			float interp = (to == from) ? 0.0f : cfg.weatherBlend;
			if (interp < 0.0f) interp = 0.0f;
			if (interp > 1.0f) interp = 1.0f;

			memcpy(copy + gsig::WEATHER_OLDTYPE_OFF, &from,   sizeof(from));
			memcpy(copy + gsig::WEATHER_NEWTYPE_OFF, &to,     sizeof(to));
			memcpy(copy + gsig::WEATHER_INTERP_OFF,  &interp, sizeof(interp));

			// Negative means "leave the recording's own wetness alone", which is
			// not the same as zero: a clip recorded in the rain has wet roads
			// that a dry preset should not necessarily scrub off.
			if (cfg.weatherWetness >= 0.0f)
			{
				float wet = cfg.weatherWetness > 1.0f ? 1.0f : cfg.weatherWetness;
				memcpy(copy + gsig::WEATHER_WETNESS_OFF, &wet, sizeof(wet));
			}

			origExtract(copy);
		}

		// weather.xml load order. The index in CPacketWeather is a position in
		// the game's weather type list, which is filled in file order, so this is
		// stock file's - a game with a modified weather.xml will be off by
		// whatever it added. Names are cosmetic; the index is what is written.
		const char* const kWeatherNames[] = {
			"EXTRASUNNY", "CLEAR", "CLOUDS", "SMOG", "FOGGY", "OVERCAST",
			"RAIN", "THUNDER", "CLEARING", "NEUTRAL", "SNOW", "BLIZZARD",
			"SNOWLIGHT", "XMAS", "HALLOWEEN",
		};
	}

	int weatherTypeCount()
	{
		return (int)(sizeof(kWeatherNames) / sizeof(kWeatherNames[0]));
	}

	const char* weatherTypeName(int index)
	{
		if (index < 0 || index >= weatherTypeCount()) return "?";
		return kWeatherNames[index];
	}

	bool ready() { return s_installed; }

	bool canRelight() { return game::addr_g_TcVarInfos != 0; }

	// Whether the Scene page does anything at all.
	//
	// Timecycle is the master switch, and it gates the BEHAVIOUR and not just
	// the menu: on As Recorded the clip's baked lighting stands, so moving the
	// clock would swing the sun across an unchanged sky and picking RAIN would
	// drop particles into sunshine. Applying the overrides anyway while the
	// rows sat greyed would be the same inconsistency with a lie on top.
	bool overridesActive()
	{
		return s_installed && canRelight() && Config::get().liveTimecycle;
	}

	void restore()
	{
		putBackClock();
		if (s_tcFlagsCleared) setReplayOverrideFlags(false);
	}

	// Called from a hook that runs in ordinary gameplay too, not just during
	// playback. That is deliberate and it is the whole reason this is a tick
	// rather than something done inside the Extract hook: that flag is
	// also read while RECORDING, so leaving it cleared after the editor closes
	// would make every clip recorded afterwards carry no lighting at all. The
	// common case is one compare.
	void tick()
	{
		const Config& cfg = Config::get();
		const bool want = overridesActive()
		               && (cfg.overrideTimeOfDay || cfg.overrideWeather)
		               && game::isEditModeActive();
		(void)cfg;

		if (want == s_tcFlagsCleared) return;
		setReplayOverrideFlags(want);
	}

	void install()
	{
		if (!game::addr_PacketWeatherExtract)
		{
			logger::write("info",
				"scene: CPacketWeather::Extract unresolved - clip time and weather "
				"stay as recorded");
			return;
		}

		s_installed = memory(game::addr_PacketWeatherExtract)
			.hook(hkExtract, &origExtract, "CPacketWeather::Extract");

		if (!s_installed) return;

		const Config& c = Config::get();
		logger::write("info", "scene: time=%s weather=%s relight=%s",
			c.overrideTimeOfDay ? "override" : "as recorded",
			c.overrideWeather   ? "override" : "as recorded",
			canRelight() ? (c.liveTimecycle ? "live" : "as recorded")
			             : "UNAVAILABLE (timecycle table unresolved)");
	}
}
