// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once

// =============================================================================
//  RockstarEditorPlus — game signatures
// =============================================================================
//  Patterns are IDA-style, matched by memory::scan() against the LIVE,
//  DECRYPTED module in-process (both exes are packed at rest — these were
//  derived from a pe-sieve dump of the running process, analysed in Ghidra).
//
//  Legacy (GTA5.exe) is fully mapped. Enhanced patterns are EMPTY: that build
//  has not been analysed yet, so the ASI resolves nothing and stays dormant
//  there rather than hooking something it hasn't verified.
//
//  NOTE: namespace is 'gsig' (not 'sig') — 'sig' collides with a member in
//  memory.h's func<> template.
// =============================================================================
namespace gsig
{
	struct Sig { const char* enh; const char* leg; };

	// A value derived from inside an already-scanned function rather than
	// scanned itself: small shared helpers and globals whose own prologues are
	// too generic to pattern uniquely. Each is opcode-guarded in game.cpp, so a
	// shifted build yields a clean 0 instead of a pointer built from noise.
	//   insn = offset of the instruction (its opcode is verified)
	//   disp = offset of that instruction's rel32 / disp32 operand
	//   extra = bytes that FOLLOW the disp32 inside the same instruction. Zero
	//           for the usual `mov reg,[rip+d]`; one for forms carrying a
	//           trailing imm8 such as `mov byte [rip+d],imm8` or
	//           `cmp dword [rip+d],imm8`, where the displacement is relative to
	//           the end of the instruction and the naive disp+4 lands short.
	struct Derive { int insn; int disp; const unsigned char* op; int opLen; int extra = 0; };

	// The same value in both builds, resolved from a different instruction in
	// each. MSVC and Clang pick different encodings and different orderings, so
	// almost nothing derived from inside a function ports across.
	struct DerivePair { Derive enh; Derive leg; };

	inline constexpr unsigned char OP_CALL[]   = { 0xE8 };             // call rel32
	inline constexpr unsigned char OP_MOV_ECX[]= { 0x8B, 0x0D };       // mov ecx,[rip+d]
	inline constexpr unsigned char OP_MOV_EAX[]= { 0x8B, 0x05 };       // mov eax,[rip+d]
	inline constexpr unsigned char OP_LEA_RCX[]= { 0x48, 0x8D, 0x0D }; // lea rcx,[rip+d]
	inline constexpr unsigned char OP_MOVB_IMM[]= { 0xC6, 0x05 };      // mov byte [rip+d],imm8
	inline constexpr unsigned char OP_MOVB_BL[] = { 0x88, 0x1D };      // mov byte [rip+d],bl
	inline constexpr unsigned char OP_MOV_MEM_ECX[] = { 0x89, 0x0D };  // mov [rip+d],ecx
	inline constexpr unsigned char OP_CMPB_IMM[]= { 0x80, 0x3D };      // cmp byte [rip+d],imm8
	inline constexpr unsigned char OP_MOVZX_EAX[]={ 0x0F, 0xB6, 0x05 };// movzx eax,byte [rip+d]
	inline constexpr unsigned char OP_MOV_RDI_MEM[]={ 0x48, 0x8B, 0x3D };// mov rdi,[rip+d]
	inline constexpr unsigned char OP_MOV_R15_MEM[]={ 0x4C, 0x8B, 0x3D };// mov r15,[rip+d]
	inline constexpr unsigned char OP_CMPD_IMM[]= { 0x83, 0x3D };      // cmp dword [rip+d],imm8
	inline constexpr unsigned char OP_LEA_RAX[]= { 0x48, 0x8D, 0x05 }; // lea rax,[rip+d]
	inline constexpr unsigned char OP_MOV_RCX_MEM[]={ 0x48, 0x8B, 0x0D };// mov rcx,[rip+d]
	inline constexpr unsigned char OP_MOV_RSI_MEM[]={ 0x48, 0x8B, 0x35 };// mov rsi,[rip+d]
	inline constexpr unsigned char OP_MOV_EDI[]= { 0x8B, 0x3D };       // mov edi,[rip+d]
	inline constexpr unsigned char OP_MOV_EDX[]= { 0x8B, 0x15 };       // mov edx,[rip+d]


	// -------------------------------------------------------------------------
	// camReplayDirector::UpdateSmoothing(this)
	//   leg RVA 0x2A19F8
	//
	// The whole target. Runs once per replay frame and is the LAST thing to
	// touch the camera frame, so owning it owns the output. Prologue is free of
	// RIP-relative operands, so the only wildcard is the stack-frame size.
	//
	//   48 8B C4              mov  rax, rsp
	//   48 89 58 08           mov  [rax+08], rbx
	//   48 89 70 10           mov  [rax+10], rsi
	//   48 89 78 18           mov  [rax+18], rdi
	//   55 41 54 41 55 41 56 41 57   push rbp,r12,r13,r14,r15
	//   48 8D 6C 24 80        lea  rbp, [rsp-80]
	//   48 81 EC ?? ?? ?? ??  sub  rsp, 0x180        <- wildcarded
	//   0F 29 70 C8 ...       movaps spills
	//   48 8B D9              mov  rbx, rcx          <- this
	//   48 8B 89 C0 02 00 00  mov  rcx, [rcx+2C0]    <- this->m_ActiveCamera
	// -------------------------------------------------------------------------
	inline constexpr Sig REPLAYDIRECTOR_UPDATESMOOTHING = {
		// enh 0x23E6F0. The Clang prologue alone matches four functions; it only
		// becomes unique at the `mov rcx,[rcx+0x2C0]` active-camera load.
		"41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 68 01 00 00 "
		"44 0F 29 9C 24 50 01 00 00 44 0F 29 94 24 40 01 00 00 "
		"44 0F 29 8C 24 30 01 00 00 44 0F 29 84 24 20 01 00 00 "
		"0F 29 BC 24 10 01 00 00 0F 29 B4 24 00 01 00 00 "
		"49 89 CD 48 8B 89 C0 02 00 00 48 85 C9",
		"48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 "
		"48 8D 6C 24 80 48 81 EC ? ? ? ? 0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 "
		"48 8B D9 48 8B 89 C0 02 00 00"
	};

	// -------------------------------------------------------------------------
	// camReplayDirector::GetNextMarkerIndex(int startIdx)   leg RVA 0x243AE8
	//
	// Wanted for two reasons: we call it to walk to the next/next-next marker
	// (it already skips anchors, m_markerType==2, and applies the end-of-
	// timeline count-1 adjustment), AND its prologue loads g_ReplayMarkerStorage
	// into rdi via a RIP-relative mov, which is how we resolve that global.
	//
	//   ... 48 83 EC 20       sub  rsp, 20
	//   48 8B 3D ?? ?? ?? ??  mov  rdi, [rip+disp]   <- g_ReplayMarkerStorage
	//   44 8B F1              mov  r14d, ecx
	//   48 8B 07              mov  rax, [rdi]
	//   48 8B CF              mov  rcx, rdi
	//   FF 90 08 01 00 00     call [rax+108]         <- GetMarkerCount, vtbl 33
	// -------------------------------------------------------------------------
	inline constexpr Sig REPLAYDIRECTOR_GETNEXTMARKERINDEX = {
		// enh 0x235870 - same shape as Legacy: load the storage global, then the
		// GetMarkerCount vtable call at +0x108.
		"41 57 41 56 56 57 55 53 48 83 EC 28 89 CE 4C 8B 3D ? ? ? ? "
		"49 8B 07 4C 89 F9 FF 90 08 01 00 00",
		"48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 "
		"48 8B 3D ? ? ? ? 44 8B F1 48 8B 07 48 8B CF FF 90 08 01 00 00"
	};

	// -------------------------------------------------------------------------
	// camReplayDirector::GetMaxDistanceAllowedFromPlayer(bool considerEditMode)
	//   leg RVA 0x24399C
	//
	// THE single gate on how far the free camera may travel from the player.
	// Every distance check routes through it — IsPositionOutsidePlayerLimits,
	// SetToSafePosition's clamp, the EDIT_WARNING_OUT_OF_RANGE toast, and the
	// fallback-to-recorded-camera switch in the director. Override this one
	// return value and the whole limitation is gone.
	//
	//   83 3D ?? ?? ?? ?? 02      cmp [rip+IsEditModeActive], 2
	//   4C 8B 81 F0 02 00 00      mov r8, [rcx+2F0]        ; m_Metadata
	//   F3 41 0F 10 40 38         movss xmm0, [r8+38]      ; MaxCameraDistanceFromPlayer
	//   75 1D / 84 D2 / 74 19
	//   8B 81 30 06 00 00         mov eax, [rcx+630]       ; m_CurrentMarkerIndex
	//   83 F8 FF / 74 0E / 39 05  cmp eax,-1 ; cmp [rip+EditingMarkerIndex],eax
	//
	// Metadata offsets this confirmed: +0x38 m_MaxCameraDistanceFromPlayer,
	//                                  +0x3C m_MaxCameraDistanceReductionForEditMode
	// -------------------------------------------------------------------------
	// GetMaxDistanceAllowedFromPlayer opens with
	//     83 3D <disp32> 02      cmp dword ptr [rip+CReplayMgr::sm_uMode], 2
	// which gives us the replay-mode global for free (2 == REPLAYMODE_EDIT).
	//
	// CAREFUL: a RIP displacement is relative to the END of the instruction,
	// and this one has a trailing imm8. So the target is base+7+disp, NOT
	// base+6+disp - the usual "disp offset + 4" shortcut is wrong here and would
	// land us one byte short. game.cpp computes it explicitly for that reason.
	inline constexpr int  GMD_REPLAYMODE_DISP_OFF = 2;
	inline constexpr int  GMD_REPLAYMODE_INSN_LEN = 7;
	inline constexpr unsigned char GMD_REPLAYMODE_OPCODE[2] = { 0x83, 0x3D };
	inline constexpr int  REPLAYMODE_EDIT = 2;

	// The replay manager's mode enum, in order. EDIT is the editor proper;
	// LOADCLIP is the transient the engine drops into while it streams the next
	// clip of a multi-clip project, and it returns to EDIT on its own afterwards.
	// DISABLED is the only value that actually means the editor is gone.
	inline constexpr int  REPLAYMODE_DISABLED = 0;
	inline constexpr int  REPLAYMODE_RECORD   = 1;
	inline constexpr int  REPLAYMODE_LOADCLIP = 3;

	// Despite the name, this is the ordinary way INTO playback, not an error
	// state: CReplayMgrInternal::TriggerPlayback sets it as the desired mode and
	// the clip is committed through the savegame queue before the manager settles
	// to EDIT. A diverted export sits here for as long as that takes.
	//
	// It is named because a bare "saw 4" in a user's log is not diagnosable, and
	// mode 4 is the one a stalled export actually reports.
	inline constexpr int  REPLAYMODE_WAITINGFORSAVE = 4;

	inline constexpr const char* replayModeName(int m)
	{
		return m == REPLAYMODE_DISABLED        ? "DISABLED"
		     : m == REPLAYMODE_RECORD          ? "RECORD"
		     : m == REPLAYMODE_EDIT            ? "EDIT"
		     : m == REPLAYMODE_LOADCLIP        ? "LOADCLIP"
		     : m == REPLAYMODE_WAITINGFORSAVE  ? "WAITINGFORSAVE"
		     : "?";
	}

	inline constexpr Sig REPLAYDIRECTOR_GETMAXDISTANCE = {
		// enh 0x235EE0. Body is identical to Legacy line for line -
		//   f = [[dir+0x2F0]+0x38];
		//   if (g_ReplayMode==2 && arg2 && dir[0x630]!=-1 && g_X==dir[0x630])
		//       f -= [[dir+0x2F0]+0x3C];
		// but the CODEGEN is not: Legacy emits the `cmp g_ReplayMode,2` first and
		// uses r8, Enhanced loads rax/xmm0 first. So the two patterns share no
		// prefix and this one had to come from Enhanced's own bytes.
		//
		// Found via `subss xmm0,[rax+0x3c]` inside the camReplayDirector cluster -
		// that field pair is rare, where +0x2F0 alone is not. It also
		// independently re-confirms g_ReplayMode = 0x3E31ACC.
		//
		// Whole function is 0x32 bytes; the 14 fixed ones are unique binary-wide.
		"48 8B 81 F0 02 00 00 F3 0F 10 40 38 83 3D ? ? ? ? 02 75 1C 84 D2 74 18 "
		"8B 89 30 06 00 00 83 F9 FF 74 0D 39 0D ? ? ? ? 75 05 F3 0F 5C 40 3C C3",
		"83 3D ? ? ? ? 02 4C 8B 81 F0 02 00 00 F3 41 0F 10 40 38 75 1D 84 D2 74 19 "
		"8B 81 30 06 00 00 83 F8 FF 74 0E 39 05"
	};

