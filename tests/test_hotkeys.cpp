// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicemodel.h"
#include "hotkeys.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace pp;

namespace {

// The desktop's shortcuts, as far as the tests need them.
class FakeShortcuts : public ShortcutBackend
{
public:
    [[nodiscard]] bool isAvailable() const override { return true; }
    void add(const std::string &id, const std::string &name) override { names[id] = name; }
    void remove(const std::string &id) override
    {
        names.erase(id);
        m_keys.erase(id);
    }
    void setKeys(const std::string &id, const QList<int> &keys) override
    {
        m_keys[id] = keys;
        Q_EMIT keysChanged(QString::fromStdString(id));
    }
    [[nodiscard]] QList<int> keys(const std::string &id) const override
    {
        const auto at = m_keys.find(id);
        return at == m_keys.end() ? QList<int>{} : at->second;
    }

    void press(const std::string &id) { Q_EMIT pressed(QString::fromStdString(id)); }
    void repeat(const std::string &id) { Q_EMIT repeated(QString::fromStdString(id)); }
    void release(const std::string &id) { Q_EMIT released(QString::fromStdString(id)); }

    std::map<std::string, std::string> names; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes): for the tests to read

private:
    std::map<std::string, QList<int>> m_keys;
};

GraphObject source(std::uint32_t id, const std::string &name, const std::string &description)
{
    return {.id = id,
            .type = "PipeWire:Interface:Node",
            .version = 3,
            .props = {{"media.class", "Audio/Source"},
                      {"node.name", name},
                      {"node.description", description},
                      {"audio.channels", "2"},
                      {"audio.position", "[ FL, FR ]"}}};
}

MidiTarget target(MidiAction action, const std::string &key)
{
    MidiTarget t;
    t.action = action;
    t.key = key;
    return t;
}

} // namespace

