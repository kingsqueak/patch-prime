// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "mackiecontrol.h"

#include "busmanager.h"
#include "devicemodel.h"
#include "midiport.h"
#include "recordingmanager.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace pp {

namespace {

// Mackie Control's notes (buttons and their lights) and controllers.
constexpr int noteRec = 0x00;
constexpr int noteSolo = 0x08;
constexpr int noteMute = 0x10;
constexpr int noteSelect = 0x18;
constexpr int noteVPotPush = 0x20;
constexpr int noteSend = 0x29;
constexpr int noteBankLeft = 0x2E;
constexpr int noteBankRight = 0x2F;
constexpr int noteChannelLeft = 0x30;
constexpr int noteChannelRight = 0x31;
constexpr int noteF1 = 0x36;
constexpr int noteF8 = 0x3D;
constexpr int noteTouch = 0x68; // strip 1's fader touched, ... 0x70 the master's
constexpr int ccVPot = 0x10;
constexpr int ccRing = 0x30;
constexpr int masterChannel = 8;
constexpr int faderTop = 16383;
constexpr int lineLength = 56; // characters on each of the display's two lines
constexpr int cellLength = 7;  // per strip
// The display's sysex header: Mackie's id, then the Mackie Control's.
constexpr std::array<std::uint8_t, 5> sysexHeader{0xF0, 0x00, 0x00, 0x66, 0x14};
constexpr std::uint8_t sysexDeviceQuery = 0x00;
constexpr std::uint8_t sysexHostQuery = 0x01;
constexpr std::uint8_t sysexHostReply = 0x02;
constexpr std::uint8_t sysexText = 0x12;
constexpr std::size_t serialLength = 7;

// Points on the fader law: (position 0..1, dB), straight lines between.
constexpr std::array<std::pair<double, double>, 6> faderLaw{{
    {0.0, DeviceModel::minimumVolumeDb()},
    {0.15, -40.0},
    {0.35, -20.0},
    {0.55, -10.0},
    {0.75, 0.0},
    {1.0, DeviceModel::maximumVolumeDb()},
}};

double faderPositionImpl(double db)
{
    db = std::clamp(db, faderLaw.front().second, faderLaw.back().second);
    for (std::size_t i = 1; i < faderLaw.size(); ++i) {
        const auto [p0, d0] = faderLaw.at(i - 1);
        const auto [p1, d1] = faderLaw.at(i);
        if (db <= d1) {
            return p0 + ((db - d0) / (d1 - d0) * (p1 - p0));
        }
    }
    return 1.0;
}

enum class StageOrder : std::uint8_t
{
    Input,
    Bus,
    Output,
    None,
};

StageOrder stageAt(const DeviceModel &devices, int row)
{
    const auto stage = devices.data(devices.index(row), DeviceModel::StageRole).toString();
    if (stage == QLatin1String("input")) {
        return StageOrder::Input;
    }
    if (stage == QLatin1String("bus")) {
        return StageOrder::Bus;
    }
    if (stage == QLatin1String("output")) {
        return StageOrder::Output;
    }
    return StageOrder::None;
}

QVariant roleAt(const DeviceModel &devices, int row, DeviceModel::Role role)
{
    return devices.data(devices.index(row), role);
}

// The display shows ASCII only: anything else becomes '?'.
std::string ascii(const QString &text)
{
    std::string out;
    for (const QChar c : text) {
        out += c.unicode() >= 0x20 && c.unicode() < 0x7F ? static_cast<char>(c.unicode()) : '?';
    }
    return out;
}

// text cut or padded to n characters.
std::string fit(std::string text, std::size_t n)
{
    text.resize(n, ' ');
    return text;
}

std::string dbText(double db)
{
    if (db <= DeviceModel::minimumVolumeDb()) {
        return "  -inf ";
    }
    const auto text = std::abs(db) < 0.05 ? std::string("0.0") : std::format("{:+.1f}", db);
    return fit(std::format("{:>6}", text), cellLength);
}

} // namespace

double faderLawPosition(double db)
{
    return faderPositionImpl(db);
}

