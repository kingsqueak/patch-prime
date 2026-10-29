// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "graphsnapshot.h"

#include <QTest>

class TestDefaultNodeName : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parses_data()
    {
        QTest::addColumn<QString>("json");
        QTest::addColumn<QString>("expected");

        QTest::newRow("sink") << QStringLiteral(
                                     R"({"name":"alsa_output.pci-0000_0b_00.6.analog-stereo"})")
                              << QStringLiteral("alsa_output.pci-0000_0b_00.6.analog-stereo");
        QTest::newRow("extra keys")
            << QStringLiteral(R"({"name":"a","x":1})") << QStringLiteral("a");
        QTest::newRow("no name") << QStringLiteral(R"({"other":"a"})") << QString();
        QTest::newRow("array") << QStringLiteral(R"(["a"])") << QString();
        QTest::newRow("not json") << QStringLiteral("alsa_output.foo") << QString();
        QTest::newRow("empty") << QString() << QString();
    }

    void parses()
    {
        QFETCH(QString, json);
        QFETCH(QString, expected);
        QCOMPARE(QString::fromStdString(pp::defaultNodeName(json.toStdString())), expected);
    }
};

QTEST_GUILESS_MAIN(TestDefaultNodeName)
#include "test_defaultnodename.moc"
