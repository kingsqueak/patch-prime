// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include "devicestore.h"
#include "endpoint.h"

#include <QAbstractListModel>
#include <QTimer>

#include <optional>
#include <vector>

namespace pp {

class PipeWireGraph;

// Every audio input and output: the ones PipeWire has now, plus the ones the
// user configured that are currently missing (shown offline, never forgotten
// until the user says so). Rows are sorted inputs first, then hardware before
// virtual, then by system description, so editing a label never moves a row.
//
// When an endpoint appears under a node.name nothing is saved for, it's matched
// against the offline saved endpoints by signature (see matchSignature) and,
// when one clearly fits, that entry is moved to the new name.
class DeviceModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

public:
    // Unscoped on purpose: Qt passes roles around as plain ints.
    enum Role : std::uint16_t // NOLINT(cppcoreguidelines-use-enum-class)
    {
        KeyRole = Qt::UserRole + 1,
        LabelRole,       // the user's label, or the description when there's none
        CustomLabelRole, // the user's label, possibly empty
        DescriptionRole,
        NodeNameRole,
        DirectionRole, // "input" or "output"
        VirtualRole,
        OnlineRole,
        EnabledRole,
        ChannelsRole,
        PositionsRole,    // e.g. "FL FR"
        DefaultRole,      // the system default sink or source
        SavedRole,        // has an entry in the store
        SectionStartRole, // first row of its direction; the QML draws the header
        VolumeRole,       // fader position in dB; minimumVolumeDb() means silent
        MutedRole,
        BusIdRole,
    };
    Q_ENUM(Role)

    explicit DeviceModel(DeviceStore *store, QObject *parent = nullptr);
    ~DeviceModel() override;
    DeviceModel(const DeviceModel &) = delete;
    DeviceModel &operator=(const DeviceModel &) = delete;
    DeviceModel(DeviceModel &&) = delete;
    DeviceModel &operator=(DeviceModel &&) = delete;

    // Follows a live graph. Without one, feed the model with the node functions
    // below (that's what the tests do).
    void attach(PipeWireGraph *graph);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void setEnabled(int row, bool enabled);
    Q_INVOKABLE void setLabel(int row, const QString &label);
    // Drops the saved settings. An offline endpoint disappears from the list.
    Q_INVOKABLE void forget(int row);
    Q_INVOKABLE void setVolume(int row, double volumeDb);
    Q_INVOKABLE void setMuted(int row, bool muted);

    // The fader's bottom: at or below this the bus is silent.
    static constexpr double minimumVolumeDb() { return -60.0; }
    static constexpr double maximumVolumeDb() { return 12.0; }

    struct Row
    {
        Endpoint endpoint;
        std::optional<std::uint32_t> nodeId; // set while online
    };
    [[nodiscard]] const std::vector<Row> &rows() const { return m_rows; }
    [[nodiscard]] const DeviceStore &store() const { return *m_store; }

    [[nodiscard]] bool isConnected() const { return m_connected; }

    void upsertNode(const GraphObject &node, const GraphObject *device);
    void removeNode(std::uint32_t nodeId);
    void setDefaults(const std::string &sinkName, const std::string &sourceName);
    void setConnected(bool connected);

    [[nodiscard]] int rowForKey(const std::string &key) const;

Q_SIGNALS:
    void connectedChanged();
    // Anything the bus manager acts on changed for this endpoint: enabled,
    // label, volume, mute, forgotten, or its key moved (then both keys).
    void settingsChanged(const QString &key);
    // A saved endpoint was recognised under a new node.name.
    void reattached(const QString &label, const QString &oldNodeName, const QString &newNodeName);

private:
    [[nodiscard]] static bool lessThan(const Row &a, const Row &b);
    [[nodiscard]] int rowForNode(std::uint32_t nodeId) const;
    [[nodiscard]] int findReattachCandidate(const Endpoint &live) const;
    [[nodiscard]] bool isDefault(const Row &row) const;
    void insertSorted(Row row);
    void replaceRow(int index, Row row);
    void removeRowAt(int index);
    void refreshSectionStarts();
    void remember(const Row &row, bool seenNow);
    void saveStore();
    void saveLater();

    DeviceStore *m_store;
    PipeWireGraph *m_graph = nullptr;
    std::vector<Row> m_rows;
    std::string m_defaultSink;
    std::string m_defaultSource;
    bool m_connected = false;
    QTimer m_saveTimer;
};

} // namespace pp