double faderLawDb(double position)
{
    const double p = std::clamp(position, 0.0, 1.0);
    for (std::size_t i = 1; i < faderLaw.size(); ++i) {
        const auto [p0, d0] = faderLaw.at(i - 1);
        const auto [p1, d1] = faderLaw.at(i);
        if (p <= p1) {
            return d0 + ((p - p0) / (p1 - p0) * (d1 - d0));
        }
    }
    return faderLaw.back().second;
}

double mackieFaderDb(int value)
{
    const double db = faderLawDb(std::clamp(value, 0, faderTop) / static_cast<double>(faderTop));
    return std::round(db * 10.0) / 10.0;
}

int mackieFaderValue(double db)
{
    return static_cast<int>(std::lround(faderLawPosition(db) * faderTop));
}

int mackieMeterStep(double db)
{
    constexpr std::array<double, 12> steps{-60, -50, -40, -30, -20, -14, -10, -8, -6, -4, -2, 0};
    return static_cast<int>(std::ranges::count_if(steps, [db](double s) { return db >= s; }));
}

std::array<std::uint8_t, 4> mackieChallengeResponse(const std::array<std::uint8_t, 4> &challenge)
{
    const int l1 = challenge[0];
    const int l2 = challenge[1];
    const int l3 = challenge[2];
    const int l4 = challenge[3];
    const auto low7 = [](int v) { return static_cast<std::uint8_t>(v & 0x7F); };
    return {low7(l1 + (l2 ^ 0x0A) - l4), low7((l3 >> 4) ^ (l1 + l4)),
            low7((l4 - (l3 << 2)) ^ (l1 | l2)), low7(l2 - l3 + (0xF0 ^ (l4 << 4)))};
}

MackieControl::MackieControl(DeviceModel *devices, MidiPort *port, QObject *parent)
    : QObject(parent)
    , m_devices(devices)
    , m_port(port)
{
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(10);
    connect(&m_syncTimer, &QTimer::timeout, this, &MackieControl::sync);
    m_meterTimer.setInterval(100);
    connect(&m_meterTimer, &QTimer::timeout, this, &MackieControl::sendMeters);
    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(1500);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        m_flash.clear();
        scheduleSync();
    });

    connect(m_port, &MidiPort::received, this, &MackieControl::receive);
    connect(m_port, &MidiPort::connectedChanged, this, &MackieControl::connectionChanged);
    connect(m_devices, &QAbstractItemModel::dataChanged, this, &MackieControl::scheduleSync);
    connect(m_devices, &QAbstractItemModel::rowsInserted, this, &MackieControl::scheduleSync);
    connect(m_devices, &QAbstractItemModel::rowsRemoved, this, &MackieControl::scheduleSync);
    connect(m_devices, &QAbstractItemModel::modelReset, this, &MackieControl::scheduleSync);
    connect(m_devices, &QAbstractItemModel::layoutChanged, this, &MackieControl::scheduleSync);
    connect(m_devices, &DeviceModel::cueChanged, this, &MackieControl::scheduleSync);
    if (m_port->isConnected()) {
        connectionChanged();
    }
}

void MackieControl::setRecordings(RecordingManager *recordings)
{
    m_recordings = recordings;
    connect(m_recordings, &RecordingManager::changed, this, &MackieControl::scheduleSync);
}

bool MackieControl::wantsMeters() const
{
    return m_port->isConnected();
}

std::vector<std::string> MackieControl::strips() const
{
    std::vector<std::string> out;
    for (const auto stage : {StageOrder::Input, StageOrder::Bus, StageOrder::Output}) {
        for (int row = 0; row < m_devices->rowCount(); ++row) {
            if (stageAt(*m_devices, row) == stage &&
                roleAt(*m_devices, row, DeviceModel::EnabledRole).toBool()) {
                out.push_back(roleAt(*m_devices, row, DeviceModel::KeyRole).toString().toStdString());
            }
        }
    }
    return out;
}

