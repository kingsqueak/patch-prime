// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;

// Is there a newer Patch Prime? Asks GitHub for the latest release (once, a
// few seconds after start, and from Help > Check for Updates…) and compares
// its version with this one. Nothing is downloaded or installed, and nothing
// is sent but the request itself. The same design as 590Ctrl's and
// SimpleHPSDR's checkers.
class UpdateChecker : public QObject
{
    Q_OBJECT
    // The newest release found, and its page; empty until a check succeeds.
    Q_PROPERTY(QString latest READ latest NOTIFY changed)
    Q_PROPERTY(QString page READ page NOTIFY changed)
    Q_PROPERTY(QString current READ current CONSTANT)
    // A check is running.
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    // The last check couldn't reach GitHub, or got an answer it couldn't read.
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    // The latest release is newer than this one.
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // ... and its notice hasn't been closed (closing it hides that version).
    Q_PROPERTY(bool showNotice READ showNotice NOTIFY changed)

public:
    static constexpr const char *releasesPage = "https://github.com/kingsqueak/patch-prime/releases";
    static constexpr const char *latestApi = "https://api.github.com/repos/kingsqueak/patch-prime/releases/latest";

    // url: where to ask (PATCHPRIME_UPDATE_URL overrides it, for testing; a
    // file:// URL works). startDelayMs < 0: no check at start.
    explicit UpdateChecker(QString url, int startDelayMs, QObject *parent = nullptr);

    [[nodiscard]] QString latest() const { return m_latest; }
    [[nodiscard]] QString page() const { return m_page; }
    [[nodiscard]] static QString current();
    [[nodiscard]] bool checking() const { return m_checking; }
    [[nodiscard]] bool failed() const { return m_failed; }
    [[nodiscard]] bool available() const;
    [[nodiscard]] bool showNotice() const;

    Q_INVOKABLE void check();
    // Hides the notice for this version (kept in patchprime.conf).
    Q_INVOKABLE void dismiss();

    // "0.50" against "0.15", part by part as numbers, as the package managers
    // compare them (so 0.5 is older than 0.15); a leading v is ignored, and so
    // is anything after a part's digits ("0.51-beta").
    [[nodiscard]] static bool isNewer(const QString &candidate, const QString &current);

signals:
    void changed();
    // A check has finished (for Help > Check for Updates…).
    void checked();

private:
    QNetworkAccessManager *m_network;
    QTimer m_startTimer;
    QString m_url;
    QString m_latest;
    QString m_page;
    QString m_dismissed;
    bool m_checking = false;
    bool m_failed = false;
};