	// -------------------------------------------------------------------------
	// CReplayMgrInternal::JumpTo(float timeMs, u32 jumpOptions)  leg 0x126500
	//
	// The seek primitive the whole editor transport bottoms out in:
	//   timeline click -> JumpToNonDilatedTimeMs -> CReplayMgr::JumpToFloat
	//                  -> CReplayMgrInternal::JumpTo
	// A static, so there is no controller instance to find. Returns true when a
	// jump was actually requested (it no-ops within 0.001ms of the current time).
	//
	// Identified by its jumpOptions decode - &1 FineScrubbing, &2 Force,
	// &4 FreezeRenderPhase, &8 LinearJump - and the literal
	// "CReplayMgrInternal::UnPauseRenderPhase" in its body.
	//
	//   48 89 5C 24 08 57 48 83 EC 40      prologue
	//   0F 29 74 24 30 / 0F 29 7C 24 20    xmm spills
	//   F3 0F 10 3D ?? ?? ?? ??            movss xmm7,[rip+g_ReplayTimeMs]
	//   8B DA                              mov ebx,edx     (jumpOptions)
	//   BF 02 00 00 00                     mov edi,2
	//
	// NOTE we call this rather than CReplayMgr::JumpToFloat, which additionally
	// stops and re-kicks precaching. Skipping that costs streaming prefetch on a
	// large seek (pop-in), not correctness.
	// -------------------------------------------------------------------------
	inline constexpr Sig REPLAYMGR_JUMPTO = {
		// enh 0x785800 - (float ms in xmm0, u32 opts in edx)
		"56 48 83 EC 30 0F 29 74 24 20 89 D6 0F 28 F0 C6 05 ? ? ? ? 00 "
		"C6 05 ? ? ? ? 00 C6 05 ? ? ? ? 01",
		"48 89 5C 24 08 57 48 83 EC 40 0F 29 74 24 30 0F 29 7C 24 20 "
		"F3 0F 10 3D ? ? ? ? 8B DA BF 02 00 00 00 0F 28 C8"
	};

	// CReplayMgr::JumpOptions
	inline constexpr unsigned JO_None          = 0;
	inline constexpr unsigned JO_FineScrubbing = 1 << 0;
	inline constexpr unsigned JO_Force         = 1 << 1;

	// -------------------------------------------------------------------------
	// CReplayMgrInternal::SetCursorSpeed(float speed)   leg 0x1A8234
	//
	// Playback rate. Negative plays backwards, 1.0 is normal; the 1.0 case takes
	// a separate branch that also resets the cursor to NORMAL.
	//
	// Identified by its constants: 0x200 REPLAY_CURSOR_SPEED, 0x100
	// REPLAY_CURSOR_NORMAL, 0x20001 PLAY|DIRECTION_BACK, 0x10001 PLAY|
	// DIRECTION_FWD - matching SetCursorSpeed's source line for line.
	//
	// We scan this rather than SetNextPlayBackState because the latter is a tiny
	// setter with no distinctive prologue; we derive it from the call below.
	// -------------------------------------------------------------------------
	// The Enhanced side was blank for a long time because this was DESCOPED, not
	// because it was hunted and missed: its only consumer was the ReShade
	// overlay, and that was deleted. It matters again now - the replay speed is
	// the only lever for controlling how much scene time a ReShade
	// depth-of-field session accumulates over.
	//
	// enh 0x6A4110, a branch-for-branch match for Legacy's. Clang reordered the
	// == 1.0 case to the top and made the compares NaN-aware, but every constant
	// is the same (0x10001 / 0x20001 / 0x100 / 0x200), it makes the same
	// [0,1] slow-motion setup call, and it writes the same two globals.
	//
	// USEFUL FINDING: there is no clamping anywhere in it. The float is written
	// straight through to the speed global, so an arbitrary rate - 0.007 - is
	// reachable, and the FUNCTION is not even needed if the global is known:
	//     speed global   enh 0x289ABBC   leg 0x1D1A79C
	// which sits a few bytes from ms_lastEditWarning (enh 0x289ABF0), i.e. the
	// same CReplayMgr static block, and is a cheap cross-check that a future
	// re-derive landed in the right place.
	//
	// game.cpp logs the resolved RVA. Expect 0x6A4110; anything else means the
	// pattern matched something it should not have.
	inline constexpr Sig REPLAYMGR_SETCURSORSPEED = {
		"48 83 EC 48 0F 29 7C 24 30 0F 29 74 24 20 0F 2E 05 ? ? ? ? 75 02 7B ? "
		"0F 28 F0 0F 57 FF 0F 2E C7",
		"48 83 EC 38 F3 0F 10 0D ? ? ? ? 0F 29 74 24 20 0F 28 F0 0F 2E F1 74 4D "
		"0F 2F 35 ? ? ? ? 72 0A 0F 2F F1 77 05 E8 ? ? ? ? B9 00 02 00 00"
	};

	// CALL CReplayMgrInternal::SetNextPlayBackState(u32), inside SetCursorSpeed.
	//
	// A PAIR, because the call sits at a different offset in each build - Clang
	// hoisted the == 1.0 case to the top, which moves everything after it. This
	// was a plain Derive while only Legacy resolved SetCursorSpeed; reusing the
	// Legacy offset on Enhanced would have read whatever happens to sit at +0x38
	// and handed a garbage pointer to something we then CALL. Derives do not
	// port for free, and this one is the reason to say so twice.
	//
	//   enh  +0x42  E8 <disp32>   (after `mov ecx,0x200` at +0x3D)
	//   leg  +0x38  E8 <disp32>
	inline constexpr DerivePair SCS_SETNEXTPLAYBACKSTATE = {
		{ 0x42, 0x43, OP_CALL, 1, 0 },   // enh
		{ 0x38, 0x39, OP_CALL, 1, 0 },   // leg
	};

	// CReplayState bits, read off the transport call sites.
	inline constexpr unsigned RS_PLAY      = 1 << 0;
	inline constexpr unsigned RS_PAUSE     = 1 << 3;
	inline constexpr unsigned RS_DIR_FWD   = 1 << 16;
	inline constexpr unsigned RS_DIR_BACK  = 1 << 17;
	inline constexpr unsigned RS_PLAY_FWD  = RS_PLAY | RS_DIR_FWD;   // 0x10001

	// -------------------------------------------------------------------------
	// netProfanityFilter::GetStatusForRequest(const CheckTextToken&)
	//   leg RVA 0x172940C
	//
	// The editor polls this while naming a project or an export. It is a Social
	// Club round-trip, so on an offline or downgraded build it never resolves
	// and comes back RESULT_ERROR (-1) or RESULT_INVALID_TOKEN (3). The editor
	// turns either straight into SetErrorState("PROFANITY_FILT_UNAVAIL_EXP")
	// and aborts - which is why rendering is impossible on those builds.
	//
	// Returning RESULT_STRING_OK (0) is the same answer the check produces when
	// it succeeds, so nothing downstream has to be taught a new case - the
	// editor simply proceeds the way it does for any name that passed.
	//
	// We do NOT pattern the function itself: it is a three-instruction thunk
	// (mov r8,rdx / mov rdx,rcx / lea rcx,[mgr] / jmp) and that shape occurs
	// eight times in the binary, so a scan would silently hook the wrong one.
	// Instead we scan the editor's poll site, which is unmistakable, and take
	// the call target from it:
	//
	//   48 8D 0D ?? ?? ?? ??   lea  rcx,[ms_profanityToken]
	//   33 D2                  xor  edx,edx
	//   E8 ?? ?? ?? ??         call GetStatusForRequest      <- +0x09
	//   48 83 CB FF            or   rbx,-1                   ; RESULT_ERROR
	//   3B C3                  cmp  eax,ebx
	//   0F 84 ?? ?? ?? ??      jz   -> unavailable
	//   41 3B C4               cmp  eax,r12d                 ; INVALID_TOKEN(3)
	//   0F 84 ?? ?? ?? ??      jz   -> unavailable
	//   41 3B C5               cmp  eax,r13d                 ; STRING_FAILED(1)
	// -------------------------------------------------------------------------
	inline constexpr Sig PROFANITY_POLLSITE = {
		"",
		"48 8D 0D ? ? ? ? 33 D2 E8 ? ? ? ? 48 83 CB FF 3B C3 0F 84 ? ? ? ? "
		"41 3B C4 0F 84 ? ? ? ? 41 3B C5"
	};

	// CALL netProfanityFilter::GetStatusForRequest, inside the poll site.
	inline constexpr Derive PPS_GETSTATUS = { 0x09, 0x0A, OP_CALL, 1 };

	inline constexpr int PROFANITY_RESULT_STRING_OK = 0;

	// -------------------------------------------------------------------------
	// camReplayFreeCamera::UpdateCollision(initialPos, cameraPos)  leg 0x28114D4
	//
	// The free camera's world-collision sweep. Takes the desired position in
	// cameraPos, sweeps a capsule to it, and writes back the constrained
	// result. Returning 0 WITHOUT touching cameraPos leaves the desired
	// position intact, which is exactly "no collision".
	//
	// Confirmed by the GTA_MAP_TYPE_MOVER|GLASS|DEEP_SURFACE include mask
	// (0x84000004), the < 1e-12 no-movement early-out, and the second test at
	// radius-0.022 (= m_CapsuleRadius - MinRadiusReduction*2).
	// Also pinned: camReplayFreeCamera::m_Metadata at +0x250,
	//              m_CapsuleRadius at metadata+0x178.
	// -------------------------------------------------------------------------
	inline constexpr Sig FREECAM_UPDATECOLLISION = {
		// enh 0x23E7A0. Confirmed by five independent things, not just shape:
		//   - signature (self, float* initialPos, float* cameraPos), as Legacy
		//   - 0x1308 stack frame (Legacy 0x12E0) via __chkstk
		//   - copies param_2 into param_3, world-probes, then writes the
		//     constrained position back into param_3 and returns 0/1
		//   - early-outs to 0 when |desired - initial|^2 < 1e-12
		//   - it is a callee of the free-cam update (0x2397C0), matching
		//     Legacy where UpdateCollision has exactly one caller
		//
		// Found by decompiling the free-cam update's camera-cluster callees; the
		// update itself was located because it still shows an inlined
		// GetMaxDistanceAllowedFromPlayer (`subss xmm0,[rax+0x3c]`), which is why
		// it does NOT appear in that function's caller list on Enhanced.
		//
		// The prologue is NOT unique on its own - 0x174EDA0 has the identical
		// push set and the same 0x1308 frame. They diverge at the first movaps:
		// ours saves XMM15/XMM14 (44 0F 29 BC / B4), the other XMM9/XMM8
		// (44 0F 29 8C / 84). Do not trim the pattern back before those.
		"41 57 41 56 41 55 41 54 56 57 55 53 B8 08 13 00 00 E8 ? ? ? ? 48 29 C4 "
		"44 0F 29 BC 24 F0 12 00 00 44 0F 29 B4 24 E0 12 00 00",
		"48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 EE FF FF "
		"B8 E0 12 00 00 E8 ? ? ? ? 48 2B E0 8B 02"
	};

	// -------------------------------------------------------------------------
	// camReplayFreeCamera::ComputeSafePosition(entity, cameraPos)  leg 0x223774
	//
	// The attach/look-at entity capsule test reached from SetToSafePosition —
	// this is what shoves the camera off a vehicle or ped it is anchored to.
	// Same treatment: return 0, leave the position alone.
	// -------------------------------------------------------------------------
	inline constexpr Sig FREECAM_COMPUTESAFEPOSITION = {
		// enh 0x237190. Same 0x19C0 __chkstk frame as Legacy's, which is a
		// useful cross-build confirmation, and the 15-byte prefix is unique.
		//
		// Found via the WARNING GLOBAL, after two earlier structural hunts
		// picked the wrong function. CReplayMarkerContext::SetWarning is
		// `inline { ms_lastEditWarning = warning; }`, so every warning site is
		// just a store of an immediate - and those stores SURVIVE INLINING even
		// where the enclosing function does not. That is what made this findable
		// at all: Enhanced inlined camReplayFreeCamera::SetToSafePosition, and
		// GetMaxDistanceAllowedFromPlayer inside it.
		//
		// Route: "VEUI_WARN_OO_RANGE" -> sc_warningTextKeys (0x260D9C0) ->
		// GetWarningLngKey (0x7D27D0) -> its one caller -> ms_lastEditWarning
		// (0x289ABF0) -> writers. The writer with TWO stores is
		// SetToSafePosition (0x236EA0), matching the observed order exactly: clamp then
		// `warning = 1`, then the attach test then `warning = 2 | blendFlag`
		// (the compiler folds COLLISION and BLEND_COLLISION into one write).
		// ComputeSafePosition is the call between them.
		//
		// Generalisable: when a function is inlined away, look for a global it
		// WRITES rather than a function it calls.
		"41 57 41 56 41 54 56 57 55 53 B8 C0 19 00 00 E8 ? ? ? ? 48 29 C4 "
		"44 0F 29 94 24 B0 19 00 00",
		"48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 40 E7 FF FF "
		"B8 C0 19 00 00 E8 ? ? ? ? 48 2B E0"
	};

	// -------------------------------------------------------------------------
	// CVideoEditorPlayback::PopulateCameraMenu(u32 focusToRestore)  leg 0x17202C
	//
	// Builds the marker camera menu column, one ADD_COLUMN_ITEM_WITH_OPTIONS at
	// a time, then finishes with ADD_COLUMN_HELP_TEXT. We inject our own rows
	// just before that help-text call.
	//
	// It is also the redraw path: the game itself re-calls this after a blend
	// change, which is why we can refresh our rows by simply calling it rather
	// than needing UpdateItemTextValue.
	// -------------------------------------------------------------------------
	inline constexpr Sig PLAYBACK_POPULATECAMERAMENU = {
		// enh 0x678130. Pinned by STRING anchor, not by field offsets: this is the
		// only function referencing "VEUI_EDIT_CAM_LOOKAT" / "_ATTACH" / "_BLEND"
		// / "_SHAKE" / "_MOUNTTYPE", which is exactly Legacy's row set.
		//
		// A previous pattern here matched 0x67AA80 and was WRONG - that is
		// PopulateEditMarkerMenu (VEUI_ED_MK_SPEED / _TIME / _ANCHOR /
		// VEUI_CAM_OPT / _DOF_OPT / _PFX_OPT / _AUD_OPT), the parent menu whose
		// "Camera" row opens this one. The two are emitted back to back
		// (0x678130 ends at 0x67A6F5, 0x67AA80 starts just after) and share the
		// same 12-byte push prologue, which is how they got swapped. Identify
		// menu-populate functions by their VEUI_ strings, never by prologue or by
		// marker-field offsets.
		//
		// Head decodes as: push r15..rbx / sub rsp,0x78 /
		// cmp dword[g_MenuRestoreFocus],-1 / lea rax,[g_MenuFocusIndex] /
		// lea rdx,[+4] / cmove / mov r14d,ecx. focusToRestore arrives in ECX,
		// same as Legacy. The 18-byte fixed prefix is already unique binary-wide.
		"41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 83 3D ? ? ? ? FF "
		"48 8D 05 ? ? ? ? 48 8D 15 ? ? ? ? 48 0F 44 D0 41 89 CE",
		"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 "
		"48 8B EC 48 83 EC 50 8B 05 ? ? ? ? 41 83 CC FF 8B F1 44 39 25"
	};

