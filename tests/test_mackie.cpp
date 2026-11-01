// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "alsamidi.h"
#include "devicemodel.h"
#include "mackiecontrol.h"
#include "midiport.h"
#include "fakemidiport.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <alsa/asoundlib.h>

#include <algorithm>

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

Message fader(int channel, int value)
{
    return {static_cast<std::uint8_t>(0xE0 | channel), static_cast<std::uint8_t>(value & 0x7F),
            static_cast<std::uint8_t>(value >> 7)};
}

Message note(int n, bool on = true)
{
    return {0x90, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(on ? 0x7F : 0)};
}

} // namespace

class TestMackie : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_store = std::make_unique<DeviceStore>(m_dir->filePath(QStringLiteral("devices.json")));
        m_devices = std::make_unique<DeviceModel>(m_store.get());
        m_devices->upsertNode(node(10, "Audio/Source", "mic", "Microphone"), nullptr);
        m_devices->upsertNode(node(11, "Audio/Source", "line", "Line In"), nullptr);
        m_devices->upsertNode(node(20, "Audio/Sink", "speakers", "Speakers"), nullptr);
        for (int row = 0; row < m_devices->rowCount(); ++row) {
            m_devices->setEnabled(row, true);
        }
        m_bus = m_devices->createVirtual(QStringLiteral("Stream Mix"), EndpointKind::VirtualSink)
                    .toStdString();
    }

    void cleanup()
    {
        m_devices.reset();
        m_store.reset();
        m_dir.reset();
    }

    void faderLaw()
    {
        QCOMPARE(mackieFaderDb(0), DeviceModel::minimumVolumeDb());
        QCOMPARE(mackieFaderDb(16383), DeviceModel::maximumVolumeDb());
        QCOMPARE(mackieFaderDb(mackieFaderValue(0.0)), 0.0);
        QCOMPARE(mackieFaderValue(0.0), 12287); // three quarters up
        for (const double db : {-60.0, -45.5, -20.0, -6.0, -0.5, 3.0, 12.0}) {
            QVERIFY(std::abs(mackieFaderDb(mackieFaderValue(db)) - db) <= 0.1);
        }
        QVERIFY(mackieFaderValue(-6.0) < mackieFaderValue(-5.0));
        QCOMPARE(mackieMeterStep(-90.0), 0);
        QCOMPARE(mackieMeterStep(-60.0), 1);
        QCOMPARE(mackieMeterStep(-3.0), 10);
        QCOMPARE(mackieMeterStep(-2.0), 11);
        QCOMPARE(mackieMeterStep(0.0), 12);
    }

    void showsTheMixer()
    {
        FakePort port;
        MackieControl mackie(m_devices.get(), &port);
        QCOMPARE(mackie.strips().size(), 4U); // line, mic (by name), Stream Mix, speakers
        QCOMPARE(mackie.strips().at(2), m_bus);
        port.setConnected(true);
        // A device query, every light, every fader and the display.
        QVERIFY(port.wasSent({0xF0, 0x00, 0x00, 0x66, 0x14, 0x00, 0xF7}));
        QVERIFY(port.wasSent(note(0x10, false))); // strip 1's MUTE off
        QVERIFY(port.wasSent(fader(0, mackieFaderValue(0.0))));
        QVERIFY(port.wasSent(fader(4, 0))); // no strip 5: fader down
        const auto display = port.display();
        QCOMPARE(display.substr(0, 28), std::string("Line I Microp Stream Speake "));
        QCOMPARE(display.substr(56, 14), std::string("   0.0    0.0 "));
    }

    void movesTheMixer()
    {
        FakePort port;
        MackieControl mackie(m_devices.get(), &port);
        port.setConnected(true);
        const int mic = m_devices->rowForKey(mackie.strips().at(0));

        port.feed(fader(0, mackieFaderValue(-20.0)));
        QCOMPARE(m_devices->data(m_devices->index(mic), DeviceModel::VolumeRole).toDouble(), -20.0);
        // The fader isn't sent back to where it already is.
        QTest::qWait(30);
        QVERIFY(!port.wasSent(fader(0, mackieFaderValue(-20.0))));
        QVERIFY(port.display().substr(56, 7) == " -20.0 ");

        port.sent.clear();
        port.feed(note(0x10)); // MUTE
        port.feed(note(0x10, false));
        QVERIFY(m_devices->data(m_devices->index(mic), DeviceModel::MutedRole).toBool());
        QTRY_VERIFY(port.wasSent(note(0x10, true)));

        // The mixer moves the motor, but not while the fader is touched.
        port.sent.clear();
        port.feed(note(0x68)); // strip 1's fader touched
        m_devices->setVolume(mic, -10.0);
        QTest::qWait(30);
        QVERIFY(!port.wasSent(fader(0, mackieFaderValue(-10.0))));
        port.feed(note(0x68, false));
        QTRY_VERIFY(port.wasSent(fader(0, mackieFaderValue(-10.0))));

        // Running status: two fader moves after one status byte.
        port.feed({0xE1, 0x00, 0x00, 0x7F, 0x7F});
        const int line = m_devices->rowForKey(mackie.strips().at(1));
        QCOMPARE(m_devices->data(m_devices->index(line), DeviceModel::VolumeRole).toDouble(),
                 DeviceModel::maximumVolumeDb());

        // The master fader is the output's.
        port.feed(fader(8, mackieFaderValue(-6.0)));
        const int speakers = m_devices->rowForKey(mackie.strips().at(3));
        QCOMPARE(m_devices->data(m_devices->index(speakers), DeviceModel::VolumeRole).toDouble(), -6.0);
    }

    void vpotsSetSends()
    {
        FakePort port;
        MackieControl mackie(m_devices.get(), &port);
        port.setConnected(true);
        const int mic = m_devices->rowForKey(mackie.strips().at(0));
        const auto bus = QString::fromStdString(m_bus);

        port.feed(note(0x18)); // SELECT on strip 1, an input: not a destination
        QVERIFY(mackie.sendTarget().empty());
        port.feed(note(0x1A)); // SELECT on Stream Mix
        QCOMPARE(mackie.sendTarget(), m_bus);
        QTRY_VERIFY(port.display().starts_with("V-Pots: sends to Stream Mix"));
        QTRY_VERIFY(port.wasSent(note(0x1A, true))); // its SELECT lit

        port.feed({0xB0, 0x10, 0x04}); // the send is off: turning does nothing
        QVERIFY(!m_devices->data(m_devices->index(mic), DeviceModel::SendsRole).toStringList().contains(bus));
        port.feed(note(0x20)); // V-Pot pushed: send on
        QVERIFY(m_devices->data(m_devices->index(mic), DeviceModel::SendsRole).toStringList().contains(bus));
        port.feed({0xB0, 0x10, 0x04}); // 4 clicks clockwise
        port.feed({0xB0, 0x10, 0x41}); // 1 anticlockwise
        QCOMPARE(m_devices->data(m_devices->index(mic), DeviceModel::SendLevelsRole)
                     .toMap().value(bus).toDouble(), 1.5);
        QTRY_VERIFY(std::ranges::any_of(port.sent, [](const Message &m) {
            return m.size() == 3 && m[0] == 0xB0 && m[1] == 0x30 && (m[2] & 0x20) != 0;
        }));

        port.feed(note(0x29)); // SEND: the next destination
        QCOMPARE(mackie.sendTarget(), mackie.strips().at(3));
        port.feed(note(0x29)); // then off
        QVERIFY(mackie.sendTarget().empty());
    }

    void buttonsAndBanks()
    {
        FakePort port;
        MackieControl mackie(m_devices.get(), &port);
        QSignalSpy scenes(&mackie, &MackieControl::sceneRequested);
        port.setConnected(true);
        port.feed(note(0x38)); // F3
        QCOMPARE(scenes.size(), 1);
        QCOMPARE(scenes.at(0).at(0).toInt(), 2);

        // Fewer strips than the surface has: no banking.
        port.feed(note(0x2F));
        port.feed(note(0x31));
        QCOMPARE(mackie.bank(), 0);
        for (int i = 0; i < 6; ++i) {
            m_devices->createVirtual(QStringLiteral("Bus %1").arg(i), EndpointKind::VirtualSink);
        }
        port.feed(note(0x31)); // CHANNEL right
        QCOMPARE(mackie.bank(), 1);
        port.feed(note(0x2F)); // BANK right: as far as it goes (10 strips)
        QCOMPARE(mackie.bank(), 2);
        port.feed(note(0x2E));
        QCOMPARE(mackie.bank(), 0);

        // Cue needs a cue output.
        const int speakers = m_devices->rowForKey(mackie.strips().back());
        m_devices->setCueOutput(mackie.strips().back());
        port.feed(note(0x08)); // SOLO on strip 1
        QVERIFY(m_devices->data(m_devices->index(m_devices->rowForKey(mackie.strips().at(0))),
                                DeviceModel::CuedRole).toBool());
        static_cast<void>(speakers);
    }

    void answersTheHandshake()
    {
        FakePort port;
        MackieControl mackie(m_devices.get(), &port);
        port.setConnected(true);
        port.sent.clear();
        const Message serial{'A', 'B', 'C', 'D', 'E', 'F', 'G'};
        Message query{0xF0, 0x00, 0x00, 0x66, 0x14, 0x01};
        query.insert(query.end(), serial.begin(), serial.end());
        query.insert(query.end(), {0x11, 0x22, 0x33, 0x44, 0xF7});
        port.feed(query);
        QCOMPARE(port.sent.size(), 1U);
        const auto &reply = port.sent.front();
        QCOMPARE(reply.size(), query.size());
        QCOMPARE(reply[5], 0x02);
        QVERIFY(std::equal(serial.begin(), serial.end(), reply.begin() + 6));
        const auto r = mackieChallengeResponse({0x11, 0x22, 0x33, 0x44});
        QVERIFY(std::equal(r.begin(), r.end(), reply.begin() + 13));
        QCOMPARE(reply.back(), 0xF7);
    }

    // The real sequencer, with a stand-in controller as its own client.
    void alsaSequencer()
    {
        AlsaMidi midi;
        if (!midi.isAvailable()) {
            QSKIP("no ALSA sequencer");
        }
        snd_seq_t *seq = nullptr;
        QVERIFY(snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) >= 0);
        const auto name = QStringLiteral("PP Test Surface %1").arg(QCoreApplication::applicationPid()).toStdString();
        snd_seq_set_client_name(seq, name.c_str());
        const int port = snd_seq_create_simple_port(
            seq, "Mackie", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE |
                               SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
        QVERIFY(port >= 0);
        const auto id = name + ":Mackie";
        QTRY_VERIFY(std::ranges::any_of(midi.ports(), [&](const auto &p) { return p.id == id; }));

        QSignalSpy received(&midi, &MidiPort::received);
        midi.setTarget(id);
        QVERIFY(midi.isConnected());

        // Controller -> us.
        snd_seq_event_t event{};
        event.type = SND_SEQ_EVENT_NOTEON; // snd_seq_ev_set_noteon converts signs
        event.data.note.note = 0x10;
        event.data.note.velocity = 0x7F;
        event.source.port = static_cast<unsigned char>(port);
        snd_seq_ev_set_subs(&event);
        snd_seq_ev_set_direct(&event);
        QVERIFY(snd_seq_event_output_direct(seq, &event) >= 0);
        QTRY_VERIFY(!received.isEmpty());
        QCOMPARE(received.at(0).at(0).toByteArray(), QByteArray("\x90\x10\x7F"));

        // Us -> controller, a long sysex included.
        midi.send(Message{0x90, 0x10, 0x00});
        Message text{0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00};
        text.insert(text.end(), 56, 'x');
        text.push_back(0xF7);
        midi.send(text);
        bool gotNote = false;
        bool gotText = false;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            snd_seq_event_t *in = nullptr;
            while (snd_seq_event_input(seq, &in) >= 0 && in != nullptr) {
                if (in->type == SND_SEQ_EVENT_NOTEON && in->data.note.note == 0x10) {
                    gotNote = true;
                } else if (in->type == SND_SEQ_EVENT_SYSEX && in->data.ext.len == text.size()) {
                    gotText = true;
                }
            }
            return gotNote && gotText;
        }()), 2000);

        // The controller goes away, and comes back under the same name.
        QSignalSpy connection(&midi, &MidiPort::connectedChanged);
        snd_seq_delete_simple_port(seq, port);
        QTRY_VERIFY(!midi.isConnected());
        QVERIFY(snd_seq_create_simple_port(
                    seq, "Mackie", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
                                       SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                    SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE) >= 0);
        QTRY_VERIFY(midi.isConnected());
        snd_seq_close(seq);
        QTRY_VERIFY(!midi.isConnected());
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<DeviceStore> m_store;
    std::unique_ptr<DeviceModel> m_devices;
    std::string m_bus;
};

QTEST_GUILESS_MAIN(TestMackie)
#include "test_mackie.moc"
