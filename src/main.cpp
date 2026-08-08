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
	// Per-marker settings bind themselves - rsettings::syncScope() picks up the
	// open project and edited clip once the editor is up. Nothing to do here:
	// there is no project open at process start, and binding one now would only
	// create a file that the first real bind immediately replaces.

	fxcapture::init();
	render::applyConfig();
	render::installHooks();
	smoothblend::install();
	menu::install();
	limits::install();
	// Before scene::install() for no reason other than ordering with limits -
	// nothing here depends on install order; both only place hooks.
	precache::install();
	scene::install();
	exporthook::install();
	// After exporthook, which owns what the Export button DOES; this owns the
	// rows on the screen that configure it.
	exportmenu::install();
}

static bool InstallAll()
{
	logger::write("info", "RockstarEditorPlus: init '%s' (base=%p)",
		ExeName(), (void*)memory::base());

	const bool ok = game::resolve();
	if (ok)
		InstallResolved();

	logger::write("info", "RockstarEditorPlus: ready");
	return ok;
}

// -----------------------------------------------------------------------------
//  One install, whichever route gets there first
// -----------------------------------------------------------------------------
//  Under FiveM there are now TWO routes in - ScriptHookV, and a fallback thread
//  for the case where ScriptHookV never runs a script at all (see below). Either
//  may arrive first, and both must not install.
//
//  A FAILED resolve does not count as done. The fallback runs early on purpose,
//  so it can find the module before it is ready; latching on that attempt would
//  leave the mod permanently dormant on a machine where waiting another second
//  would have worked. So the claim is released again and the loser is free to
//  retry.
// -----------------------------------------------------------------------------
static volatile LONG g_installBusy = 0;
static volatile LONG g_installDone = 0;

static bool InstallOnce(const char* who)
{
	if (InterlockedCompareExchange(&g_installDone, 0, 0) != 0) return true;
	// Somebody else is inside InstallAll right now - do not race them into it.
	if (InterlockedCompareExchange(&g_installBusy, 1, 0) != 0) return false;

	bool ok = false;
	if (InterlockedCompareExchange(&g_installDone, 0, 0) == 0)
	{
		logger::write("info", "RockstarEditorPlus: init via %s", who);
		ok = InstallAll();
		if (ok) InterlockedExchange(&g_installDone, 1);
	}
	else ok = true;

	InterlockedExchange(&g_installBusy, 0);
	return ok;
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
			// Through the same guard as the FiveM routes. This path cannot race
			// anything - it is the only one on Legacy and Enhanced - but going
			// around the latch would leave two ways to install and only one of
			// them recording that it had.
			InstallOnce("the kernel32 trampoline");
		}
	}
	origGetSystemTimeAsFileTime(lpSystemTimeAsFileTime);
}

// ---------------------------------------------------------------------------
//  Is the replay heap already committed by the time we get here?
// ---------------------------------------------------------------------------
//  Under FiveM the answer decides whether long recordings are reachable AT ALL.
//  Recording length is a memory budget out of one 172 MB pool, and widening that
//  pool means editing the size immediate BEFORE the allocation runs. On the
//  normal start path we are early enough. Under FiveM we are not: init is handed
//  to ScriptHookV, whose script thread did not run until 35 seconds after attach
//  in testing, by which point the pool is long since committed and the block
//  count clamps back to stock.
//
//  So the real question is whether there is a window between DLL attach and that
//  allocation. This answers it and nothing else - it is READ-ONLY on purpose.
//  Building the early-patch path before knowing the window exists would be
//  writing code that cannot work.
//
//  Deliberately free of the CRT. memory::scan opens with a function-local static
//  lambda and a std::vector, and a function-local static is exactly what faulted
//  inside msvcp140 when an earlier attempt ran the install from this thread (see
//  the note on FiveMRegisterThread). Everything here is stack arrays, raw loops
//  and WinAPI, so there is no static-init guard to trip.
// ---------------------------------------------------------------------------
namespace earlyprobe
{
	// Pattern scan over the game image. 'x' matches, '?' is a wildcard.
	static const unsigned char* rawScan(const unsigned char* pat, const char* mask, int len)
	{
		const auto* base = (const unsigned char*)GetModuleHandleA(nullptr);
		if (!base) return nullptr;

		const auto* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
		const auto* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

		const size_t size = nt->OptionalHeader.SizeOfImage;
		if (size <= (size_t)len) return nullptr;

		for (size_t i = 0; i <= size - (size_t)len; ++i)
		{
			int j = 0;
			for (; j < len; ++j)
				if (mask[j] == 'x' && base[i + j] != pat[j]) break;
			if (j == len) return base + i;
		}
		return nullptr;
	}