	// CALL CVideoEditorPlayback::GetCurrentEditMarker()
	inline constexpr Derive PCM_GETMARKER   = { 0x49,  0x4A,  OP_CALL,    1 };
	// MOV ECX,[CVideoEditorUi::ms_movieId]
	inline constexpr Derive PCM_MOVIEID     = { 0x91,  0x93,  OP_MOV_ECX, 2 };
	// CALL CScaleformMgr::BeginMethod(movie, cls, name, a, b)
	inline constexpr Derive PCM_BEGINMETHOD = { 0xA9,  0xAA,  OP_CALL,    1 };
	// CALL CScaleformMgr::AddParamString(str, bool)
	inline constexpr Derive PCM_ADDPARAMSTR = { 0xC6,  0xC7,  OP_CALL,    1 };
	// CALL CScaleformMgr::EndMethod(bool)
	inline constexpr Derive PCM_ENDMETHOD   = { 0xDB,  0xDC,  OP_CALL,    1 };
	// LEA RCX,[ms_activeMenuOptions]  — atArray{ T* data; u16 count; u16 cap }
	inline constexpr Derive PCM_MENUOPTIONS = { 0x186, 0x189, OP_LEA_RCX, 3 };
	// CALL atArray<MenuOption>::Grow(int) -> MenuOption*
	inline constexpr Derive PCM_GROW        = { 0x197, 0x198, OP_CALL,    1 };

	// -------------------------------------------------------------------------
	// Enhanced interior offsets of PopulateCameraMenu (0x678130).
	//
	// These are ENHANCED-ONLY and must never be run against the Legacy body -
	// the two functions share nothing structurally. Read off the disassembled
	// prologue, which is:
	//
	//   +0x00  push r15..rbx / sub rsp,0x78
	//   +0x10  cmp  dword [g_MenuRestoreFocus], -1     83 3D d32 imm8   (extra 1)
	//   +0x17  lea  rax, [g_MenuFocusIndex]            48 8D 05 d32
	//   +0x1E  lea  rdx, [g_MenuFocusIndex+4]
	//   +0x42  mov  rcx, [g_MenuOptions]               48 8B 0D d32
	//   +0x4E  call free(rcx)                          E8 d32
	//
	// PCM_FREE deliberately comes from here rather than from a standalone
	// signature: it is then guaranteed to be the counterpart of whatever
	// allocated the option array we are about to grow.
	// -------------------------------------------------------------------------
	//   +0x6D  mov  rsi, [g_EditClipController]        48 8B 35 d32
	//   +0x7D  mov  edi, [g_EditClipIndex]            8B 3D d32
	//   +0xBF  mov  edx, [g_EditMarkerIndex]          8B 15 d32
	//   +0x10A mov  ecx, [g_MovieId]                  8B 0D d32
	//   +0x135 call BeginMethod                       E8 d32
	//   +0x15A call AddParamString                    E8 d32
	//   +0x1DF call EndMethod                         E8 d32
	//   +0x1FA call ScaleformRelease                  E8 d32
	//
	// +0x6D..+0xBF is the inlined GetCurrentEditMarker (with GetCurrentClip
	// inlined inside it), which is why Enhanced has no function to resolve for
	// it and menu.cpp walks the clip array by hand instead.
	inline constexpr Derive PCM_E_RESTOREFOCUS = { 0x10,  0x12,  OP_CMPD_IMM,    2, 1 };
	inline constexpr Derive PCM_E_FOCUSINDEX   = { 0x17,  0x1A,  OP_LEA_RAX,     3 };
	inline constexpr Derive PCM_E_MENUOPTIONS  = { 0x42,  0x45,  OP_MOV_RCX_MEM, 3 };
	inline constexpr Derive PCM_E_FREE         = { 0x4E,  0x4F,  OP_CALL,        1 };
	inline constexpr Derive PCM_E_CLIPCTRL     = { 0x6D,  0x70,  OP_MOV_RSI_MEM, 3 };
	inline constexpr Derive PCM_E_CLIPINDEX    = { 0x7D,  0x7F,  OP_MOV_EDI,     2 };
	inline constexpr Derive PCM_E_EDITMARKERIDX= { 0xBF,  0xC1,  OP_MOV_EDX,     2 };
	inline constexpr Derive PCM_E_MOVIEID      = { 0x10A, 0x10C, OP_MOV_ECX,     2 };
	inline constexpr Derive PCM_E_BEGINMETHOD  = { 0x135, 0x136, OP_CALL,        1 };
	inline constexpr Derive PCM_E_ADDPARAMSTR  = { 0x15A, 0x15B, OP_CALL,        1 };
	inline constexpr Derive PCM_E_ENDMETHOD    = { 0x1DF, 0x1E0, OP_CALL,        1 };
	inline constexpr Derive PCM_E_SFRELEASE    = { 0x1FA, 0x1FB, OP_CALL,        1 };

	// -------------------------------------------------------------------------
	// CVideoEditorPlayback::PopulateEditMarkerMenu()   leg 0x1709AC  enh 0x67AA80
	//
	// The TOP-LEVEL marker menu - the one with Camera / Depth of Field / Effects
	// / Audio / Speed / Position / Set Thumbnail / Bookmark. Distinct from
	// PopulateCameraMenu, which is the Camera submenu one level down.
	//
	// This is where the GLOBAL rows go (camera collision, distance leash, spline
	// settings): they are not per-keyframe, so the per-marker camera submenu is
	// the wrong home for them.
	//
	// Both were pinned by string anchor, the same way PopulateCameraMenu was:
	// the sole referencer of "VEUI_CAM_OPT" in each build. The Legacy id is
	// corroborated by it appearing in MenuInput's callee list.
	//
	// NOTE the Enhanced pattern below is the one that used to sit on
	// PLAYBACK_POPULATECAMERAMENU by mistake - 0x67AA80 really is this function.
	// See the 2026-07-27 correction in ENHANCED_PORT.md before touching either.
	// -------------------------------------------------------------------------
	inline constexpr Sig PLAYBACK_POPULATEMARKERMENU = {
		"41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 88 00 00 00 41 89 CD "
		"48 8B 3D ? ? ? ? 31 C0 48 89 44 24 58 48 85 FF",
		"48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 "
		"8B F9 E8"
	};

	// -------------------------------------------------------------------------
	// CVideoEditorPlayback::UpdateItemTextValue(s32 index, const char* text)
	//   leg 0x1CA4FC   enh 0x684DD0
	//
	// Writes text into an existing menu row. We use it for our rows' help line:
	// the stock help array is indexed by option id and ours cannot address it,
	// so we drive the help row directly. Optional on both builds - without it
	// the rows just carry no help text.
	//
	// SIGNED rather than derived out of MenuInput (which is how Legacy used to
	// get it) so it does not ride on MenuInput being correctly identified. That
	// identification is still unverified on Enhanced, and this is the only thing
	// the menu needed from it.
	//
	// Same shape on both builds: BeginMethod("UPDATE_LIST_ITEM_ELEMENT"), three
	// int params, the string, EndMethod. Found as the sole referencer of that
	// string in each build.
	//
	// The Legacy prologue alone is NOT unique - 0x15E938 is a sibling Scaleform
	// wrapper with an identical frame, same `or r9d,-1`, same `lea edx,[r9+0xa]`.
	// The two diverge immediately after the movie-id load: ours is
	// `mov rbx,rdx` (48 8B DA), the sibling's is `mov ebx,edx` (8B DA). That is
	// what makes this unique - do not trim the pattern back past it.
	// -------------------------------------------------------------------------
	inline constexpr Sig PLAYBACK_UPDATEITEMTEXT = {
		// enh: the 16-byte prologue is already unique binary-wide.
		"41 56 56 57 53 48 83 EC 48 49 89 D6 89 CF 8B 0D",
		"48 89 5C 24 08 57 48 83 EC 30 41 83 C9 FF 8B F9 8B 0D ? ? ? ? "
		"48 8B DA 4C 8D 05"
	};

	// -------------------------------------------------------------------------
	// CVideoEditorPlayback::UpdateMenuHelpText(s32 menuIndex)
	//   leg 0x1CB6EC   enh 0x671CA0
	//
	// THE help-text choke point. Every route that changes which row has focus
	// ends here - the tail of each Populate*, the focus-change block in the input
	// dispatcher, and the mouse hover path - so hooking it is what lets our rows
	// carry a real help line instead of the blank one they get today.
	//
	// What it does, read off both decompiles:
	//
	//   opt        = options.data[idx].m_option           (+0)
	//   restrict   = options.data[idx].m_editRestrictions (+4)
	//   if (restrict != 0)  key = s_restrictionStrings[ restrict <= 7 ? restrict : "" ]
	//   else                key = <per-option special cases>
	//                             ?: sc_EditMarkerMenuHelpText[ opt ]
	//   if (key) UPDATE_COLUMN_HELP_TEXT( TheText.Get(key) )
	//
	// Two things that pin our own design and were previously only inferred:
	//
	//   * `sc_EditMarkerMenuHelpText[opt]` is indexed with NO UPPER BOUND on
	//     EITHER build (leg `mov rdx,[base+rax*8+0x19edb10]` after a bare
	//     movsxd; enh `lea rcx,[helpArray]` / `mov rdx,[rcx+rax*8]`). So an
	//     option id >= MARKER_MENU_OPTION_MAX reads past the array and hands a
	//     garbage pointer to TheText::Get. OURS_OPTION_ID staying inside the
	//     enum is a hard requirement, not tidiness - see the note there.
	//   * the RESTRICTION path is bounded, at 7, on both builds. That is
	//     EDIT_RESTRICTION_MAX - 1, which is what makes writing a restriction
	//     into our own option entries safe.
	//
	// The final call sequence is the ordinary BeginMethod / AddParamString /
	// EndMethod trio we already resolve, and it takes a LOCALIZED STRING rather
	// than a text key - so our literals go straight in with nothing added to the
	// game's string table.
	//
	// Both patterns verified unique in their own image. Legacy's tail carries
	// `mov ecx, MARKER_MENU_OPTION_MAX` (B9 28 00 00 00), the null-option
	// fallback, which is the fingerprint; Enhanced inlines TryGetMenuOption
	// entirely and is pinned by the count/focus-index pair plus the `setl dl`.
	// -------------------------------------------------------------------------
	inline constexpr Sig PLAYBACK_UPDATEMENUHELPTEXT = {
		"41 57 41 56 41 54 56 57 55 53 48 83 EC 40 0F B7 05 ? ? ? ? "
		"39 05 ? ? ? ? 0F 9C C2 85 C0 0F 84 ? ? ? ? 84 D2 0F 84 ? ? ? ? 31 DB 39 C8",
		"40 53 48 83 EC 30 0F B7 05 ? ? ? ? 85 C0 0F 8E ? ? ? ? 39 05 ? ? ? ? "
		"0F 8D ? ? ? ? E8 ? ? ? ? 48 85 C0 74 04 8B 08 EB 05 B9 28 00 00 00"
	};

	// -------------------------------------------------------------------------
	// The active PROJECT, and how to get its name.
	//
	// Per-marker settings live in a side-car keyed by marker time, and until now
	// there was one file for everything - so a marker at 2000ms in one project
	// picked up whatever a marker at 2000ms in another had been given. The fix
	// needs a project identity, and this is it.
	//
	//   project  = *(void**)g_Project
	//   montage  = *(void**)(project + 0x320)
	//   name     = (const char*)(montage + 0x70)
	//
	// +0x320 is read straight out of the disassembly. The routine that opens a
	// playback tests exactly that slot for null to decide whether a project is
	// loaded, and the pointer sitting there is the same one menu.cpp already
	// walks to reach the clip array.
	//
	// The head of the montage is a run of atArrays - an 8-byte data pointer, a
	// u16 count, a u16 capacity, 4 bytes of padding, 16 bytes each. Two of them
	// are pinned by code we can read:
	//
	//   +0x00  data +0x00, count +0x08   the CLIPS - menu.cpp walks this today
	//   +0x20  data +0x20, count +0x28   read as a u16 count while validating
	//                                    music entries, 16-byte elements
	//
	// That fixes the stride. The name is at +0x70, and unlike the two above it
	// was found by TRYING it: game::projectName() reads the offset and checks
	// what comes back is printable and NUL-terminated, and the log then showed
	// the real project name. Between the arrays and the name is a stretch of
	// fixed-size scalars with no padding slack, which is why nothing else in
	// there is worth naming - it is not read.
	//
	// The offset stays DERIVED regardless, so the validation is not a formality:
	// a wrong guess costs a fallback rather than garbage in a filename. The
	// earlier 0x58 here was exactly that - it assumed an 8-byte array stride and
	// landed on a plausible-looking number, and the only reason it did no damage
	// is that the check refused it. If a game update moves the field, the
	// failure path dumps the head of the montage to the log so the new offset
	// can be read off directly.
	//
	// A new project is named the moment it is created, not when it is saved, so
	// this is never empty for an unsaved project.
	// -------------------------------------------------------------------------
	inline constexpr int PROJECT_MONTAGE_OFF = 0x320;
	inline constexpr int MONTAGE_NAME_OFF    = 0x70;
	inline constexpr int MONTAGE_NAME_MAX    = 256;  // sanity bound when reading