class TestHotkeys : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_store = std::make_unique<DeviceStore>(m_dir->filePath(QStringLiteral("devices.json")));
        m_devices = std::make_unique<DeviceModel>(m_store.get());
        m_devices->upsertNode(source(10, "mic", "Mic"), nullptr);
        m_devices->setEnabled(0, true);
        m_mic = m_devices->rows().at(0).endpoint.key;
    }

    void cleanup()
    {
        m_devices.reset();
        m_store.reset();
        m_dir.reset();
    }

    void json()
    {
        Hotkey h;
        h.id = "x";
        h.mode = HotkeyMode::PushToTalk;
        h.target = target(MidiAction::Mute, "k");
        QCOMPARE(hotkeyFromJson(hotkeyToJson(h)), std::optional{h});
        // A mode has to suit its target.
        h.target.action = MidiAction::Cue;
        QVERIFY(!hotkeyFromJson(hotkeyToJson(h)));
        h.mode = HotkeyMode::Toggle;
        QVERIFY(hotkeyFromJson(hotkeyToJson(h)));
        h.target.action = MidiAction::Volume;
        QVERIFY(!hotkeyFromJson(hotkeyToJson(h)));
    }

    void registersAndRenames()
    {
        FakeShortcuts shortcuts;
        Hotkeys hotkeys(m_devices.get(), &shortcuts);
        QVERIFY(hotkeys.add(HotkeyMode::PushToTalk, target(MidiAction::Cue, m_mic)).empty());
        const auto id = hotkeys.add(HotkeyMode::PushToTalk, target(MidiAction::Mute, m_mic));
        QVERIFY(!id.empty());
        QCOMPARE(shortcuts.names.at(id), std::string("Patch Prime: Push to talk: Mic"));
        hotkeys.setKeys(id, {0x4000041}); // Ctrl+A, say
        QCOMPARE(hotkeys.keys(id), QList<int>{0x4000041});

        m_devices->setLabel(0, QStringLiteral("Podcast Mic"));
        QCOMPARE(shortcuts.names.at(id), std::string("Patch Prime: Push to talk: Podcast Mic"));
        hotkeys.remove(id);
        QVERIFY(shortcuts.names.empty());
    }

    void holdsAndToggles()
    {
        FakeShortcuts shortcuts;
        Hotkeys hotkeys(m_devices.get(), &shortcuts);
        const auto ptt = hotkeys.add(HotkeyMode::PushToTalk, target(MidiAction::Mute, m_mic));
        const auto ptm = hotkeys.add(HotkeyMode::PushToMute, target(MidiAction::Mute, m_mic));
        const auto toggle = hotkeys.add(HotkeyMode::Toggle, target(MidiAction::Mute, m_mic));

        shortcuts.press(ptt);
        QVERIFY(!muted());
        shortcuts.release(ptt);
        QVERIFY(muted());
        shortcuts.press(ptm);
        shortcuts.release(ptm);
        QVERIFY(!muted());
        shortcuts.press(ptm);
        QVERIFY(muted());
        shortcuts.release(ptm);

        shortcuts.press(toggle);
        QVERIFY(muted());
        shortcuts.repeat(toggle); // held: no flapping
        shortcuts.release(toggle);
        QVERIFY(muted());
        shortcuts.press(toggle);
        QVERIFY(!muted());

        MidiTarget scene;
        scene.action = MidiAction::Scene;
        scene.scene = "Live";
        const auto sceneKey = hotkeys.add(HotkeyMode::Toggle, scene);
        QSignalSpy scenes(&hotkeys, &Hotkeys::sceneRequested);
        shortcuts.press(sceneKey);
        QCOMPARE(scenes.size(), 1);
        QCOMPARE(scenes.at(0).at(0).toString(), QStringLiteral("Live"));
    }

    void ducksAndSteps()
    {
        FakeShortcuts shortcuts;
        Hotkeys hotkeys(m_devices.get(), &shortcuts);
        const auto duck = hotkeys.add(HotkeyMode::Duck, target(MidiAction::Volume, m_mic));
        const auto up = hotkeys.add(HotkeyMode::LevelUp, target(MidiAction::Volume, m_mic));
        const auto down = hotkeys.add(HotkeyMode::LevelDown, target(MidiAction::Volume, m_mic));

        m_devices->setVolume(0, -3.0);
        shortcuts.press(duck);
        QCOMPARE(volume(), -3.0 - Hotkeys::duckDb);
        shortcuts.release(duck);
        QCOMPARE(volume(), -3.0);
        // Moved while ducked: left where it was put.
        shortcuts.press(duck);
        m_devices->setVolume(0, -30.0);
        shortcuts.release(duck);
        QCOMPARE(volume(), -30.0);

        shortcuts.press(up);
        shortcuts.repeat(up);
        shortcuts.repeat(up);
        QCOMPARE(volume(), -27.0);
        shortcuts.press(down);
        QCOMPARE(volume(), -28.0);
    }

    // What's shown on screen when a key changes something.
    void feedback()
    {
        FakeShortcuts shortcuts;
        Hotkeys hotkeys(m_devices.get(), &shortcuts);
        QSignalSpy shown(&hotkeys, &Hotkeys::feedback);
        const auto mute = hotkeys.add(HotkeyMode::Toggle, target(MidiAction::Mute, m_mic));
        const auto ptt = hotkeys.add(HotkeyMode::PushToTalk, target(MidiAction::Mute, m_mic));
        const auto up = hotkeys.add(HotkeyMode::LevelUp, target(MidiAction::Volume, m_mic));

        shortcuts.press(mute);
        QCOMPARE(shown.size(), 1);
        QCOMPARE(shown.at(0).at(0).toString(), QStringLiteral("microphone-sensitivity-muted")); // an input
        QCOMPARE(shown.at(0).at(1).toString(), QStringLiteral("Mic muted"));
        shortcuts.press(mute);
        QCOMPARE(shown.at(1).at(1).toString(), QStringLiteral("Mic on"));

        shortcuts.press(ptt); // push to talk: the tray shows it, no message
        shortcuts.release(ptt);
        QCOMPARE(shown.size(), 2);

        m_devices->setVolume(0, -4.0);
        shortcuts.press(up);
        QCOMPARE(shown.at(2).at(1).toString(), QStringLiteral("Mic: level -3.0 dB"));
    }

    void followTheStrips()
    {
        FakeShortcuts shortcuts;
        Hotkeys hotkeys(m_devices.get(), &shortcuts);
        m_devices->addEffect(0, QStringLiteral("gain"));
        MidiTarget effect = target(MidiAction::EffectEnabled, m_mic);
        effect.effect = 0;
        const auto id = hotkeys.add(HotkeyMode::Toggle, effect);
        QCOMPARE(m_store->hotkeys().size(), 1U);
        m_devices->removeEffect(0, 0);
        QVERIFY(m_store->hotkeys().empty());
        QVERIFY(!shortcuts.names.contains(id));

        hotkeys.add(HotkeyMode::Toggle, target(MidiAction::Cue, m_mic));
        QVERIFY(m_store->save());
        DeviceStore again(m_store->path());
        QVERIFY(again.load());
        QCOMPARE(again.hotkeys(), m_store->hotkeys());
        QVERIFY(again.remove(m_mic));
        QVERIFY(again.hotkeys().empty());
    }

private:
    [[nodiscard]] bool muted() const { return m_devices->data(m_devices->index(0), DeviceModel::MutedRole).toBool(); }
    [[nodiscard]] double volume() const
    {
        return m_devices->data(m_devices->index(0), DeviceModel::VolumeRole).toDouble();
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<DeviceStore> m_store;
    std::unique_ptr<DeviceModel> m_devices;
    std::string m_mic;
};

QTEST_GUILESS_MAIN(TestHotkeys)
#include "test_hotkeys.moc"