	// Parse a signature string into bytes+mask. Same text the normal path feeds
	// memory::scan, so the two cannot drift onto different patterns - but done
	// with stack buffers instead of a std::vector.
	static int parseSig(const char* sig, unsigned char* pat, char* mask, int cap)
	{
		auto hex = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};

		int n = 0;
		for (const char* p = sig; *p && n < cap; )
		{
			if (*p == ' ') { ++p; continue; }
			if (*p == '?')
			{
				while (*p == '?') ++p;      // '?' and '??' both mean one byte
				pat[n] = 0; mask[n] = '?'; ++n;
				continue;
			}
			const int hi = hex(*p);
			if (hi < 0) { ++p; continue; }
			const int lo = hex(p[1]);
			pat[n]  = (unsigned char)(lo < 0 ? hi : (hi << 4) | lo);
			mask[n] = 'x';
			++n;
			p += (lo < 0 ? 1 : 2);
		}
		return n;
	}

	static const unsigned char* scanSig(const char* sig)
	{
		unsigned char pat[64]{};
		char          mask[64]{};
		const int len = parseSig(sig, pat, mask, 64);
		return len ? rawScan(pat, mask, len) : nullptr;
	}

	// ReplayBlocks, straight out of the ini.
	//
	// Config cannot be used here - it is std::string all the way down and this
	// runs on the thread that must not touch the CRT - so the ini is read through
	// WinAPI instead. Section and key come from signatures.h so the two readers
	// cannot disagree about what they are reading.
	static int iniReplayBlocks()
	{
		char path[MAX_PATH]{};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   (LPCSTR)&iniReplayBlocks, &self);
		GetModuleFileNameA(self, path, MAX_PATH);

		char* slash = nullptr;
		for (char* p = path; *p; ++p) if (*p == '\\') slash = p;
		if (!slash) return gsig::REPLAY_BLOCKS_DEFAULT;
		*(slash + 1) = '\0';
		lstrcatA(path, "RockstarEditorPlus\\RockstarEditorPlus.ini");

		int n = GetPrivateProfileIntA(gsig::INI_SECTION, gsig::INI_KEY_REPLAY_BLOCKS,
		                              -1, path);

		// The mod falls back to %LOCALAPPDATA% when its own folder is not
		// writable, so look there too before assuming the default.
		if (n < 0)
		{
			char alt[MAX_PATH]{};
			if (GetEnvironmentVariableA("LOCALAPPDATA", alt, MAX_PATH))
			{
				lstrcatA(alt, "\\RockstarEditorPlus\\RockstarEditorPlus.ini");
				n = GetPrivateProfileIntA(gsig::INI_SECTION, gsig::INI_KEY_REPLAY_BLOCKS,
				                          -1, alt);
			}
		}

		// Absent means an ini written before the option existed. Use the shipped
		// default rather than doing nothing, so an older ini still benefits.
		return n < 0 ? gsig::REPLAY_BLOCKS_DEFAULT : n;
	}

	static bool patchImm32(void* at, unsigned value)
	{
		DWORD old = 0;
		if (!VirtualProtect(at, sizeof(value), PAGE_EXECUTE_READWRITE, &old)) return false;
		memcpy(at, &value, sizeof(value));
		VirtualProtect(at, sizeof(value), old, &old);
		FlushInstructionCache(GetCurrentProcess(), at, sizeof(value));
		return true;
	}

	// Widen the replay pool before the game commits it.
	//
	// The normal install does this too, and does it better - it has Config, real
	// logging and every address already resolved. This exists only because under
	// FiveM that install happens ~20-35 seconds too late. Everything here is the
	// same decision made with worse tools, so it stays conservative: it refuses
	// on anything unexpected rather than guessing, and it is a no-op unless the
	// ini actually asks for more blocks than the stock pool holds.
	static void widenReplayHeap()
	{
		const unsigned char* alloc = scanSig(gsig::REPLAY_HEAP_ALLOC_SITE.leg);
		if (!alloc)
		{
			logger::write("info",
				"RockstarEditorPlus: [FiveM] replay heap - reservation site not found, "
				"leaving the pool at stock");
			return;
		}

		// Has the pool already been committed? The allocator object is a static,
		// so its first qword is null until its constructor has run. Validate the
		// lea before trusting its displacement: a displacement read from the wrong
		// place gives a plausible pointer that only fails on dereference.
		if (alloc[0x10] != 0x48 || alloc[0x11] != 0x8D || alloc[0x12] != 0x2D)
		{
			logger::write("info",
				"RockstarEditorPlus: [FiveM] replay heap - no lea at +0x10, build "
				"drifted, leaving the pool at stock");
			return;
		}
		int32_t disp = 0;
		memcpy(&disp, alloc + 0x13, sizeof(disp));
		const void* built = *(void* const*)(alloc + 0x17 + disp);
		if (built)
		{
			logger::write("info",
				"RockstarEditorPlus: [FiveM] replay heap ALREADY BUILT at attach (%p) - "
				"too late even here; recording stays at stock length", built);
			return;
		}

		int blocks = iniReplayBlocks();
		if (blocks > gsig::REPLAY_BLOCKS_HARD_MAX) blocks = gsig::REPLAY_BLOCKS_HARD_MAX;
		if (blocks <= gsig::REPLAY_BLOCKS_SAFE_MAX)
		{
			logger::write("info",
				"RockstarEditorPlus: [FiveM] replay heap - ReplayBlocks=%d fits the "
				"stock pool, nothing to widen", blocks);
			return;
		}

		const unsigned char* ctor = scanSig(gsig::REPLAY_HEAP_CTOR_SITE.leg);
		if (!ctor)
		{
			logger::write("info",
				"RockstarEditorPlus: [FiveM] replay heap - only one of the two size "
				"sites found; both or neither, so leaving the pool at stock");
			return;
		}

		auto* allocImm = (void*)(alloc + gsig::RHA_SIZE_IMM_OFF);
		auto* ctorImm  = (void*)(ctor  + gsig::RHC_SIZE_IMM_OFF_LEG);

		// Both must currently read the stock size. If either does not, something
		// else has been here or the pattern landed off by a few bytes, and
		// writing a size into the wrong instruction is far worse than not
		// widening at all.
		unsigned a = 0, c = 0;
		memcpy(&a, allocImm, sizeof(a));
		memcpy(&c, ctorImm,  sizeof(c));
		if (a != gsig::REPLAY_HEAP_STOCK_BYTES || c != gsig::REPLAY_HEAP_STOCK_BYTES)
		{
			logger::write("info",
				"RockstarEditorPlus: [FiveM] replay heap - size immediates read "
				"0x%X / 0x%X, expected 0x%X. Not touching them.",
				a, c, gsig::REPLAY_HEAP_STOCK_BYTES);
			return;
		}

		const unsigned bytes = gsig::replayHeapBytesFor(blocks);

		// Can this machine actually give the game that reservation? Ask for it
		// ourselves and hand it straight back. The game's own attempt has no
		// failure path we survive - it never checks the null it gets from a short
		// pool - so the only safe place to find out is before it tries.
		void* probe = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
		if (!probe)
		{
			logger::write("info",
				"RockstarEditorPlus: [FiveM] replay heap - cannot reserve %u MB in this "
				"process, leaving the pool at stock", bytes / (1024u * 1024u));
			return;
		}
		VirtualFree(probe, 0, MEM_RELEASE);

		if (!patchImm32(allocImm, bytes) || !patchImm32(ctorImm, bytes))
		{
			logger::write("info",
				"RockstarEditorPlus: [FiveM] replay heap - could not write the size "
				"immediates");
			return;
		}

		// Hand the sites to the normal install, which will NOT be able to find
		// them for itself.
		//
		// The patterns match on the stock size immediate - `B9 00 00 C0 0A` is
		// literally the first five bytes of the reservation one - so the moment
		// that immediate is rewritten the pattern stops matching its own site.
		// The first version of this widened the pool correctly and then watched
		// the install report "heap size sites unresolved" and clamp the block
		// count back to 30, with a 604 MB pool sitting right there.
		//
		// Passing the addresses across is exact. Loosening the patterns to
		// wildcard the immediate would work too, but it would cost four bytes of
		// uniqueness on a pattern whose distinctiveness is mostly that immediate.
		//
		// Plain stores to zero-initialised globals - nothing here needs the CRT.
		game::addr_ReplayHeapAllocImm  = (uintptr_t)allocImm;
		game::addr_ReplayHeapCtorImm   = (uintptr_t)ctorImm;
		game::replayHeapWidenedEarly   = true;

		logger::write("info",
			"RockstarEditorPlus: [FiveM] replay heap %u -> %u MB for %d+%d blocks, "
			"patched at attach - before the game commits it",
			gsig::REPLAY_HEAP_STOCK_BYTES / (1024u * 1024u), bytes / (1024u * 1024u),
			blocks, gsig::REPLAY_TEMP_BLOCKS);
	}
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

	InstallOnce("ScriptHookV");

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

