// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QString>

// The engine's own words reach the GUI in English: effect names, their
// parameters and options, and the built-in voice chains. The list behind
// these holds them for translation, in the "Effects" context;
// tests/test_effectstrings.cpp checks that it stays complete.

// The text in the user's language, or as it came if it isn't in the list
// (a plugin's own words). "Band 3 gain" is put together from its parts.
[[nodiscard]] QString effectText(const QString &text);

// Whether the list has it (the test's question).
[[nodiscard]] bool isEffectText(const QString &text);