std::string MackieControl::keyAt(int strip) const
{
    const auto all = strips();
    const int at = m_bank + strip;
    return at >= 0 && std::cmp_less(at, all.size()) ? all[static_cast<std::size_t>(at)] : std::string{};
}

std::string MackieControl::masterKey() const
{
    std::string first;
    for (int row = 0; row < m_devices->rowCount(); ++row) {
        if (stageAt(*m_devices, row) != StageOrder::Output ||
            !roleAt(*m_devices, row, DeviceModel::EnabledRole).toBool()) {
            continue;
        }
        auto key = roleAt(*m_devices, row, DeviceModel::KeyRole).toString().toStdString();
        if (roleAt(*m_devices, row, DeviceModel::BusDefaultRole).toBool()) {
            return key;
        }
        if (first.empty()) {
            first = std::move(key);
        }
    }
    return first;
}

std::vector<std::string> MackieControl::destinations() const
{
    std::vector<std::string> out;
    for (const auto &key : strips()) {
        const auto stage = stageAt(*m_devices, m_devices->rowForKey(key));
        if (stage == StageOrder::Bus || stage == StageOrder::Output) {
            out.push_back(key);
        }
    }
    return out;
}

bool MackieControl::canSend(const std::string &from, const std::string &to) const
{
    const int fromRow = m_devices->rowForKey(from);
    const int toRow = m_devices->rowForKey(to);
    if (fromRow < 0 || toRow < 0) {
        return false;
    }
    const auto a = stageAt(*m_devices, fromRow);
    const auto b = stageAt(*m_devices, toRow);
    return a != StageOrder::None && b != StageOrder::None && a < b && b != StageOrder::Input;
}

int MackieControl::faderOf(const std::string &key) const
{
    const int row = m_devices->rowForKey(key);
    return row < 0 ? 0
                   : mackieFaderValue(roleAt(*m_devices, row, DeviceModel::VolumeRole).toDouble());
}

MackieControl::StripView MackieControl::viewOf(const std::string &key) const
{
    StripView view{.fader = 0,
                   .ring = 0,
                   .top = std::string(cellLength, ' '),
                   .bottom = std::string(cellLength, ' ')};
    const int row = key.empty() ? -1 : m_devices->rowForKey(key);
    if (row < 0) {
        return view;
    }
    const double volume = roleAt(*m_devices, row, DeviceModel::VolumeRole).toDouble();
    view.fader = mackieFaderValue(volume);
    view.mute = roleAt(*m_devices, row, DeviceModel::MutedRole).toBool();
    view.solo = roleAt(*m_devices, row, DeviceModel::CuedRole).toBool();
    view.rec = m_recordings != nullptr && m_recordings->recording().contains(key);
    view.select = key == m_sendTarget;
    view.top = fit(ascii(roleAt(*m_devices, row, DeviceModel::LabelRole).toString()), cellLength - 1) + ' ';
    view.bottom = dbText(volume);
    if (!m_sendTarget.empty() && canSend(key, m_sendTarget)) {
        const auto target = QString::fromStdString(m_sendTarget);
        if (roleAt(*m_devices, row, DeviceModel::SendsRole).toStringList().contains(target)) {
            const double level =
                roleAt(*m_devices, row, DeviceModel::SendLevelsRole).toMap().value(target, 0.0).toDouble();
            constexpr int ringFill = 0x20; // lit from the left up to the position
            view.ring = ringFill | (1 + static_cast<int>(std::lround(faderLawPosition(level) * 10)));
            view.bottom = dbText(level);
        } else {
            view.bottom = "  off  ";
        }
    }
    return view;
}

void MackieControl::connectionChanged()
{
    if (m_port->isConnected()) {
        resetSurface();
        m_meterTimer.start();
    } else {
        m_meterTimer.stop();
        m_parser.reset();
        m_touched = {};
    }
}

void MackieControl::resetSurface()
{
    m_sent = {};
    m_sentMaster.reset();
    m_sentTop.clear();
    m_sentBottom.clear();
    m_sentMeter = {};
    m_touched = {};
    std::vector<std::uint8_t> query(sysexHeader.begin(), sysexHeader.end());
    query.push_back(sysexDeviceQuery);
    query.push_back(0xF7);
    sendRaw(std::move(query));
    sync();
}

