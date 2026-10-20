#include "core/FileSystemController.h"
#include "core/Settings.h"
#include "core/AppPaths.h"
#include "proto/LocalBackend.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"
#include "ui/FilePane.h"
#include "ui/SessionEditor.h"
#include "ui/RemoteFileOpener.h"
#include "terminal/TerminalWidget.h"
#include "ui/MainWindow.h"
#include "ui/TerminalPanel.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QSplitter>
#include <QDesktopServices>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMimeData>
#include <QProgressDialog>
#include <QSaveFile>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
#include <algorithm>

using namespace wsc;

class FileHandler : public QObject {
    Q_OBJECT
public:
    QList<QUrl> opened;
public slots:
    void open(const QUrl &url) { opened.append(url); }
};

class WindowTest : public QObject {
    Q_OBJECT
public:
    SessionData ssh;
    SessionData ftp;

private slots:
    void automaticTerminal()
    {
        Settings::instance().load();
        Settings::instance().askUnknownHostKey = false;
        m_window = std::make_unique<MainWindow>();
        m_window->show();
        m_window->setInitialSession(ssh, true);
        m_fs = m_window->findChild<FileSystemController *>();
        m_panel = m_window->findChild<TerminalPanel *>();
        QVERIFY(m_fs);
        QVERIFY(m_panel);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs->isConnected(), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(m_panel->terminalCount(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(m_panel->findChild<TerminalWidget *>()->isSessionRunning(), 10000);
        auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("MainSplitter"));
        QVERIFY(splitter);
        QCOMPARE(splitter->orientation(), Qt::Vertical);
        QCOMPARE(splitter->indexOf(m_panel), 1);
        for (auto *pane : m_window->findChildren<FilePane *>()) {
            if (pane->isRemote())
                m_remote = pane;
            else
                m_local = pane;
        }
        QVERIFY(m_remote);
        QVERIFY(m_local);
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test"), 10000);
        m_local->navigateLocal(m_temporary.path());
        QDesktopServices::setUrlHandler(QStringLiteral("file"), &m_handler, "open");
    }

    void navigationAndSynchronization()
    {
        QVERIFY(m_remote->entryAtViewRow(0).isParent);
        auto *table = m_remote->findChild<QTableView *>();
        table->sortByColumn(0, Qt::DescendingOrder);
        QVERIFY(m_remote->entryAtViewRow(0).isParent);
        m_remote->setFilterText(QStringLiteral("*.unlikely"));
        QVERIFY(m_remote->entryAtViewRow(0).isParent);
        table->selectRow(0);
        QVERIFY(m_remote->selectedEntries().isEmpty());
        m_remote->setFilterText({});
        table->sortByColumn(0, Qt::AscendingOrder);

        QSignalSpy operations(m_fs, &FileSystemController::operationFinished);
        m_fs->createDirectory(QStringLiteral("/home/test/navigation"));
        QTRY_VERIFY_WITH_TIMEOUT(!operations.isEmpty(), 10000);
        QVERIFY(operations.last().at(1).toBool());
        m_panel->setDirectorySynchronizationEnabled(true);
        QSignalSpy directories(m_panel, &TerminalPanel::directoryChanged);
        m_remote->navigateRemote(QStringLiteral("/home/test/navigation"));
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test/navigation"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(reported(directories, QStringLiteral("/home/test/navigation")), 10000);
        command("cd /home/test");
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test"), 10000);

        auto *refresh = m_remote->findChild<QToolButton *>(QStringLiteral("AutoRefreshButton"));
        QVERIFY(refresh);
        refresh->setChecked(true);
        command("printf 'refresh contents' > refreshed.txt");
        QTRY_VERIFY_WITH_TIMEOUT(entry(m_remote, QStringLiteral("refreshed.txt")).isValid(), 6000);
        m_remote->selectByName({QStringLiteral("refreshed.txt")});
        QSignalSpy listings(m_fs, &FileSystemController::directoryListed);
        QTRY_VERIFY_WITH_TIMEOUT(!listings.isEmpty(), 4000);
        QCOMPARE(m_remote->selectedEntries().size(), 1);
        QCOMPARE(m_remote->selectedEntries().first().name, QStringLiteral("refreshed.txt"));
        refresh->setChecked(false);

        m_remote->navigateRemote(QStringLiteral("/home/test/navigation"));
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test/navigation"), 10000);
        auto *breadcrumbs = m_remote->findChild<QLabel *>(QStringLiteral("PathBreadcrumbs"));
        QVERIFY(breadcrumbs->text().contains(QStringLiteral("navigation")));
        QVERIFY(QMetaObject::invokeMethod(breadcrumbs, "linkActivated", Q_ARG(QString, QStringLiteral("%2Fhome%2Ftest"))));
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test"), 10000);
    }

    void dragUploadAndDownload()
    {
        const QString source = QDir(m_temporary.path()).filePath(QStringLiteral("drag.txt"));
        QFile local(source);
        QVERIFY(local.open(QIODevice::WriteOnly));
        const QByteArray contents("drag transfer contents\n");
        local.write(contents);
        local.close();
        QMimeData upload;
        upload.setUrls({QUrl::fromLocalFile(source)});
        const QRect mainGeometry = m_window->geometry();
        const QRect fileGeometry = m_window->centralWidget()->geometry();
        QVERIFY(drop(m_remote, upload, QStringLiteral("navigation")));
        auto *queue = m_window->findChild<QDialog *>(QStringLiteral("TransferQueueWindow"));
        QVERIFY(queue);
        QVERIFY(queue->isVisible());
        QVERIFY(queue->isWindow());
        QVERIFY(!queue->isModal());
        QCOMPARE(m_window->geometry(), mainGeometry);
        QCOMPARE(m_window->centralWidget()->geometry(), fileGeometry);
        queue->close();
        for (auto *action : m_window->findChildren<QAction *>())
            if (action->text().contains(QStringLiteral("显示传输队列")))
                QVERIFY(!action->isChecked());
        auto *transfers = m_window->findChild<TransferManager *>();
        QTRY_VERIFY_WITH_TIMEOUT(transfers->activeCount() == 0 && transfers->pendingCount() == 0, 15000);
        m_remote->navigateRemote(QStringLiteral("/home/test/navigation"));
        QTRY_VERIFY_WITH_TIMEOUT(entry(m_remote, QStringLiteral("drag.txt")).isValid(), 10000);

        const QString destination = QDir(m_temporary.path()).filePath(QStringLiteral("destination"));
        QVERIFY(QDir().mkpath(destination));
        m_local->refresh();
        const auto file = entry(m_remote, QStringLiteral("drag.txt"));
        QJsonObject info{{QStringLiteral("name"), file.name}, {QStringLiteral("path"), file.path},
                         {QStringLiteral("size"), static_cast<double>(file.size)},
                         {QStringLiteral("modified"), file.modified.toString(Qt::ISODate)}};
        QMimeData download;
        download.setData(QStringLiteral("application/x-winscpqt-entries"), QJsonDocument(QJsonObject{
            {QStringLiteral("sourceRemote"), true}, {QStringLiteral("entries"), QJsonArray{info}}}).toJson());
        QVERIFY(drop(m_local, download, QStringLiteral("destination")));
        QVERIFY(queue->isVisible());
        QCOMPARE(m_window->centralWidget()->geometry(), fileGeometry);
        const QString target = QDir(destination).filePath(QStringLiteral("drag.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(transfers->activeCount() == 0 && transfers->pendingCount() == 0, 15000);
        QFile downloaded(target);
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), contents);
    }

    void synchronizationWaitsForPrompt()
    {
        QSignalSpy directories(m_panel, &TerminalPanel::directoryChanged);
        command("cd /home/test");
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test"), 10000);
        auto *terminal = m_panel->findChild<TerminalWidget *>();
        QTest::keyClicks(terminal, QStringLiteral("printf typed"));
        directories.clear();
        m_remote->navigateRemote(QStringLiteral("/home/test/navigation"));
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test/navigation"), 10000);
        QTest::qWait(100);
        QVERIFY(!reported(directories, QStringLiteral("/home/test/navigation")));
        QTest::keyClick(terminal, Qt::Key_U, Qt::ControlModifier);
        QTest::keyClick(terminal, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(reported(directories, QStringLiteral("/home/test/navigation")), 10000);
    }

    void remoteOpenCache()
    {
        auto *transfers = m_window->findChild<TransferManager *>();
        auto *queue = m_window->findChild<QDialog *>(QStringLiteral("TransferQueueWindow"));
        QVERIFY(queue);
        queue->close();
        QSignalSpy added(transfers, &TransferManager::taskAdded);
        const FileInfo file = entry(m_remote, QStringLiteral("drag.txt"));
        QVERIFY(file.isValid());
        m_remote->openRequested(file, true);
        auto *cancelled = m_window->findChild<QProgressDialog *>(QStringLiteral("RemoteOpenProgress"));
        QVERIFY(cancelled);
        cancelled->cancel();
        // Cancellation followed by reopen must ignore the previous stat reply.
        QMetaObject::invokeMethod(cancelled, "canceled");
        m_remote->openRequested(file, true);
        // Repeated opens share the in-flight job.
        m_remote->openRequested(file, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_handler.opened.size(), 1, 15000);
        QCOMPARE(added.size(), 1);
        QVERIFY(!queue->isVisible());
        const QString cached = m_handler.opened.last().toLocalFile();
        QVERIFY(cached.startsWith(AppPaths::downloadCacheDir() + QLatin1Char('/')));
        QVERIFY(QFileInfo::exists(cached));
        m_remote->openRequested(file, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_handler.opened.size(), 2, 10000);
        QCOMPARE(added.size(), 1);

        // Fresh stat must notice changes even while the browser listing is stale.
        QSignalSpy written(m_fs, &FileSystemController::fileWriteFinished);
        m_fs->writeFile(file.path, QByteArrayLiteral("changed remote contents, different size"));
        QTRY_VERIFY_WITH_TIMEOUT(!written.isEmpty(), 10000);
        m_remote->openRequested(file, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_handler.opened.size(), 3, 15000);
        QCOMPARE(added.size(), 2);
        QVERIFY(m_handler.opened.last().toLocalFile() != cached);
        QFile modified(m_handler.opened.last().toLocalFile());
        QVERIFY(modified.open(QIODevice::ReadOnly));
        QCOMPARE(modified.readAll(), QByteArrayLiteral("changed remote contents, different size"));
        modified.close();

        m_fs->writeFile(QStringLiteral("/home/test/drag.txt"), QByteArrayLiteral("other directory"));
        QTRY_COMPARE_WITH_TIMEOUT(written.size(), 2, 10000);
        FileInfo other = file;
        other.path = QStringLiteral("/home/test/drag.txt");
        m_remote->openRequested(other, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_handler.opened.size(), 4, 15000);
        QCOMPARE(added.size(), 3);
        QVERIFY(m_handler.opened.last().toLocalFile() != cached);
        QVERIFY(m_handler.opened.last() != m_handler.opened.at(2));

        const QString recordPath = QDir(QFileInfo(cached).absolutePath()).filePath(QStringLiteral("../metadata.json"));
        QFile recordFile(recordPath);
        QVERIFY(recordFile.open(QIODevice::ReadOnly));
        auto record = QJsonDocument::fromJson(recordFile.readAll()).object();
        recordFile.close();
        record.insert(QStringLiteral("lastUsed"), QDateTime::currentDateTimeUtc().addDays(-8).toString(Qt::ISODateWithMs));
        QVERIFY(recordFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        recordFile.write(QJsonDocument(record).toJson());
        recordFile.close();
        m_window->findChild<RemoteFileOpener *>()->cleanup();
        QVERIFY(!QFileInfo::exists(cached));
        QVERIFY(QFileInfo::exists(m_handler.opened.last().toLocalFile()));
    }

    void remembersDirectories()
    {
        const QString local = QDir(m_temporary.path()).filePath(QStringLiteral("destination"));
        m_local->navigateLocal(local);
        m_remote->navigateRemote(QStringLiteral("/home/test/navigation"));
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test/navigation"), 10000);
        const auto restored = Settings::instance().restoreDirectories(ssh);
        QCOMPARE(restored.localDirectory, local);
        QCOMPARE(restored.remoteDirectory, QStringLiteral("/home/test/navigation"));
        SessionEditor editor;
        for (auto *label : editor.findChildren<QLabel *>()) {
            QVERIFY(!label->text().contains(QStringLiteral("本地目录")));
            QVERIFY(!label->text().contains(QStringLiteral("远程目录")));
        }
    }

    void multipleShells()
    {
        m_panel->openTerminal();
        QTRY_COMPARE_WITH_TIMEOUT(m_panel->terminalCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(allRunning(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test/navigation"), 10000);
        QCOMPARE(m_local->currentPath(), QDir(m_temporary.path()).filePath(QStringLiteral("destination")));
    }

    void disconnectAndReconnect()
    {
        m_fs->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!m_fs->isConnected(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_panel->terminalCount(), 0, 10000);
        m_window->setInitialSession(ssh, true);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs->isConnected(), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(m_panel->terminalCount(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(allRunning(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test/navigation"), 10000);
        QCOMPARE(m_local->currentPath(), QDir(m_temporary.path()).filePath(QStringLiteral("destination")));
    }

    void missingRememberedDirectory()
    {
        m_fs->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!m_fs->isConnected(), 10000);
        Settings::instance().rememberDirectories(ssh, m_temporary.path(), QStringLiteral("/home/test/removed-directory"));
        m_window->setInitialSession(ssh, true);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs->isConnected(), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test"), 10000);
        QCOMPARE(Settings::instance().restoreDirectories(ssh).remoteDirectory, QStringLiteral("/home/test"));
    }

    void switchToNonSshSession()
    {
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (box && box->isVisible()) {
                    if (auto *yes = box->button(QMessageBox::Yes))
                        yes->click();
                }
            }
        });
        answer.start(20);
        m_window->setInitialSession(ftp, true);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs->isConnected() && m_fs->protocol() == Protocol::Ftp, 20000);
        QCOMPARE(m_panel->terminalCount(), 0);
        for (auto *action : m_window->findChildren<QAction *>())
            QVERIFY(!action->text().contains(QStringLiteral("关于")));
    }

    void cleanupTestCase()
    {
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        m_window.reset();
    }

private:
    void command(const char *text)
    {
        auto *terminal = m_panel->findChild<TerminalWidget *>();
        terminal->setFocus();
        QTest::keyClicks(terminal, QString::fromLatin1(text));
        QTest::keyClick(terminal, Qt::Key_Return);
    }
    static bool reported(const QSignalSpy &spy, const QString &directory)
    {
        return std::any_of(spy.begin(), spy.end(), [&](const auto &args) { return args.first().toString() == directory; });
    }
    static FileInfo entry(FilePane *pane, const QString &name)
    {
        for (int row = 0; pane->entryAtViewRow(row).isValid(); ++row)
            if (pane->entryAtViewRow(row).name == name)
                return pane->entryAtViewRow(row);
        return {};
    }
    static bool drop(FilePane *pane, QMimeData &mime, const QString &folder)
    {
        auto *table = pane->findChild<QTableView *>();
        QPoint point(5, table->viewport()->height() - 5);
        for (int row = 0; pane->entryAtViewRow(row).isValid(); ++row)
            if (pane->entryAtViewRow(row).name == folder) {
                point = table->visualRect(table->model()->index(row, 0)).center();
                break;
            }
        QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(table->viewport(), &enter);
        if (!enter.isAccepted())
            return false;
        QDragMoveEvent move(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(table->viewport(), &move);
        if (!move.isAccepted())
            return false;
        QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(table->viewport(), &drop);
        return drop.isAccepted();
    }
    bool allRunning() const
    {
        const auto terminals = m_panel->findChildren<TerminalWidget *>();
        return !terminals.isEmpty() && std::all_of(terminals.begin(), terminals.end(),
                                                  [](auto *terminal) { return terminal->isSessionRunning(); });
    }
    std::unique_ptr<MainWindow> m_window;
    FileSystemController *m_fs = nullptr;
    TerminalPanel *m_panel = nullptr;
    FilePane *m_remote = nullptr;
    FilePane *m_local = nullptr;
    QTemporaryDir m_temporary;
    FileHandler m_handler;
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    Q_INIT_RESOURCE(resources);
    if (argc < 3)
        return 2;
    WindowTest test;
    if (!SessionData::parseUrl(QString::fromLocal8Bit(argv[1]), &test.ssh) ||
        !SessionData::parseUrl(QString::fromLocal8Bit(argv[2]), &test.ftp))
        return 2;
    return QTest::qExec(&test, QStringList{QString::fromLocal8Bit(argv[0]), QStringLiteral("-o"),
                                          qEnvironmentVariable("WINSCPQT_TEST_LOG", QStringLiteral("-")) + QStringLiteral(",txt")});
}

#include "window_test.moc"
