// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// Plugins: the effect types, the catalog and its scan (of the test plugin
// in tests/lv2, the only one LV2_PATH points to), the chain split, and
// adding plugins to a strip. Running them is in test_buslive.

#include "busmanager.h"
#include "devicemodel.h"
#include "devicestore.h"
#include "plugins.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace pp;

namespace {

const std::string testGain = "lv2:https://github.com/kingsqueak/patch-prime/tests/gain";

EffectSettings effect(std::string type)
{
    return {.type = std::move(type), .enabled = true, .params = {}, .key = {}, .state = {}};
}

std::vector<std::vector<std::size_t>> indices(const std::vector<ChainSegment> &segments)
{
    std::vector<std::vector<std::size_t>> out;
    out.reserve(segments.size());
    for (const auto &s : segments) {
        out.push_back(s.indices);
    }
    return out;
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

} // namespace

class TestPlugins : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void types()
    {
        QVERIFY(isPluginType("clap:com.x.y"));
        QCOMPARE(pluginFormat("vst3:ABCDEF"), "vst3");
        QCOMPARE(pluginUri("vst3:ABCDEF"), "ABCDEF");
        QCOMPARE(pluginFormat("clap:com.x.y"), "clap");
        QCOMPARE(pluginFormat("lv2:urn:x"), "lv2");
        QCOMPARE(pluginFormat("gain"), "");
        QCOMPARE(pluginUri("clap:com.x.y"), "com.x.y");
        QVERIFY(!isPluginType("clap:"));
        QVERIFY(isPluginType("lv2:urn:x"));
        QVERIFY(!isPluginType("lv2:"));
        QVERIFY(!isPluginType("gain"));
        QCOMPARE(pluginType("urn:x"), "lv2:urn:x");
        QCOMPARE(pluginUri("lv2:urn:x"), "urn:x");
        QCOMPARE(pluginUri("gain"), "");
    }

    void nodeNames()
    {
        QCOMPARE(effectsNodeName("patchprime.in.mic"), "patchprime.fx.in.mic");
        QCOMPARE(effectsNodeName("patchprime.in.mic", 2), "patchprime.fx.in.mic.2");
        QCOMPARE(pluginNodeName("patchprime.in.mic", 1), "patchprime.plug.in.mic.1");
    }

    void splitsTheChain()
    {
        // Runs of built-ins share a node; each plugin has its own; unknown
        // types are left out.
        const std::vector effects{effect("gate"),  effect("eq"),     effect("lv2:urn:a"),
                                  effect("lv2:urn:b"), effect("nonsense"), effect("limiter")};
        const auto segments = splitChain(effects);
        QCOMPARE(segments.size(), 4U);
        QVERIFY(!segments[0].plugin && segments[1].plugin && segments[2].plugin &&
                !segments[3].plugin);
        QCOMPARE(indices(segments), (std::vector<std::vector<std::size_t>>{{0, 1}, {2}, {3}, {5}}));
        QVERIFY(splitChain({}).empty());
        QCOMPARE(splitChain({effect("lv2:urn:a")}).size(), 1U);
    }

    void listRoundTrip()
    {
        PluginInfo p{.type = "lv2:urn:x",
                     .name = "X Comp",
                     .author = "Someone",
                     .category = "Compressor",
                     .binary = {},
                     .audioIn = 2,
                     .audioOut = 2,
                     .params = {ParamInfo{.name = "mode",
                                          .label = "Mode",
                                          .unit = "",
                                          .minimum = 0,
                                          .maximum = 2,
                                          .initial = 1,
                                          .kind = ParamKind::Choice,
                                          .options = {"A", "B", "C"}}},
                     .problem = {}};
        const auto back = pluginsFromJson(pluginsToJson({p}));
        QCOMPARE(back.size(), 1U);
        QCOMPARE(back[0].name, p.name);
        QCOMPARE(back[0].category, p.category);
        QCOMPARE(back[0].audioOut, 2);
        QCOMPARE(back[0].params.size(), 1U);
        QCOMPARE(back[0].params[0].kind, ParamKind::Choice);
        QCOMPARE(back[0].params[0].options, p.params[0].options);
        QCOMPARE(back[0].params[0].initial, 1.0);
        QVERIFY(pluginsFromJson("not json").empty());

        // For clients: the list without the parameters, then one plugin's.
        const auto summary = QJsonDocument::fromJson(pluginSummariesToJson({p})).array();
        QCOMPARE(summary.size(), 1);
        QVERIFY(!summary.at(0).toObject().contains(QLatin1String("params")));
        QCOMPARE(summary.at(0).toObject().value(QLatin1String("paramCount")).toInt(), 1);
        QCOMPARE(summary.at(0).toObject().value(QLatin1String("name")).toString(), QStringLiteral("X Comp"));
        const auto params = QJsonDocument::fromJson(pluginParamsToJson(p)).array();
        QCOMPARE(params.size(), 1);
        QCOMPARE(params.at(0).toObject().value(QLatin1String("options")).toArray().size(), 3);
    }