	// Legacy takes it out of UpdateMenuHelpText, which loads it for the
	// music/ambient help cases. Enhanced already resolves the same pointer as
	// g_EditClipController - see the note on that in game.h.
	inline constexpr unsigned char OP_MOV_RAX_MEM[] = { 0x48, 0x8B, 0x05 };
	inline constexpr Derive UMH_MSPROJECT_LEG = { 0x12A, 0x12D, OP_MOV_RAX_MEM, 3, 0 };

	// -------------------------------------------------------------------------
	// IReplayMarkerStorage::eEditRestrictionReason
	//
	// The middle dword of a MenuOption. 0 means the row is live; ANY other value
	// makes the game disable it, and it does so through three separate paths we
	// therefore get for free by writing one field:
	//
	//   UpdateEditMenuState()             greys the row (SET_ITEMS_GREYED_OUT),
	//                                     and runs at the END of the populate,
	//                                     i.e. after our injection
	//   SetCurrentItemIntoCorrectState()  draws it without left/right arrows
	//   UpdateFocusedInputEditMenu()      plays ERROR on accept, ignores toggles
	//
	// Confirmed in the retail decompile of PopulateCameraMenu, which writes
	// `IS_ENDPOINT` here for the Camera Type row on the last marker:
	//     *puVar7 = 0x11;  puVar7[1] = -(uint)bVar2 & 6;  puVar7[2] = -1;
	// -------------------------------------------------------------------------
	// The marker-storage virtual at vtable offset 0x160 (slot 44). Takes a
	// control id and returns one of the reasons above, 0 meaning "editable" -
	// and it is what locks a clip recorded in first person out of the free
	// camera.
	//
	// Found by disassembly, not guessed. The marker-menu populate carries five
	// `call qword ptr [rax+0x160]` sites on BOTH builds, and the one feeding the
	// camera row reads:
	//
	//     mov  rcx, [rsp+0x58]        ; the marker storage
	//     mov  rax, [rcx]             ; its vtable
	//     mov  edx, 9                 ; the control id
	//     call qword ptr [rax+0x160]
	//     test eax, eax               ; non-zero -> row disabled
	//
	// Walking the same populate for the other four call sites gives the rest of
	// the control ids; 8 and 9 are the two camera rows, 0 is the speed row.
	//
	// For the camera rows the answer is FIRST_PERSON, CUTSCENE or CAMERA_BLOCKED
	// depending on flags baked into the recording, so it is a property of the
	// clip and cannot be edited after the fact - the only way in is to stop the
	// answer being consulted.
	//
	// The restriction is purely a UI gate: no camera code reads it, so once the
	// menu lets a marker be set to a free camera the camera runs it with no
	// further objection. That was worth establishing before hooking anything -
	// a second gate in the camera itself would have made this pointless.
	inline constexpr int MARKERSTORAGE_VT_ISEDITABLE = 0x160;

	// Control ids, as passed in edx at the call sites above.
	inline constexpr int MARKER_CONTROL_SPEED       = 0;
	inline constexpr int MARKER_CONTROL_CAMERA      = 8;
	inline constexpr int MARKER_CONTROL_CAMERA_TYPE = 9;
	inline constexpr int MARKER_CONTROL_MAX         = 10;

	// -------------------------------------------------------------------------
	// The CURRENT CLIP'S recorded flags, and the second lock on the free camera.
	//
	// Unlocking the menu is not enough. Before the camera director looks at what
	// the marker asks for, it tests ONE bit of these flags and, if it is set,
	// returns the recorded camera regardless - so the marker says free camera and
	// the director hands back the recorded one. The symptom is a free camera that
	// will not move, because there is no free camera.
	//
	// The trap is that recording in first person sets TWO bits, not one. The
	// condition behind the camera-disabled bit includes "the dominant rendered
	// camera is a first-person camera", which is the same thing that sets the
	// first-person bit - so a first-person clip is ALWAYS also flagged as
	// camera-disabled, and clearing the first restriction alone leaves the
	// stronger one in place.
	//
	// Bit values read straight out of the restriction virtual, which tests all
	// three in one place:
	//
	//     mov  ecx, [rip+clipFlags]
	//     mov  eax, 2   / test cl, 4    -> CUTSCENE
	//     mov  eax, 1   / test cl, 8    -> FIRST_PERSON
	//     and  ecx, 1   / neg / and 3   -> CAMERA_BLOCKED
	//
	// The two checks read DIFFERENT STORAGE, which is the whole trap here. The
	// restriction virtual reads a single aggregate global describing the clip.
	// The director does NOT - it asks whether the flag is set on the frame packet
	// being played right now, and every recorded frame carries its own copy. So
	// clearing the aggregate satisfies the menu and changes nothing about the
	// camera; the two have to be addressed separately.
	// -------------------------------------------------------------------------
	inline constexpr unsigned CLIPFLAG_DISABLE_CAMERA_MOVEMENT = 1u << 0;
	inline constexpr unsigned CLIPFLAG_RECORDED_CUTSCENE       = 1u << 2;
	inline constexpr unsigned CLIPFLAG_RECORDED_FIRST_PERSON   = 1u << 3;

	// -------------------------------------------------------------------------
	// CReplayMgrInternal::IsPlaybackFlagSet(u32 flag) - the per-frame reader.
	//
	//     packet = *(void**)g_CurrentFramePacket
	//     return packet && (*(u32*)(packet + 0x18) & flag) != 0;
	//
	// This is the SECOND lock on the free camera, and the one that actually
	// stops it moving. The camera director calls it with the camera-disabled bit
	// and, when it comes back true, returns the recorded camera without ever
	// looking at what the marker asked for:
	//
	//     if (IsPlaybackFlagSet(1)) -> recorded camera
	//     else switch (marker[+0xA1]) { ... free camera ... }
	//
	// So the marker says free camera, the director hands back the recorded one,
	// and the symptom is a free camera that will not move because there is no
	// free camera. Confirmed by decompiling the caller on both builds; both are
	// the same shape and both call a real, un-inlined function.
	//
	// Answering false for that ONE bit is the unlock. The same function serves
	// every other playback flag - in-vehicle, aircraft shadows, first person for
	// VFX - so it must not be neutered wholesale, and every caller passes a
	// single bit rather than a mask. Bit 0 has exactly one caller: the camera
	// selection above.
	//
	// The bodies are tiny but the `test [reg+0x18], ecx` + `setnz al` pair is
	// distinctive; both patterns verified unique in their own image.
	// -------------------------------------------------------------------------
	inline constexpr Sig REPLAY_ISPLAYBACKFLAGSET = {
		"48 8B 05 ? ? ? ? 48 85 C0 74 07 85 48 18 0F 95 C0 C3 31 C0 C3",
		"48 8B 15 ? ? ? ? 33 C0 48 85 D2 74 06 85 4A 18 0F 95 C0 C3"
	};

	inline constexpr unsigned EDIT_RESTRICTION_NONE           = 0;
	inline constexpr unsigned EDIT_RESTRICTION_FIRST_PERSON   = 1;
	inline constexpr unsigned EDIT_RESTRICTION_CUTSCENE       = 2;
	inline constexpr unsigned EDIT_RESTRICTION_CAMERA_BLOCKED = 3;
	inline constexpr unsigned EDIT_RESTRICTION_DOF_DISABLED   = 4;
	inline constexpr unsigned EDIT_RESTRICTION_IS_ANCHOR      = 5;
	inline constexpr unsigned EDIT_RESTRICTION_IS_ENDPOINT    = 6;
	inline constexpr unsigned EDIT_RESTRICTION_NEEDS_BLEND    = 7;
	inline constexpr unsigned EDIT_RESTRICTION_MAX            = 8;

	// -------------------------------------------------------------------------
	// CReplayMgrInternal::SetupReplayBuffer(u16 normalBlocks, u16 tempBlocks)
	//   leg 0x144938   enh 0x7B7380
	//
	// HOW LONG YOU CAN RECORD. Not a duration limit - a memory one. The recorder
	// writes into a ring of fixed 4 MB blocks, so a street full of traffic fills
	// them in seconds where an empty desert lasts a minute. Stock is 7 blocks,
	// i.e. 28 MB.
	//
	// This function is the entire patch point, because it does its own memory
	// management:
	//
	//   total = normal + temp;
	//   if (total > blocksAllocated) {
	//       FreeMemory();
	//       if (!AllocateMemory(total, 0x400000)) { FreeMemory(); return false; }
	//   }
	//   if (blocksAllocated) { bufferInfo.Reset(); SetBlockCount(normal, temp); ... }
	//
	// So raising the first argument is all there is to it - it frees, reallocates
	// and redoes its own block bookkeeping. It only reallocates when the request
	// GROWS, so one raised call sizes the buffer once and it never churns after.
	//
	// TWO THINGS THAT MATTER:
	//
	//   * ONLY RAISE IT ON THE RECORDING PATH. Clip loading calls this as
	//     (header.PhysicalBlockCount, 0) and teardown as (0, 0). A non-zero temp
	//     count is what marks the recording setup. Inflating a load would tell
	//     the buffer it holds more blocks than the clip ever wrote, and playback
	//     would walk blocks nothing filled.
	//   * THE COUNT IS BOUNDED BY THE REPLAY HEAP, not by this function. Blocks
	//     are allocated one at a time out of a fixed pool reserved at startup,
	//     and going past what that pool holds does not fail cleanly - see
	//     REPLAY_HEAP_ALLOC_SITE below, which is what has to move first.
	//
	// Both patterns verified unique. Legacy loads the allocated-block count
	// first and widens both u16 args; Clang moves them into esi/edi and uses a
	// `lea ebx,[rsi+rdi]` for the total - different shape, same function, and
	// each pattern's rip-relative operand resolves to that build's own
	// blocks-allocated global (leg 0xD1AFF2, enh 0x3E31762), which is the
	// cross-check that they are the same function on both.
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// CReplayBufferInfo, off the movzx in SetupReplayBuffer's prologue.
	//
	// The prologue loads m_numberOfBlocksAllocated, which sits at +0x2A, so the
	// struct base is that address minus 0x2A. Layout read off SetBlockCount,
	// which writes two PAIRS of counters:
	//
	//   +0x08 m_numBlocks            +0x0A m_numTempBlocks          configured
	//   +0x0C m_currentNumBlocks     +0x0E m_currentNumTempBlocks   LIVE
	//   +0x10 m_pBlocks              +0x2A m_numberOfBlocksAllocated
	//
	// The live pair is the one that matters and the reason this is instrumented
	// at all. The routine that hands a block to the temp buffer decrements the
	// live normal count and increments the live temp one, so blocks move OUT of
	// the recording ring while a save is in flight. The ring you configure is
	// therefore not necessarily the ring you record into, and the only way to
	// know which you got is to read it back.
	// -------------------------------------------------------------------------
	inline constexpr unsigned char OP_MOVZX_EAX_M16[] = { 0x0F, 0xB7, 0x05 };

	inline constexpr DerivePair SRB_BLOCKSALLOCATED = {
		{ 0x0E, 0x11, OP_MOVZX_EAX_M16, 3, 0 },   // enh
		{ 0x0F, 0x12, OP_MOVZX_EAX_M16, 3, 0 },   // leg
	};
	inline constexpr int BUFINFO_ALLOCATED_OFF   = 0x2A;  // to get back to the base
	inline constexpr int BUFINFO_NUMBLOCKS       = 0x08;
	inline constexpr int BUFINFO_NUMTEMP         = 0x0A;
	inline constexpr int BUFINFO_CURRENTBLOCKS   = 0x0C;
	inline constexpr int BUFINFO_CURRENTTEMP     = 0x0E;

	inline constexpr Sig REPLAY_SETUPREPLAYBUFFER = {
		"56 57 53 48 83 EC 20 89 D6 89 CF 8D 1C 3E 0F B7 05 ? ? ? ? "
		"66 39 C3 76 20 E8",
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B7 05 ? ? ? ? "
		"0F B7 FA 0F B7 F1 8D 1C 37 66 3B D8"
	};

	// -------------------------------------------------------------------------
	// The RECORDING call site, and CReplayMgrInternal::NumberOfReplayBlocks.
	//
	// Hooking SetupReplayBuffer alone was not enough, and the reason is worth
	// recording: all four of its call sites were decoded, and the two recording
	// ones read the block count out of a GLOBAL rather than computing it -
	//
	//   enh  mov byte [rip+d],1
	//        movzx edx,word [NumberOfTempReplayBlocks]
	//        movzx ecx,word [NumberOfReplayBlocks]      <- this one
	//        call  SetupReplayBuffer
	//
	//   leg  movzx ecx,word [NumberOfReplayBlocks]      <- this one
	//        mov   edx,6                                 (the temp-block count)
	//        call  SetupReplayBuffer
	//
	// so writing the global covers every path that reads it, whenever it runs,
	// with no dependence on our hook being installed before the call. The other
	// two sites are the teardown (0,0) and the clip load (count,0) - both pass a
	// zero temp count, which is what the hook's discriminator keys on.
	//
	// Patterns verified unique in each image.
	// -------------------------------------------------------------------------
	inline constexpr unsigned char OP_MOVZX_ECX_M16[] = { 0x0F, 0xB7, 0x0D }; // movzx ecx,word[rip+d]
	inline constexpr unsigned char OP_MOV_M16_IMM16[] = { 0x66, 0xC7, 0x05 }; // mov word[rip+d],imm16

