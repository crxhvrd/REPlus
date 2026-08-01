// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "capture/fxcapture.h"
#include "capture/render.h"
#include "capture/exporthook.h"

static bool bInited = false;
static void (*origGetSystemTimeAsFileTime)(LPFILETIME) = nullptr;

// Everything the mod installs, once the signatures are known good. Shared by the
// normal start path and the FiveM one so the two cannot drift.
static void InstallResolved()
{
	// Single shared store for now. Per-project separation needs the active clip
	// name, which we do not resolve yet вЂ” until then all projects share one file,
	// so marker times from different clips could in principle collide.
	rsettings::bindProject("default");

	fxcapture::init();
	render::applyConfig();
	render::installHooks();
	smoothblend::install();
	menu::install();
	limits::install();
	exporthook::install();
}

static void InstallAll()
{
	logger::write("info", "RockstarEditorPlus: init '%s' (base=%p)",
		ExeName(), (void*)memory::base());

	if (game::resolve())
		InstallResolved();

	logger::write("info", "RockstarEditorPlus: ready");
}

// Deferred init trampoline (same approach as fxReloader): DllMain cannot safely
// scan or hook under the loader lock, and Enhanced obfuscates imports so an IAT
// hook is unreliable вЂ” so we piggyback the first GetSystemTimeAsFileTime call,
// by which point the image is fully mapped and decrypted.
static void HookGetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime)
{
	if (!bInited)
	{
		bInited = true;

		if (!IsSupportedGame())
		{
			logger::write("info", "RockstarEditorPlus: '%s' is not GTA5 вЂ” staying dormant", ExeName());
		}
		else
		{
			InstallAll();
		}
	}
	origGetSystemTimeAsFileTime(lpSystemTimeAsFileTime);
}

// ---------------------------------------------------------------------------
//  ScriptHookV script registration, resolved at RUNTIME.
//
//  NOT a static import, deliberately: this mod's whole point is that it needs
//  only an ASI loader, and linking ScriptHookV would make it a hard requirement
//  for every singleplayer user. GetProcAddress costs nothing where it is absent.
//
//  This is how Menyoo gets a usable thread under FiveM, and why it works where
//  our own thread does not. It does not invent a deferred-init mechanism at all
//  — `scriptRegister(hInstance, ThreadMenyooMain)` and ScriptHookV calls that
//  back on its own fiber, on a GAME thread, once the game is actually up. Note
//  Menyoo also runs setupHooks() and GTAmemory::Init() straight from DllMain
//  under FiveM without trouble, which is the evidence that pattern scanning and
//  hooking were never the issue here — only which thread runs them.
//
//  FiveM's scripthookv shim exports scriptRegister/scriptWait/scriptUnregister,
//  so this route is available there even though getScriptHandleBaseAddress and
//  much else is not.
// ---------------------------------------------------------------------------
using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn     = void (*)(DWORD);

static ScriptRegisterFn pScriptRegister = nullptr;
static ScriptWaitFn     pScriptWait     = nullptr;

static bool ResolveScriptHook()
{
	// Case-insensitive, so one call covers either spelling of the module.
	const HMODULE shv = GetModuleHandleA("ScriptHookV.dll");
	if (!shv)
		return false;

	pScriptRegister = reinterpret_cast<ScriptRegisterFn>(
		GetProcAddress(shv, "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z"));
	pScriptWait = reinterpret_cast<ScriptWaitFn>(
		GetProcAddress(shv, "?scriptWait@@YAXK@Z"));

	return pScriptRegister != nullptr && pScriptWait != nullptr;
}

// Entry point ScriptHookV calls on its fiber. A game thread, at a point where
// the game is up — exactly the two guarantees the kernel32 trampoline gave us
// and the DllMain-spawned thread could not.
static void ScriptThreadMain()
{
	logger::write("info", "RockstarEditorPlus: [FiveM] ScriptHookV thread entered");

	InstallAll();

	// Registered scripts are expected not to return; ScriptHookV re-enters one
	// that does. Nothing here needs a tick, so park it on the longest wait
	// rather than spinning per frame.
	for (;;)
		pScriptWait(0xFFFFFFFF);
}

