// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "recording.h"
#include "recordingmanager.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

class TestRecording : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void formatNames()
    {
        using pp::RecordingFormat;
        for (const auto f : {RecordingFormat::Flac, RecordingFormat::Wav, RecordingFormat::Mp3}) {
            QCOMPARE(pp::recordingFormatFromName(pp::recordingFormatName(f)), f);
        }
        QCOMPARE(pp::recordingFormatFromName("ogg"), RecordingFormat::Flac);
        QVERIFY(!pp::parseRecordingFormat("ogg"));
        QVERIFY(pp::recordingFormatAvailable(RecordingFormat::Flac));
        QVERIFY(pp::recordingFormatAvailable(RecordingFormat::Wav));
    }

    void fileNames()
    {
        const QDateTime when(QDate(2026, 10, 31), QTime(11, 42, 7));
        QCOMPARE(pp::recordingFileName(QStringLiteral("Stream Mix"), when, pp::RecordingFormat::Flac),
                 QStringLiteral("Stream Mix 2026-10-31 11-42-07.flac"));
        QCOMPARE(pp::recordingFileName(QStringLiteral("A/B: C"), when, pp::RecordingFormat::Mp3),
                 QStringLiteral("A-B- C 2026-10-31 11-42-07.mp3"));
        QCOMPARE(pp::recordingFileName(QStringLiteral(".."), when, pp::RecordingFormat::Wav),
                 QStringLiteral("Recording.. 2026-10-31 11-42-07.wav"));
    }

    void uniquePaths()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("take.flac"));
        QCOMPARE(pp::uniquePath(path), path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(pp::uniquePath(path), dir.filePath(QStringLiteral("take (2).flac")));
    }

    void ringWrapsAndClears()
    {
        pp::SampleRing ring(5); // rounds up to 8
        QCOMPARE(ring.space(), std::size_t{8});
        std::vector<float> in{1, 2, 3, 4, 5, 6};
        QCOMPARE(ring.push(in.data(), in.size()), std::size_t{6});
        std::vector<float> out(4);
        QCOMPARE(ring.pop(out.data(), 4), std::size_t{4});
        QCOMPARE(out, (std::vector<float>{1, 2, 3, 4}));
        // Wraps round the end; only what fits goes in.
        std::vector<float> more{7, 8, 9, 10, 11, 12, 13};
        QCOMPARE(ring.push(more.data(), more.size()), std::size_t{6});
        out.assign(8, 0.0F);
        QCOMPARE(ring.pop(out.data(), 8), std::size_t{8});
        QCOMPARE(out, (std::vector<float>{5, 6, 7, 8, 9, 10, 11, 12}));
        ring.push(in.data(), 3);
        ring.clear();
        QCOMPARE(ring.available(), std::size_t{0});
    }

    void strips()
    {
        using pp::Direction;
        using pp::EndpointKind;
        QVERIFY(pp::canRecord(EndpointKind::VirtualSink, Direction::Output));
        QVERIFY(pp::canRecord(EndpointKind::VirtualInput, Direction::Output));
        QVERIFY(pp::canRecord(EndpointKind::Hardware, Direction::Input));
        QVERIFY(!pp::canRecord(EndpointKind::Hardware, Direction::Output));
        QVERIFY(!pp::canRecord(EndpointKind::VirtualSource, Direction::Input));
        QVERIFY(pp::canPlayInto(EndpointKind::Hardware));
        QVERIFY(pp::canPlayInto(EndpointKind::VirtualInput));
        QVERIFY(!pp::canPlayInto(EndpointKind::VirtualSource));
    }

    // Records a second of a 1 kHz tone at -6 dBFS through the realtime
    // interface, as PipeWire would call it, then plays the file back.
    void recordsAndPlaysBack_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("flac") << static_cast<int>(pp::RecordingFormat::Flac);
        QTest::newRow("wav") << static_cast<int>(pp::RecordingFormat::Wav);
        QTest::newRow("mp3") << static_cast<int>(pp::RecordingFormat::Mp3);
    }
    void recordsAndPlaysBack()
    {
        QFETCH(int, format);
        const auto f = static_cast<pp::RecordingFormat>(format);
        if (!pp::recordingFormatAvailable(f)) {
            QSKIP("this libsndfile can't write the format");
        }
        QTemporaryDir dir;
        const auto path = dir.filePath(
            QStringLiteral("sub/tone.") + QString::fromStdString(pp::recordingFormatName(f)));
        constexpr int rate = 48000;
        constexpr std::uint32_t block = 240; // 200 blocks a second
        const float amplitude = 0.5F;
        {
            pp::Recorder recorder(path, f);
            recorder.setRate(rate);
            std::vector<float> buffer(std::size_t{block} * 2);
            std::size_t n = 0;
            for (int b = 0; b < rate / static_cast<int>(block); ++b) {
                for (std::size_t i = 0; i < block; ++i, ++n) {
                    const auto x = amplitude * static_cast<float>(std::sin(
                                                   2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / rate));
                    buffer[i * 2] = x;
                    buffer[(i * 2) + 1] = -x;
                }
                recorder.process(buffer.data(), block, 2);
            }
            recorder.stop();
            QVERIFY2(!recorder.failed(), qPrintable(recorder.error()));
            QCOMPARE(recorder.droppedFrames(), std::uint64_t{0});
            QCOMPARE(recorder.seconds(), 1.0);
        }
        QVERIFY(QFileInfo::exists(path));

        QString error;
        const auto player = pp::Player::open(path, &error);
        QVERIFY2(player, qPrintable(error));
        QCOMPARE(player->fileRate(), rate);
        QVERIFY(std::abs(player->duration() - 1.0) < 0.06); // MP3 pads a little
        // Pull it all through, as the realtime thread would, and find the peak.
        std::vector<float> buffer(std::size_t{block} * 2);
        float peakLeft = 0.0F;
        float peakRight = 0.0F;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!player->finished() && std::chrono::steady_clock::now() < deadline) {
            player->process(buffer.data(), block, 2);
            for (std::size_t i = 0; i < block; ++i) {
                peakLeft = std::max(peakLeft, buffer[i * 2]);
                peakRight = std::max(peakRight, -buffer[(i * 2) + 1]);
            }
            std::this_thread::sleep_for(100us);
        }
        QVERIFY(player->finished());
        QVERIFY2(std::abs(peakLeft - amplitude) < 0.05F, qPrintable(QString::number(peakLeft)));
        QVERIFY2(std::abs(peakRight - amplitude) < 0.05F, qPrintable(QString::number(peakRight)));
        QVERIFY(player->position() > 0.9);

        // Seeking drops what's queued and plays from there.
        player->seek(0.5);
        for (int i = 0; i < 200 && player->position() > 0.6; ++i) {
            player->process(buffer.data(), block, 2);
            std::this_thread::sleep_for(1ms);
        }
        QVERIFY2(player->position() >= 0.5 && player->position() < 0.6,
                 qPrintable(QString::number(player->position())));
        QVERIFY(!player->finished());
    }

    // Looping plays on past the end, without finishing.
    void loops()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("short.wav"));
        {
            pp::Recorder recorder(path, pp::RecordingFormat::Wav);
            recorder.setRate(48000);
            std::vector<float> buffer(std::size_t{4800} * 2, 0.25F); // 0.1 s
            recorder.process(buffer.data(), 4800, 2);
            recorder.stop();
        }
        QString error;
        const auto player = pp::Player::open(path, &error);
        QVERIFY2(player, qPrintable(error));
        player->setLoop(true);
        std::vector<float> buffer(std::size_t{480} * 2);
        std::size_t played = 0;
        for (int i = 0; i < 2000 && played < std::size_t{48000} * 3 / 10; ++i) {
            buffer.assign(buffer.size(), 0.0F);
            player->process(buffer.data(), 480, 2);
            if (buffer.back() == 0.25F) {
                played += 480;
            }
            std::this_thread::sleep_for(1ms);
        }
        QVERIFY(played >= std::size_t{48000} * 3 / 10); // three times round
        QVERIFY(!player->finished());
        QVERIFY(player->position() < 0.1);
    }

    void stoppedBeforeTheRateLeavesNoFile()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("none.flac"));
        pp::Recorder recorder(path, pp::RecordingFormat::Flac);
        recorder.stop();
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!recorder.failed());
    }

    void mp3RefusesHighRates()
    {
        if (!pp::recordingFormatAvailable(pp::RecordingFormat::Mp3)) {
            QSKIP("this libsndfile can't write MP3");
        }
        QTemporaryDir dir;
        pp::Recorder recorder(dir.filePath(QStringLiteral("hi.mp3")), pp::RecordingFormat::Mp3);
        recorder.setRate(96000);
        for (int i = 0; i < 100 && !recorder.failed(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
        QVERIFY(recorder.failed());
        QVERIFY(recorder.error().contains(QStringLiteral("96000")));
    }
};

QTEST_GUILESS_MAIN(TestRecording)
#include "test_recording.moc"