	// -------------------------------------------------------------------------
	// The Enhanced site opens with the CLAMP, and that is the point of starting
	// it nine bytes earlier than the call:
	//
	//   66 C7 05 d32 2A 00   mov  word [TotalNumberOfReplayBlocks], 42
	//   C6 05 d32 01         mov  byte [..], 1
	//   0F B7 15 d32         movzx edx, word [NumberOfTempReplayBlocks]
	//   0F B7 0D d32         movzx ecx, word [NumberOfReplayBlocks]
	//   E8 rel32             call SetupReplayBuffer
	//
	// The enable path caps TotalNumberOfReplayBlocks at 42 immediately before
	// setting the buffer up, and THAT is what bounds a
	// recording - not the count passed to SetupReplayBuffer. Allocating 70
	// blocks and leaving this at 42 gave a buffer two thirds of which was never
	// written: 24 seconds instead of the expected 35.
	//
	// It does not need patching. The clamp runs before the call, we hook the
	// call, so writing the global from inside the hook lands after it - and
	// nothing has read it yet. One less immediate to patch, and it follows the
	// ini live like everything else.
	// -------------------------------------------------------------------------
	inline constexpr Sig REPLAY_RECORDBUFFER_SITE = {
		"66 C7 05 ? ? ? ? 2A 00 C6 05 ? ? ? ? 01 0F B7 15 ? ? ? ? 0F B7 0D ? ? ? ? E8",
		"0F B7 0D ? ? ? ? BA 06 00 00 00 E8"
	};

	// u16 CReplayMgrInternal::NumberOfReplayBlocks, off the site above.
	inline constexpr DerivePair RBS_NUMBLOCKS = {
		{ 0x17, 0x1A, OP_MOVZX_ECX_M16, 3, 0 },   // enh - the second movzx
		{ 0x00, 0x03, OP_MOVZX_ECX_M16, 3, 0 },   // leg - the site opens with it
	};

	// u16 CReplayMgrInternal::TotalNumberOfReplayBlocks - the one that actually
	// bounds a recording. Enhanced only; Legacy's clamp has not been located,
	// and without it that build simply cannot exceed the stock budget.
	// extra = 2 for the trailing imm16, which the plain disp+4 would miss.
	inline constexpr Derive RBS_TOTALBLOCKS = { 0x00, 0x03, OP_MOV_M16_IMM16, 3, 2 };

	// The block-count range.
	//
	// MIN and the 36 the engine clamps its own setter to are both visible as
	// immediates in that setter's decompile. HARD_MAX is OURS - anything above
	// the game's own count is only reachable once the replay heap below has been
	// widened, and limits.cpp clamps back down if that could not be done.
	//
	// STOCK (7) is what the count global holds before the settings code runs.
	// Do NOT treat it as the live value: both builds settle on 30, read out of
	// the running game, because the settings apply force-clamps to 30 whenever
	// the replay heap measures under ~196 MB - and stock is 172 MB.
	inline constexpr int REPLAY_BLOCKS_MIN      = 3;
	inline constexpr int REPLAY_BLOCKS_STOCK    = 7;
	inline constexpr int REPLAY_BLOCK_BYTES     = 4 * 1024 * 1024;

	// EVERY BLOCK COSTS MORE THAN A BLOCK.
	//
	// The routine that sizes the block array ends by allocating a second buffer
	// per block - (blockCount + 1) of them - out of the same replay heap:
	//
	//     *(u64*)(p - 4) = 0x10000000200;              // 512 wide, 256 high
	//     alloc( (256 * 512) * 3, 16, 0 );             // 393216 = 384 KB, RGB
	//
	// A thumbnail per block. Miss it and the heap arithmetic is out by 27 MB at
	// 70 blocks, which is exactly how the first widening attempt still crashed.
	//
	// So the real cost is 4 MB + 384 KB per block, plus one spare thumbnail.
	inline constexpr int REPLAY_BLOCK_THUMB_BYTES = 256 * 512 * 3;   // 384 KB
	inline constexpr int REPLAY_BLOCK_TOTAL_BYTES =
		REPLAY_BLOCK_BYTES + REPLAY_BLOCK_THUMB_BYTES;               // ~4.375 MB

	// What the STOCK 172 MB heap can really hold.
	//
	// Not 36. The heap is sized as (42 + 1) * 4 MB, which does not count the
	// thumbnails - so the engine's own stated maximum does not fit its own heap:
	//   42 total -> 42*4.375 MB + 384 KB = 184 MB, against a 172 MB pool.
	// The 30 + 6 = 36 total both builds settle on comes to 158 MB and fits with
	// 14 MB spare, which is presumably why 30 is the number.
	//
	// So without widening the heap, 30 is the ceiling - and there is no reason to
	// squeeze past it, because anything higher can simply widen instead.
	inline constexpr int REPLAY_BLOCKS_SAFE_MAX = 30;

	// -------------------------------------------------------------------------
	// The replay HEAP - the thing that actually bounds the block count.
	//   enh site 0x1247, inside the memory-manager init at rva 0x1020
	//
	// The whole limit is one allocation, made once, at the top of the process:
	//
	//   heap = reserve(0xAC00000);                 // 172 MB
	//   allocator.init(heap, 0xAC00000, 8, 0);     // heap type 8, quit-on-fail OFF
	//   g_pReplayAllocator = &allocator;
	//
	// 0xAC00000 is 172 MB, and the engine's own maximum of 42 blocks times the
	// 4 MB block size is 168 MB. The heap is (42 + 1) * 4 MB - the block budget
	// plus exactly one block of slack for the allocator's own headers.
	//
	// So 36 was never a policy choice or an array bound. It is the largest block
	// count that FITS, and asking for 70 blocks wanted 280 MB out of a 172 MB
	// pool. Quit-on-fail is off, so a short pool returns a null rather than
	// aborting, and the block setup has no surviving check on it - the null
	// reaches a dereference, which is the crash.
	//
	// Raising it means widening this allocation, which is TWO immediates in one
	// function: the reservation size and the allocator's own size. The
	// open question is not how, it is WHEN - this runs at rva 0x1020, and if it
	// has already happened by the time we install then patching the immediates
	// changes nothing. addr_g_ReplayAllocator exists to answer exactly that: it
	// is null until the line above runs.
	//
	// Legacy is not patterned yet - settle the timing on Enhanced first, since a
	// second hunt for a site we cannot use would be wasted.
	// -------------------------------------------------------------------------
	inline constexpr Sig REPLAY_HEAP_INIT_SITE = {
		"48 8D 3D ? ? ? ? 48 89 F9 31 D2 E8 ? ? ? ? 8B 05 ? ? ? ? 8B 0D ? ? ? ? "
		"65 48 8B 14 25 58 00 00 00 48 8B 0C CA 3B 81 34 06 00 00 0F 8F ? ? ? ? 48 89 3D",
		""
	};


	// -------------------------------------------------------------------------
	// The two places 0xAC00000 appears, and both must move together.
	//
	//   rva 0x1213   B9 00 00 C0 0A       mov ecx,0xAC00000    -> reserve(size)
	//   rva 0x1563   41 B8 00 00 C0 0A    mov r8d,0xAC00000    -> allocator.init(heap,size,..)
	//
	// One is how much memory is RESERVED, the other is how much the allocator
	// believes it owns. Patching only the second would hand it a window past the
	// end of the reservation - which is worse than the limit we are lifting, and
	// would not fail until something allocated into that tail. So it is both or
	// neither, and limits.cpp treats a half-resolve as a refusal.
	//
	// The stock size is part of each pattern deliberately. If a future build
	// changes the heap, these stop matching and we leave it alone, which is the
	// correct outcome - far better than writing a size derived from assumptions
	// that no longer hold.
	// -------------------------------------------------------------------------
	// Same 0xAC00000 on BOTH builds - the heap is 172 MB either way. Only the
	// codegen around it differs: Clang zeroes edx with `31 D2`, MSVC with
	// `33 D2`, and MSVC keeps the allocator object in rbp across the call.
	inline constexpr Sig REPLAY_HEAP_ALLOC_SITE = {
		"B9 00 00 C0 0A 31 D2 E8 ? ? ? ? 48 89 C7",
		"B9 00 00 C0 0A E8 ? ? ? ? 8B 0D ? ? ? ? 48 8D 2D ? ? ? ?"
	};
	inline constexpr int RHA_SIZE_IMM_OFF = 1;    // imm32 follows the B9, both builds

	inline constexpr Sig REPLAY_HEAP_CTOR_SITE = {
		"C6 44 24 20 00 48 8D 0D ? ? ? ? 41 B8 00 00 C0 0A 48 89 FA 41 B9",
		"83 C9 40 41 B9 08 00 00 00 41 B8 00 00 C0 0A"
	};
	// The imm32 sits at a different depth in each - Clang loads the object
	// pointer before the size, MSVC loads the heap type before it.
	inline constexpr int RHC_SIZE_IMM_OFF_ENH = 14;
	inline constexpr int RHC_SIZE_IMM_OFF_LEG = 11;

	// The allocator OBJECT (not the pointer to it).
	//
	// Checked instead of m_pReplayAllocator because it is reachable on both
	// builds from a site we already have: Clang leas it into rdi at the top of
	// the init block, MSVC into rbp just after the reservation. It is a static,
	// so its vtable slot is zero until the constructor runs - which is exactly
	// the "has the heap been committed yet" question, and the only thing that
	// decides whether widening it is still possible.
	inline constexpr unsigned char OP_LEA_RDI[] = { 0x48, 0x8D, 0x3D };
	inline constexpr unsigned char OP_LEA_RBP[] = { 0x48, 0x8D, 0x2D };

	inline constexpr Derive RHI_ALLOCATOR_OBJ_ENH = { 0x00, 0x03, OP_LEA_RDI, 3, 0 };  // off HEAP_INIT_SITE
	inline constexpr Derive RHI_ALLOCATOR_OBJ_LEG = { 0x10, 0x13, OP_LEA_RBP, 3, 0 };  // off HEAP_ALLOC_SITE

	inline constexpr unsigned REPLAY_HEAP_STOCK_BYTES = 0x0AC00000u;  // 172 MB

	// Above the engine's own maximum this is ours, and only reachable when the
	// heap is widened first. 128 blocks is 512 MB of blocks plus headroom.
	inline constexpr int REPLAY_BLOCKS_HARD_MAX = 128;

	// The temp-block count, 6. Passed alongside the normal count at every
	// recording setup, so the heap has to cover both.
	inline constexpr int REPLAY_TEMP_BLOCKS = 6;

	// What the pool has to be to hold `blocks` recording blocks.
	//
	// A function rather than a number because TWO paths need it and they must
	// agree exactly: the normal install, and the early FiveM one that has to
	// patch the size before ScriptHookV hands us a thread. If those two ever
	// computed different sizes the game would reserve one amount and then record
	// against another, which is a crash rather than a wrong number.
	//
	// Sized from what a block ACTUALLY costs - 4 MB plus a 384 KB thumbnail,
	// with one spare thumbnail on top, then 16 MB of slack for allocator
	// bookkeeping. The engine's own (n+1)*4MB ignores thumbnails entirely, which
	// is why its stated 42-block maximum does not fit its own heap. Rounded to a
	// 4 MB boundary to keep the pool block-aligned.
	inline constexpr unsigned replayHeapBytesFor(int blocks)
	{
		const unsigned total = (unsigned)(blocks + REPLAY_TEMP_BLOCKS);
		const unsigned bytes = total * (unsigned)REPLAY_BLOCK_TOTAL_BYTES
		                     + (unsigned)REPLAY_BLOCK_THUMB_BYTES
		                     + 16u * 1024u * 1024u;
		return (bytes + 0x3FFFFFu) & ~0x3FFFFFu;
	}

	// The shipped default, shared with Config so the early path - which reads the
	// ini through WinAPI and cannot touch Config at all - falls back to the same
	// number when the key is absent from an older ini.
	inline constexpr int REPLAY_BLOCKS_DEFAULT = 128;

	// Section and key, likewise shared so the two readers cannot disagree.
	inline constexpr const char* INI_SECTION           = "RockstarEditorPlus";
	inline constexpr const char* INI_KEY_REPLAY_BLOCKS = "ReplayBlocks";

	// -------------------------------------------------------------------------
	// Game allocator, Enhanced only.  alloc(size) -> allocAligned(size,16,0,0)
	//
	// Needed because Enhanced inlines atArray::Grow, so growing
	// ms_activeMenuOptions is on us - and it MUST go through the game's heap,
	// since PopulateCameraMenu frees that buffer on the next rebuild. A CRT
	// block handed to the game's allocator would crash.
	//
	// Whole function is 16 bytes; the 12 fixed ones are unique binary-wide.
	// -------------------------------------------------------------------------
	inline constexpr Sig GAME_ALLOC = {
		"BA 10 00 00 00 45 31 C0 45 31 C9 E9",   // enh 0x18A0
		""                                        // Legacy never needs it
	};

