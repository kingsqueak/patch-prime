// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "ctl.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace pp::ctl;

namespace {

// A daemon state with a mic (two effects, sent to Stream Mix at -6 dB),
// two strips both labelled "Line", a mix bus and an output.
QJsonObject state()
{
    const auto json = QByteArrayLiteral(R"({
        "cueOutput": "out|phones",
        "scenes": ["Live", "Podcast"],
        "pads": [{"id": "p1", "label": "Applause", "playing": false}, {"id": "p2", "label": "Air Horn", "playing": true}],
        "recording": {"virtual|stream": "/tmp/x.flac"},
        "devices": [
            {"key": "in|mic", "nodeName": "mic", "label": "Mic", "stage": "input", "isEnabled": true,
             "volumeDb": -3, "muted": false, "cued": false,
             "sends": ["virtual|stream"], "sendLevels": {"virtual|stream": -6},
             "effects": [{"type": "gate", "enabled": true}, {"type": "eq", "enabled": false}]},
            {"key": "in|line1", "nodeName": "line1", "label": "Line", "stage": "input", "isEnabled": true,
             "volumeDb": 0, "muted": true, "cued": false, "sends": [], "sendLevels": {}, "effects": []},
            {"key": "in|line2", "nodeName": "line2", "label": "Line", "stage": "input", "isEnabled": true,
             "volumeDb": 0, "muted": false, "cued": false, "sends": [], "sendLevels": {}, "effects": []},
            {"key": "in|off", "nodeName": "off", "label": "Unused", "stage": "input", "isEnabled": false},
            {"key": "out|phones", "nodeName": "phones", "label": "Phones", "stage": "output", "isEnabled": true,
             "volumeDb": -60, "muted": false, "cued": false, "sends": [], "sendLevels": {}, "effects": []},
            {"key": "virtual|stream", "nodeName": "stream", "label": "Stream Mix", "stage": "bus", "isEnabled": true,
             "volumeDb": 0, "muted": false, "cued": true, "sends": [], "sendLevels": {}, "effects": []}
        ]})");
    return QJsonDocument::fromJson(json).object();
}

Plan ok(const QStringList &args)
{
    const auto result = plan(args, state());
    if (!result) {
        qWarning() << result.error();
        return {};
    }
    return *result;
}

QString error(const QStringList &args)
{
    const auto result = plan(args, state());
    return result ? QString() : result.error();
}

Call only(const QStringList &args)
{
    const auto p = ok(args);
    return p.calls.size() == 1 ? p.calls.front() : Call{};
}

} // namespace

