// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "devicemodel.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace pp;

namespace {

struct Card
{
    std::uint32_t id;
    std::string serial;
    std::string busPath;
    std::string productId = "0x2024";

    [[nodiscard]] GraphObject object() const
    {
        return {.id = id,
                .type = "PipeWire:Interface:Device",
                .version = 3,
                .props = {{"device.name", "alsa_card." + serial + busPath},
                          {"device.serial", serial},
                          {"device.vendor.id", "0x0763"},
                          {"device.product.id", productId},
                          {"device.bus-path", busPath}}};
    }
};

GraphObject sinkNode(std::uint32_t id, const std::string &name, const Card *card,
                     const std::string &profile = "analog-stereo")
{
    Properties props{{"media.class", "Audio/Sink"},        {"node.name", name},
                     {"node.description", "Sink " + name}, {"audio.channels", "2"},
                     {"audio.position", "[ FL, FR ]"},     {"device.profile.name", profile}};
    if (card != nullptr) {
        props["device.id"] = std::to_string(card->id);
    }
    return {.id = id, .type = "PipeWire:Interface:Node", .version = 3, .props = props};
}

GraphObject sourceNode(std::uint32_t id, const std::string &name)
{
    return {.id = id,
            .type = "PipeWire:Interface:Node",
            .version = 3,
            .props = {{"media.class", "Audio/Source"},
                      {"node.name", name},
                      {"node.description", "Source " + name}}};
}

QVariant role(const DeviceModel &model, int row, DeviceModel::Role r)
{
    return model.data(model.index(row), r);
}

} // namespace

