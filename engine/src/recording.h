// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "pipewiregraph.h"

#include <QDateTime>
#include <QString>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pp {

// --- Pure helpers (unit tested) ---------------------------------------------

enum class RecordingFormat : std::uint8_t
{
    Flac, // lossless, 24-bit
    Wav,  // uncompressed, 24-bit
    Mp3,  // lossy (LAME, variable bit rate); rates up to 48 kHz
};

// "flac", "wav", "mp3" (in the settings file and the D-Bus state); unknown
// names are FLAC. The name is also the file extension.
std::string recordingFormatName(RecordingFormat format);
RecordingFormat recordingFormatFromName(std::string_view name);
std::optional<RecordingFormat> parseRecordingFormat(std::string_view name);
// Whether this build of libsndfile can write the format (MP3 is optional).
bool recordingFormatAvailable(RecordingFormat format);

// "<label> 2026-10-31 11-42-07.<ext>", with characters that don't belong in
// a file name (/ \ : and the like) replaced.
QString recordingFileName(const QString &label, const QDateTime &when, RecordingFormat format);

// path, or "<stem> (2).<ext>", "(3)"... when a file of that name exists.
QString uniquePath(const QString &path);

// A single-producer, single-consumer queue of samples that hands audio
// between the realtime thread and a file thread without locks or
// allocation. Its memory is allocated up front.
class SampleRing
{
public:
    // Room for at least capacity samples (rounded up to a power of two).
    explicit SampleRing(std::size_t capacity);

    // Producer: queues up to count samples; returns how many were queued.
    std::size_t push(const float *samples, std::size_t count) noexcept;
    // Consumer: takes up to count samples; returns how many were taken.
    std::size_t pop(float *samples, std::size_t count) noexcept;
    // Consumer: drops everything queued.
    void clear() noexcept;
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] std::size_t space() const noexcept;

private:
    std::vector<float> m_data;
    std::size_t m_mask = 0;
    std::atomic<std::size_t> m_write{0};
    std::atomic<std::size_t> m_read{0};
};

// --- Recorder ----------------------------------------------------------------

// Records a stereo stream to a file. The realtime thread only queues
// samples; a thread of its own encodes and writes them. The file is opened
// once the stream's rate is known and closed by stop().
class Recorder : public StreamIo
{
public:
    static constexpr int channels = 2;

    Recorder(QString path, RecordingFormat format);
    ~Recorder() override;
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;
    Recorder(Recorder &&) = delete;
    Recorder &operator=(Recorder &&) = delete;

    void setRate(int rate) noexcept override;
    void process(float *samples, std::uint32_t frames, std::uint32_t channelCount) noexcept override;

    // Writes what's queued, closes the file and waits for the thread. Call
    // once the stream is gone.
    void stop();

    [[nodiscard]] const QString &path() const { return m_path; }
    // Written so far.
    [[nodiscard]] double seconds() const;
    // Frames lost because the disk couldn't keep up.
    [[nodiscard]] std::uint64_t droppedFrames() const { return m_dropped.load(); }
    // Set when the file can't be written (the recording is then over).
    [[nodiscard]] bool failed() const { return m_failed.load(); }
    [[nodiscard]] QString error() const;

private:
    void run(const std::stop_token &stop);
    void fail(const QString &message);

    QString m_path;
    RecordingFormat m_format;
    SampleRing m_ring;
    std::atomic<int> m_rate{0};
    std::atomic<std::uint64_t> m_written{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<bool> m_failed{false};
    mutable std::mutex m_errorMutex;
    QString m_error;
    std::jthread m_thread; // last: it uses everything above
};

// --- Player ------------------------------------------------------------------

// Plays a sound file as a stereo stream at the file's own rate (PipeWire
// converts it to the graph's). A thread of its own decodes ahead into a
// queue; the realtime thread only takes from it. Mono files play in both
// channels; files with more channels play their first two.
class Player : public StreamIo
{
public:
    static constexpr int channels = 2;

    // Opens path; null with error set when it can't be read.
    static std::shared_ptr<Player> open(const QString &path, QString *error);
    ~Player() override;
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;
    Player(Player &&) = delete;
    Player &operator=(Player &&) = delete;

    void setRate(int rate) noexcept override;
    void process(float *samples, std::uint32_t frames, std::uint32_t channelCount) noexcept override;

    [[nodiscard]] const QString &path() const { return m_path; }
    [[nodiscard]] int fileRate() const { return m_fileRate; }
    [[nodiscard]] double duration() const;
    [[nodiscard]] double position() const;
    // Paused: plays silence and keeps its place.
    void setPaused(bool paused) { m_paused.store(paused); }
    [[nodiscard]] bool isPaused() const { return m_paused.load(); }
    void seek(double seconds);
    // Looping: starts again from the beginning at the end, without a gap.
    void setLoop(bool loop) { m_loop.store(loop); }
    [[nodiscard]] bool isLooping() const { return m_loop.load(); }
    // Played to the end.
    [[nodiscard]] bool finished() const { return m_finished.load(); }

private:
    struct File;
    Player(QString path, std::unique_ptr<File> file);
    void run(const std::stop_token &stop);

    QString m_path;
    std::unique_ptr<File> m_file; // the decoding thread's
    int m_fileRate = 0;
    std::int64_t m_frames = 0;
    SampleRing m_ring;
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_loop{false};
    std::atomic<bool> m_eof{false};
    std::atomic<bool> m_finished{false};
    // Seeking: the decoding thread asks the realtime thread to drop what's
    // queued (request), waits until it has (done), then reads from there.
    std::atomic<std::int64_t> m_seekTo{-1};
    std::atomic<std::uint32_t> m_flushRequest{0};
    std::atomic<std::uint32_t> m_flushDone{0};
    std::atomic<std::int64_t> m_base{0};   // frame the queue started at
    std::atomic<std::int64_t> m_played{0}; // frames played since then
    std::jthread m_thread;                 // last: it uses everything above
};

} // namespace pp