class TestCtl : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void findsStrips()
    {
        QCOMPARE(only({QStringLiteral("mute"), QStringLiteral("mic")}).args.at(0).toString(), QStringLiteral("in|mic"));
        QCOMPARE(only({QStringLiteral("mute"), QStringLiteral("in|line2")}).args.at(0).toString(),
                 QStringLiteral("in|line2"));
        QCOMPARE(only({QStringLiteral("mute"), QStringLiteral("line1")}).args.at(0).toString(),
                 QStringLiteral("in|line1")); // node name
        QVERIFY(error({QStringLiteral("mute"), QStringLiteral("Line")}).contains(QStringLiteral("in|line1, in|line2")));
        QVERIFY(error({QStringLiteral("mute"), QStringLiteral("Unused")}).startsWith(QStringLiteral("no strip")));
        QVERIFY(error({QStringLiteral("frobnicate"), QStringLiteral("Mic")}).startsWith(QStringLiteral("unknown")));
        QVERIFY(!error({}).isEmpty());
    }

    void switches()
    {
        auto c = only({QStringLiteral("mute"), QStringLiteral("Mic")});
        QCOMPARE(c.method, QStringLiteral("SetMuted"));
        QCOMPARE(c.args.at(1).toBool(), true); // toggled
        QCOMPARE(only({QStringLiteral("mute"), QStringLiteral("line1"), QStringLiteral("toggle")}).args.at(1).toBool(),
                 false);
        QCOMPARE(only({QStringLiteral("mute"), QStringLiteral("Mic"), QStringLiteral("off")}).args.at(1).toBool(),
                 false);
        QVERIFY(error({QStringLiteral("mute"), QStringLiteral("Mic"), QStringLiteral("maybe")}).contains(QStringLiteral("on, off")));

        c = only({QStringLiteral("cue"), QStringLiteral("Stream Mix")});
        QCOMPARE(c.method, QStringLiteral("SetCue"));
        QCOMPARE(c.args.at(1).toBool(), false);

        c = only({QStringLiteral("send"), QStringLiteral("Mic"), QStringLiteral("stream mix")});
        QCOMPARE(c.method, QStringLiteral("SetSend"));
        QCOMPARE(c.args, (QVariantList{QStringLiteral("in|mic"), QStringLiteral("virtual|stream"), false}));

        c = only({QStringLiteral("effect"), QStringLiteral("Mic"), QStringLiteral("2")});
        QCOMPARE(c.args, (QVariantList{QStringLiteral("in|mic"), 1, true}));
        QVERIFY(!error({QStringLiteral("effect"), QStringLiteral("Mic"), QStringLiteral("3")}).isEmpty());

        // Recording: starting answers with an error message.
        c = only({QStringLiteral("record"), QStringLiteral("Mic")});
        QCOMPARE(c.method, QStringLiteral("StartRecording"));
        QVERIFY(c.returnsError);
        QCOMPARE(only({QStringLiteral("record"), QStringLiteral("Stream Mix")}).method, QStringLiteral("StopRecording"));
        QVERIFY(ok({QStringLiteral("record"), QStringLiteral("Stream Mix"), QStringLiteral("on")}).calls.empty());
    }

    void levels()
    {
        QCOMPARE(only({QStringLiteral("volume"), QStringLiteral("Mic"), QStringLiteral("-12")}).args.at(1).toDouble(), -12.0);
        QCOMPARE(only({QStringLiteral("volume"), QStringLiteral("Mic"), QStringLiteral("+2dB")}).args.at(1).toDouble(), 2.0);
        QCOMPARE(only({QStringLiteral("volume"), QStringLiteral("Mic"), QStringLiteral("up")}).args.at(1).toDouble(), -2.0);
        QCOMPARE(only({QStringLiteral("volume"), QStringLiteral("Mic"), QStringLiteral("down"), QStringLiteral("3")})
                     .args.at(1)
                     .toDouble(),
                 -6.0);
        QVERIFY(!error({QStringLiteral("volume"), QStringLiteral("Mic"), QStringLiteral("loud")}).isEmpty());
        QVERIFY(!error({QStringLiteral("volume"), QStringLiteral("Mic")}).isEmpty());

        const auto c = only({QStringLiteral("send-level"), QStringLiteral("Mic"), QStringLiteral("Stream Mix"),
                             QStringLiteral("up"), QStringLiteral("2")});
        QCOMPARE(c.method, QStringLiteral("SetSendLevel"));
        QCOMPARE(c.args.at(2).toDouble(), -4.0);
        QVERIFY(error({QStringLiteral("send-level"), QStringLiteral("Line1"), QStringLiteral("Stream Mix"),
                       QStringLiteral("0")})
                    .contains(QStringLiteral("isn't sent")));
    }

    void pads()
    {
        const auto c = only({QStringLiteral("pad"), QStringLiteral("air"), QStringLiteral("horn")});
        QCOMPARE(c.method, QStringLiteral("TriggerPad"));
        QCOMPARE(c.args.at(0).toString(), QStringLiteral("p2"));
        QVERIFY(c.returnsError);
        QVERIFY(!error({QStringLiteral("pad"), QStringLiteral("Boo")}).isEmpty());
        QCOMPARE(ok({QStringLiteral("pads")}).output, QStringLiteral("Applause\nAir Horn  (playing)\n"));
        QCOMPARE(only({QStringLiteral("pads"), QStringLiteral("stop")}).method, QStringLiteral("StopAllPads"));
    }

    void scenesAndReading()
    {
        QCOMPARE(only({QStringLiteral("scene"), QStringLiteral("podcast")}).args.at(0).toString(), QStringLiteral("Podcast"));
        QVERIFY(!error({QStringLiteral("scene"), QStringLiteral("Nope")}).isEmpty());
        QCOMPARE(ok({QStringLiteral("scenes")}).output, QStringLiteral("Live\nPodcast\n"));

        QCOMPARE(ok({QStringLiteral("get"), QStringLiteral("Mic"), QStringLiteral("volume")}).output, QStringLiteral("-3.0\n"));
        QCOMPARE(ok({QStringLiteral("get"), QStringLiteral("phones"), QStringLiteral("volume")}).output, QStringLiteral("-inf\n"));
        QCOMPARE(ok({QStringLiteral("get"), QStringLiteral("line1"), QStringLiteral("muted")}).output, QStringLiteral("yes\n"));

        // In the mixer's order: inputs, mix buses, outputs; enabled only.
        const auto lines = ok({QStringLiteral("list")}).output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 5);
        QVERIFY(lines.at(3).startsWith(QStringLiteral("Stream Mix")));
        QVERIFY(lines.at(3).endsWith(QStringLiteral("cued")));
        const auto json = QJsonDocument::fromJson(ok({QStringLiteral("list"), QStringLiteral("--json")}).output.toUtf8());
        QCOMPARE(json.array().size(), 5);
        QCOMPARE(json.array().at(0).toObject().value(QLatin1String("key")).toString(), QStringLiteral("in|mic"));
        // Nothing to call for reading.
        QVERIFY(ok({QStringLiteral("list")}).calls.empty());
    }
};

QTEST_GUILESS_MAIN(TestCtl)
#include "test_ctl.moc"
