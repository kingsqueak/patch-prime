// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "recording.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

#include <sndfile.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <utility>

Q_LOGGING_CATEGORY(lcRecording, "patchprime.recording")

namespace pp {

using namespace std::chrono_literals;

namespace {

// About ten seconds of stereo at 96 kHz: the file thread wakes every few
// tens of milliseconds, so this only fills when the disk stalls.
constexpr std::size_t recorderQueueSamples = std::size_t{1} << 21;
// About two seconds at 48 kHz decoded ahead.
constexpr std::size_t playerQueueSamples = std::size_t{1} << 18;
constexpr std::size_t chunkFrames = 4096;

int sndfileFormat(RecordingFormat format)
{
    switch (format) {
    case RecordingFormat::Wav:
        return SF_FORMAT_WAV | SF_FORMAT_PCM_24;
    case RecordingFormat::Mp3:
        return SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    case RecordingFormat::Flac:
        break;
    }
    return SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
}

struct SndfileCloser
{
    void operator()(SNDFILE *file) const { sf_close(file); }
};
using SndfilePtr = std::unique_ptr<SNDFILE, SndfileCloser>;

} // namespace

std::string recordingFormatName(RecordingFormat format)
{
    switch (format) {
    case RecordingFormat::Wav:
        return "wav";
    case RecordingFormat::Mp3:
        return "mp3";
    case RecordingFormat::Flac:
        break;
    }
    return "flac";
}

std::optional<RecordingFormat> parseRecordingFormat(std::string_view name)
{
    for (const auto format : {RecordingFormat::Flac, RecordingFormat::Wav, RecordingFormat::Mp3}) {
        if (name == recordingFormatName(format)) {
            return format;
        }
    }
    return std::nullopt;
}

bool recordingFormatAvailable(RecordingFormat format)
{
    SF_INFO info{};
    info.samplerate = 48000;
    info.channels = 2;
    info.format = sndfileFormat(format);
    return sf_format_check(&info) != 0;
}

RecordingFormat recordingFormatFromName(std::string_view name)
{
    return parseRecordingFormat(name).value_or(RecordingFormat::Flac);
}

QString recordingFileName(const QString &label, const QDateTime &when, RecordingFormat format)
{
    QString stem = label.trimmed();
    for (auto &c : stem) {
        if (c == u'/' || c == u'\\' || c == u':' || c == u'*' || c == u'?' || c == u'"' ||
            c == u'<' || c == u'>' || c == u'|' || c.category() == QChar::Other_Control) {
            c = u'-';
        }
    }
    if (stem.isEmpty() || stem.startsWith(u'.')) {
        stem.prepend(QStringLiteral("Recording"));
    }
    return QStringLiteral("%1 %2.%3")
        .arg(stem, when.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")),
             QString::fromStdString(recordingFormatName(format)));
}

QString uniquePath(const QString &path)
{
    if (!QFileInfo::exists(path)) {
        return path;
    }
    const QFileInfo info(path);
    const QString base = info.dir().filePath(info.completeBaseName());
    const QString suffix = info.suffix().isEmpty() ? QString() : u'.' + info.suffix();
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix);
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
}

// --- SampleRing --------------------------------------------------------------

SampleRing::SampleRing(std::size_t capacity)
    : m_data(std::bit_ceil(std::max<std::size_t>(capacity, 2))), m_mask(m_data.size() - 1)
{
}

std::size_t SampleRing::available() const noexcept
{
    return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
}

std::size_t SampleRing::space() const noexcept
{
    return m_data.size() - available();
}

std::size_t SampleRing::push(const float *samples, std::size_t count) noexcept
{
    const auto write = m_write.load(std::memory_order_relaxed);
    const auto read = m_read.load(std::memory_order_acquire);
    count = std::min(count, m_data.size() - (write - read));
    const auto at = write & m_mask;
    const auto first = std::min(count, m_data.size() - at);
    std::copy_n(samples, first, m_data.begin() + static_cast<std::ptrdiff_t>(at));
    std::copy_n(samples + first, count - first, m_data.begin());
    m_write.store(write + count, std::memory_order_release);
    return count;
}

std::size_t SampleRing::pop(float *samples, std::size_t count) noexcept
{
    const auto read = m_read.load(std::memory_order_relaxed);
    const auto write = m_write.load(std::memory_order_acquire);
    count = std::min(count, write - read);
    const auto at = read & m_mask;
    const auto first = std::min(count, m_data.size() - at);
    std::copy_n(m_data.begin() + static_cast<std::ptrdiff_t>(at), first, samples);
    std::copy_n(m_data.begin(), count - first, samples + first);
    m_read.store(read + count, std::memory_order_release);
    return count;
}