// The FiveM start path.
//
// The kernel32 detour cannot be installed under FiveM: MinHook needs a
// trampoline within +/-2GB of kernel32 and no free block is left in reach, so it
// fails MH_ERROR_MEMORY_ALLOC and the mod dies three lines into the log.
//
// The first attempt at a way round that ran the whole install from THIS thread.
// It got far - all signatures resolved, menu injection reported available - and
// then faulted inside msvcp140, in the C++ runtime's own static-init machinery,
// on the first function-local static the install path touches. Adding a 20s
// settle made no difference whatsoever: same fault, same place. The problem is
// not when this thread runs, it is that it is THIS thread - one created inside
// DllMain, which the CRT is entitled not to treat as fully-formed.
//
// So do not run init here at all. Wait for ScriptHookV and hand the job to it,
// which is precisely what Menyoo does and why Menyoo is fine under FiveM. This
// thread now only ever calls WinAPI: no CRT statics, nothing to fault on.
static HMODULE g_selfModule = nullptr;

static DWORD WINAPI FiveMRegisterThread(LPVOID)
{
	// A thread is still wanted, but only for WAITING. scripthookv may not be
	// loaded yet at DLL_PROCESS_ATTACH, and LoadLibrary from DllMain is not an
	// option, so poll for it. ~60s at 250ms.
	for (int attempt = 0; attempt < 240; ++attempt)
	{
		if (ResolveScriptHook())
		{
			logger::write("info", "RockstarEditorPlus: [FiveM] ScriptHookV found after "
				"%d attempt(s) - handing init to scriptRegister", attempt + 1);

			pScriptRegister(g_selfModule, ScriptThreadMain);
			return 0;
		}

		Sleep(250);
	}

	logger::write("info", "RockstarEditorPlus: [FiveM] no ScriptHookV in this process - "
		"no way to reach a game thread, staying dormant. That mechanism is what "
		"Menyoo relies on here.");
	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hModule);


		Config::get().load(hModule);
		logger::init();
		// Stamp the build on the first line.
		//
		// Triaging user reports without this is guesswork: most of what a fix
		// changes only shows in the log when the thing it fixes is exercised, so
		// two builds a dozen commits apart produce identical startup logs. Every
		// "is this the latest .asi?" round trip in a bug report is this line
		// missing.
		logger::write("info", "RockstarEditorPlus: attached (build " __DATE__ " " __TIME__ ")");

		// Decide whether this process is ours BEFORE touching it.
		//
		// This check also exists in the deferred trampoline below, but there it
		// is far too late: by the time the trampoline first runs, MinHook has
		// been initialised and kernel32!GetSystemTimeAsFileTime is already
		// detoured - and that detour stays for the life of the process even
		// after we decide to stay dormant. An .asi gets dropped into whatever
		// plugins folder is to hand, so "some process that is not the game"
		// is the normal case, not the exotic one, and patching a hot system
		// export in one of those is not something to do by accident.
		//
		// So the gate belongs here, ahead of memory::init(). On an unsupported
		// host we allocate nothing, initialise nothing, and patch nothing - the
		// DLL is inert and simply returns.
		//
		// FiveM takes a different START PATH, not a different decision. The
		// kernel32 detour below cannot be placed in its address space, so init is
		// routed through ScriptHookV instead - see FiveMRegisterThread.
		// memory::init() is still wanted for base() and the trampoline slab
		// MinHook uses for the GAME hooks; only the kernel32 target was ever out
		// of reach.
		if (IsFiveM())
		{
			logger::write("info",
				"RockstarEditorPlus: running under FiveM ('%s') - deferring init to "
				"ScriptHookV", ExeName());

			g_selfModule = hModule;
			memory::init();

			if (HANDLE h = CreateThread(nullptr, 0, FiveMRegisterThread, nullptr, 0, nullptr))
				CloseHandle(h);
			else
				logger::write("info", "RockstarEditorPlus: [FiveM] CreateThread failed (%lu)",
					GetLastError());

			return TRUE;
		}

		if (!IsSupportedGame())
		{
			logger::write("info",
				"RockstarEditorPlus: '%s' is not a supported game - staying dormant",
				ExeName());
			return TRUE;
		}

		if (!Config::get().enabled)
		{
			logger::write("info", "RockstarEditorPlus: disabled via ini вЂ” not hooking");
			return TRUE;
		}

		memory::init(); // base(), trampoline slab, MH_Initialize()

		if (!memory::HookApi(L"kernel32.dll", "GetSystemTimeAsFileTime",
			(PVOID)HookGetSystemTimeAsFileTime, (PVOID*)&origGetSystemTimeAsFileTime))
		{
			logger::write("info", "RockstarEditorPlus: GetSystemTimeAsFileTime hook failed (%lu)",
				GetLastError());
		}
	}
	return TRUE;
}