	// -------------------------------------------------------------------------
	// CVideoEditorPlayback marker-menu input dispatcher   leg 0x1C09CC
	//
	// ECX = nav code. 0xBE = right (+1), 0xBD = left (-1), 0xBB/0xBC move focus,
	// 0xC9 = accept. Everything else falls through the option switch.
	// -------------------------------------------------------------------------
	inline constexpr Sig PLAYBACK_MENUINPUT = {
		// enh 0x683950. Found by nav-code dispatch, NOT by what it writes:
		// Enhanced moved the marker-field writes and the edited-flag bit sets out
		// into storage setters, so none of Legacy's `mov [rdi+0xA1]` /
		// `bts qword [rdi],N` / 64-bit-mask fingerprints exist here. What is left
		// is the dispatch: eight `cmp reg,0xBE`, one `cmp reg,0xC9`, two
		// `cmp reg,0x162` - the only function in the editor region with all three.
		// Nav code arrives in a register from a parameter, as on Legacy.
		"41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 58 0F 29 74 24 40 89 CD "
		"48 8B 3D ? ? ? ? 31 DB 48 85 FF 74",
		"48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 40 "
		"45 33 FF 0F 29 74 24 30 44 8B F1 41 8B DF E8 ? ? ? ? 4C 8B E8"
	};

	// MOV EAX,[ms_menuFocusIndex]
	inline constexpr Derive MI_FOCUSINDEX = { 0x3C, 0x3E, OP_MOV_EAX, 2 };

	// MI_FOCUSINDEX above is now the ONLY thing derived out of MenuInput, and
	// only on Legacy - Enhanced takes the focus index from PopulateCameraMenu's
	// prologue instead (PCM_E_FOCUSINDEX). That matters because Enhanced's
	// MenuInput identification is still unverified: nothing the menu needs
	// depends on it being right, only the nav hook itself.
	//
	// Removed 2026-07-27: MI_UPDATEITEMTEXT (now the standalone
	// PLAYBACK_UPDATEITEMTEXT signature), MI_EDITMARKERIDX, and the eight
	// attach / look-at / DOF / attach-type stepper derives. Those existed solely
	// for the ReShade overlay's target panel; the in-editor menu never used
	// them, and with ReShade reduced to the IGCS capture path the panel went
	// too. They were also the deepest, most build-fragile offsets in the file
	// (up to +0xC70 inside MenuInput).
	//
	// If target editing is ever wanted back, ENHANCED_PORT.md has the resolved
	// addresses for BOTH builds - and note the Enhanced six are far better found
	// via the shared core stepper's caller list than as interior offsets, which
	// is how the hunt actually succeeded.

	inline constexpr int NAV_ACCEPT = 0xC9;
	inline constexpr int NAV_RIGHT = 0xBE;
	inline constexpr int NAV_LEFT  = 0xBD;

	// Option id stamped on our injected rows.
	//
	// MUST stay < MARKER_MENU_OPTION_MAX (0x28). That was an inference; it is now
	// CONFIRMED on both builds - UpdateMenuHelpText indexes sc_EditMarkerMenuHelpText
	// with the option id and has no upper bound on either side, so an invented id
	// reads past the array and hands whatever it finds to TheText::Get. See
	// PLAYBACK_UPDATEMENUHELPTEXT above.
	//
	// 0x27 is MARKER_MENU_OPTION_HELP_TEXT, the last valid entry, and its help
	// string is "" - which is exactly why our rows carried a blank help line
	// until we started hooking that function. It is handled by neither the accept
	// path nor the toggle switch, so a row carrying it is inert if it ever
	// reaches stock code, which is the property we actually want from it. We
	// identify our rows by menu index plus this id, never by either alone.
	inline constexpr int OURS_OPTION_ID = 0x27;

	// Stock ids from eMARKER_MENU_OPTIONS, read out of the retail decompile.
	inline constexpr int OPT_CAMERA_MENU     = 0x10;
	inline constexpr int OPT_CAMERA_TYPE     = 0x11;
	inline constexpr int OPT_EDIT_CAMERA     = 0x12;
	inline constexpr int OPT_ATTACH_TARGET   = 0x13;
	inline constexpr int OPT_LOOKAT_TARGET   = 0x14;
	inline constexpr int OPT_CAMERA_BLEND    = 0x15;
	inline constexpr int OPT_CAMERA_SHAKE    = 0x16;
	inline constexpr int OPT_SHAKE_INTENSITY = 0x17;
	inline constexpr int OPT_SHAKE_SPEED     = 0x18;
	inline constexpr int OPT_ATTACH_TYPE     = 0x19;
	inline constexpr int OPT_BLEND_EASING    = 0x1A;
	inline constexpr int OPT_AUDIO_MENU      = 0x1B;
	inline constexpr int OPT_SPEED           = 0x25;

	// Our rows are inserted immediately AFTER the stock row carrying this id,
	// instead of being appended at the end of the column.
	//
	// The Shake row is the anchor because it is the one camera-menu row with no
	// condition on it - shake intensity and speed only exist once a shake is
	// set, look-at and attach only on a free camera, mount type only with an
	// attach target. Anchoring on an id rather than a row NUMBER is what makes
	// this survive those conditionals.
	inline constexpr int ANCHOR_OPTION_ID = OPT_CAMERA_SHAKE;

	// Byte offset, from the start of GetNextMarkerIndex, of the disp32 belonging
	// to `mov rdi,[rip+g_ReplayMarkerStorage]`. memory::rip() resolves it.
	// g_ReplayMarkerStorage, loaded in GetNextMarkerIndex's prologue in both
	// builds - Legacy `mov rdi,[rip+d]`, Enhanced `mov r15,[rip+d]`.
	inline constexpr DerivePair GNMI_MARKERSTORAGE = {
		{ 0x0E, 0x11, OP_MOV_R15_MEM, 3, 0 },   // enh
		{ 0x19, 0x1C, OP_MOV_RDI_MEM, 3, 0 },   // leg
	};
	inline constexpr int GETNEXTMARKERINDEX_STORAGE_DISP_OFF = 0x1C;

	// -------------------------------------------------------------------------
	// CReplayMgr current-time-relative-ms global (leg 0x1EC2620).
	//
	// Read inside UpdateSmoothing to build the segment phase. It is a FLOAT
	// that the game truncates to u32 before use:
	//   UpdateSmoothing+0x12C:  F3 48 0F 2C 0D ?? ?? ?? ??
	//                           cvttss2si rcx, [rip+disp]
	//
	// This is an interior offset rather than a scan, so game.cpp verifies the
	// five opcode bytes before trusting the disp — if a future build shifts
	// this, we want a clean bail, not a wild pointer.
	// -------------------------------------------------------------------------
	// The replay clock, read with the same `cvttss2si rcx,[rip+d]` truncation in
	// both builds - only the offset moved.
	inline constexpr unsigned char OP_CVTTSS2SI_RCX[] =
		{ 0xF3, 0x48, 0x0F, 0x2C, 0x0D };
	inline constexpr DerivePair US_REPLAYTIME = {
		{ 0x13A, 0x13F, OP_CVTTSS2SI_RCX, 5, 0 },   // enh
		{ 0x12C, 0x131, OP_CVTTSS2SI_RCX, 5, 0 },   // leg
	};

	// -------------------------------------------------------------------------
	// camReplayDirector::GetPreviousMarkerIndex(int startIdx)  leg RVA 0x244590
	// Derived from the `call rel32` inside GetNextMarkerIndex rather than
	// scanned — it is a small function with an unremarkable prologue.
	// Offset of that CALL's rel32 operand from GetNextMarkerIndex's start.
	// -------------------------------------------------------------------------
	// LEGACY ONLY - inlined on Enhanced, so game.cpp refuses to take it there.
	// The E8 is at +0x34; +0x35 is its rel32. The old constant was named
	// ..._CALL_GETPREV_OFF but held the DISPLACEMENT offset, which is exactly the
	// trap that produced an off-by-one here the first time.
	inline constexpr Derive GNMI_GETPREV = { 0x34, 0x35, OP_CALL, 1, 0 };

	// -------------------------------------------------------------------------
	// CVideoEditorPlayback::Open(ePLAYBACK_TYPE type, u32)   leg RVA 0x1966E0
	//
	// Every route into the editor's playback goes through here, including the
	// Export button: CVideoEditorUi::TriggerExport calls Open(PLAYBACK_TYPE_BAKE).
	// Rewriting that one argument to PLAYBACK_TYPE_PREVIEW_FULL_PROJECT is all it
	// takes to send Export down the preview path instead of the game's encoder -
	// no menu injection, so none of the risk that comes with appending rows to
	// R*'s data-driven export menu.
	//
	//   40 53              push rbx
	//   48 83 EC 20        sub  rsp,20h
	//   48 8B 05 ...       mov  rax,[rip+..]
	//   89 15 ...          mov  [rip+..],edx     ; param_2
	//   4C 8D 05 ...       lea  r8,[rip+..]
	//   48 F7 D8           neg  rax
	//   C6 05 .. 01        mov  byte [rip+..],1
	//   89 0D ...          mov  [rip+..],ecx     ; <- ms_playbackType = type
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// CVideoProjectPlaybackController::JumpToNonDilatedTimeMs(float, u32)
	//                                                        leg RVA 0x192198
	//
	// THE PROJECT-LEVEL SEEK. CReplayMgrInternal::JumpTo only addresses the clip
	// that is currently loaded, so seeking with it cannot cross a clip boundary -
	// a multi-clip export would render clip 1 and stop. This one clamps to the
	// whole project range and crosses clips.
	//
	// Two things the decompile makes plain:
	//   * `this` is never touched, so it can be called with any pointer.
	//   * it is gated on the replay mode being EDIT (2) and on the target time
	//     differing from the current one, so a render must not start until the
	//     mode has settled - see the note in render.cpp.
	//
	//   40 53                 push rbx
	//   48 83 EC 40           sub  rsp,40h
	//   0F 29 74 24 30        movaps [rsp+30h],xmm6
	//   32 D2                 xor  dl,dl
	//   83 3D .. .. .. .. 02  cmp  dword [rip+..],2    ; g_ReplayMode == EDIT
	//   0F 29 7C 24 20        movaps [rsp+20h],xmm7
	//   41 8B D8              mov  ebx,r8d
	//   0F 28 F1              movaps xmm6,xmm1
	//   75 ..                 jne  bail
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// ENHANCED ONLY - the INLINED project seek.
	//
	// Clang inlined JumpToNonDilatedTimeMs into its editor callers, so there is
	// no function to scan. This patterns the inlined block instead, purely to
	// derive the two globals it references:
	//
	//   83 3D .. .. .. .. 02   cmp dword [rip+d],2   ; g_ReplayMode == EDIT
	//   F3 48 0F 2C 05 ..      cvttss2si rax,[rip+d] ; g_ReplayTimeMs
	//   48 8D 35 .. .. .. ..   lea rsi,[rip+d]       ; the playback controller
	//   FF 50 60 / FF 50 68    vtable start/end, arg 0xFFFFFFFF
	//   BA 05 00 00 00 / call  JumpTo(clamped, 5)
	//
	// It matches THREE sites and that is fine - all three are byte-identical
	// copies of the same inlined body and reference the same globals, so
	// whichever the scanner lands on derives the same answers. Do not "fix" the
	// pattern to force uniqueness.
	// -------------------------------------------------------------------------
	inline constexpr Sig VIDEOEDITOR_INLINED_SEEK = {
		"83 3D ? ? ? ? 02 75 ? F3 48 0F 2C 05 ? ? ? ? 89 C0 0F 57 C0 "
		"F3 48 0F 2A C0 0F 2E F8 75 02 7B ? 48 8B 05 ? ? ? ? "
		"48 8D 35 ? ? ? ? 48 89 F1 BA FF FF FF FF FF 50 60",
		""
	};

	inline constexpr unsigned char OP_LEA_RSI[] = { 0x48, 0x8D, 0x35 };

	// Both taken from the inlined seek above (Enhanced only).
	inline constexpr Derive IS_REPLAYMODE  = { 0x00, 0x02, OP_CMPD_IMM,  2, 1 };
	inline constexpr Derive IS_CONTROLLER  = { 0x2A, 0x2D, OP_LEA_RSI,   3, 0 };

	// The controller's range accessors, both called with 0xFFFFFFFF. Slots
	// confirmed identical in both builds, read out of Legacy's
	// JumpToNonDilatedTimeMs and Enhanced's inlined copy of it.
	//
	// They report the CURRENT CLIP's own span, not the project's - a clip cut
	// out of the middle of a recording answers something like 19200..22200 in a
	// six-second project. That is the whole reason render.cpp maps project time
	// into clip time before seeking.
	inline constexpr int PBC_VT_GETSTARTTIME = 0x60;
	inline constexpr int PBC_VT_GETENDTIME   = 0x68;

	// -------------------------------------------------------------------------
	// The rest of the clip interface, off the SAME object and the same vtable.
	//
	// What the two slots above are is now settled: the instance is
	// CReplayCoordinator's CReplayPlaybackController (leg 0x1D5B9E0,
	// enh 0x3D72750), whose vtable is IReplayPlaybackController's declaration
	// order with no virtual destructor - which is why GetClipStartNonDilated
	// lands on slot 12 and matches the 0x60 read out of both builds. Every slot
	// below is that same count continued, and each was verified against the
	// disassembly on Legacy AND Enhanced:
	//
	//   0x50  s32  GetCurrentClipIndex()          IsValid() ? m_activeClip : -1
	//   0x58  s32  GetTotalClipCount()            montage->clips.count
	//   0x60  f32  GetClipStartNonDilatedTimeMs( s32 clipIndex )
	//   0x68  f32  GetClipNonDilatedEndTimeMs( s32 clipIndex )
	//   0x130 void JumpToClip( s32 clipIndex, f32 clipNonDilatedTimeMs )
	//
	// JumpToClip is three instructions on both builds - it only records the
	// request:
	//     mov [rcx+0x14],edx        m_pendingJumpClip
	//     movss [rcx+0x18],xmm2     m_pendingJumpClipNonDilatedTimeMs
	// CReplayMgrInternal::UpdateClip picks it up on the next replay update and
	// runs the real transition. That update is NOT gated on playing, so a
	// paused frame render can step clips with it - which is the whole reason
	// this is here. A time >= 0 also makes the engine pause on arrival rather
	// than resume, so the render stays in control.
	//
	// A negative clipIndex means "current" to the two range accessors, which is
	// what the 0xFFFFFFFF above is; passing a real index queries that clip
	// without going near it.
	// -------------------------------------------------------------------------
	inline constexpr int PBC_VT_GETCLIPINDEX = 0x50;
	inline constexpr int PBC_VT_GETCLIPCOUNT = 0x58;
	inline constexpr int PBC_VT_JUMPTOCLIP   = 0x130;