void SampleRing::clear() noexcept
{
    m_read.store(m_write.load(std::memory_order_acquire), std::memory_order_release);
}

// --- Recorder ----------------------------------------------------------------

Recorder::Recorder(QString path, RecordingFormat format)
    : m_path(std::move(path)), m_format(format), m_ring(recorderQueueSamples),
      m_thread([this](const std::stop_token &stop) { run(stop); })
{
}

Recorder::~Recorder()
{
    stop();
}

void Recorder::setRate(int rate) noexcept
{
    m_rate.store(rate);
}

void Recorder::process(float *samples, std::uint32_t frames, std::uint32_t channelCount) noexcept
{
    if (channelCount != channels || m_failed.load(std::memory_order_relaxed)) {
        return;
    }
    // Whole frames only, so the channels never slip.
    const std::size_t wanted = std::size_t{frames} * channels;
    const std::size_t fits = std::min(wanted, m_ring.space() / channels * channels);
    m_ring.push(samples, fits);
    if (fits < wanted) {
        m_dropped.fetch_add((wanted - fits) / channels, std::memory_order_relaxed);
    }
}

void Recorder::stop()
{
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
}

double Recorder::seconds() const
{
    const int rate = m_rate.load();
    return rate > 0 ? static_cast<double>(m_written.load()) / rate : 0.0;
}

QString Recorder::error() const
{
    const std::scoped_lock lock(m_errorMutex);
    return m_error;
}

void Recorder::fail(const QString &message)
{
    {
        const std::scoped_lock lock(m_errorMutex);
        m_error = message;
    }
    m_failed.store(true);
    qCWarning(lcRecording, "%s: %s", qPrintable(m_path), qPrintable(message));
}

void Recorder::run(const std::stop_token &stop)
{
    // The file is opened once the rate is known; a recording stopped
    // before then leaves no file.
    while (m_rate.load() == 0) {
        if (stop.stop_requested()) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }
    const int rate = m_rate.load();
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        fail(QStringLiteral("can't create the folder"));
        return;
    }
    SF_INFO info{};
    info.samplerate = rate;
    info.channels = channels;
    info.format = sndfileFormat(m_format);
    if ((m_format == RecordingFormat::Mp3 && rate > 48000) || sf_format_check(&info) == 0) {
        fail(m_format == RecordingFormat::Mp3
                 ? QStringLiteral("MP3 doesn't support %1 Hz; use FLAC or WAV").arg(rate)
                 : QStringLiteral("this format doesn't support %1 Hz").arg(rate));
        return;
    }
    const SndfilePtr file(sf_open(QFile::encodeName(m_path).constData(), SFM_WRITE, &info));
    if (!file) {
        fail(QString::fromUtf8(sf_strerror(nullptr)));
        return;
    }
    // Overs clip instead of wrapping round in integer formats.
    sf_command(file.get(), SFC_SET_CLIPPING, nullptr, SF_TRUE);
    if (m_format == RecordingFormat::Mp3) {
        int mode = SF_BITRATE_MODE_VARIABLE;
        sf_command(file.get(), SFC_SET_BITRATE_MODE, &mode, sizeof(mode));
        double quality = 0.1; // 0 = best .. 1 = smallest; about 225 kbit/s
        sf_command(file.get(), SFC_SET_COMPRESSION_LEVEL, &quality, sizeof(quality));
    }

    std::vector<float> chunk(chunkFrames * channels);
    for (;;) {
        const bool stopping = stop.stop_requested();
        const auto got = m_ring.pop(chunk.data(), chunk.size());
        if (got > 0) {
            const auto frames = static_cast<sf_count_t>(got / channels);
            if (sf_writef_float(file.get(), chunk.data(), frames) != frames) {
                fail(QString::fromUtf8(sf_strerror(file.get())));
                return;
            }
            m_written.fetch_add(static_cast<std::uint64_t>(frames));
        }
        if (m_rate.load() != rate) {
            fail(QStringLiteral("the mixer's sample rate changed"));
            return;
        }
        if (got < chunk.size()) {
            if (stopping) {
                return; // everything queued is written
            }
            std::this_thread::sleep_for(20ms);
        }
    }
}

// --- Player ------------------------------------------------------------------

struct Player::File
{
    SndfilePtr file;
    SF_INFO info{};
};

std::shared_ptr<Player> Player::open(const QString &path, QString *error)
{
    auto file = std::make_unique<File>();
    file->file.reset(sf_open(QFile::encodeName(path).constData(), SFM_READ, &file->info));
    if (!file->file || file->info.channels <= 0 || file->info.samplerate <= 0) {
        if (error != nullptr) {
            *error = QString::fromUtf8(sf_strerror(file->file.get()));
        }
        return nullptr;
    }
    // The constructor is private: make_shared can't reach it.
    return std::shared_ptr<Player>(new Player(path, std::move(file)));
}

