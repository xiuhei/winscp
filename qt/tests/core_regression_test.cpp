#include "core/PathUtils.h"
#include "core/AppPaths.h"
#include "core/KnownHosts.h"
#include "core/Settings.h"
#include "proto/LocalBackend.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace wsc;

class CoreRegressionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("WINSCPQT_HOME", m_home.path().toUtf8());
        Settings::instance().load();
        Settings::instance().conflictPolicy = QStringLiteral("overwrite");
    }

    void remotePathsPreserveNames()
    {
        QCOMPARE(path::remoteJoin(QStringLiteral("/home"), QStringLiteral(" report ")),
                 QStringLiteral("/home/ report "));
        QCOMPARE(path::remoteJoin(QStringLiteral("/home"), QStringLiteral("a\\b")),
                 QStringLiteral("/home/a\\b"));
        QCOMPARE(path::remoteParent(QStringLiteral("/home/sub/")), QStringLiteral("/home"));
        QCOMPARE(path::normalizeRemote(QStringLiteral("/a//b/../c/")), QStringLiteral("/a/c"));
        QCOMPARE(path::resolveRemoteInitial(QStringLiteral("/home"), QStringLiteral("/ spaced ")),
                 QStringLiteral("/ spaced "));
    }

    void hostKeySaveFailureDoesNotTrustHost()
    {
        auto &hosts = KnownHosts::instance();
        const QString host = QStringLiteral("regression.example");
        const QString file = AppPaths::knownHostsFile();
        QVERIFY(QDir().mkpath(file));
        QString error;
        QVERIFY(!hosts.addOrUpdate(host, 22, QStringLiteral("ssh-ed25519"), QStringLiteral("SHA256:test"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!hosts.contains(host, 22));
        QVERIFY(QDir().rmdir(file));
        QVERIFY(hosts.addOrUpdate(host, 22, QStringLiteral("ssh-ed25519"), QStringLiteral("SHA256:test"), &error));
        hosts.reload();
        QCOMPARE(hosts.verify(host, 22, QStringLiteral("ssh-ed25519"), QStringLiteral("SHA256:test")), HostKeyStatus::Match);
        QVERIFY(hosts.remove(host, 22));
    }

    void downloadNamesAreWindowsSafe()
    {
        QCOMPARE(path::safeRemoteFileName(QStringLiteral("a:b?c*.txt")), QStringLiteral("a_b_c_.txt"));
        QCOMPARE(path::safeRemoteFileName(QStringLiteral("NUL.txt")), QStringLiteral("_NUL.txt"));
        QCOMPARE(path::safeRemoteFileName(QStringLiteral("com1")), QStringLiteral("_com1"));
        QCOMPARE(path::safeRemoteFileName(QStringLiteral("name. ")), QStringLiteral("name__"));
        QCOMPARE(path::safeRemoteFileName(QStringLiteral("../escape")), QStringLiteral(".._escape"));
        QCOMPARE(path::safeRemoteFileName(QStringLiteral("日志.txt")), QStringLiteral("日志.txt"));
    }

    void renameNeverDeletesAnExistingTarget()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalBackend backend;
        QString error;
        const QString source = directory.filePath(QStringLiteral("source.txt"));
        const QString target = directory.filePath(QStringLiteral("target.txt"));
        QVERIFY(backend.writeFile(source, "source", &error));
        QVERIFY(backend.writeFile(target, "target", &error));
        QVERIFY(!backend.rename(source, target, &error));
        QByteArray content;
        QVERIFY(backend.readFile(target, &content, &error));
        QCOMPARE(content, QByteArray("target"));
        QVERIFY(backend.rename(source, source, &error));
        QVERIFY(backend.readFile(source, &content, &error));
        QCOMPARE(content, QByteArray("source"));
        QVERIFY(!backend.rename(directory.filePath(QStringLiteral("missing")), target, &error));
        QVERIFY(backend.readFile(target, &content, &error));
        QCOMPARE(content, QByteArray("target"));
        QVERIFY(backend.rename(source, directory.filePath(QStringLiteral("renamed.txt")), &error));
    }

    void collidingDownloadsFailBeforeWriting()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalBackend backend;
        QString error;
        const QString source = directory.filePath(QStringLiteral("source.txt"));
        QVERIFY(backend.writeFile(source, "new", &error));
        FileInfo first = LocalBackend::infoForPath(source);
        FileInfo second = first;
        first.name = QStringLiteral("a:b.txt");
        second.name = QStringLiteral("a?b.txt");
        const QString targetDirectory = directory.filePath(QStringLiteral("target"));
        QVERIFY(QDir().mkpath(targetDirectory));
        const QString target = QDir(targetDirectory).filePath(QStringLiteral("a_b.txt"));
        QVERIFY(backend.writeFile(target, "original", &error));
        TransferManager transfers;
        QSignalSpy finished(&transfers, &TransferManager::allTransfersFinished);
        transfers.enqueueDownload(SessionData::localSession(), {first, second}, targetDirectory);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(transfers.model()->rows().first().status, TransferStatus::Failed);
        QByteArray content;
        QVERIFY(backend.readFile(target, &content, &error));
        QCOMPARE(content, QByteArray("original"));
    }

    void downloadTimestampOption_data()
    {
        QTest::addColumn<bool>("preserve");
        QTest::newRow("enabled") << true;
        QTest::newRow("disabled") << false;
    }

    void downloadTimestampOption()
    {
        QFETCH(bool, preserve);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalBackend backend;
        QString error;
        const QString source = directory.filePath(QStringLiteral("source.txt"));
        QVERIFY(backend.writeFile(source, "payload", &error));
        const QDateTime old = QDateTime::currentDateTime().addDays(-30);
        QVERIFY(backend.setModifiedTime(source, old, &error));
        Settings::instance().preserveTimestamps = preserve;
        TransferManager transfers;
        QSignalSpy finished(&transfers, &TransferManager::allTransfersFinished);
        transfers.enqueueDownload(SessionData::localSession(), {LocalBackend::infoForPath(source)},
                                  directory.filePath(QStringLiteral("target")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(transfers.model()->rows().first().status, TransferStatus::Completed);
        const QDateTime actual = QFileInfo(directory.filePath(QStringLiteral("target/source.txt"))).lastModified();
        if (preserve)
            QVERIFY(qAbs(actual.secsTo(old)) <= 2);
        else
            QVERIFY(qAbs(actual.secsTo(QDateTime::currentDateTime())) <= 10);
    }

    void downloadDoesNotFollowDestinationJunction()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QTemporaryDir outside;
        QVERIFY(directory.isValid());
        QVERIFY(outside.isValid());
        const QString target = directory.filePath(QStringLiteral("target"));
        QVERIFY(QDir().mkpath(target));
        const QString link = QDir(target).filePath(QStringLiteral("bundle"));
        QProcess junction;
        junction.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
            QStringLiteral("New-Item -ItemType Junction -Path '%1' -Target '%2' | Out-Null")
                .arg(link, outside.path())});
        QVERIFY(junction.waitForFinished(10000));
        QCOMPARE(junction.exitCode(), 0);
        QVERIFY(QFileInfo(link).isJunction());
        LocalBackend backend;
        QString error;
        const QString source = directory.filePath(QStringLiteral("source/bundle/file.txt"));
        QVERIFY(QDir().mkpath(QFileInfo(source).absolutePath()));
        QVERIFY(backend.writeFile(source, "payload", &error));
        TransferManager transfers;
        QSignalSpy finished(&transfers, &TransferManager::allTransfersFinished);
        transfers.enqueueDownload(SessionData::localSession(),
            {LocalBackend::infoForPath(QFileInfo(source).absolutePath())}, target);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(transfers.model()->rows().first().status, TransferStatus::Failed);
        QVERIFY(!QFileInfo::exists(outside.filePath(QStringLiteral("file.txt"))));
        QVERIFY(QDir().rmdir(link));
#endif
    }

private:
    QTemporaryDir m_home;
};

QTEST_MAIN(CoreRegressionTest)
#include "core_regression_test.moc"