void MackieControl::receive(const QByteArray &bytes)
{
    m_parser.feed(bytes, [this](const std::vector<std::uint8_t> &message) { handle(message); });
}

void MackieControl::handle(const std::vector<std::uint8_t> &message)
{
    const int kind = message.front() & 0xF0;
    if (message.front() == 0xF0) {
        handleSysex(message);
    } else if (kind == 0x90) {
        handleNote(message[1], message[2] > 0);
    } else if (kind == 0x80) {
        handleNote(message[1], false);
    } else if (kind == 0xE0) {
        handleFader(message.front() & 0x0F, message[1] | (message[2] << 7));
    } else if (kind == 0xB0 && message[1] >= ccVPot && message[1] < ccVPot + stripCount) {
        handleVPot(message[1] - ccVPot, message[2]);
    }
}

void MackieControl::handleSysex(const std::vector<std::uint8_t> &message)
{
    // A real Mackie Control asks who's there: its serial number and a
    // challenge, to be answered (other controllers don't bother).
    constexpr std::size_t queryLength = sysexHeader.size() + 1 + serialLength + 4 + 1;
    if (message.size() != queryLength ||
        !std::equal(sysexHeader.begin(), sysexHeader.end(), message.begin()) ||
        message[sysexHeader.size()] != sysexHostQuery) {
        return;
    }
    const auto serial = message.begin() + sysexHeader.size() + 1;
    std::array<std::uint8_t, 4> challenge{};
    std::copy_n(serial + serialLength, 4, challenge.begin());
    std::vector<std::uint8_t> reply(sysexHeader.begin(), sysexHeader.end());
    reply.push_back(sysexHostReply);
    reply.insert(reply.end(), serial, serial + serialLength);
    for (const auto r : mackieChallengeResponse(challenge)) {
        reply.push_back(r);
    }
    reply.push_back(0xF7);
    sendRaw(std::move(reply));
}

void MackieControl::handleNote(int note, bool pressed)
{
    if (note >= noteTouch && note <= noteTouch + stripCount) {
        const auto strip = static_cast<std::size_t>(note - noteTouch);
        m_touched.at(strip) = pressed;
        if (!pressed) {
            // Let go: the motor puts the fader where the mixer has it.
            if (strip == masterChannel) {
                m_sentMaster.reset();
            } else if (auto &sent = m_sent.at(strip); sent.has_value()) {
                sent->fader = -1;
            }
            scheduleSync();
        }
        return;
    }
    if (!pressed) {
        return;
    }
    const auto stripKey = [this, note](int base) { return keyAt(note - base); };
    const auto inRange = [note](int base) { return note >= base && note < base + stripCount; };
    if (inRange(noteRec)) {
        const auto key = stripKey(noteRec);
        if (key.empty() || m_recordings == nullptr) {
            return;
        }
        if (m_recordings->recording().contains(key)) {
            m_recordings->stop(key);
        } else if (QString error; !m_recordings->start(key, &error)) {
            flash(ascii(error));
        }
    } else if (inRange(noteSolo) || inRange(noteMute)) {
        const bool solo = inRange(noteSolo);
        const int row = m_devices->rowForKey(stripKey(solo ? noteSolo : noteMute));
        if (row < 0) {
            return;
        }
        if (solo) {
            m_devices->setCue(row, !roleAt(*m_devices, row, DeviceModel::CuedRole).toBool());
        } else {
            m_devices->setMuted(row, !roleAt(*m_devices, row, DeviceModel::MutedRole).toBool());
        }
    } else if (inRange(noteSelect)) {
        const auto key = stripKey(noteSelect);
        const int row = m_devices->rowForKey(key);
        if (row < 0) {
            return;
        }
        const auto stage = stageAt(*m_devices, row);
        if (stage != StageOrder::Bus && stage != StageOrder::Output) {
            flash("V-Pots send to a mix bus or an output: SELECT one of those");
            return;
        }
        m_sendTarget = key == m_sendTarget ? std::string{} : key;
        flash(m_sendTarget.empty()
                  ? std::string("V-Pots: off")
                  : "V-Pots: sends to " + ascii(roleAt(*m_devices, row, DeviceModel::LabelRole).toString()));
    } else if (inRange(noteVPotPush)) {
        const auto key = stripKey(noteVPotPush);
        const int row = m_devices->rowForKey(key);
        if (row < 0 || m_sendTarget.empty() || !canSend(key, m_sendTarget)) {
            return;
        }
        const auto target = QString::fromStdString(m_sendTarget);
        const bool on = roleAt(*m_devices, row, DeviceModel::SendsRole).toStringList().contains(target);
        m_devices->setSend(row, target, !on);
    } else if (note == noteSend) {
        const auto all = destinations();
        // Steps through the destinations, then off.
        auto next = std::ranges::find(all, m_sendTarget);
        next = next == all.end() ? all.begin() : std::next(next);
        m_sendTarget = next == all.end() ? std::string{} : *next;
        const int row = m_devices->rowForKey(m_sendTarget);
        flash(row < 0 ? std::string("V-Pots: off")
                      : "V-Pots: sends to " +
                            ascii(roleAt(*m_devices, row, DeviceModel::LabelRole).toString()));
    } else if (note == noteBankLeft || note == noteBankRight) {
        setBank(m_bank + (note == noteBankLeft ? -stripCount : stripCount));
    } else if (note == noteChannelLeft || note == noteChannelRight) {
        setBank(m_bank + (note == noteChannelLeft ? -1 : 1));
    } else if (note >= noteF1 && note <= noteF8) {
        Q_EMIT sceneRequested(note - noteF1);
    }
}

