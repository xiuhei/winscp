#include "core/FileSystemController.h"
#include "core/Settings.h"
#include "core/AppPaths.h"
#include "proto/LocalBackend.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"
#include "ui/FilePane.h"
#include "ui/SessionEditor.h"
#include "ui/RemoteFileOpener.h"
#include "ui/TextEditorDialog.h"
#include "ui/FileTableModel.h"
#include "terminal/TerminalWidget.h"
#include "ui/MainWindow.h"
#include "ui/SessionWorkspace.h"
#include "ui/TerminalPanel.h"

#include <QAction>
#include <QComboBox>
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
#include <QLineEdit>
#include <QMimeData>
#include <QProgressDialog>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSaveFile>
#include <QTableView>
#include <QTabBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTextDocument>
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
        QVERIFY(!m_panel->findChild<QToolButton *>(QStringLiteral("DirectorySyncButton")));
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
        QTRY_COMPARE_WITH_TIMEOUT(m_local->currentPath(), m_temporary.path(), 10000);
        QDesktopServices::setUrlHandler(QStringLiteral("file"), &m_handler, "open");
    }

    void navigationAndAutoRefresh()
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
        m_remote->navigateRemote(QStringLiteral("/home/test/navigation"));
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test/navigation"), 10000);
        command("printf 'CURRENT_DIRECTORY:%s\\n' \"$PWD\"");
        auto *terminal = m_panel->findChild<TerminalWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(terminal->screen()->textRange(0, 0, terminal->screen()->totalLines() - 1,
            terminal->screen()->columns()).contains(QStringLiteral("CURRENT_DIRECTORY:/home/test")), 10000);
        QCOMPARE(m_remote->currentPath(), QStringLiteral("/home/test/navigation"));
        m_remote->navigateRemote(QStringLiteral("/home/test"));
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
        const QRect geometry = table->geometry();
        QSignalSpy reset(m_remote->findChild<FileTableModel *>(), &QAbstractItemModel::modelReset);
        QSignalSpy selection(table->selectionModel(), &QItemSelectionModel::selectionChanged);
        bool visibleLoading = false;
        const auto loading = connect(m_fs, &FileSystemController::directoryLoading, this, [&] {
            visibleLoading |= m_remote->findChild<QProgressBar *>()->isVisible();
            visibleLoading |= table->geometry() != geometry;
        });
        listings.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!listings.isEmpty(), 4000);
        disconnect(loading);
        QVERIFY(!visibleLoading);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(selection.size(), 0);
        QCOMPARE(table->geometry(), geometry);
        refresh->setChecked(false);

        m_remote->navigateRemote(QStringLiteral("/home/test/navigation"));
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test/navigation"), 10000);
        auto *breadcrumbs = m_remote->findChild<QLabel *>(QStringLiteral("PathBreadcrumbs"));
        QVERIFY(breadcrumbs->text().contains(QStringLiteral("navigation")));
        QTextDocument rendered;
        rendered.setHtml(breadcrumbs->text());
        QCOMPARE(rendered.toPlainText(), QStringLiteral("/home/test/navigation"));
        auto *localPath = m_local->findChild<QLabel *>(QStringLiteral("PathBreadcrumbs"));
        rendered.setHtml(localPath->text());
        QCOMPARE(rendered.toPlainText(), QDir::toNativeSeparators(m_local->currentPath()));
        QVERIFY(QMetaObject::invokeMethod(breadcrumbs, "linkActivated", Q_ARG(QString, QStringLiteral("%2Fhome%2Ftest"))));
        QTRY_COMPARE_WITH_TIMEOUT(m_remote->currentPath(), QStringLiteral("/home/test"), 10000);
    }

    void automaticRefreshReadsRemoteChanges()
    {
        auto backend = createBackend(ssh.protocol);
        QString error;
        QVERIFY2(backend->connect(ssh, &error), qPrintable(error));
        auto *button = m_remote->findChild<QToolButton *>(QStringLiteral("AutoRefreshButton"));
        QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(button->isChecked());
        QVERIFY(button->text().contains(QStringLiteral("开")));
        QTRY_VERIFY_WITH_TIMEOUT(button->toolTip().contains(QStringLiteral("上次成功刷新")), 5000);

        const QString remoteFile = QStringLiteral("/home/test/automatic-refresh.txt");
        const QByteArray content("written from an independent connection");
        QVERIFY2(backend->writeFile(remoteFile, content, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(entry(m_remote, QStringLiteral("automatic-refresh.txt")).size, content.size(), 4500);
        const QByteArray changed("updated from an independent connection, with a different size");
        QVERIFY2(backend->writeFile(remoteFile, changed, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(entry(m_remote, QStringLiteral("automatic-refresh.txt")).size, changed.size(), 4500);

        m_remote->focusPathEditor();
        auto *path = m_remote->findChild<QComboBox *>();
        path->lineEdit()->setText(QStringLiteral("/unfinished user input"));
        QSignalSpy listings(m_fs, &FileSystemController::directoryListed);
        QTRY_VERIFY_WITH_TIMEOUT(!listings.isEmpty(), 4500);
        QVERIFY(path->isVisible());
        QCOMPARE(path->lineEdit()->text(), QStringLiteral("/unfinished user input"));
        m_remote->focusFileList();
        QVERIFY2(backend->removeFile(remoteFile, &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!entry(m_remote, QStringLiteral("automatic-refresh.txt")).isValid(), 4500);

        QSignalSpy failed(m_fs, &FileSystemController::directoryFailed);
        m_remote->navigateRemote(QStringLiteral("/home/test/missing-refresh-directory"));
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 5000);
        QVERIFY(button->text().contains(QStringLiteral("重试")));
        listings.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!listings.isEmpty(), 4500);
        QVERIFY(button->text().contains(QStringLiteral("开")));

        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(!button->isChecked());
        QVERIFY(button->text().contains(QStringLiteral("关")));
        listings.clear();
        QVERIFY2(backend->writeFile(remoteFile, content, &error), qPrintable(error));
        QTest::qWait(2300);
        QVERIFY(listings.isEmpty());
        QVERIFY(!entry(m_remote, QStringLiteral("automatic-refresh.txt")).isValid());
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(entry(m_remote, QStringLiteral("automatic-refresh.txt")).isValid(), 1500);
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY2(backend->removeFile(remoteFile, &error), qPrintable(error));
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
        QTRY_VERIFY_WITH_TIMEOUT(entry(m_local, QStringLiteral("destination")).isDir, 10000);
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
        QVERIFY(!queue->isVisible());
        for (auto *action : m_window->findChildren<QAction *>())
            if (action->text().contains(QStringLiteral("显示传输队列")))
                QVERIFY(!action->isChecked());
    }

    void transferQueueRetainsErrors()
    {
        auto *transfers = m_window->findChild<TransferManager *>();
        auto *queue = m_window->findChild<QDialog *>(QStringLiteral("TransferQueueWindow"));
        QSignalSpy finished(transfers, &TransferManager::allTransfersFinished);
        FileInfo missing;
        missing.name = QStringLiteral("missing-download.txt");
        missing.path = QStringLiteral("/home/test/missing-download.txt");
        missing.size = 1;
        const QString destination = QDir(m_temporary.path()).filePath(QStringLiteral("queue-results"));
        const int failed = transfers->enqueueDownload(ssh, {missing}, destination);
        QVERIFY(queue->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QCOMPARE(transfers->model()->rowAt(transfers->model()->rowForId(failed)).status, TransferStatus::Failed);
        QVERIFY(queue->isVisible());
        const FileInfo file = entry(m_remote, QStringLiteral("drag.txt"));
        QVERIFY(file.isValid());
        transfers->enqueueDownload(ssh, {file}, destination);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 15000);
        QVERIFY(QFileInfo::exists(QDir(destination).filePath(file.name)));
        QVERIFY(!queue->isVisible());
        const int cancelled = transfers->enqueueDownload(ssh, {file}, destination);
        transfers->cancelTask(cancelled);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 15000);
        QCOMPARE(transfers->model()->rowAt(transfers->model()->rowForId(cancelled)).status, TransferStatus::Cancelled);
        QVERIFY(queue->isVisible());
    }

    void remoteOpenCache()
    {
        auto *transfers = m_window->findChild<TransferManager *>();
        auto *queue = m_window->findChild<QDialog *>(QStringLiteral("TransferQueueWindow"));
        QVERIFY(queue);
        queue->close();
        QSignalSpy added(transfers, &TransferManager::taskAdded);
        QSignalSpy prepared(m_fs, &FileSystemController::fileWriteFinished);
        m_fs->writeFile(QStringLiteral("/home/test/navigation/cache.bin"), QByteArrayLiteral("binary\0contents"));
        QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(entry(m_remote, QStringLiteral("cache.bin")).isValid(), 10000);
        const FileInfo file = entry(m_remote, QStringLiteral("cache.bin"));
        QVERIFY(file.isValid());
        int cancelledId = -1;
        int selectedId = -1;
        bool cancelClicked = false;
        QObject cancellation;
        const auto cancel = connect(transfers, &TransferManager::taskAdded, &cancellation, [&](int id) {
            cancelledId = id;
            QTimer::singleShot(0, &cancellation, [&] {
                auto *table = queue->findChild<QTableView *>(QStringLiteral("TransferTable"));
                const auto selected = table->selectionModel()->selectedRows();
                if (!selected.isEmpty())
                    selectedId = selected.first().data(TransferQueueModel::TaskIdRole).toInt();
                for (auto *button : queue->findChildren<QToolButton *>())
                    if (button->text() == QStringLiteral("取消")) {
                        cancelClicked = true;
                        button->click();
                    }
            });
        });
        m_remote->openRequested(file, true);
        QTRY_VERIFY_WITH_TIMEOUT(cancelledId >= 0 &&
            !transfers->model()->rowAt(transfers->model()->rowForId(cancelledId)).canCancel, 10000);
        disconnect(cancel);
        QVERIFY(cancelClicked);
        QCOMPARE(selectedId, cancelledId);
        QCOMPARE(transfers->model()->rowAt(transfers->model()->rowForId(cancelledId)).status, TransferStatus::Cancelled);
        QVERIFY(m_handler.opened.isEmpty());
        QVERIFY(m_window->findChildren<QProgressDialog *>().isEmpty());
        queue->close();
        bool singleWindow = false;
        QObject observation;
        const auto shown = connect(transfers, &TransferManager::taskAdded, &observation, [&](int) {
            singleWindow = queue->isVisible() && m_window->findChildren<QProgressDialog *>().isEmpty() &&
                m_window->findChildren<QDialog *>(QStringLiteral("TransferQueueWindow")).size() == 1;
        });
        m_remote->openRequested(file, true);
        // Repeated opens share the in-flight job.
        m_remote->openRequested(file, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_handler.opened.size(), 1, 15000);
        disconnect(shown);
        QVERIFY(singleWindow);
        QCOMPARE(added.size(), 2);
        QVERIFY(!queue->isVisible());
        const QString cached = m_handler.opened.last().toLocalFile();
        QVERIFY(cached.startsWith(AppPaths::downloadCacheDir() + QLatin1Char('/')));
        QVERIFY(QFileInfo::exists(cached));
        m_remote->openRequested(file, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_handler.opened.size(), 2, 10000);
        QCOMPARE(added.size(), 2);

        // Fresh stat must notice changes even while the browser listing is stale.
        QSignalSpy written(m_fs, &FileSystemController::fileWriteFinished);
        m_fs->writeFile(file.path, QByteArrayLiteral("changed\0remote contents, different size"));
        QTRY_VERIFY_WITH_TIMEOUT(!written.isEmpty(), 10000);
        m_remote->openRequested(file, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_handler.opened.size(), 3, 15000);
        QCOMPARE(added.size(), 3);
        QVERIFY(m_handler.opened.last().toLocalFile() != cached);
        QFile modified(m_handler.opened.last().toLocalFile());
        QVERIFY(modified.open(QIODevice::ReadOnly));
        QCOMPARE(modified.readAll(), QByteArrayLiteral("changed\0remote contents, different size"));
        modified.close();

        m_fs->writeFile(QStringLiteral("/home/test/cache.bin"), QByteArrayLiteral("other\0directory"));
        QTRY_COMPARE_WITH_TIMEOUT(written.size(), 2, 10000);
        FileInfo other = file;
        other.path = QStringLiteral("/home/test/cache.bin");
        m_remote->openRequested(other, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_handler.opened.size(), 4, 15000);
        QCOMPARE(added.size(), 4);
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

    void opensExtensionlessLogsAndSymlinks()
    {
        auto backend = createBackend(ssh.protocol);
        QString error;
        QVERIFY2(backend->connect(ssh, &error), qPrintable(error));
        const QString directory = QStringLiteral("/home/test/navigation");
        const QString name = QStringLiteral("log2026年10月19日00时00分");
        const QString logPath = directory + QLatin1Char('/') + name;
        const QByteArray contents = QStringLiteral("服务启动成功\nrequest completed\n").toUtf8();
        QVERIFY2(backend->writeFile(logPath, contents, &error), qPrintable(error));
        QVERIFY2(backend->createSymlink(name, directory + QStringLiteral("/log"), &error), qPrintable(error));
        m_remote->navigateRemote(directory);
        QTRY_VERIFY_WITH_TIMEOUT(entry(m_remote, QStringLiteral("log")).isValid(), 10000);
        QVERIFY(entry(m_remote, QStringLiteral("log")).isSymlink);
        auto *opener = m_window->findChild<RemoteFileOpener *>();
        QSignalSpy ready(opener, &RemoteFileOpener::textFileReady);
        QSignalSpy uploaded(m_fs, &FileSystemController::fileWriteFinished);
        const int externalOpens = m_handler.opened.size();
        bool editorSeen = false;
        QByteArray displayed;
        QTimer editorTimer;
        connect(&editorTimer, &QTimer::timeout, this, [&] {
            auto *editor = m_window->findChild<TextEditorDialog *>();
            if (!editor || !editor->isVisible())
                return;
            auto *text = editor->findChild<QPlainTextEdit *>();
            if (text->isReadOnly())
                return;
            editorSeen = true;
            displayed = text->toPlainText().toUtf8();
            text->appendPlainText(QStringLiteral("saved through link"));
            QMetaObject::invokeMethod(editor, "accept");
        });
        editorTimer.start(20);
        auto *table = m_remote->findChild<QTableView *>();
        QModelIndex logIndex;
        for (int row = 0; row < table->model()->rowCount(); ++row)
            if (table->model()->index(row, 0).data().toString() == QLatin1String("log"))
                logIndex = table->model()->index(row, 0);
        QVERIFY(logIndex.isValid());
        table->scrollTo(logIndex);
        const QPoint logPosition = table->visualRect(logIndex).center();
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, logPosition);
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, logPosition);
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, Qt::NoModifier, logPosition);
        QTRY_VERIFY_WITH_TIMEOUT(editorSeen, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(uploaded.size(), 1, 15000);
        QVERIFY(uploaded.last().at(1).toBool());
        QVERIFY(editorSeen);
        QCOMPARE(displayed, contents);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toString(), directory + QStringLiteral("/log"));
        QCOMPARE(m_handler.opened.size(), externalOpens);
        QByteArray saved;
        QVERIFY2(backend->readFile(logPath, &saved, &error), qPrintable(error));
        QVERIFY(saved.contains("saved through link"));
        editorSeen = false;
        m_remote->openRequested(entry(m_remote, name), true);
        QTRY_COMPARE_WITH_TIMEOUT(uploaded.size(), 2, 15000);
        QVERIFY(editorSeen);
        QCOMPARE(m_handler.opened.size(), externalOpens);
        editorTimer.stop();
    }

    void opensGrowingLogSnapshot()
    {
        auto backend = createBackend(ssh.protocol);
        QString error;
        QVERIFY2(backend->connect(ssh, &error), qPrintable(error));
        const QString remotePath = QStringLiteral("/home/test/navigation/growing-log");
        const QByteArray original("existing log line\n");
        QVERIFY2(backend->writeFile(remotePath, original, &error), qPrintable(error));
        bool appended = false;
        bool appendSucceeded = false;
        const auto append = connect(m_fs, &FileSystemController::fileStated, this,
            [&](quint64, const SessionData &, const QString &path, const FileInfo &, const QString &) {
                if (path != remotePath || appended)
                    return;
                appended = true;
                appendSucceeded = backend->writeFile(remotePath, original + "new log line\n", &error);
            });
        bool snapshotSeen = false;
        QTimer closeEditor;
        connect(&closeEditor, &QTimer::timeout, this, [&] {
            auto *editor = m_window->findChild<TextEditorDialog *>();
            if (editor && editor->isVisible()) {
                snapshotSeen = editor->windowTitle().contains(QStringLiteral("只读快照")) &&
                    editor->findChild<QPlainTextEdit *>()->isReadOnly() &&
                    editor->content().startsWith(original);
                editor->reject();
            }
        });
        closeEditor.start(20);
        auto *opener = m_window->findChild<RemoteFileOpener *>();
        QSignalSpy opened(opener, &RemoteFileOpener::textFileReady);
        FileInfo file;
        file.path = remotePath;
        file.name = QStringLiteral("growing-log");
        opener->open(ssh, file);
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 15000);
        QVERIFY(appendSucceeded);
        QVERIFY(snapshotSeen);
        QVERIFY(opened.first().at(2).toBool());
        disconnect(append);
    }

    void remembersDirectories()
    {
        const QString local = QDir(m_temporary.path()).filePath(QStringLiteral("destination"));
        m_local->navigateLocal(local);
        QTRY_COMPARE_WITH_TIMEOUT(m_local->currentPath(), local, 10000);
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
        auto *sshFiles = m_fs;
        auto *sshPanel = m_panel;
        auto *sshView = m_window->activeWorkspace();
        m_window->setInitialSession(ftp, true);
        auto *ftpView = m_window->activeWorkspace();
        QVERIFY(ftpView != sshView);
        m_fs = ftpView->findChild<FileSystemController *>();
        m_panel = ftpView->findChild<TerminalPanel *>();
        for (auto *pane : ftpView->findChildren<FilePane *>()) {
            if (pane->isRemote())
                m_remote = pane;
            else
                m_local = pane;
        }
        QTRY_VERIFY_WITH_TIMEOUT(m_fs->isConnected() && m_fs->protocol() == Protocol::Ftp, 20000);
        QCOMPARE(m_panel->terminalCount(), 0);
        QVERIFY(sshFiles->isConnected());
        QCOMPARE(sshPanel->terminalCount(), 1);
        auto *tabs = m_window->findChild<QTabBar *>(QStringLiteral("SessionTabs"));
        QCOMPARE(tabs->count(), 3);
        tabs->setCurrentIndex(0);
        QCOMPARE(m_window->activeWorkspace(), sshView);
        auto *terminal = sshPanel->findChild<TerminalWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(terminal->isSessionRunning(), 10000);
        const QString text = QStringLiteral("SESSION_TAB_TERMINAL_OK");
        terminal->setFocus();
        QTest::keyClicks(terminal, QStringLiteral("echo ") + text);
        QTest::keyClick(terminal, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(terminal->screen()->textRange(0, 0, terminal->screen()->totalLines() - 1,
            terminal->screen()->columns()).contains(QLatin1Char('\n') + text + QLatin1Char('\n')), 10000);
        tabs->setCurrentIndex(1);
        QCOMPARE(m_window->activeWorkspace(), ftpView);
        tabs->tabCloseRequested(0);
        QCOMPARE(m_window->activeWorkspace(), ftpView);
        QVERIFY(m_fs->isConnected());
        QCOMPARE(m_window->findChildren<QDialog *>(QStringLiteral("TransferQueueWindow")).size(), 1);
        for (auto *action : m_window->findChildren<QAction *>())
            QVERIFY(!action->text().contains(QStringLiteral("关于")));
    }

    void automaticRefreshWithoutTerminal()
    {
        QCOMPARE(m_panel->terminalCount(), 0);
        auto backend = createBackend(ftp.protocol);
        QString error;
        QVERIFY2(backend->connect(ftp, &error), qPrintable(error));
        auto *button = m_remote->findChild<QToolButton *>(QStringLiteral("AutoRefreshButton"));
        button->setChecked(true);
        const QByteArray content("FTP automatic refresh without an SSH terminal");
        QVERIFY2(backend->writeFile(QStringLiteral("/ftp-auto-refresh.txt"), content, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(entry(m_remote, QStringLiteral("ftp-auto-refresh.txt")).size, content.size(), 4500);
        QVERIFY2(backend->removeFile(QStringLiteral("/ftp-auto-refresh.txt"), &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!entry(m_remote, QStringLiteral("ftp-auto-refresh.txt")).isValid(), 4500);
        button->setChecked(false);
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
