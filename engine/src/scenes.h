// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <string>

namespace pp {

class AppRouter;
class AppStore;
class DeviceModel;
class DeviceStore;
class PipeWireGraph;

// A scene is the mix: every strip's level, mute, mono, sends and send levels
// (mixToJson; the effects chain will join there), every app's route, and the
// system default output and input. Loading one changes only those: it never
// creates, deletes or restarts anything, so it's safe mid-stream.
//
// {"version": 1, "name": "...", "strips": {key: mix}, "apps": {appKey: target},
//  "defaults": {"output": nodeName, "input": nodeName}}
constexpr int sceneVersion = 1;

QJsonObject captureScene(const QString &name, const DeviceStore &devices, const AppStore &apps,
                         const std::string &defaultOutput, const std::string &defaultInput);

// Applies a scene. Strips, apps and defaults it doesn't mention stay as
// they are; ones that no longer exist are skipped. graph may be null (tests):
// then the defaults are left alone. Returns how many strips it set.
int applyScene(const QJsonObject &scene, DeviceModel &devices, AppRouter *apps,
               PipeWireGraph *graph);

// ~/.config/patchprime/scenes/<file>.json, one per scene, listed by the name
// inside. Saving under an existing name replaces that scene.
class SceneStore
{
public:
    // An empty path means the standard folder under ~/.config/patchprime.
    explicit SceneStore(QString dir = {});

    [[nodiscard]] const QString &dir() const { return m_dir; }
    [[nodiscard]] QStringList names() const;
    bool save(const QJsonObject &scene);
    [[nodiscard]] QJsonObject load(const QString &name) const; // empty when missing
    bool remove(const QString &name);

    static QString defaultDir();

private:
    [[nodiscard]] QString fileFor(const QString &name) const;

    QString m_dir;
};

} // namespace pp
