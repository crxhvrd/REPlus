// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdint>
#include <string>

// =============================================================================
//  Frame capture channel
// =============================================================================
//  An ASI cannot read the GPU back buffer; a ReShade addon can. Simple Camera
//  already solved this with a shared-memory channel to a modified
//  IgcsConnector.addon64: the ASI writes an output path and bumps a request
//  counter, the addon grabs the frame on its next present via ReShade's
//  backend-agnostic capture_screenshot(), writes the file and echoes the
//  counter back.
//
//  We speak that EXACT protocol - same mapping name, same struct, same version.
//  So the addon needs no changes and does not care which ASI is driving it.
//  The struct below must stay byte-identical to the copy in the addon's
//  Main.cpp and in Simple Camera's fx_capture.h; if that layout ever changes,
//  all three move together.
//
//  Backend-agnostic matters beyond tidiness: capture_screenshot() works on DX12
//  as well, so this half of video export is already Enhanced-ready.
// =============================================================================

#pragma pack(push, 4)
struct FxCaptureBlock
{
	uint32_t magic;       // 'SCFX' (0x53434658) - set by the ASI once mapped
	uint32_t version;     // 6
	uint32_t requestId;   // ASI increments to request a capture
	uint32_t ackId;       // addon echoes requestId once handled
	uint32_t status;      // 0 = ok, 1 = capture failed, 2 = file write failed
	uint32_t width;       // addon writes the captured dimensions
	uint32_t height;
	uint32_t sampleCount; // motion-blur samples for this output frame (1 = none)
	uint32_t sampleIndex; // which sample, 0..sampleCount-1 (addon accumulates;
	                      // resets on 0, averages + writes on sampleCount-1)
	uint32_t quality;        // JPEG quality 1..100 (ignored for PNG)
	float highlightBoost;    // 0..~1 - extra highlight lift in linear accumulation
	uint32_t addonHeartbeat; // addon bumps this every present; 0 = addon not loaded
	char outPath[512];       // ASI writes the full destination path; the addon
	                         // picks PNG vs JPEG from the .png / .jpg extension
	uint32_t channelOrder;   // 0 = Auto (addon detects the back-buffer format),
	                         // 1 = force RGBA (no swap), 2 = force BGRA (swap R/B)
};
#pragma pack(pop)

namespace fxcapture
{
	// Map (or attach to) the shared block. Safe to call more than once.
	void init();

	bool available();      // channel mapped
	bool addonPresent();   // addon is alive - it bumps a heartbeat every present

	// Which ReShade, if any, is in this process.
	//
	// The renderer reads the back buffer through a ReShade ADD-ON, so the
	// ordinary ReShade build cannot drive it at all - and that is the single
	// most common reason Export falls back to the game's own encoder. Decided by
	// the same test ReShade's own add-on header uses to find its host: a loaded
	// module exporting ReShadeRegisterAddon.
	//
	// Reported at startup, and read by the export menu so the requirement can be
	// stated on screen with the CURRENT answer next to it rather than as a line
	// in a readme nobody has open.
	enum HostState
	{
		HOST_NONE = 0,   // no ReShade in the process at all
		HOST_NO_ADDONS,  // ReShade, but the build that cannot load add-ons
		HOST_ADDONS,     // ReShade with full add-on support
	};
	HostState hostState();
	bool lastDone();       // addon has acknowledged the most recent request

	// The addon's per-present counter. The render loop uses this as its frame
	// clock: we have no ScriptHookV and therefore no WAIT(0), so "a frame went
	// out" is exactly "this number changed".
	uint32_t heartbeat();

	// Request one motion-blur sub-sample. The addon resets its accumulator on
	// sampleIndex 0, adds each sample, and on the last one averages and writes.
	// sampleCount of 1 is a plain single-frame capture.
	bool requestSample(const char* fullPath, int sampleCount, int sampleIndex);

	void setQuality(int quality);         // JPEG only, 1..100
	void setHighlightBoost(float boost);  // 0..~1
	void setChannelOrder(int order);      // 0 auto / 1 RGBA / 2 BGRA

	// Create a fresh auto-numbered output folder for a sequence render.
	// `base` may be null or empty, in which case a single
	// RockstarEditorPlus_Captures folder beside GTA5.exe is used. Either way the
	// render itself gets a numbered subfolder inside it.
	bool newSequenceFolder(const char* base, char* outFolder, int cap);

	// Where renders WILL go, without creating anything. Same resolution
	// newSequenceFolder uses, minus the numbered subfolder - so it can be logged
	// at startup, before any render exists. Under FiveM this is beside the .asi
	// in plugins\, which is not where people look first.
	std::string captureBaseDir(const char* base);
}