// -----------------------------------------------------------------------------
//  FiveM fallback: init without ScriptHookV ever running a script
// -----------------------------------------------------------------------------
//  scriptRegister only gets you a thread once ScriptHookV actually SCHEDULES
//  scripts, and it does not do that until the game is in a session. FiveM lets
//  you go straight from its own menu into the Rockstar Editor without ever
//  starting one - at which point the registration sits there for the whole
//  session, nothing installs, and the log stops four lines in with no error
//  because we are waiting on an event that is never coming. Every RE+ feature is
//  dead in that flow, not just the export rows, and the user is given no reason.
//
//  Confirmed in-game: entering the editor from FiveM's menu produced no init at
//  all; loading a clip started a session, ScriptHookV finally ticked, and
//  everything appeared at once.
//
//  WHY A SECOND THREAD RATHER THAN JUST DOING IT IN THE POLLER. Running init on
//  the DllMain-created thread was tried before and faulted inside msvcp140's
//  static-init machinery - see the comment on FiveMRegisterThread. That note is
//  precise about the cause: "it is THIS thread - one created inside DllMain,
//  which the CRT is entitled not to treat as fully-formed". A thread created by
//  an ordinary runtime CreateThread, from a thread that is already running, is
//  not that thread and does get its per-thread CRT set up normally.
//
//  So this races ScriptHookV rather than replacing it. ScriptHookV remains the
//  preferred route - it is a real game thread at a known-good moment - and wins
//  whenever it turns up. This only covers the case where it never will.
// -----------------------------------------------------------------------------
static DWORD WINAPI FiveMFallbackInitThread(LPVOID)
{
	// Let the process settle before the first attempt. Nothing here is urgent:
	// if ScriptHookV is going to run, it usually beats this anyway, and losing
	// the race costs nothing but a log line.
	Sleep(5000);

	// ~2 minutes of attempts. Retries only matter while resolve is still
	// failing, which on a slow or heavily-modded start means the module is not
	// finished coming up rather than that the signatures are wrong.
	for (int attempt = 0; attempt < 60; ++attempt)
	{
		if (InterlockedCompareExchange(&g_installDone, 0, 0) != 0)
			return 0;   // ScriptHookV got there first, which is the better route

		if (InstallOnce("the FiveM fallback thread (no ScriptHookV script ran)"))
			return 0;

		Sleep(2000);
	}

	logger::write("info",
		"RockstarEditorPlus: [FiveM] fallback init gave up after ~2 minutes - "
		"signatures never resolved. The mod is dormant this session.");
	return 0;
}

