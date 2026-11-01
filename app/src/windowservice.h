// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QObject>

class QWindow;

// One window at a time: the first app instance owns
// org.kingsqueak.PatchPrime.Window on the session bus; starting the app
// again (from the menu, or the tray icon) brings that window forward
// instead of opening another.
class WindowService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kingsqueak.PatchPrime.Window1")

public:
    static constexpr auto serviceName = "org.kingsqueak.PatchPrime.Window";
    static constexpr auto objectPath = "/Window";

    // True when this instance now owns the name; otherwise asks the one
    // that does to come forward.
    static bool claimOrActivate();

    void setWindow(QWindow *window) { m_window = window; }

public Q_SLOTS: // NOLINT(readability-redundant-access-specifiers): moc's slots section
    void Activate();

private:
    QWindow *m_window = nullptr;
};
