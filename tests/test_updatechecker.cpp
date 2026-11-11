// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "updatechecker.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

// No raw string literals here: moc misreads them (see CLAUDE.md).
// NOLINTBEGIN(modernize-raw-string-literal)
// The version comparison, and a check against a release file on disk (the
// network part is the same request; GitHub itself isn't asked in tests).
class TestUpdateChecker : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    [[nodiscard]] QString releaseFile(const QByteArray &json)
    {
        const auto path = m_dir.filePath(u"latest.json"_s);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(json);
        }
        return QUrl::fromLocalFile(path).toString();
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true); // settings in a test location
        QCoreApplication::setOrganizationDomain(u"kingsqueak.org"_s);
        QCoreApplication::setApplicationName(u"patchprime-test-updates"_s);
        QCoreApplication::setApplicationVersion(u"0.50"_s);
        QSettings().clear();
    }
    void cleanupTestCase() { QSettings().clear(); }

    void compare_data()
    {
        QTest::addColumn<QString>("candidate");
        QTest::addColumn<QString>("current");
        QTest::addColumn<bool>("newer");
        QTest::newRow("numbers, not text") << u"0.50"_s << u"0.15"_s << true;
        QTest::newRow("0.5 is older than 0.15") << u"0.5"_s << u"0.15"_s << false;
        QTest::newRow("point release") << u"0.50.1"_s << u"0.50"_s << true;
        QTest::newRow("same") << u"0.50"_s << u"0.50.0"_s << false;
        QTest::newRow("older") << u"0.15"_s << u"0.50"_s << false;
        QTest::newRow("leading v") << u"v0.51"_s << u"0.50"_s << true;
        QTest::newRow("suffix") << u"0.51-beta"_s << u"0.50"_s << true;
        QTest::newRow("empty") << QString() << u"0.50"_s << false;
    }
    void compare()
    {
        QFETCH(QString, candidate);
        QFETCH(QString, current);
        QFETCH(bool, newer);
        QCOMPARE(UpdateChecker::isNewer(candidate, current), newer);
    }

    void findsANewerRelease()
    {
        UpdateChecker checker(releaseFile("{\"tag_name\": \"v0.51\", \"html_url\": \"https://example.org/r\"}"), -1);
        QSignalSpy done(&checker, &UpdateChecker::checked);
        checker.check();
        QVERIFY(done.wait());
        QVERIFY(!checker.failed());
        QCOMPARE(checker.latest(), u"0.51"_s);
        QCOMPARE(checker.page(), u"https://example.org/r"_s);
        QVERIFY(checker.available());
        QVERIFY(checker.showNotice());

        // Closing the notice hides this version, also for the next start...
        checker.dismiss();
        QVERIFY(!checker.showNotice());
        UpdateChecker later(releaseFile("{\"tag_name\": \"v0.51\"}"), -1);
        QSignalSpy laterDone(&later, &UpdateChecker::checked);
        later.check();
        QVERIFY(laterDone.wait());
        QVERIFY(later.available());
        QVERIFY(!later.showNotice());
        QCOMPARE(later.page(), QString::fromLatin1(UpdateChecker::releasesPage));

        // ...but not the next one.
        UpdateChecker next(releaseFile("{\"tag_name\": \"v0.52\"}"), -1);
        QSignalSpy nextDone(&next, &UpdateChecker::checked);
        next.check();
        QVERIFY(nextDone.wait());
        QVERIFY(next.showNotice());
    }

    void sameVersionShowsNothing()
    {
        UpdateChecker checker(releaseFile("{\"tag_name\": \"v0.50\"}"), -1);
        QSignalSpy done(&checker, &UpdateChecker::checked);
        checker.check();
        QVERIFY(done.wait());
        QVERIFY(!checker.failed());
        QVERIFY(!checker.available());
        QVERIFY(!checker.showNotice());
    }

    // Only with PATCHPRIME_TEST_NETWORK=1: the real GitHub, through Qt's TLS.
    void realGitHub()
    {
        if (!qEnvironmentVariableIsSet("PATCHPRIME_TEST_NETWORK")) {
            QSKIP("asks GitHub; set PATCHPRIME_TEST_NETWORK=1 to run");
        }
        UpdateChecker checker(QString::fromLatin1(UpdateChecker::latestApi), -1);
        QSignalSpy done(&checker, &UpdateChecker::checked);
        checker.check();
        QVERIFY(done.wait(15000));
        QVERIFY(!checker.failed());
        QVERIFY(UpdateChecker::isNewer(checker.latest(), u"0.15"_s));
        qInfo("latest release: %s at %s", qPrintable(checker.latest()), qPrintable(checker.page()));
    }

    void unreadableAnswerFails()
    {
        UpdateChecker broken(releaseFile("<html>rate limited</html>"), -1);
        QSignalSpy done(&broken, &UpdateChecker::checked);
        broken.check();
        QVERIFY(done.wait());
        QVERIFY(broken.failed());
        QVERIFY(!broken.available());

        UpdateChecker missing(QUrl::fromLocalFile(m_dir.filePath(u"none.json"_s)).toString(), -1);
        QSignalSpy missingDone(&missing, &UpdateChecker::checked);
        missing.check();
        QVERIFY(missingDone.wait());
        QVERIFY(missing.failed());
    }
};

// NOLINTEND(modernize-raw-string-literal)

QTEST_GUILESS_MAIN(TestUpdateChecker)
#include "test_updatechecker.moc"