static DWORD WINAPI FiveMRegisterThread(LPVOID)
{
	// Before anything else, and before the wait below burns the only early
	// moment we get. Read-only, and wrapped because a fault here would take the
	// game's startup with it - the answer is worth one log line, not a crash.
	__try { earlyprobe::widenReplayHeap(); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		logger::write("info",
			"RockstarEditorPlus: [FiveM] replay heap widen faulted - ignoring, the "
			"pool stays at stock");
	}

	// Start the fallback BEFORE the poll below, and from here rather than from
	// DllMain - this thread is already running, so the one it creates gets a
	// normally-initialised CRT. See FiveMFallbackInitThread.
	if (HANDLE fb = CreateThread(nullptr, 0, FiveMFallbackInitThread, nullptr, 0, nullptr))
		CloseHandle(fb);
	else
		logger::write("info",
			"RockstarEditorPlus: [FiveM] fallback init thread could not be created (%lu) - "
			"init depends on ScriptHookV scheduling a script", GetLastError());

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


		// The logger comes up BEFORE the config, and that ordering is load-bearing:
		// init() TRUNCATES the log, and Config::load() writes to it. The one-time
		// split of the render settings into Render.ini logs itself from inside
		// migrateRenderIni, and that line was being deleted a moment after it was
		// written - on the single run it can ever appear, and the one where the
		// mod rewrites somebody's settings. Neither call depends on the other;
		// both need only paths::, which is self-contained.
		logger::init();

		// Stamp the build on the first line.
		//
		// Triaging user reports without this is guesswork: most of what a fix
		// changes only shows in the log when the thing it fixes is exercised, so
		// two builds a dozen commits apart produce identical startup logs. Every
		// "is this the latest .asi?" round trip in a bug report is this line
		// missing.
		logger::write("info", "RockstarEditorPlus: attached (build " __DATE__ " " __TIME__ ")");

		Config::get().load(hModule);

		if (paths::usingFallbackDir())
			logger::write("info",
				"RockstarEditorPlus: the folder beside the .asi is not writable, so "
				"settings, logs and renders are in %s instead. That is normal for a "
				"game installed under Program Files. Nothing needs administrator "
				"rights; set RenderOutputFolder in Render.ini if you want renders "
				"somewhere specific.",
				paths::baseDir().c_str());

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
