// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QString>

// Plasma's on-screen display (org.kde.plasmashell /org/kde/osdService): the
// message that pops up in the middle of the screen, as for volume keys.
// Does nothing where there's no Plasma.
namespace osd {
void showText(const QString &icon, const QString &text);
}
