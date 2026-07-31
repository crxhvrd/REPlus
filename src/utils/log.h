// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#pragma once
#include <cstdarg>

namespace logger
{
	void init();
	void write(const char* type, const char* msg, ...);
	void log(const char* type, const char* msg, ...);
	void vwrite(const char* type, const char* msg, va_list args);
}
