#include "core/AppPaths.h"
#include "core/Backend.h"
#include "core/FileSystemController.h"
#include "core/Settings.h"
#include "sync/SyncPlanner.h"
#include "terminal/SshShellSession.h"
#include "ui/FilePane.h"
#include "ui/FileTableModel.h"
#include "ui/MainWindow.h"
#include "ui/SessionWorkspace.h"
#include "ui/LoginDialog.h"
#include "ui/SessionEditor.h"
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
#include <QTabBar>
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
                        QByteArray body = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>" + path +
                            "</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype>"
                            "</d:prop></d:propstat></d:response></d:multistatus>";
                        const int delay = header.contains("Depth: 1") ? listDelay : connectDelay;
                        if (header.contains("Depth: 1") && listings.contains(path))
                            body = listings.value(path);
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
    QHash<QByteArray, QByteArray> listings;
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

    void webDavRejectsInvalidListings_data()
    {
        QTest::addColumn<QByteArray>("response");
        QTest::newRow("truncated XML") << QByteArray("<d:multistatus xmlns:d=\"DAV:\"><d:response>");
        QTest::newRow("HTML error") << QByteArray("<html><body>error</body></html>");
        QTest::newRow("missing collection") << wrapDav(davEntry("/file.txt", false));
        QTest::newRow("empty multistatus") << wrapDav({});
        QTest::newRow("failed resource type") << wrapDav(davEntry("/", true) +
            "<d:response><d:href>/file.txt</d:href><d:propstat><d:prop>"
            "<d:getcontentlength>42</d:getcontentlength></d:prop><d:status>HTTP/1.1 200 OK</d:status>"
            "</d:propstat><d:propstat><d:prop><d:resourcetype/></d:prop>"
            "<d:status>HTTP/1.1 403 Forbidden</d:status></d:propstat></d:response>");
        QTest::newRow("failed resource") << wrapDav(davEntry("/", true) +
            "<d:response><d:href>/denied</d:href><d:status>HTTP/1.1 403 Forbidden</d:status></d:response>");
        QTest::newRow("outside collection") << wrapDav(davEntry("/", true) + davEntry("/outside/file.txt", false));
        QTest::newRow("duplicate resource") << wrapDav(davEntry("/", true) +
            davEntry("/same", false) + davEntry("/same", false));
    }

    void webDavRejectsInvalidListings()
    {
        QFETCH(QByteArray, response);
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.listings.insert("/", response);
        auto backend = createBackend(Protocol::WebDav);
        QString error;
        QVERIFY2(backend->connect(server.session(), &error), qPrintable(error));
        FileList entries;
        QVERIFY(!backend->list(QStringLiteral("/"), &entries, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(entries.isEmpty());
    }

    void webDavUsesHrefAndSuccessfulProperties()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.listings.insert("/", wrapDav(davEntry("/", true) +
            "<d:response><d:href>/actual.txt</d:href>"
            "<d:propstat><d:prop><d:displayname>Friendly title</d:displayname>"
            "<d:getcontentlength>42</d:getcontentlength><d:resourcetype/></d:prop>"
            "<d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
            "<d:propstat><d:prop><d:getcontentlength>0</d:getcontentlength></d:prop>"
            "<d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response>"));
        auto backend = createBackend(Protocol::WebDav);
        QString error;
        QVERIFY2(backend->connect(server.session(), &error), qPrintable(error));
        FileList entries;
        QVERIFY2(backend->list(QStringLiteral("/"), &entries, &error), qPrintable(error));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().name, QStringLiteral("actual.txt"));
        QCOMPARE(entries.first().size, 42);
        QVERIFY(!entries.first().isDir);
    }

    void syncIncludesMissingDirectoriesWithoutDuplicateChildren()
    {
        QTemporaryDir local;
        QVERIFY(local.isValid());
        QVERIFY(QDir().mkpath(local.filePath(QStringLiteral("empty"))));
        QVERIFY(QDir().mkpath(local.filePath(QStringLiteral("bundle/nested"))));
        QFile source(local.filePath(QStringLiteral("bundle/nested/file.txt")));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("payload");
        source.close();
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.listings.insert("/", wrapDav(davEntry("/", true) + davEntry("/remote-empty", true)));
        SyncOptions options;
        options.direction = SyncDirection::Both;
        auto plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QCOMPARE(plan.items.size(), 3);
        QSet<QString> names;
        for (const auto &item : plan.items) {
            QVERIFY(item.file.isDir);
            names.insert(item.relativePath);
        }
        QCOMPARE(names, (QSet<QString>{QStringLiteral("bundle"), QStringLiteral("empty"), QStringLiteral("remote-empty")}));
        options.direction = SyncDirection::LocalToRemote;
        options.deleteExtras = true;
        plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QCOMPARE(plan.items.size(), 3);
        int deletes = 0;
        for (const auto &item : plan.items)
            deletes += item.action == SyncAction::DeleteRemote;
        QCOMPARE(deletes, 1);
        options.recursive = false;
        plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QVERIFY(plan.items.isEmpty());
    }

    void syncRejectsTypeConflictsAndIncompleteListings()
    {
        QTemporaryDir local;
        QVERIFY(local.isValid());
        QFile file(local.filePath(QStringLiteral("conflict")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.listings.insert("/", wrapDav(davEntry("/", true) + davEntry("/conflict", true)));
        SyncOptions options;
        options.deleteExtras = true;
        auto plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY(!plan.error.isEmpty());
        QVERIFY(plan.items.isEmpty());
        server.listings.insert("/", wrapDav(davEntry("/", true) + davEntry("/CONFLICT", false)));
        plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY(!plan.error.isEmpty());
        QVERIFY(plan.items.isEmpty());
        server.listings.insert("/", "<broken");
        plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY(!plan.error.isEmpty());
        QVERIFY(plan.items.isEmpty());
        server.listings.insert("/", wrapDav(davEntry("/", true) +
            davEntry("/NUL.txt", false)));
        plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY(!plan.error.isEmpty());
        QVERIFY(plan.items.isEmpty());
    }

    void bidirectionalSyncHonorsDisabledTimeComparison()
    {
        QTemporaryDir local;
        QVERIFY(local.isValid());
        QFile file(local.filePath(QStringLiteral("same.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("data");
        file.close();
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.listings.insert("/", wrapDav(davEntry("/", true) + davEntry("/same.txt", false, 4)));
        SyncOptions options;
        options.direction = SyncDirection::Both;
        options.compareTime = false;
        auto plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QVERIFY(plan.items.isEmpty());
        options.compareTime = true;
        plan = buildSyncPlan(server.session(), local.path(), QStringLiteral("/"), options, {});
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QCOMPARE(plan.items.size(), 1);
        QCOMPARE(plan.items.first().action, SyncAction::Upload);
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

    void remoteNavigationPreservesLiteralNames()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        FileSystemController files;
        FilePane pane(FilePane::Role::Remote);
        pane.attachController(&files);
        QSignalSpy listed(&files, &FileSystemController::directoryListed);
        files.connectToHost(server.session());
        QTRY_COMPARE_WITH_TIMEOUT(listed.size(), 1, 5000);
        pane.showRemoteConnected(QStringLiteral("/"), QStringLiteral("/"));
        pane.navigateRemote(QStringLiteral("/ spaced "));
        QTRY_COMPARE_WITH_TIMEOUT(files.currentPath(), QStringLiteral("/ spaced "), 5000);
        pane.navigateRemote(QStringLiteral("/a\\b"));
        QTRY_COMPARE_WITH_TIMEOUT(files.currentPath(), QStringLiteral("/a\\b"), 5000);
    }

    void sessionTabsKeepConnectionsAndDirectories()
    {
        HttpFixture firstServer;
        HttpFixture secondServer;
        QVERIFY(firstServer.listen(QHostAddress::LocalHost));
        QVERIFY(secondServer.listen(QHostAddress::LocalHost));
        MainWindow window;
        window.show();
        auto first = firstServer.session();
        first.password = QStringLiteral("test");
        first.remoteDirectory = QStringLiteral("/first");
        window.setInitialSession(first, true);
        auto *firstView = window.activeWorkspace();
        auto *firstFiles = firstView->findChild<FileSystemController *>();
        QTRY_COMPARE_WITH_TIMEOUT(firstFiles->currentPath(), QStringLiteral("/first"), 5000);
        auto second = secondServer.session();
        second.remoteDirectory = QStringLiteral("/second");
        int logins = 0;
        QTimer loginTimer;
        connect(&loginTimer, &QTimer::timeout, &window, [&] {
            auto *login = window.findChild<LoginDialog *>();
            if (!login || !login->isVisible())
                return;
            ++logins;
            login->findChild<SessionEditor *>()->setSession(second);
            QMetaObject::invokeMethod(login, "accept");
        });
        loginTimer.start(10);
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("SessionTabs"));
        QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(tabs->count() - 1).center());
        QTRY_COMPARE_WITH_TIMEOUT(logins, 1, 5000);
        loginTimer.stop();
        auto *secondView = window.activeWorkspace();
        QVERIFY(secondView != firstView);
        QPointer<FileSystemController> secondFiles = secondView->findChild<FileSystemController *>();
        QTRY_COMPARE_WITH_TIMEOUT(secondFiles->currentPath(), QStringLiteral("/second"), 5000);
        QVERIFY(firstFiles->isConnected());
        firstFiles->listDirectory(QStringLiteral("/background"));
        QTRY_COMPARE_WITH_TIMEOUT(firstFiles->currentPath(), QStringLiteral("/background"), 5000);
        QCOMPARE(secondFiles->currentPath(), QStringLiteral("/second"));
        tabs->setCurrentIndex(0);
        QCOMPARE(window.activeWorkspace(), firstView);
        QVERIFY(firstFiles->isConnected());
        QVERIFY(secondFiles->isConnected());
        tabs->tabCloseRequested(1);
        QTRY_VERIFY_WITH_TIMEOUT(secondFiles.isNull(), 5000);
        QVERIFY(firstFiles->isConnected());
        firstFiles->listDirectory(QStringLiteral("/still-connected"));
        QTRY_COMPARE_WITH_TIMEOUT(firstFiles->currentPath(), QStringLiteral("/still-connected"), 5000);
    }

    void immediateReconnectReusesDisconnectedTab()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MainWindow window;
        auto session = server.session();
        session.password = QStringLiteral("test");
        window.setInitialSession(session, true);
        auto *view = window.activeWorkspace();
        auto *files = view->findChild<FileSystemController *>();
        QTRY_COMPARE_WITH_TIMEOUT(files->currentPath(), QStringLiteral("/"), 5000);
        files->disconnectFromHost();
        window.setInitialSession(session, true);
        QCOMPARE(window.activeWorkspace(), view);
        QCOMPARE(window.findChild<QTabBar *>(QStringLiteral("SessionTabs"))->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(files->isConnected(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(files->currentPath(), QStringLiteral("/"), 5000);
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
    static QByteArray wrapDav(const QByteArray &responses)
    {
        return "<d:multistatus xmlns:d=\"DAV:\">" + responses + "</d:multistatus>";
    }

    static QByteArray davEntry(const QByteArray &href, bool directory, qint64 size = 0)
    {
        return "<d:response><d:href>" + href + "</d:href><d:propstat><d:prop><d:resourcetype>" +
            (directory ? QByteArray("<d:collection/>") : QByteArray()) +
            "</d:resourcetype><d:getcontentlength>" + QByteArray::number(size) +
            "</d:getcontentlength><d:getlastmodified>Wed, 01 Jan 2020 00:00:00 GMT</d:getlastmodified>"
            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    }

    QTemporaryDir m_home;
};

QTEST_MAIN(RuntimeTest)
#include "runtime_test.moc"
