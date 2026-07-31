// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "log.h"
#include "paths.h"

// Self-contained logger. Writes into the mod's own folder rather than the game
// root. No config dependency (keeps the memory.h drop-in from RageOpenV happy,
// which only needs logger::write / logger::log).
//
// Resolved once, lazily: paths::baseDir() creates the folder, and doing that at
// static-init time would run before the loader has finished with us.
static const std::string& LogPath()
{
	static const std::string p = [] {
		std::string np = paths::file("RockstarEditorPlus.log");
		paths::migrate("RockstarEditorPlus.log", np);
		return np;
	}();
	return p;
}

void logger::init()
{
	if (std::filesystem::exists(LogPath().c_str()))
	{
		std::filesystem::remove(LogPath().c_str());
	}
}

void logger::vwrite(const char* type, const char* msg, va_list args)
{
	char buffer[512]{ 0 };
	char timeBuffer[32]{ 0 };

	SYSTEMTIME st;
	GetLocalTime(&st);

	snprintf(timeBuffer, sizeof(timeBuffer),
		"%02d:%02d:%02d.%03d",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	int offset = snprintf(buffer, sizeof(buffer), "[%s][%s] ", timeBuffer, type);

	if (offset < 0 || offset >= (int)sizeof(buffer))
		offset = sizeof(buffer) - 1;

	vsnprintf(buffer + offset, sizeof(buffer) - offset, msg, args);

	printf("%s\n", buffer);

	std::ofstream logFile(LogPath().c_str(), std::ofstream::out | std::ofstream::app);
	logFile << buffer << '\n';
}

void logger::write(const char* type, const char* msg, ...)
{
	va_list args;
	va_start(args, msg);
	vwrite(type, msg, args);
	va_end(args);
}

void logger::log(const char* type, const char* msg, ...)
{
#ifdef _LOG
	va_list args;
	va_start(args, msg);
	vwrite(type, msg, args);
	va_end(args);
#endif
}
