// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QString>

namespace pp {

// A settings file that exists but can't be read (broken JSON, say) is moved
// aside as "<file>.unreadable-<date>-<time>" before the store starts empty:
// otherwise the next save writes over it, and everything in it is lost. It
// stays for the user to recover by hand. Returns the new path, or an empty
// string if it couldn't be moved.
QString setAsideUnreadable(const QString &path);

} // namespace pp
