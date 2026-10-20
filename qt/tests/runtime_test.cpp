#include "core/AppPaths.h"
#include "core/Backend.h"
#include "core/FileSystemController.h"
#include "core/Settings.h"
#include "terminal/SshShellSession.h"
#include "ui/FilePane.h"
#include "ui/FileTableModel.h"
#include "proto/LocalBackend.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"

#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

using namespace wsc;

class HttpFixture : public QTcpServer {
    Q_OBJECT
public:
    HttpFixture()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray input = socket->property("request").toByteArray() + socket->readAll();
                    const int end = input.indexOf("\r\n\r\n");
                    if (end < 0) {
                        socket->setProperty("request", input);
                        return;
                    }
                    const auto header = input.left(end);
                    qint64 length = 0;
                    for (const auto &line : header.split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toLongLong();
                    if (input.size() < end + 4 + length) {
                        socket->setProperty("request", input);
                        return;
                    }
                    socket->disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
                    const auto request = header.split('\n').first().split(' ');
                    const QByteArray method = request.value(0);
                    const QByteArray path = request.value(1);
                    emit requested(method, path);
                    if (method == "PROPFIND") {
                        const QByteArray body = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>" + path +
                            "</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype>"
                            "</d:prop></d:propstat></d:response></d:multistatus>";
                        const int delay = header.contains("Depth: 1") ? listDelay : connectDelay;
                        QTimer::singleShot(delay, socket, [socket, body] {
                            socket->write("HTTP/1.1 207 Multi-Status\r\nContent-Length: " + QByteArray::number(body.size()) +
                                          "\r\nConnection: close\r\n\r\n" + body);
                            socket->disconnectFromHost();
                        });
                    } else if (path == "/stall") {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\npartial-data");
                    } else if (path == "/large") {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 34603008\r\nConnection: close\r\n\r\n");
                        auto *timer = new QTimer(socket);
                        timer->setInterval(1);
                        connect(timer, &QTimer::timeout, socket, [timer, socket, remaining = qint64(33 * 1024 * 1024)]() mutable {
                            if (socket->state() != QAbstractSocket::ConnectedState) {
                                timer->stop();
                                return;
                            }
                            if (socket->bytesToWrite() > 256 * 1024)
                                return;
                            if (remaining == 0) {
                                timer->stop();
                                socket->disconnectFromHost();
                                return;
                            }
                            const qint64 size = qMin<qint64>(remaining, 256 * 1024);
                            socket->write(QByteArray(size, 'x'));
                            remaining -= size;
                        });
                        timer->start();
                    }
                });
            }
        });
    }

    SessionData session() const
    {
        SessionData result;
        result.protocol = Protocol::WebDav;
        result.hostName = QStringLiteral("127.0.0.1");
        result.port = serverPort();
        result.remoteDirectory = QStringLiteral("/");
        result.webDavHttps = false;
        result.timeoutSeconds = 5;
        result.keepAliveSeconds = 0;
        return result;
    }

    int listDelay = 0;
    int connectDelay = 0;
signals:
    void requested(const QByteArray &method, const QByteArray &path);
};

class RuntimeTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("WINSCPQT_HOME", m_home.path().toUtf8());
        Settings::instance().load();
    }

    void disconnectDuringWebDavListing()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.listDelay = 200;
        FileSystemController files;
        QSignalSpy disconnected(&files, &FileSystemController::disconnected);
        QSignalSpy listed(&files, &FileSystemController::directoryListed);
        connect(&files, &FileSystemController::directoryLoading, &files, [&] {
            QTimer::singleShot(20, &files, &FileSystemController::disconnectFromHost);
        });
        files.connectToHost(server.session());
        QTRY_COMPARE_WITH_TIMEOUT(disconnected.size(), 1, 5000);
        QVERIFY(!files.isConnected());
        QCOMPARE(listed.size(), 0);
    }

    void replacePendingConnection()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.connectDelay = 200;
        FileSystemController files;
        QSignalSpy connected(&files, &FileSystemController::connected);
        SessionData first = server.session();
        first.remoteDirectory = QStringLiteral("/first");
        SessionData second = first;
        second.remoteDirectory = QStringLiteral("/second");
        bool replaced = false;
        connect(&server, &HttpFixture::requested, &files, [&](const QByteArray &, const QByteArray &path) {
            if (!replaced && path == "/first") {
                replaced = true;
                files.connectToHost(second);
            }
        });
        files.connectToHost(first);
        QTRY_COMPARE_WITH_TIMEOUT(files.currentPath(), QStringLiteral("/second"), 5000);
        QCOMPARE(connected.size(), 1);
        QCOMPARE(files.session().remoteDirectory, QStringLiteral("/second"));
    }

    void timedOutDownloadPreservesDestination()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto backend = createBackend(Protocol::WebDav);
        QString error;
        QVERIFY2(backend->connect(server.session(), &error), qPrintable(error));
        const QString destination = m_home.filePath(QStringLiteral("existing.txt"));
        QFile file(destination);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original");
        file.close();
        QVERIFY(!backend->downloadFile(QStringLiteral("/stall"), destination, 100000, {}, nullptr, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
    }

    void editorReadIsBounded()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto backend = createBackend(Protocol::WebDav);
        QString error;
        QVERIFY2(backend->connect(server.session(), &error), qPrintable(error));
        QByteArray content;
        QVERIFY(!backend->readFile(QStringLiteral("/large"), &content, &error));
        QVERIFY(error.contains(QStringLiteral("32 MB")));
        QVERIFY(content.isEmpty());
    }

    void closingShellDoesNotWaitForWorker()
    {
        QThread worker;
        auto *shell = new SshShellSession(80, 24);
        shell->moveToThread(&worker);
        connect(&worker, &QThread::finished, shell, &QObject::deleteLater);
        QSignalSpy closed(shell, &SshShellSession::sessionClosed);
        worker.start();
        QMetaObject::invokeMethod(shell, [] { QThread::msleep(300); });
        QElapsedTimer timer;
        timer.start();
        shell->stop();
        const qint64 elapsed = timer.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 3000);
        worker.quit();
        QVERIFY(worker.wait(3000));
        QVERIFY(elapsed < 100);
    }

    void localNavigationDiscardsStaleResults()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        QFile file(second.filePath(QStringLiteral("latest.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        FilePane pane(FilePane::Role::Local);
        QSignalSpy changed(&pane, &FilePane::pathChanged);
        pane.navigateLocal(first.path());
        pane.navigateLocal(second.path());
        QTRY_COMPARE_WITH_TIMEOUT(pane.currentPath(), second.path(), 5000);
        QCOMPARE(changed.size(), 1);
        auto *model = pane.findChild<FileTableModel *>();
        QCOMPARE(model->fileCount(), 1);
        QCOMPARE(model->entryAt(1).name, QStringLiteral("latest.txt"));
    }

    void completedTransferHistoryIsBounded()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath(QStringLiteral("source.txt"));
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload");
        file.close();
        SessionData session;
        session.protocol = Protocol::Local;
        const FileInfo entry = LocalBackend::infoForPath(source);
        TransferManager transfers;
        transfers.setParallelLimit(4);
        QSignalSpy finished(&transfers, &TransferManager::allTransfersFinished);
        for (int index = 0; index < 220; ++index)
            transfers.enqueueDownload(session, {entry}, directory.filePath(QString::number(index)));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(transfers.model()->rowCount() <= 200);
        for (const auto &row : transfers.model()->rows())
            QCOMPARE(row.status, TransferStatus::Completed);
    }

private:
    QTemporaryDir m_home;
};

QTEST_MAIN(RuntimeTest)
#include "runtime_test.moc"
