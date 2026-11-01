// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "hotkeys.h"

#include <QDBusServiceWatcher>

#include <map>
#include <string>

// Global shortcuts through KDE's kglobalaccel (org.kde.kglobalaccel on the
// session bus), as KGlobalAccel does it, without Qt GUI: each hotkey is an
// action of the component "patchprime" ("Patch Prime" in System Settings →
// Shortcuts, where its keys can be changed too). kglobalaccel keeps the
// keys. Works on X11 and Wayland.
class KGlobalAccelShortcuts : public pp::ShortcutBackend
{
    Q_OBJECT

public:
    // component: the component's unique name (the tests use their own).
    explicit KGlobalAccelShortcuts(QString component = QStringLiteral("patchprime"),
                                   QString friendlyName = QStringLiteral("Patch Prime"),
                                   QObject *parent = nullptr);
    // Marks the shortcuts inactive (shown as such while the engine is off).
    ~KGlobalAccelShortcuts() override;
    KGlobalAccelShortcuts(const KGlobalAccelShortcuts &) = delete;
    KGlobalAccelShortcuts &operator=(const KGlobalAccelShortcuts &) = delete;
    KGlobalAccelShortcuts(KGlobalAccelShortcuts &&) = delete;
    KGlobalAccelShortcuts &operator=(KGlobalAccelShortcuts &&) = delete;

    [[nodiscard]] bool isAvailable() const override { return m_available; }
    void add(const std::string &id, const std::string &name) override;
    void remove(const std::string &id) override;
    void setKeys(const std::string &id, const QList<int> &keys) override;
    [[nodiscard]] QList<int> keys(const std::string &id) const override;

    // Removes the component and all its shortcuts from kglobalaccel (the
    // tests' clean-up).
    void forgetComponent();

private Q_SLOTS:
    void onPressed(const QString &component, const QString &action, qlonglong timestamp);
    void onRepeated(const QString &component, const QString &action, qlonglong timestamp);
    void onReleased(const QString &component, const QString &action, qlonglong timestamp);
    void onKeysChanged(const QStringList &actionId, const QList<int> &keys);

private:
    [[nodiscard]] QStringList actionId(const std::string &id) const;
    void setShortcut(const std::string &id, const QList<int> &keys, uint flags);

    QString m_component;
    QString m_friendlyName;
    bool m_available = false;
    std::map<std::string, std::string> m_names; // id -> name
    std::map<std::string, QList<int>> m_keys;
    QDBusServiceWatcher m_watcher;
};