void MackieControl::handleFader(int channel, int value)
{
    if (channel > masterChannel) {
        return;
    }
    const auto key = channel == masterChannel ? masterKey() : keyAt(channel);
    const int row = m_devices->rowForKey(key);
    if (row < 0) {
        return;
    }
    m_devices->setVolume(row, mackieFaderDb(value));
    // Don't move the motor back to where the level rounded to.
    if (channel == masterChannel) {
        m_sentMaster = faderOf(key);
    } else if (auto &sent = m_sent.at(static_cast<std::size_t>(channel))) {
        sent->fader = faderOf(key);
    }
}

void MackieControl::handleVPot(int strip, int value)
{
    const auto key = keyAt(strip);
    const int row = m_devices->rowForKey(key);
    if (row < 0 || m_sendTarget.empty() || !canSend(key, m_sendTarget)) {
        return;
    }
    const auto target = QString::fromStdString(m_sendTarget);
    if (!roleAt(*m_devices, row, DeviceModel::SendsRole).toStringList().contains(target)) {
        return;
    }
    // Relative: 1..63 clicks clockwise, 65..127 (bit 6 set) anticlockwise.
    const int clicks = (value & 0x40) != 0 ? -(value & 0x3F) : (value & 0x3F);
    constexpr double dbPerClick = 0.5;
    const double level =
        roleAt(*m_devices, row, DeviceModel::SendLevelsRole).toMap().value(target, 0.0).toDouble();
    m_devices->setSendLevel(row, target, level + (clicks * dbPerClick));
}

void MackieControl::setBank(int bank)
{
    const int last = std::max(0, static_cast<int>(strips().size()) - stripCount);
    bank = std::clamp(bank, 0, last);
    if (bank != m_bank) {
        m_bank = bank;
        scheduleSync();
    }
}

void MackieControl::flash(const std::string &text)
{
    m_flash = text;
    m_flashTimer.start();
    scheduleSync();
}

void MackieControl::scheduleSync()
{
    if (m_port->isConnected() && !m_syncTimer.isActive()) {
        m_syncTimer.start();
    }
}