	// Legacy's route to the controller. JumpToNonDilatedTimeMs is a real
	// function there and does the clamp itself, so the instance is loaded right
	// in front of the GetStartTime call:
	//     +0x40  48 8D 0D <disp32>   lea rcx,[g_PlaybackController]
	// Enhanced inlines that whole function and takes IS_CONTROLLER instead.
	// Without this, clipRange() fails on Legacy and multi-clip renders stop at
	// the first clip boundary.
	// disp is an offset from the FUNCTION START, not from the instruction - so it
	// is insn + opLen, 0x40 + 3. It read 3 for a long time, which passed the
	// opcode guard (that checks +0x40, which is correct) and then took the
	// displacement from the function's prologue instead: a pointer ~190MB past
	// the end of the image, reported as a healthy-looking rva 0xF5A0E22 in the
	// log. Nothing touches it until a render starts, so the ASI came up clean and
	// then took the game down the moment Export was pressed with the renderer on.
	// The same trap the GNMI_GETPREV comment above warns about.
	inline constexpr Derive JTND_CONTROLLER = { 0x40, 0x43, OP_LEA_RCX, 3, 0 };

	// The editor passes 5, not 0, when it seeks.
	inline constexpr unsigned JUMPOPTS_EDITOR_SEEK = 5;

	inline constexpr Sig REPLAY_JUMPTONONDILATED = {
		"",
		"40 53 48 83 EC 40 0F 29 74 24 30 32 D2 83 3D ? ? ? ? 02 0F 29 7C 24 20 "
		"41 8B D8 0F 28 F1 75"
	};

	inline constexpr Sig VIDEOEDITOR_PLAYBACK_OPEN = {
		// enh 0x641EA0 - the three stores of ms_bActive / ms_playbackType /
		// ms_clipIndexToEdit straight after the prologue are already unique.
		"56 57 55 53 48 83 EC 28 89 D5 C6 05 ? ? ? ? 01 89 0D ? ? ? ? "
		"89 15 ? ? ? ? 48 8B 05",
		"40 53 48 83 EC 20 48 8B 05 ? ? ? ? 89 15 ? ? ? ? 4C 8D 05 ? ? ? ? "
		"48 F7 D8 C6 05 ? ? ? ? 01 89 0D"
	};

	// ePLAYBACK_TYPE. The four values Open() is called with, taken from its
	// call sites in the editor.
	enum : int
	{
		PLAYBACK_TYPE_PREVIEW_FULL_PROJECT = 0,
		PLAYBACK_TYPE_BAKE                 = 1,
		PLAYBACK_TYPE_EDIT_CLIP            = 2,
		PLAYBACK_TYPE_RAW_CLIP_PREVIEW     = 3,
	};

	// -------------------------------------------------------------------------
	// CVideoEditorPlayback::ms_shouldRender  (leg RVA 0x1D1F29C)
	//
	// The bool behind the editor's own hide-HUD key. The editor's render path
	// tests it before drawing EITHER the video-editor movie or the
	// instructional buttons - both are gated on the same flag, which is why
	// clearing one byte removes the timer, the transport row, the scrub bar and
	// the button strip at once - using the game's own suppression path rather
	// than fighting Scaleform. Open() sets it back to true, so it is per-playback
	// state and safe to restore.
	//
	// Derived from `mov byte [rip+disp32],1` inside Open at +0x77. NOTE the
	// trailing imm8: the displacement is relative to the END of the seven-byte
	// instruction, so this needs base+7+disp, not the usual disp+4.
	// -------------------------------------------------------------------------

	// -------------------------------------------------------------------------
	// CMousePointer update  (leg RVA 0x2F6CDC) -> ms_State.m_bVisible (0x1F81680)
	//
	// Anchored on the only reference to the "mousePointerMC" string. The
	// function positions the cursor each frame and, when the visible flag is
	// clear, RELEASES the Scaleform pointer object:
	//
	//   if( wasVisible ) { if( !isVisible ) { release(ptrMC); release(root); } }
	//
	// So clearing that one byte takes the cursor down through the game's own
	// path, exactly as an ordinary hide would. It has to be re-asserted every
	// frame, because the input code writes it whenever the mouse is in use.
	//
	//   48 83 EC 38            sub  rsp,38h
	//   80 3D .. .. .. .. 00   cmp  byte [rip+..],0   ; previous visible state
	//   ...
	//   80 3D .. .. .. .. 00   cmp  byte [rip+..],0   ; <- ms_State.m_bVisible
	// -------------------------------------------------------------------------
	inline constexpr Sig MOUSEPOINTER_UPDATE = {
		// enh 0x555B50
		"56 48 83 EC 40 66 0F 6E 05 ? ? ? ? 66 0F 3A 22 05 ? ? ? ? "
		"01 0F 5B C0 F3 0F 10 0D",
		"48 83 EC 38 80 3D ? ? ? ? 00 66 0F 6E 05 ? ? ? ? 66 0F 6E 0D ? ? ? ? "
		"0F 5B C0 0F 5B C9"
	};

	// The live visibility flag inside that function. Both builds read it right
	// after the previous-state byte, but with different instructions: Legacy
	// `cmp byte [rip+d],0` (trailing imm8, so extra = 1), Enhanced
	// `movzx eax,byte [rip+d]` (disp32 ends the instruction, extra = 0).
	inline constexpr DerivePair MP_CURSORVISIBLE = {
		{ 0x3B, 0x3E, OP_MOVZX_EAX, 3, 0 },   // enh
		{ 0x3F, 0x41, OP_CMPB_IMM,  2, 1 },   // leg
	};

	// -------------------------------------------------------------------------
	// CPauseMenu::RenderAnimatedSpinner(float, bool, bool)   leg RVA 0x19F058
	//
	// The loading spinner. CVideoEditorPlayback::Render() draws it from three
	// different branches:
	//
	//   if( ShouldShowLoadingScreen() )                     Clear(black)
	//   else if( !ms_bReadyToPlay || IsPendingCleanup() )   Clear(black); Spinner()
	//   else if( (ms_bPreCaching && !IsBaking())
	//            || RenderSnapmaticSpinner() )              Spinner()
	//
	// Rather than resolve every flag in those conditions and hope none was
	// missed, hook the spinner itself: whatever reason it is being drawn for,
	// we see it. During a render it is suppressed AND marks the frame dirty, so
	// it can neither be composited into a capture nor let a bad frame through.
	//
	// Note CVideoEditorPlayback::Render() is NOT gated on ms_shouldRender, which
	// is why hiding the HUD does not remove the spinner on its own.
	// -------------------------------------------------------------------------
	inline constexpr Sig VIDEOEDITOR_RENDERSPINNER = {
		// enh 0x5D9810
		"48 83 EC 38 45 89 C1 41 89 D0 F3 0F 10 0D ? ? ? ? "
		"F3 0F 10 15 ? ? ? ? F3 0F C2 D0 04",
		"48 83 EC 48 0F 2E 05 ? ? ? ? F3 0F 10 0D ? ? ? ? F3 0F 10 15 ? ? ? ? 74 03 0F 28 C8"
	};

	// -------------------------------------------------------------------------
	// CVideoEditorInterface::ShouldShowLoadingScreen()   leg RVA 0x5AD200
	//
	// The first branch above - the black Rockstar-logo frames. A flag would not
	// do here: the branch calls this function, not ms_bLoadingScreen.
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// CBusySpinner::Render()   enh 0x1818BC0
	//
	// The game has TWO spinner systems and they are unrelated:
	//   CPauseMenu::RenderAnimatedSpinner - VIDEOEDITOR_RENDERSPINNER above
	//   CBusySpinner::Render              - this one
	//
	// Suppressing only the first still left a spinner composited into rendered
	// frames on Enhanced. This is the one that draws it: its own Scaleform movie
	// ("BUSY_SPINNER"), rendered from the RENDER thread, so the hook body must
	// stay trivial.
	//
	// Identified by its shape in the decompile: render-thread check, critical
	// section, then IsMovieActive(ms_iSpinnerMovie) / the
	// instructional-button-movie loop / widescreen scale mode / ChangeMovieParams
	// / render. ms_iSpinnerMovie is 0x292F1E0.
	//
	// The `56 48 83 EC 50 8B 05` prologue matches three functions; the other two
	// continue `test al,0x8E` and `cmp eax,3` where this one goes into the TLS
	// read, so the pattern must keep everything through that.
	//
	// Legacy is left empty deliberately: its renders come out clean with only
	// RenderAnimatedSpinner suppressed, so there is nothing to fix there.
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// CBusySpinner::On(const char* bodyText, int icon, int sourceIndex)
	//   enh 0x1818E80   leg 0x45B5A4
	//
	// The editor raises a busy spinner for every seek, which is why scrubbing the
	// timeline flashes a ring in the middle of the screen - and why a render, which
	// seeks for a living, gets one burned into its frames.
	//
	// Suppressing the DRAW does not work and two attempts proved it. CBusySpinner
	// hands the spinner to other movies via SetSavingText and then SKIPS its own
	// Render while one of those movies is up, so the hooked Render is not on the
	// path; and refusing the SET_SAVING_TEXT Scaleform call was observed to land on
	// movies 1 and 17 with the ring still drawn. So turn it off at the SOURCE
	// instead - the same "patch the input, not the call site" move the distance
	// leash needed.
	//
	// Found by string anchor on both builds: "VEUI_LOADING_CLIP" has exactly one
	// referencer, and it ends `TheText.Get(key)` / icon=5 / sourceIndex=5 / call.
	// That call target is this function. Confirmed structurally in the decompile:
	// a sourceIndex bound check, ms_SpinnerList indexed with
	// STRIDE 68 (int Icon + char BodyText[64], SPINNER_MAX_MESSAGE_LEN=64), the
	// "same icon and same text" early-out, and the BS_STATE_ACTIVE -> RESETUP
	// transition at the end. Enhanced bounds at >9, Legacy at >8 - Enhanced added
	// a source - which is why the index is taken from the CALL SITE, not the enum.
	//
	// Both prologues verified unique in their own image.
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// CScaleformMgr::RenderMovie(int movieId, Vector2* pos, Vector2* scale, ...)
	//   enh 0x5EA6B0
	//
	// The point every Scaleform movie goes through to reach the screen. This is
	// the layer BELOW method calls, and it is the only one left that can reach a
	// movie which was configured earlier and then animates itself in
	// ActionScript - such a movie issues no native calls at all while it draws,
	// which an always-on log of BeginMethod proved: during a full render with the
	// ring on screen, ZERO Scaleform methods were invoked.
	//
	// Found without a new hunt: CBusySpinner::Render (already resolved) calls it
	// as RenderMovie(ms_iSpinnerMovie, &pos, &scale{1,1}, ...), sitting between
	// the IsMovieActive and IsMovieRendering pair.
	//
	// Legacy is left empty deliberately - its renders come out clean, so there is
	// nothing there to suppress.
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// The animated spinner's actual DRAW, one level under RenderAnimatedSpinner
	//   enh 0x5EB8C0   leg 0x19F0C0
	//
	// VIDEOEDITOR_RENDERSPINNER above is only a thin wrapper: it substitutes a
	// default size when the caller passes -1.0 and forwards to this. Hooking the
	// wrapper is enough on LEGACY, which is why Legacy renders come out clean.
	//
	// On ENHANCED it is not, and this is why. The wrapper has ELEVEN callers here
	// and Clang inlined it into most of them - including the editor's own Render,
	// which reaches this function FOUR separate times, matching the three
	// spinner branches described on RENDERSPINNER above. So the editor never goes
	// through the wrapper and the detour on it never fires.
	//
	// Same lesson as GetCurrentEditMarker, atArray::Grow,
	// GetMaxDistanceAllowedFromPlayer and SetToSafePosition before it: when
	// Legacy works and Enhanced does not, suspect that Clang inlined the thing we
	// hooked. Hook the callee instead - it cannot be bypassed, because every
	// route including the wrapper ends here.
	//
	// Both prologues verified unique in their own image.
	// -------------------------------------------------------------------------
	inline constexpr Sig VIDEOEDITOR_DRAWSPINNER = {
		"41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC D8 00 00 00 "
		"0F 29 B4 24 C0 00 00 00 48 83 3D",
		"48 8B C4 48 89 58 08 48 89 70 10 4C 89 70 18 4C 89 78 20 55 "
		"48 8D 68 A9 48 81 EC F0 00 00 00"
	};

	inline constexpr Sig SCALEFORM_RENDERMOVIE = {
		"48 83 EC 78 44 8B 94 24 A0 00 00 00 44 0F B6 9C 24 A8 00 00 00 "
		"0F B6 84 24 B0 00 00 00",
		""
	};

	inline constexpr Sig BUSYSPINNER_ON = {
		"56 57 55 53 48 83 EC 28 48 85 C9 0F 84 ? ? ? ? 41 83 F8 09 "
		"0F 87 ? ? ? ? 89 D5 48 89 CE",
		"48 85 C9 0F 84 ? ? ? ? 48 89 4C 24 08 48 83 EC 28 44 8B D2 4C 8B C9 "
		"41 83 F8 08 77 ? 49 63 C8"
	};