class TestDeviceModel : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_path = m_dir->filePath(QStringLiteral("devices.json"));
    }

    void unsavedRowsComeAndGo()
    {
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sinkNode(10, "speakers", nullptr), nullptr);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(role(model, 0, DeviceModel::OnlineRole).toBool(), true);
        QCOMPARE(role(model, 0, DeviceModel::SavedRole).toBool(), false);
        model.removeNode(10);
        QCOMPARE(model.rowCount(), 0);
    }

    void inputsSortFirst()
    {
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sinkNode(10, "speakers", nullptr), nullptr);
        model.upsertNode(sourceNode(11, "mic"), nullptr);
        QCOMPARE(role(model, 0, DeviceModel::DirectionRole).toString(), QStringLiteral("input"));
        QCOMPARE(role(model, 1, DeviceModel::DirectionRole).toString(), QStringLiteral("output"));
    }

    void savedRowsGoOfflineAndPersist()
    {
        const Card card{.id = 5, .serial = "SN1", .busPath = "usb-0:3"};
        const auto device = card.object();
        {
            DeviceStore store(m_path);
            DeviceModel model(&store);
            QAbstractItemModelTester tester(&model);
            model.upsertNode(sinkNode(10, "fasttrack", &card), &device);
            model.setEnabled(0, true);
            model.setLabel(0, QStringLiteral("  Headphones  "));
            QCOMPARE(role(model, 0, DeviceModel::LabelRole).toString(),
                     QStringLiteral("Headphones"));

            model.removeNode(10); // unplugged
            QCOMPARE(model.rowCount(), 1);
            QCOMPARE(role(model, 0, DeviceModel::OnlineRole).toBool(), false);
            QCOMPARE(role(model, 0, DeviceModel::EnabledRole).toBool(), true);
        }
        // A restart: the saved endpoint is listed before PipeWire reports anything.
        DeviceStore store(m_path);
        QVERIFY(store.load());
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(role(model, 0, DeviceModel::OnlineRole).toBool(), false);
        QCOMPARE(role(model, 0, DeviceModel::LabelRole).toString(), QStringLiteral("Headphones"));

        // Plugged back in under a new PipeWire id: same row, online again.
        model.upsertNode(sinkNode(42, "fasttrack", &card), &device);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(role(model, 0, DeviceModel::OnlineRole).toBool(), true);
        QCOMPARE(role(model, 0, DeviceModel::EnabledRole).toBool(), true);
    }

    void reattachesBySerialOnAnotherPort()
    {
        const Card before{.id = 5, .serial = "SN1", .busPath = "usb-0:3"};
        const Card after{.id = 6, .serial = "SN1", .busPath = "usb-0:7"};
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        QSignalSpy reattached(&model, &DeviceModel::reattached);

        const auto d1 = before.object();
        model.upsertNode(sinkNode(10, "alsa_output.port3", &before), &d1);
        model.setLabel(0, QStringLiteral("Headphones"));
        model.removeNode(10);

        const auto d2 = after.object();
        model.upsertNode(sinkNode(11, "alsa_output.port7", &after), &d2);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(reattached.count(), 1);
        QCOMPARE(role(model, 0, DeviceModel::KeyRole).toString(),
                 QStringLiteral("Audio/Sink|alsa_output.port7"));
        QCOMPARE(role(model, 0, DeviceModel::LabelRole).toString(), QStringLiteral("Headphones"));
        QVERIFY(store.find("Audio/Sink|alsa_output.port3") == nullptr);
        QCOMPARE(store.find("Audio/Sink|alsa_output.port7")->signature.busPath, "usb-0:7");
    }

    void reattachesAfterProfileChange()
    {
        const Card card{.id = 5, .serial = "", .busPath = "usb-0:3"};
        const auto device = card.object();
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sinkNode(10, "card.analog-stereo", &card, "analog-stereo"), &device);
        model.setEnabled(0, true);
        model.removeNode(10);
        model.upsertNode(sinkNode(11, "card.iec958-stereo", &card, "iec958-stereo"), &device);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(role(model, 0, DeviceModel::EnabledRole).toBool(), true);
    }

    void ambiguousOrWeakMatchesStayApart()
    {
        // Two identical saved devices without serials, both offline: a third
        // one can't be told apart, so it gets its own row.
        const Card a{.id = 5, .serial = "", .busPath = "usb-0:1"};
        const Card b{.id = 6, .serial = "", .busPath = "usb-0:2"};
        const Card c{.id = 7, .serial = "", .busPath = "usb-0:9"};
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        const auto da = a.object();
        const auto db = b.object();
        const auto dc = c.object();
        model.upsertNode(sinkNode(10, "a", &a), &da);
        model.upsertNode(sinkNode(11, "b", &b), &db);
        model.setEnabled(0, true);
        model.setEnabled(1, true);
        model.removeNode(10);
        model.removeNode(11);
        model.upsertNode(sinkNode(12, "c", &c), &dc);
        QCOMPARE(model.rowCount(), 3);

        // Same model on another port but a different profile: too weak.
        model.forget(model.rowForKey("Audio/Sink|b"));
        model.removeNode(12);
        model.upsertNode(sinkNode(13, "d", &c, "iec958-stereo"), &dc);
        QVERIFY(model.rowForKey("Audio/Sink|a") >= 0);
        QVERIFY(model.rowForKey("Audio/Sink|d") >= 0);
    }

    void virtualEndpointsNeverReattach()
    {
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sinkNode(10, "stream-mix", nullptr), nullptr);
        model.setEnabled(0, true);
        model.removeNode(10);
        model.upsertNode(sinkNode(11, "other-null-sink", nullptr), nullptr);
        QCOMPARE(model.rowCount(), 2);
    }

    void emptyLabelOnUnsavedRowSavesNothing()
    {
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sinkNode(10, "speakers", nullptr), nullptr);
        model.setLabel(0, QStringLiteral("   "));
        QVERIFY(store.entries().empty());
    }

    void forgetOfflineRemovesRow()
    {
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sinkNode(10, "speakers", nullptr), nullptr);
        model.setEnabled(0, true);
        model.removeNode(10);
        QCOMPARE(model.rowCount(), 1);
        model.forget(0);
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(store.entries().empty());
    }

    void savedRowComesOnlineBesideNewRows()
    {
        // Two saved endpoints (an output and an input), then PipeWire reports a
        // new input that sorts first, and the saved output.
        {
            DeviceStore store(m_path);
            DeviceModel model(&store);
            QAbstractItemModelTester tester(&model);
            model.upsertNode(sinkNode(10, "out", nullptr), nullptr);
            model.upsertNode(sourceNode(11, "yeti"), nullptr);
            model.setLabel(model.rowForKey("Audio/Sink|out"), QStringLiteral("Headphones"));
            model.setEnabled(model.rowForKey("Audio/Source|yeti"), true);
        }
        DeviceStore store(m_path);
        QVERIFY(store.load());
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sourceNode(20, "a-mic"), nullptr);
        model.upsertNode(sinkNode(21, "out", nullptr), nullptr);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(role(model, 0, DeviceModel::KeyRole).toString(),
                 QStringLiteral("Audio/Source|a-mic"));
        QCOMPARE(role(model, 1, DeviceModel::KeyRole).toString(),
                 QStringLiteral("Audio/Source|yeti"));
        QCOMPARE(role(model, 2, DeviceModel::KeyRole).toString(), QStringLiteral("Audio/Sink|out"));
        QCOMPARE(role(model, 2, DeviceModel::LabelRole).toString(), QStringLiteral("Headphones"));
        QCOMPARE(role(model, 2, DeviceModel::OnlineRole).toBool(), true);
    }

    void sectionStarts()
    {
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sinkNode(10, "speakers", nullptr), nullptr);
        QVERIFY(role(model, 0, DeviceModel::SectionStartRole).toBool());
        model.upsertNode(sourceNode(11, "mic"), nullptr); // inserted above
        QVERIFY(role(model, 0, DeviceModel::SectionStartRole).toBool());
        QVERIFY(role(model, 1, DeviceModel::SectionStartRole).toBool());
        model.upsertNode(sourceNode(12, "nmic"), nullptr);
        QVERIFY(!role(model, 1, DeviceModel::SectionStartRole).toBool());
        QVERIFY(role(model, 2, DeviceModel::SectionStartRole).toBool());
    }

    void defaults()
    {
        DeviceStore store(m_path);
        DeviceModel model(&store);
        QAbstractItemModelTester tester(&model);
        model.upsertNode(sinkNode(10, "speakers", nullptr), nullptr);
        model.upsertNode(sourceNode(11, "mic"), nullptr);
        QSignalSpy changed(&model, &DeviceModel::dataChanged);
        model.setDefaults("speakers", "mic");
        QVERIFY(changed.count() > 0);
        QVERIFY(role(model, 0, DeviceModel::DefaultRole).toBool()); // mic
        QVERIFY(role(model, 1, DeviceModel::DefaultRole).toBool()); // speakers
        model.setDefaults("other", "");
        QVERIFY(!role(model, 0, DeviceModel::DefaultRole).toBool());
        QVERIFY(!role(model, 1, DeviceModel::DefaultRole).toBool());
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
};

QTEST_GUILESS_MAIN(TestDeviceModel)
#include "test_devicemodel.moc"