void MackieControl::sync()
{
    if (!m_port->isConnected()) {
        return;
    }
    if (!m_sendTarget.empty() && !std::ranges::contains(destinations(), m_sendTarget)) {
        m_sendTarget.clear();
    }
    setBank(m_bank); // strips may have gone
    std::string top;
    std::string bottom;
    for (int i = 0; i < stripCount; ++i) {
        const auto view = viewOf(keyAt(i));
        auto &sent = m_sent.at(static_cast<std::size_t>(i));
        const bool all = !sent.has_value();
        if (all) {
            sent = StripView{};
        }
        if (all || view.mute != sent->mute) {
            sendNote(noteMute + i, view.mute);
        }
        if (all || view.solo != sent->solo) {
            sendNote(noteSolo + i, view.solo);
        }
        if (all || view.rec != sent->rec) {
            sendNote(noteRec + i, view.rec);
        }
        if (all || view.select != sent->select) {
            sendNote(noteSelect + i, view.select);
        }
        if (all || view.ring != sent->ring) {
            sendRaw({0xB0, static_cast<std::uint8_t>(ccRing + i), static_cast<std::uint8_t>(view.ring)});
        }
        // A touched fader is left alone, and moved once let go.
        int fader = sent->fader;
        if (!m_touched.at(static_cast<std::size_t>(i)) && view.fader != fader) {
            sendFader(i, view.fader);
            fader = view.fader;
        }
        *sent = view;
        sent->fader = fader;
        top += view.top;
        bottom += view.bottom;
    }
    const auto master = masterKey();
    const int masterFader = faderOf(master);
    if (!m_touched.at(masterChannel) && m_sentMaster != masterFader) {
        sendFader(masterChannel, masterFader);
        m_sentMaster = masterFader;
    }
    if (!m_flash.empty()) {
        top = fit(m_flash, lineLength);
    }
    sendLine(0, top, m_sentTop);
    sendLine(lineLength, bottom, m_sentBottom);
}

void MackieControl::sendMeters()
{
    if (m_buses == nullptr || !m_buses->isMetering()) {
        return;
    }
    const auto levels = m_buses->loudness();
    for (int i = 0; i < stripCount; ++i) {
        int step = 0;
        if (const auto at = levels.find(keyAt(i)); at != levels.end() && !at->second.rms.empty()) {
            const float rms = std::ranges::max(at->second.rms);
            step = rms > 0.0F ? mackieMeterStep(20.0 * std::log10(static_cast<double>(rms))) : 0;
        }
        // The controller's meters fall by themselves, so keep lit ones fed.
        auto &sent = m_sentMeter.at(static_cast<std::size_t>(i));
        if (step > 0 || sent > 0) {
            sendRaw({0xD0, static_cast<std::uint8_t>((i << 4) | step)});
            sent = step;
        }
    }
}

void MackieControl::sendNote(int note, bool on)
{
    sendRaw({0x90, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(on ? 0x7F : 0x00)});
}

void MackieControl::sendFader(int channel, int value)
{
    sendRaw({static_cast<std::uint8_t>(0xE0 | channel), static_cast<std::uint8_t>(value & 0x7F),
             static_cast<std::uint8_t>((value >> 7) & 0x7F)});
}

void MackieControl::sendLine(int offset, const std::string &line, std::string &sent)
{
    // Only the part that changed: the display keeps the rest.
    std::size_t first = 0;
    while (first < line.size() && first < sent.size() && line[first] == sent[first]) {
        ++first;
    }
    if (first == line.size() && sent.size() == line.size()) {
        return;
    }
    std::size_t last = line.size();
    if (sent.size() == line.size()) {
        while (last > first && line[last - 1] == sent[last - 1]) {
            --last;
        }
    }
    std::vector<std::uint8_t> message(sysexHeader.begin(), sysexHeader.end());
    message.push_back(sysexText);
    message.push_back(static_cast<std::uint8_t>(offset + static_cast<int>(first)));
    for (std::size_t i = first; i < last; ++i) {
        message.push_back(static_cast<std::uint8_t>(line[i]));
    }
    message.push_back(0xF7);
    sendRaw(std::move(message));
    sent = line;
}

void MackieControl::sendRaw(std::vector<std::uint8_t> message)
{
    m_port->send(message);
}

} // namespace pp
