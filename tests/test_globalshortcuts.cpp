// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "globalshortcuts.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QSignalSpy>
#include <QTest>

// The real kglobalaccel, with a component of the test's own (removed
// afterwards), and a key nobody has: Ctrl+Alt+Shift+Meta+F35.
class TestGlobalShortcuts : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void kglobalaccel()
    {
        const auto component = QStringLiteral("patchprime_test_%1").arg(QCoreApplication::applicationPid());
        KGlobalAccelShortcuts shortcuts(component, QStringLiteral("Patch Prime test"));
        if (!shortcuts.isAvailable()) {
            QSKIP("no kglobalaccel on the session bus");
        }
        QSignalSpy keys(&shortcuts, &pp::ShortcutBackend::keysChanged);
        shortcuts.add("mute", "Mute the test");
        QTRY_VERIFY(!keys.isEmpty());
        QVERIFY(shortcuts.keys("mute").isEmpty()); // new: no keys

        const int unusedKey = QKeyCombination(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier |
                                                  Qt::MetaModifier,
                                              Qt::Key_F35)
                                  .toCombined();
        keys.clear();
        shortcuts.setKeys("mute", {unusedKey});
        QTRY_VERIFY(!keys.isEmpty());
        QCOMPARE(shortcuts.keys("mute"), QList<int>{unusedKey});

        // Pressing it (as kglobalaccel would on the key).
        QSignalSpy pressed(&shortcuts, &pp::ShortcutBackend::pressed);
        auto invoke = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),
                                                     QStringLiteral("/component/") + component,
                                                     QStringLiteral("org.kde.kglobalaccel.Component"),
                                                     QStringLiteral("invokeShortcut"));
        invoke.setArguments({QStringLiteral("mute")});
        QDBusConnection::sessionBus().call(invoke);
        QTRY_COMPARE(pressed.size(), 1);
        QCOMPARE(pressed.at(0).at(0).toString(), QStringLiteral("mute"));

        shortcuts.forgetComponent();
    }
};

QTEST_GUILESS_MAIN(TestGlobalShortcuts)
#include "test_globalshortcuts.moc"