    void scansTheTestPlugin()
    {
        QVERIFY2(!pluginHostProgram().isEmpty(), "patchprime-pluginhost not built");
        QTemporaryDir dir;
        PluginCatalog catalog(dir.filePath(QStringLiteral("cache/plugins.json")));
        // No list yet: the first start scans, and says how far it is.
        PluginCatalog::Progress furthest;
        connect(&catalog, &PluginCatalog::changed, this, [&] {
            if (catalog.progress().done > furthest.done) {
                furthest = catalog.progress();
            }
        });
        QVERIFY(!catalog.lastScan().isValid());
        catalog.start();
        QVERIFY(catalog.isScanning());
        QVERIFY(QTest::qWaitFor([&] { return !catalog.isScanning(); }, 15000));
        QCOMPARE(furthest.total, 3); // the LV2 ones, a .clap file, a .vst3 bundle
        QCOMPARE(furthest.done, 3);
        QCOMPARE(furthest.found, 3);
        QCOMPARE(catalog.progress().total, 0); // done with
        QVERIFY(catalog.lastScan().isValid());
        QCOMPARE(catalog.plugins().size(), 3U); // LV2, CLAP and VST3
        const auto *gain = catalog.find(testGain);
        QVERIFY(gain != nullptr);
        QCOMPARE(gain->name, "Patch Prime Test Gain");
        QCOMPARE(gain->category, "Amplifier");
        QCOMPARE(gain->audioIn, 1);
        QCOMPARE(gain->audioOut, 1);
        QCOMPARE(gain->problem, "");
        QCOMPARE(gain->params.size(), 3U);
        QVERIFY(!gain->hasUi);
        QCOMPARE(gain->params[0].name, "gain");
        QCOMPARE(gain->params[0].maximum, 4.0);
        QCOMPARE(gain->params[0].initial, 1.0);
        QCOMPARE(gain->params[1].kind, ParamKind::Toggle);

        // The CLAP one: its parameters by id, its file.
        const auto *clap = catalog.find("clap:com.github.kingsqueak.patch-prime.tests.gain");
        QVERIFY(clap != nullptr);
        QCOMPARE(clap->name, "Patch Prime Test Gain (CLAP)");
        QCOMPARE(clap->category, "Utility");
        QCOMPARE(clap->audioIn, 1);
        QCOMPARE(clap->audioOut, 1);
        QCOMPARE(clap->problem, "");
        QVERIFY(!clap->hasUi);
        QVERIFY(clap->binary.ends_with("testgain.clap"));
        QCOMPARE(clap->params.size(), 3U);
        QCOMPARE(clap->params[0].name, "1");
        QCOMPARE(clap->params[0].label, "Gain");
        QCOMPARE(clap->params[0].maximum, 4.0);
        QCOMPARE(clap->params[2].kind, ParamKind::Toggle);

        // The VST3 one: stepped parameters as switches, values normalized.
        const auto *vst3 = catalog.find("vst3:5050545054657374476169" "6E56535433");
        QVERIFY(vst3 != nullptr);
        QCOMPARE(vst3->name, "Patch Prime Test Gain (VST3)");
        QCOMPARE(vst3->author, "Patch Prime tests");
        QCOMPARE(vst3->category, "Utility");
        QCOMPARE(vst3->audioIn, 1);
        QCOMPARE(vst3->problem, "");
        QVERIFY(vst3->binary.ends_with("patchprime-test.vst3"));
        QCOMPARE(vst3->params.size(), 3U);
        QCOMPARE(vst3->params[0].name, "1");
        QCOMPARE(vst3->params[0].maximum, 1.0);
        QCOMPARE(vst3->params[0].initial, 0.25);
        QCOMPARE(vst3->params[2].kind, ParamKind::Toggle);

        // Cached: the next start has it at once, and doesn't scan again.
        PluginCatalog again(dir.filePath(QStringLiteral("cache/plugins.json")));
        again.start();
        QVERIFY(!again.isScanning());
        QVERIFY(again.find(testGain) != nullptr);
        QCOMPARE(again.lastScan().toSecsSinceEpoch(), catalog.lastScan().toSecsSinceEpoch());
        // ...unless asked.
        again.rescan();
        QVERIFY(again.isScanning());
        QVERIFY(QTest::qWaitFor([&] { return !again.isScanning(); }, 15000));
        QCOMPARE(again.plugins().size(), 3U);

        // A cache from an older version (a bare list) is scanned again.
        QFile old(dir.filePath(QStringLiteral("old.json")));
        QVERIFY(old.open(QIODevice::WriteOnly));
        old.write(pluginsToJson(catalog.plugins()));
        old.close();
        PluginCatalog older(old.fileName());
        older.start();
        QVERIFY(older.isScanning());
        QVERIFY(QTest::qWaitFor([&] { return !older.isScanning(); }, 15000));
        QCOMPARE(older.plugins().size(), 3U);
    }

