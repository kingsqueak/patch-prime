// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QJsonObject>
#include <QObject>

#include <memory>

class KStatusNotifierItem;
class QAction;
class QActionGroup;
class QMenu;

// Patch Prime in the system tray: the mic's state at a glance (live or
// muted), and quick control without the window. A D-Bus client of
// patchprimed, like the app.
//
// The mic is the strip that's the system default input, else the first
// hardware input. Left click opens the window, middle click mutes or
// unmutes the mic, scrolling sets its level; the menu has the mute, the
// scenes, the soundboard's pads and the window.
class Tray : public QObject
{
    Q_OBJECT

public:
    explicit Tray(QObject *parent = nullptr);
    ~Tray() override;
    Tray(const Tray &) = delete;
    Tray &operator=(const Tray &) = delete;
    Tray(Tray &&) = delete;
    Tray &operator=(Tray &&) = delete;

    // The mic among the state's devices: {key, label, muted, volumeDb}, or
    // an empty object.
    static QJsonObject micOf(const QJsonObject &state);

private Q_SLOTS:
    void onStateChanged(const QString &state);

private:
    void fetchState();
    void apply(const QJsonObject &state);
    void setEngineRunning(bool running);
    void toggleMute();
    void nudgeLevel(int steps);
    static void openWindow();
    static void call(const QString &method, const QVariantList &args);

    std::unique_ptr<KStatusNotifierItem> m_item;
    QMenu *m_menu = nullptr;
    QAction *m_mute = nullptr;
    QMenu *m_scenes = nullptr;
    QActionGroup *m_sceneGroup = nullptr; // one scene checked at a time
    QMenu *m_pads = nullptr;
    QJsonObject m_mic;
    bool m_running = false;
};
