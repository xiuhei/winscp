#include "core/FileSystemController.h"
#include "core/PathUtils.h"
#include "core/KnownHosts.h"
#include "terminal/SshShellSession.h"

#include <QCoreApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QtTest>

using namespace wsc;

static QString plain(const QByteArray &output)
{
    QString text = QString::fromUtf8(output);
    text.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-?]*[ -/]*[@-~]")));
    text.remove(QLatin1Char('\r'));
    return text;
}

class SharedSessionTest : public QObject {
    Q_OBJECT
public:
    SessionData session;
    bool realServer = false;

private slots:
    void initTestCase()
    {
        m_fs.setHostKeyPrompt([](const QString &, int, const QString &, const QString &, QString *) {
            return true;
        });
        QSignalSpy listed(&m_fs, &FileSystemController::directoryListed);
        QSignalSpy failed(&m_fs, &FileSystemController::connectFailed);
        m_fs.connectToHost(session);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs.isConnected() || !failed.isEmpty(), 20000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QTRY_VERIFY_WITH_TIMEOUT(!listed.isEmpty(), 20000);
        m_base = m_fs.currentPath();
    }

    void shellsShareFileConnection()
    {
        // The server writes its transport ID through the existing file channel.
        const QString identityPath = path::remoteJoin(m_base, QStringLiteral(".terminal-connection"));
        QSignalSpy fileRead(&m_fs, &FileSystemController::fileRead);
        m_fs.readFile(identityPath);
        QTRY_VERIFY_WITH_TIMEOUT(!fileRead.isEmpty(), 10000);
        const QByteArray fileIdentity = fileRead.takeFirst().at(1).toByteArray().trimmed();
        QVERIFY(!fileIdentity.isEmpty());

        auto *first = m_fs.createShell(100, 30);
        auto *second = m_fs.createShell(100, 30);
        m_first = first;
        m_second = second;
        QSignalSpy readyFirst(first, &SshShellSession::connected);
        QSignalSpy readySecond(second, &SshShellSession::connected);
        QSignalSpy errorsFirst(first, &SshShellSession::sessionError);
        QSignalSpy errorsSecond(second, &SshShellSession::sessionError);
        connect(first, &SshShellSession::outputReady, this, [this](const QByteArray &data) { m_outputFirst += data; });
        connect(second, &SshShellSession::outputReady, this, [this](const QByteArray &data) { m_outputSecond += data; });
        m_fs.startShell(first);
        m_fs.startShell(second);
        QTRY_COMPARE_WITH_TIMEOUT(readyFirst.size(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(readySecond.size(), 1, 10000);

        const QByteArray command = realServer
            ? QByteArray("printf 'CONNECTION:%s\\n' \"$SSH_CONNECTION\"\r")
            : QByteArray("connection-id\r");
        first->writeData(command);
        second->writeData(command);
        const QByteArray expected = QByteArray("CONNECTION:") + fileIdentity;
        QTRY_VERIFY_WITH_TIMEOUT(m_outputFirst.contains(expected), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(m_outputSecond.contains(expected), 10000);
        QVERIFY(errorsFirst.isEmpty());
        QVERIFY(errorsSecond.isEmpty());

        QSignalSpy saved(&m_fs, &FileSystemController::fileWriteFinished);
        const QString target = path::remoteJoin(m_base, QStringLiteral("terminal-roundtrip.txt"));
        const QByteArray payload("file operations while both PTYs are open\n");
        m_fs.writeFile(target, payload);
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 10000);
        QVERIFY(saved.first().at(1).toBool());
        m_fs.readFile(target);
        QTRY_COMPARE_WITH_TIMEOUT(fileRead.size(), 1, 10000);
        QCOMPARE(fileRead.first().at(1).toByteArray(), payload);
        first->writeData("echo TERMINAL_SMOKE_OK\r");
        QTRY_VERIFY2_WITH_TIMEOUT(plain(m_outputFirst).contains(QStringLiteral("\nTERMINAL_SMOKE_OK\n")),
                                  m_outputFirst.constData(), 10000);
    }

    void resizeAndInteractiveProgram()
    {
        QVERIFY(m_first);
        m_first->resizePty(120, 40);
        if (!realServer)
            return;
        QTest::qWait(100);
        m_first->writeData("stty size\r");
        QTRY_VERIFY2_WITH_TIMEOUT(plain(m_outputFirst).contains(QStringLiteral("\n40 120\n")),
                                  m_outputFirst.constData(), 10000);
        m_first->writeData("sleep 20\r");
        QTest::qWait(150);
        m_first->writeData(QByteArray(1, '\x03'));
        m_first->writeData("echo INTERRUPT_OK\r");
        QTRY_VERIFY_WITH_TIMEOUT(plain(m_outputFirst).contains(QStringLiteral("\nINTERRUPT_OK\n")), 10000);
        m_first->writeData("vi /tmp/winscpqt-terminal-test\r");
        QTRY_VERIFY_WITH_TIMEOUT(m_outputFirst.contains("\x1b[?1049h"), 10000);
        m_first->writeData("\x1b:q!\r");
        QTRY_VERIFY_WITH_TIMEOUT(m_outputFirst.contains("\x1b[?1049l"), 10000);
    }

    void closingShellKeepsFilesConnected()
    {
        QVERIFY(m_first);
        QSignalSpy closed(m_first, &SshShellSession::sessionClosed);
        m_first->writeData("exit\r");
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 10000);
        QSignalSpy listed(&m_fs, &FileSystemController::directoryListed);
        m_fs.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(listed.size(), 1, 10000);
        QVERIFY(m_fs.isConnected());
        QVERIFY(m_second);
        m_second->writeData("echo OTHER_SHELL_OK\r");
        QTRY_VERIFY_WITH_TIMEOUT(plain(m_outputSecond).contains(QStringLiteral("\nOTHER_SHELL_OK\n")), 10000);
    }

    void disconnectClosesRemainingShell()
    {
        QVERIFY(m_second);
        QSignalSpy closed(m_second, &SshShellSession::sessionClosed);
        QSignalSpy disconnected(&m_fs, &FileSystemController::disconnected);
        m_fs.disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(disconnected.size(), 1, 10000);
        QCOMPARE(closed.size(), 1);
        QVERIFY(!m_fs.isConnected());
        QVERIFY(!m_fs.isBusy());
    }

    void shutdownDuringHostKeyDecision()
    {
        if (session.hostName != QLatin1String("127.0.0.1"))
            QSKIP("Requires the local integration server");
        const auto previousKeys = KnownHosts::instance().entries();
        KnownHosts::instance().remove(session.hostName, session.port);
        const auto restore = qScopeGuard([&] {
            KnownHosts::instance().remove(session.hostName, session.port);
            for (const auto &key : previousKeys) {
                if (key.host == session.hostName && key.port == session.port)
                    KnownHosts::instance().addOrUpdate(key.host, key.port, key.algorithm, key.fingerprint);
            }
        });
        FileSystemController pending;
        bool finished = false;
        bool decision = false;
        pending.setHostKeyPrompt([&](const QString &, int, const QString &, const QString &, QString *) {
            QMetaObject::invokeMethod(this, [&] {
                pending.shutdown();
                finished = true;
            }, Qt::QueuedConnection);
            QMetaObject::invokeMethod(this, [&] { decision = true; }, Qt::BlockingQueuedConnection);
            return true;
        });
        pending.connectToHost(session);
        QTRY_VERIFY_WITH_TIMEOUT(finished, 15000);
        QVERIFY(decision);
    }

    void reconnectCreatesUsableShell()
    {
        m_fs.connectToHost(session);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs.isConnected(), 20000);
        auto *shell = m_fs.createShell(80, 24);
        QSignalSpy ready(shell, &SshShellSession::connected);
        QSignalSpy closed(shell, &SshShellSession::sessionClosed);
        m_fs.startShell(shell);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        shell->stop();
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 10000);
        shell->deleteLater();
    }

    void cleanupTestCase() { m_fs.shutdown(); }

private:
    FileSystemController m_fs;
    QPointer<SshShellSession> m_first;
    QPointer<SshShellSession> m_second;
    QString m_base;
    QByteArray m_outputFirst;
    QByteArray m_outputSecond;
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 2)
        return 2;
    SharedSessionTest test;
    QString error;
    if (!SessionData::parseUrl(QString::fromLocal8Bit(argv[1]), &test.session, &error))
        return 2;
    test.realServer = argc > 2 && QByteArray(argv[2]) == "--real";
    return QTest::qExec(&test, QStringList{QString::fromLocal8Bit(argv[0]), QStringLiteral("-o"),
                                          qEnvironmentVariable("WINSCPQT_TEST_LOG", QStringLiteral("-")) + QStringLiteral(",txt")});
}

#include "terminal_smoke.moc"