    void addsPluginsToAStrip()
    {
        QTemporaryDir dir;
        DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
        DeviceModel model(&store);
        model.upsertNode(sourceNode(11, "mic"), nullptr);
        const int row = model.rowForKey("Audio/Source|mic");
        QVERIFY(row >= 0);

        // Without a catalog (or not in it): refused.
        model.addEffect(row, QString::fromStdString(testGain));
        QCOMPARE(store.find("Audio/Source|mic"), nullptr);

        PluginCatalog catalog(dir.filePath(QStringLiteral("plugins.json")));
        PluginInfo gain{.type = testGain,
                        .name = "Gain",
                        .author = {},
                        .category = "Amplifier",
                        .binary = {},
                        .audioIn = 1,
                        .audioOut = 1,
                        .params = {ParamInfo{.name = "gain",
                                             .label = "Gain",
                                             .unit = "",
                                             .minimum = 0,
                                             .maximum = 4,
                                             .initial = 1,
                                             .kind = ParamKind::Linear,
                                             .options = {}}},
                        .problem = {}};
        PluginInfo synth{.type = "lv2:urn:synth",
                         .name = "Synth",
                         .author = {},
                         .category = "Instrument",
                         .binary = {},
                         .audioIn = 0,
                         .audioOut = 2,
                         .params = {},
                         .problem = "it doesn't process audio"};
        catalog.setPlugins({gain, synth});
        model.setPluginCatalog(&catalog);

        model.addEffect(row, QString::fromStdString(synth.type)); // can't run here
        QCOMPARE(store.find("Audio/Source|mic"), nullptr);
        model.addEffect(row, QStringLiteral("gate"));
        model.addEffect(row, QString::fromStdString(testGain));
        const auto *entry = store.find("Audio/Source|mic");
        QVERIFY(entry != nullptr);
        QCOMPARE(entry->effects.size(), 2U);
        QCOMPARE(entry->effects[1].type, testGain);
        QCOMPARE(entry->effects[1].params.at("gain"), 1.0);

        // Parameters are checked against the plugin's ranges.
        model.setEffectParam(row, 1, QStringLiteral("gain"), 9.0);
        QCOMPARE(store.find("Audio/Source|mic")->effects[1].params.at("gain"), 4.0);
        model.setEffectParam(row, 1, QStringLiteral("nonsense"), 1.0);
        QVERIFY(!store.find("Audio/Source|mic")->effects[1].params.contains("nonsense"));

        // Saved and read back as they are, even without the catalog (the
        // plugin may be uninstalled; it stays in the chain).
        // Its own state too (only plugins have one).
        model.setEffectState(row, 1, "<urn:x> a <urn:y> .");
        model.setEffectState(row, 0, "not for a built-in");
        QCOMPARE(store.find("Audio/Source|mic")->effects[0].state, "");
        const auto json = effectsToJson(store.find("Audio/Source|mic")->effects);
        const auto back = effectsFromJson(json);
        QCOMPARE(back.size(), 2U);
        QCOMPARE(back[1].type, testGain);
        QCOMPARE(back[1].params.at("gain"), 4.0);
        QCOMPARE(back[1].state, "<urn:x> a <urn:y> .");
    }
};

QTEST_GUILESS_MAIN(TestPlugins)
#include "test_plugins.moc"