	// SPINNER_SOURCE_VIDEO_EDITOR. Read off the call site in BOTH builds rather
	// than trusting the enum, since the source list differs between them.
	inline constexpr int SPINNER_SOURCE_VIDEO_EDITOR = 5;

	inline constexpr Sig BUSYSPINNER_RENDER = {
		"56 48 83 EC 50 8B 05 ? ? ? ? 65 48 8B 0C 25 58 00 00 00 "
		"48 8B 04 C1 80 B8 38 06 00 00 00 75 05 E8",
		""
	};

	inline constexpr Sig VIDEOEDITOR_SHOULDSHOWLOADING = {
		// enh 0x2E4290. Same predicate as Legacy, restructured by Clang into
		// early-exit form (De Morgan): Legacy is "if ALL good -> return 0",
		// Enhanced is "if ANY good condition fails -> return 1". Verified the
		// polarity end to end, not just the ingredients:
		//   mode==2 skips the bit tests straight to the mode+4 check (Legacy's
		//   `A = (mode==2) || (bits clear && ...)`), any bit set returns 1, and
		//   the tail returns `state == 4`, matching Legacy's `g2 != 4 -> 0`.
		// Same globals throughout: g_ReplayMode (==2/==4), g_ReplayMode+4 (==3),
		// 0x20/0x40 on 0x3E31F28 / 0x3E31F40, and the same helper(&global).
		//
		// Found by intersecting two rare compares - `g_ReplayMode != 4` (18
		// sites) with `g_ReplayMode+4 != 3` (3 sites).
		//
		// BEWARE 0x4718D0: it has every one of those ingredients but returns the
		// INVERSE, because it *calls* this and XORs the result. It is a caller,
		// not a rival candidate. Adopting it would make replayBusy() backwards -
		// always-true stalls a render forever, always-false captures the
		// loading screen.
		"53 48 83 EC 20 83 3D ? ? ? ? 02 75 04 B3 01 EB 35 8B 05 ? ? ? ? "
		"B3 01 A8 20 75 3D 8B 0D ? ? ? ? 83 E1 20",
		"40 53 48 83 EC 20 33 DB 83 3D ? ? ? ? 02 74 ? 8B 0D ? ? ? ? 8B C1 C1 E8 05 A8 01 75"
	};

	// -------------------------------------------------------------------------
	// ms_bLoadingScreen (leg RVA 0x1F655D8) and ms_bPreCaching (0x1F655DB)
	//
	// A seek across a clip boundary makes the editor load and pre-cache, and it
	// draws its loading screen and spinner while that happens. Capturing through
	// it produced sixteen frames of Rockstar logo in the middle of a render, all
	// of which counted towards the output. So the renderer waits on these.
	//
	// ms_bLoadingScreen is CONFIRMED, not inferred: the movie-render gate reads
	//     ms_bActive && ( !ms_shouldRender || ms_bLoadingScreen )
	// which is ShouldRender() inlined, and pins this exact global.
	//
	// ms_bPreCaching is the next consecutive store in Open, so it is a strong
	// inference rather than a confirmation. Both are opcode-guarded; if either fails to resolve
	// the renderer simply loses that wait.
	//
	//   88 1D .. .. .. ..     mov byte [rip+disp32],bl   (bl == 0 here)
	// -------------------------------------------------------------------------
	// -------------------------------------------------------------------------
	// Everything Playback::Open yields, per build.
	//
	// Offsets differ (Clang reordered the stores) AND so do the encodings:
	// Legacy writes the false-flags with `mov byte [rip+d],bl`, Enhanced with
	// `mov byte [rip+d],0` - which carries a trailing imm8, hence extra = 1.
	// Each is opcode-guarded, so a drifted build resolves 0 rather than noise.
	//
	// ms_bReadyToPlay / ms_bWantDelayedClose sit immediately before the
	// `playbackType != 1` (IsBaking) test in BOTH builds. That structural
	// relationship is what makes them re-findable on a future build.
	//
	// ms_bWantDelayedClose is how the game itself ends an export: when a bake
	// finishes it sets this and Update() does
	//     else if ( ms_bWantDelayedClose ) { Close(); return; }
	// at a safe point. We must use that rather than calling Close() ourselves -
	// the renderer pumps from inside camReplayDirector::PostUpdate, and tearing
	// playback down mid-camera-update is not survivable.
	// -------------------------------------------------------------------------
	inline constexpr DerivePair PO_PLAYBACKTYPE = {
		{ 0x11,  0x13,  OP_MOV_MEM_ECX, 2, 0 },
		{ 0x24,  0x26,  OP_MOV_MEM_ECX, 2, 0 },
	};
	inline constexpr DerivePair PO_SHOULDRENDER = {
		{ 0x72,  0x74,  OP_MOVB_IMM, 2, 1 },
		{ 0x77,  0x79,  OP_MOVB_IMM, 2, 1 },
	};
	inline constexpr DerivePair PO_LOADINGSCREEN = {
		{ 0x79,  0x7B,  OP_MOVB_IMM, 2, 1 },
		{ 0x7E,  0x80,  OP_MOVB_BL,  2, 0 },
	};
	inline constexpr DerivePair PO_PRECACHING = {
		{ 0x80,  0x82,  OP_MOVB_IMM, 2, 1 },
		{ 0x84,  0x86,  OP_MOVB_BL,  2, 0 },
	};
	inline constexpr DerivePair PO_WANTDELAYEDCLOSE = {
		{ 0x162, 0x164, OP_MOVB_IMM, 2, 1 },
		{ 0xFB,  0xFD,  OP_MOVB_BL,  2, 0 },
	};

	// `mov [rip+disp32],ecx` storing the type into ms_playbackType, at the end
	// of the pattern above. The disp is relative to the END of the instruction.

	// -------------------------------------------------------------------------
	// CPacketWeather::Extract(this)   leg RVA 0x463464, enh RVA 0x1D96F50
	//
	// One address buys BOTH the weather and the time-of-day override, which is
	// why this is the only signature the scene feature needs.
	//
	// Named from the "CPacketWeather" string the replay interface's constructor
	// registers it under - the packet-descriptor table in both exes stores
	// (size << 32) | id next to that string, which is where the 288 and the 6
	// below come from.
	//
	// It has exactly ONE call site on either build, in the packet-dispatch
	// preprocess pass:
	//
	//     mov  rax, [rcx+0x848]       ; bool* - the weather-override flag
	//     cmp  byte [rax], 0
	//     jnz  skip
	//     mov  rcx, <the packet>
	//     call CPacketWeather::Extract
	//
	// so it fires once per played frame with a pointer straight into the live
	// replay buffer. The decompile shows it only ever READS from its packet -
	// it hands the fields to the weather system and memcpy's the wind state out
	// - so a patched stack copy can be substituted for it with nothing to
	// restore, and every derived piece of state (clouds, wind, wetness, the snow
	// VFX flags) is still set by the game's own code rather than by us.
	//
	// The frame's CPacketClock is exactly 20 bytes in front of the weather
	// packet: the recorder emits GameTime, Clock and Weather back to back, which
	// is visible in any clip - a packet walk over 3,193 frames across both
	// builds found the clock immediately before the weather packet every time.
	// scene.cpp validates it per frame rather than assuming it.
	//
	// Both patterns key on the same two things: the packet-version compare
	// against a global at `this+7` (the version byte of the size/version word),
	// and the pair of MOVSS loads at +0x114/+0x118 that the version guards.
	// Those offsets are what makes this a weather packet and nothing else.
	//
	//   leg: 8A 05 ?? ?? ?? ??   mov al, [rip+d]   ; minimum packet version
	//        38 41 07            cmp [rcx+7], al   ; the packet's version
	//   enh: 0F B6 05 ?? ?? ?? ? movzx eax, byte [rip+d]
	//        3A 41 07            cmp al, [rcx+7]   ; operands swapped, hence JA
	// -------------------------------------------------------------------------
	inline constexpr Sig PACKETWEATHER_EXTRACT = {
		"56 48 83 EC 70 0F 29 74 24 60 48 89 CE 0F B6 05 ? ? ? ? "
		"0F 57 C0 0F 57 C9 3A 41 07 77 10 "
		"F3 0F 10 8E 14 01 00 00 F3 0F 10 86 18 01 00 00",
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 "
		"8A 05 ? ? ? ? 0F 57 C0 48 8B D9 0F 28 C8 38 41 07 72 10 "
		"F3 0F 10 81 14 01 00 00"
	};

	// The weather and clock packets, as they sit in the replay buffer.
	//
	// The common packet header is 12 bytes, not 8: retail clips are written with
	// guards on, so every packet carries 0xAAAAAAAA right after the size word.
	// That guard is what lets scene.cpp prove it is looking at the right packet
	// instead of trusting arithmetic. All of this is visible in a .clip: the
	// packets are stored uncompressed inside each block, and the chain walks
	// cleanly from the first byte using nothing but the size field.
	inline constexpr unsigned PACKET_GUARD_BASE   = 0xAAAAAAAA;
	inline constexpr unsigned short PACKETID_CLOCK   = 5;
	inline constexpr unsigned short PACKETID_WEATHER = 6;
	inline constexpr unsigned short PACKETID_TIMECYCLE_MODIFIER = 317;
	inline constexpr int PACKET_CLOCK_SIZE   = 20;
	inline constexpr int PACKET_WEATHER_SIZE = 288;
	inline constexpr int PACKET_GUARD_OFF    = 8;    // u32 == PACKET_GUARD_BASE
	inline constexpr int PACKET_SIZE_OFF     = 4;    // u32, size:24 | version:8

	// -------------------------------------------------------------------------
	// The timecycle variable table   leg RVA 0x1E00160, enh RVA 0x293E7A0
	//
	// THE lever for re-lighting a clip, and it is data rather than code.
	//
	// A clip records the fully RESOLVED timecycle keyframe every frame - the
	// ~416 lighting variables already evaluated for the hour and weather it was
	// shot in - and the replay path copies that back over the live one. The
	// copy is per-variable and gated on a flag in this table, which decompiles
	// to roughly:
	//
	//     if (replayModeIsEdit)
	//         for (i = 0; i < varCount; ++i)
	//             if (table[i * 0x20 + 0x18])          // the per-var flag
	//                 liveKeyframe[i] = recordedKeyframe[i];
	//
	// So the base timecycle IS evaluated from the live clock and weather every
	// frame - and then thrown away, variable by variable. In the editor that
	// outer test is unconditionally true, and it is inlined on both builds, so
	// the flag is the only thing left to hold. Clear it and the freshly
	// evaluated keyframe survives.
	//
	// Do NOT confuse this with stopping the packet that carries the recorded
	// keyframe: neutering it leaves the recorded frame holding the PREVIOUS
	// frame's values, and the loop above copies those over the live keyframe
	// just the same. The result is frozen lighting that looks exactly like no
	// change at all - confirmed in-game before this was traced to the flag.
	//
	// The same flag is read by the RECORDING path, which is why it has to be
	// put back the moment the editor is left; otherwise clips recorded
	// afterwards would carry no lighting at all.
	//
	// Retail entries are 32 bytes. Read out of the table itself, cross-checked
	// against the stock values that ship in the game's own timecycle data:
	//     +0x00 int   varId          (0,1,2,... - sequential, which is how the
	//                                 end of the table is found without a count)
	//     +0x08 char* name           ("light_dir_col_r", "light_dir_col_g", ...)
	//     +0x10 float defaultValue
	//     +0x14 int   varType
	//     +0x18 bool  replay-override flag
	//     +0x1C int   modifier type
	//
	// One pattern serves both builds: the table's literal bytes are identical
	// and only the two name pointers differ. It keys on entry 0 and entry 1 -
	// light_dir_col_r 0.890 and light_dir_col_g 0.675 - and matches exactly once
	// per binary.
	inline constexpr Sig TIMECYCLE_VARINFOS = {
		"00 00 00 00 00 00 00 00 ? ? ? ? ? ? ? ? 0A D7 63 3F 04 00 00 00 "
		"01 00 00 00 02 00 00 00 01 00 00 00 00 00 00 00 ? ? ? ? ? ? ? ? "
		"CD CC 2C 3F 00 00 00 00 01 00 00 00 02 00 00 00",
		"00 00 00 00 00 00 00 00 ? ? ? ? ? ? ? ? 0A D7 63 3F 04 00 00 00 "
		"01 00 00 00 02 00 00 00 01 00 00 00 00 00 00 00 ? ? ? ? ? ? ? ? "
		"CD CC 2C 3F 00 00 00 00 01 00 00 00 02 00 00 00"
	};

	inline constexpr int TCVARINFO_STRIDE      = 32;
	inline constexpr int TCVARINFO_VARID_OFF   = 0x00;
	inline constexpr int TCVARINFO_REPLAY_OFF  = 0x18;
	// Sanity bound on the walk. The stock table is a little over 400 entries;
	// anything past this means the sequential-varId test has stopped being a
	// reliable end marker, so stop rather than keep writing.
	inline constexpr int TCVARINFO_MAX         = 1024;

	inline constexpr int CLOCK_HOURS_OFF   = 12;
	inline constexpr int CLOCK_MINUTES_OFF = 13;
	inline constexpr int CLOCK_SECONDS_OFF = 14;

	inline constexpr int WEATHER_OLDTYPE_OFF = 0x0C;
	inline constexpr int WEATHER_NEWTYPE_OFF = 0x10;
	inline constexpr int WEATHER_INTERP_OFF  = 0x14;
	inline constexpr int WEATHER_WETNESS_OFF = 0x20;
}
