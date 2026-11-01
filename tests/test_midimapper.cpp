// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicemodel.h"
#include "fakemidiport.h"
#include "mackiecontrol.h"
#include "midimapper.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace pp;

namespace {

GraphObject node(std::uint32_t id, const std::string &mediaClass, const std::string &name,
                 const std::string &description)
{
    return {.id = id,
            .type = "PipeWire:Interface:Node",
            .version = 3,
            .props = {{"media.class", mediaClass},
                      {"node.name", name},
                      {"node.description", description},
                      {"audio.channels", "2"},
                      {"audio.position", "[ FL, FR ]"}}};
}

Message cc(int number, int value, int channel = 0)
{
    return {static_cast<std::uint8_t>(0xB0 | channel), static_cast<std::uint8_t>(number),
            static_cast<std::uint8_t>(value)};
}

Message note(int number, bool on = true)
{
    return {0x90, static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(on ? 100 : 0)};
}

int ccFor(double db)
{
    return static_cast<int>(std::lround(faderLawPosition(db) * 127));
}

} // namespace

class TestMidiMapper : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_store = std::make_unique<DeviceStore>(m_dir->filePath(QStringLiteral("devices.json")));
        m_devices = std::make_unique<DeviceModel>(m_store.get());
        m_devices->upsertNode(node(10, "Audio/Source", "mic", "Mic"), nullptr);
        m_devices->setEnabled(0, true);
        m_mic = m_devices->rows().at(0).endpoint.key;
        m_bus = m_devices->createVirtual(QStringLiteral("Stream Mix"), EndpointKind::VirtualSink).toStdString();
    }

    void cleanup()
    {
        m_devices.reset();
        m_store.reset();
        m_dir.reset();
    }

    void json()
    {
        const MidiMapping m{.id = "x",
                            .kind = MidiKind::Cc,
                            .channel = 2,
                            .number = 7,
                            .relative = true,
                            .target = {.action = MidiAction::EffectParam, .key = "k", .effect = 1, .param = "gainDb"}};
        QCOMPARE(midiMappingFromJson(midiMappingToJson(m)), std::optional{m});
        // Incomplete targets are refused.
        QVERIFY(!midiTargetFromJson({{QStringLiteral("action"), QStringLiteral("send")},
                                     {QStringLiteral("key"), QStringLiteral("k")}}));
        QVERIFY(!midiTargetFromJson({{QStringLiteral("action"), QStringLiteral("dance")}}));
        QCOMPARE(describeMidiControl(m), std::string("CC 7, channel 3"));
        QCOMPARE(describeMidiControl({.kind = MidiKind::Note, .number = 60}), std::string("Note C3, channel 1"));
    }

    void learnsAndPicksUp()
    {
        FakePort port;
        MidiMapper mapper(m_devices.get(), &port);
        const int row = m_devices->rowForKey(m_mic);
        const MidiTarget level{.action = MidiAction::Volume, .key = m_mic};
        QCOMPARE(mapper.describe(level), std::string("Mic: level"));

        mapper.startLearn(level);
        port.feed(cc(7, 10));
        QVERIFY(!mapper.learning());
        QCOMPARE(m_store->midiMappings().size(), 1U);
        QCOMPARE(m_store->midiMappings().front().number, 7);
        QCOMPARE(volume(row), 0.0); // learning doesn't move anything

        // Pickup: the knob is far below 0 dB, so it doesn't take over...
        port.feed(cc(7, 20));
        QCOMPARE(volume(row), 0.0);
        // ...until it passes the current level.
        port.feed(cc(7, 120));
        QVERIFY(volume(row) > 6.0);
        port.feed(cc(7, ccFor(-20.0)));
        QVERIFY(std::abs(volume(row) + 20.0) < 0.5);

        // Moved in the app: the knob has to come back to it first.
        m_devices->setVolume(row, 0.0);
        port.feed(cc(7, ccFor(-20.0) - 2));
        QCOMPARE(volume(row), 0.0);

        // Learning the same knob for something else moves it there; learning
        // another knob for the level replaces the old one.
        mapper.startLearn({.action = MidiAction::Volume, .key = m_bus});
        port.feed(cc(7, 0));
        QCOMPARE(m_store->midiMappings().size(), 1U);
        QCOMPARE(m_store->midiMappings().front().target.key, m_bus);
        mapper.startLearn({.action = MidiAction::Volume, .key = m_bus});
        port.feed(cc(8, 0));
        QCOMPARE(m_store->midiMappings().size(), 1U);
        QCOMPARE(m_store->midiMappings().front().number, 8);

        // Learning waits for a press, not a release, and can be cancelled.
        mapper.startLearn(level);
        port.feed(note(40, false));
        QVERIFY(mapper.learning());
        mapper.cancelLearn();
        QVERIFY(!mapper.learning());
    }

    void relativeEncoders()
    {
        FakePort port;
        MidiMapper mapper(m_devices.get(), &port);
        const int row = m_devices->rowForKey(m_mic);
        mapper.startLearn({.action = MidiAction::Volume, .key = m_mic});
        port.feed(cc(16, 1));
        mapper.setRelative(m_store->midiMappings().front().id, true);
        port.feed(cc(16, 0x02)); // 2 clicks up: +1 dB
        QCOMPARE(volume(row), 1.0);
        port.feed(cc(16, 0x41)); // 1 click down
        QCOMPARE(volume(row), 0.5);
    }

    void buttonsToggle()
    {
        FakePort port;
        MidiMapper mapper(m_devices.get(), &port);
        const int row = m_devices->rowForKey(m_mic);
        mapper.startLearn({.action = MidiAction::Mute, .key = m_mic});
        port.feed(note(36));
        QVERIFY(!muted(row)); // the learning press does nothing else
        port.feed(note(36, false));
        port.feed(note(36));
        QVERIFY(muted(row));
        port.feed(note(36, false));
        QVERIFY(muted(row));
        port.feed(note(36));
        QVERIFY(!muted(row));

        // A CC button: once per press (127 then 0).
        mapper.startLearn({.action = MidiAction::Send, .key = m_mic, .destination = m_bus});
        port.feed(cc(20, 127));
        port.feed(cc(20, 0));
        port.feed(cc(20, 127));
        QVERIFY(sends(row).contains(QString::fromStdString(m_bus)));
        port.feed(cc(20, 127)); // still held: nothing
        QVERIFY(sends(row).contains(QString::fromStdString(m_bus)));

        // Scenes.
        QSignalSpy scenes(&mapper, &MidiMapper::sceneRequested);
        mapper.startLearn({.action = MidiAction::Scene, .scene = "Live"});
        port.feed(note(50));
        port.feed(note(50));
        QCOMPARE(scenes.size(), 1);
        QCOMPARE(scenes.at(0).at(0).toString(), QStringLiteral("Live"));
    }

    // On-screen messages: for buttons, not for faders.
    void onScreen()
    {
        FakePort port;
        MidiMapper mapper(m_devices.get(), &port);
        QSignalSpy shown(&mapper, &MidiMapper::feedback);
        mapper.startLearn({.action = MidiAction::Cue, .key = m_mic});
        port.feed(note(36));
        mapper.startLearn({.action = MidiAction::Volume, .key = m_mic});
        port.feed(cc(7, ccFor(0.0)));
        QCOMPARE(shown.size(), 0); // learning shows nothing
        port.feed(cc(7, ccFor(-10.0)));
        QCOMPARE(shown.size(), 0); // a fader: nothing
        // Cue needs a hardware output to play to.
        m_devices->upsertNode(node(20, "Audio/Sink", "phones", "Phones"), nullptr);
        m_devices->setEnabled(m_devices->rowForKey("Audio/Sink|phones"), true);
        m_devices->setCueOutput("Audio/Sink|phones");
        port.feed(note(36));
        QCOMPARE(shown.size(), 1);
        QCOMPARE(shown.at(0).at(1).toString(), QStringLiteral("Mic cued"));
    }

    void feedback()
    {
        FakePort port;
        MidiMapper mapper(m_devices.get(), &port);
        port.setConnected(true);
        const int row = m_devices->rowForKey(m_mic);
        mapper.startLearn({.action = MidiAction::Mute, .key = m_mic});
        port.feed(note(36));
        mapper.startLearn({.action = MidiAction::Volume, .key = m_mic});
        port.feed(cc(7, ccFor(0.0)));
        QTRY_VERIFY(port.wasSent({0x90, 36, 0})); // the button's light, off
        port.sent.clear();

        m_devices->setMuted(row, true);
        m_devices->setVolume(row, -20.0);
        QTRY_VERIFY(port.wasSent({0x90, 36, 127}));
        QTRY_VERIFY(port.wasSent(cc(7, ccFor(-20.0))));

        // Not echoed back to where the knob already is.
        port.feed(cc(7, ccFor(-20.0) + 1));
        port.sent.clear();
        port.feed(cc(7, ccFor(-10.0)));
        QTest::qWait(50);
        QVERIFY(!port.wasSent(cc(7, ccFor(-10.0))));
    }

    void effectsFollowTheChain()
    {
        FakePort port;
        MidiMapper mapper(m_devices.get(), &port);
        const int row = m_devices->rowForKey(m_mic);
        m_devices->addEffect(row, QStringLiteral("eq"));
        m_devices->addEffect(row, QStringLiteral("gain"));
        const MidiTarget gain{.action = MidiAction::EffectParam, .key = m_mic, .effect = 1, .param = "gainDb"};
        QCOMPARE(mapper.describe(gain), std::string("Mic: Gain Gain"));
        mapper.startLearn(gain);
        port.feed(cc(1, 64));
        port.feed(cc(1, 127)); // crosses 0 dB (the middle): picked up
        QCOMPARE(param(row, 1, "gainDb"), 24.0);

        // A switch parameter is a button.
        mapper.startLearn({.action = MidiAction::EffectParam, .key = m_mic, .effect = 0, .param = "b1On"});
        port.feed(note(60));
        const double before = param(row, 0, "b1On");
        port.feed(note(60));
        QCOMPARE(param(row, 0, "b1On"), 1.0 - before);

        // Moving the gain first: its mapping goes with it.
        m_devices->moveEffect(row, 1, 0);
        const auto moved = std::ranges::find(m_store->midiMappings(), "gainDb",
                                             [](const MidiMapping &m) { return m.target.param; });
        QCOMPARE(moved->target.effect, 0);
        // Removing it drops its mapping; the EQ's moves back to 0.
        m_devices->removeEffect(row, 0);
        QCOMPARE(m_store->midiMappings().size(), 1U);
        QCOMPARE(m_store->midiMappings().front().target.effect, 0);
        QCOMPARE(m_store->midiMappings().front().target.param, std::string("b1On"));
    }

    void savedWithTheDevices()
    {
        FakePort port;
        {
            MidiMapper mapper(m_devices.get(), &port);
            mapper.startLearn({.action = MidiAction::Cue, .key = m_mic});
            port.feed(note(1));
            m_devices->setMidiController("Some Controller:Port 1");
        }
        QVERIFY(m_store->save());
        DeviceStore again(m_store->path());
        QVERIFY(again.load());
        QCOMPARE(again.midiMappings(), m_store->midiMappings());
        QCOMPARE(again.midiController(), std::string("Some Controller:Port 1"));
        // Forgetting the strip forgets its mappings.
        QVERIFY(again.remove(m_mic));
        QVERIFY(again.midiMappings().empty());
    }

private:
    [[nodiscard]] double volume(int row) const { return m_devices->data(m_devices->index(row), DeviceModel::VolumeRole).toDouble(); }
    [[nodiscard]] bool muted(int row) const { return m_devices->data(m_devices->index(row), DeviceModel::MutedRole).toBool(); }
    [[nodiscard]] QStringList sends(int row) const
    {
        return m_devices->data(m_devices->index(row), DeviceModel::SendsRole).toStringList();
    }
    [[nodiscard]] double param(int row, int effect, const char *name) const
    {
        const auto effects = m_devices->data(m_devices->index(row), DeviceModel::EffectsRole).toList();
        return effects.at(effect).toMap().value(QStringLiteral("params")).toMap().value(QLatin1String(name)).toDouble();
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<DeviceStore> m_store;
    std::unique_ptr<DeviceModel> m_devices;
    std::string m_mic;
    std::string m_bus;
};

QTEST_GUILESS_MAIN(TestMidiMapper)
#include "test_midimapper.moc"