Player::Player(QString path, std::unique_ptr<File> file)
    : m_path(std::move(path)), m_file(std::move(file)), m_fileRate(m_file->info.samplerate),
      m_frames(m_file->info.frames), m_ring(playerQueueSamples),
      m_thread([this](const std::stop_token &stop) { run(stop); })
{
}

Player::~Player()
{
    m_thread.request_stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Player::setRate(int /*rate*/) noexcept
{
    // The stream runs at the file's rate; PipeWire converts.
}

double Player::duration() const
{
    return static_cast<double>(m_frames) / m_fileRate;
}

double Player::position() const
{
    auto frame = m_base.load() + m_played.load();
    if (frame > m_frames && m_frames > 0) {
        frame %= m_frames; // looped round
    }
    return static_cast<double>(frame) / m_fileRate;
}

void Player::seek(double seconds)
{
    const auto frame =
        std::clamp(static_cast<std::int64_t>(seconds * m_fileRate), std::int64_t{0}, m_frames);
    m_seekTo.store(frame);
    m_finished.store(false);
}

void Player::process(float *samples, std::uint32_t frames, std::uint32_t channelCount) noexcept
{
    const std::size_t count = std::size_t{frames} * channelCount;
    if (channelCount != channels) {
        std::fill_n(samples, count, 0.0F);
        return;
    }
    if (const auto request = m_flushRequest.load(std::memory_order_acquire);
        request != m_flushDone.load(std::memory_order_relaxed)) {
        m_ring.clear();
        m_played.store(0, std::memory_order_relaxed);
        m_flushDone.store(request, std::memory_order_release);
        std::fill_n(samples, count, 0.0F);
        return;
    }
    if (m_paused.load(std::memory_order_relaxed)) {
        std::fill_n(samples, count, 0.0F);
        return;
    }
    const bool eof = m_eof.load(std::memory_order_acquire);
    const auto got = m_ring.pop(samples, count);
    std::fill_n(samples + got, count - got, 0.0F);
    m_played.fetch_add(static_cast<std::int64_t>(got / channels), std::memory_order_relaxed);
    if (const float gain = m_gain.load(std::memory_order_relaxed); gain != 1.0F) {
        for (std::size_t i = 0; i < got; ++i) {
            samples[i] *= gain; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
    }
    if (got < count && eof && m_ring.available() == 0 &&
        m_seekTo.load(std::memory_order_relaxed) < 0) {
        m_finished.store(true, std::memory_order_relaxed);
    }
}

void Player::run(const std::stop_token &stop)
{
    auto &file = *m_file;
    const auto fileChannels = static_cast<std::size_t>(file.info.channels);
    std::vector<float> decoded(chunkFrames * fileChannels);
    std::vector<float> stereo(chunkFrames * channels);
    while (!stop.stop_requested()) {
        if (auto to = m_seekTo.load(); to >= 0) {
            // Not at the end any more, before the queue empties: otherwise
            // the realtime thread would take the empty queue for the end.
            m_eof.store(false, std::memory_order_release);
            m_base.store(to);
            const auto request = m_flushRequest.load() + 1;
            m_flushRequest.store(request, std::memory_order_release);
            // The realtime thread drops the queue on its next cycle. While
            // the stream isn't running (not linked yet), that waits.
            while (m_flushDone.load(std::memory_order_acquire) != request) {
                if (stop.stop_requested()) {
                    return;
                }
                std::this_thread::sleep_for(2ms);
            }
            sf_seek(file.file.get(), to, SEEK_SET);
            m_seekTo.compare_exchange_strong(to, -1); // unless asked again meanwhile
            continue;
        }
        const auto room = m_ring.space() / channels;
        if (m_eof.load() || room < chunkFrames) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        const auto got = sf_readf_float(file.file.get(), decoded.data(),
                                        static_cast<sf_count_t>(chunkFrames));
        if (got <= 0) {
            if (m_loop.load() && m_frames > 0) {
                sf_seek(file.file.get(), 0, SEEK_SET); // round again, straight on
            } else {
                m_eof.store(true, std::memory_order_release);
            }
            continue;
        }
        const auto frames = static_cast<std::size_t>(got);
        for (std::size_t f = 0; f < frames; ++f) {
            const float left = decoded[f * fileChannels];
            stereo[f * 2] = left;
            stereo[(f * 2) + 1] = fileChannels > 1 ? decoded[(f * fileChannels) + 1] : left;
        }
        m_ring.push(stereo.data(), frames * channels);
    }
}

} // namespace pp
