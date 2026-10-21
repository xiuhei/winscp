#include "ui/SessionWorkspace.h"

#include "core/AppPaths.h"
#include "core/FileSystemController.h"
#include "core/KnownHosts.h"
#include "core/Logger.h"
#include "core/PathUtils.h"
#include "core/Settings.h"
#include "proto/LocalBackend.h"
#include "terminal/TerminalWidget.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"
#include "ui/FilePane.h"
#include "ui/RemoteFileOpener.h"
#include "ui/IconProvider.h"
#include "ui/PreferencesDialog.h"
#include "ui/PropertiesDialog.h"
#include "ui/SyncDialog.h"
#include "ui/TerminalPanel.h"
#include "ui/TextEditorDialog.h"
#include "ui/Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStringDecoder>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace wsc {

namespace {

QString shortcutText(const QKeySequence &sequence)
{
    return sequence.toString(QKeySequence::NativeText);
}

bool isDirectoryWritable(const QString &path)
{
    return QFileInfo(path).isDir() && QFileInfo(path).isWritable();
}

FileInfo makeDemoEntry(const QString &directory, const QString &name, bool isDir, qint64 size,
                       quint32 mode, int minutesAgo, const QString &owner = QStringLiteral("demo"))
{
    FileInfo info;
    info.name = name;
    info.path = path::remoteJoin(directory, name);
    info.isDir = isDir;
    info.size = isDir ? 0 : size;
    info.mode = mode;
    info.owner = owner;
    info.group = owner;
    info.modified = QDateTime::currentDateTime().addSecs(-minutesAgo * 60);
    info.accessed = info.modified;
    if (name.startsWith(QLatin1Char('.')))
        info.isHidden = true;
    return info;
}

bool copyLocalRecursively(const QString &source, const QString &target, QString *error, int depth = 0)
{
    const QFileInfo sourceInfo(source);
    const QFileInfo targetInfo(target);
    if (depth > 128 || sourceInfo.isSymLink() || sourceInfo.isJunction() ||
        targetInfo.isSymLink() || targetInfo.isJunction() || sourceInfo == targetInfo) {
        if (error)
            *error = QStringLiteral("无法复制到自身、符号链接或过深的目录：%1").arg(target);
        return false;
    }
    if (sourceInfo.isDir()) {
        if (!QDir(source).isReadable() || !QDir().mkpath(target)) {
            if (error)
                *error = QStringLiteral("无法创建目录：%1").arg(target);
            return false;
        }
        const QFileInfoList children = QDir(source).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &child : children) {
            if (!copyLocalRecursively(child.absoluteFilePath(),
                                      QDir(target).filePath(child.fileName()), error, depth + 1))
                return false;
        }
        return true;
    }
    LocalBackend backend;
    return backend.uploadFile(source, target, {}, nullptr, error);
}

} // namespace

SessionWorkspace::SessionWorkspace(TransferManager *transfers, QWidget *parent)
    : QMainWindow(parent)
    , m_transferManager(transfers)
{
    setWindowFlags(Qt::Widget);
    setWindowIcon(IconProvider::instance().appIcon());
    setWindowTitle(QStringLiteral("WinSCP Qt"));

    buildActions();
    buildUi();
    buildMenus();
    buildToolbar();
    buildStatusBar();
    connectSignals();
    restoreWindowSettings();
    updateRecentMenu();
    updateActions();
    updateTitle();
    updateTransferSummary();
    showStatus(QStringLiteral("就绪"));

    connect(qApp, &QApplication::focusChanged, this, [this] { updateActions(); });
}

SessionWorkspace::~SessionWorkspace()
{
    shutdown();
}

bool SessionWorkspace::hasConnection() const
{
    return m_demo || m_remoteFs->isConnected() || m_remoteFs->isConnecting();
}

void SessionWorkspace::buildActions()
{
    auto makeAction = [this](const QString &text, const QString &icon, const QKeySequence &shortcut = {},
                             const QString &tooltip = {}) {
        auto *action = new QAction(IconProvider::instance().icon(icon), text, this);
        if (!shortcut.isEmpty())
            action->setShortcut(shortcut);
        action->setToolTip(tooltip.isEmpty() ? text : tooltip);
        action->setStatusTip(tooltip.isEmpty() ? text : tooltip);
        return action;
    };

    m_newSessionAction = makeAction(QStringLiteral("新建会话(&N)…"), QStringLiteral("connect"),
                                    QKeySequence::New, QStringLiteral("新建会话 (Ctrl+N)"));
    m_siteManagerAction = makeAction(QStringLiteral("站点管理器(&S)…"), QStringLiteral("site"),
                                     QKeySequence(QStringLiteral("Ctrl+Shift+S")),
                                     QStringLiteral("管理保存的站点"));
    m_connectAction = makeAction(QStringLiteral("连接(&C)"), QStringLiteral("connect"),
                                 QKeySequence(), QStringLiteral("连接到当前会话"));
    m_disconnectAction = makeAction(QStringLiteral("断开连接(&D)"), QStringLiteral("disconnect"),
                                    QKeySequence(), QStringLiteral("断开远程连接"));
    m_terminalAction = makeAction(QStringLiteral("打开终端(&T)"), QStringLiteral("terminal"),
                                  QKeySequence(QStringLiteral("Ctrl+T")),
                                  QStringLiteral("在下方打开 SSH 终端 (Ctrl+T)"));
    m_uploadAction = makeAction(QStringLiteral("上传(&U)"), QStringLiteral("upload"),
                                QKeySequence(QStringLiteral("Ctrl+U")),
                                QStringLiteral("上传选中的本地文件到远程目录 (Ctrl+U)"));
    m_downloadAction = makeAction(QStringLiteral("下载(&L)"), QStringLiteral("download"),
                                  QKeySequence(QStringLiteral("Ctrl+D")),
                                  QStringLiteral("下载选中的远程文件到本地目录 (Ctrl+D)"));
    m_syncAction = makeAction(QStringLiteral("同步目录(&Y)…"), QStringLiteral("sync"),
                              QKeySequence(QStringLiteral("Ctrl+Shift+Y")),
                              QStringLiteral("比较并同步本地与远程目录 (Ctrl+Shift+Y)"));
    m_newFolderAction = makeAction(QStringLiteral("新建文件夹(&F)…"), QStringLiteral("newfolder"),
                                   QKeySequence(), QStringLiteral("新建文件夹 (F7)"));
    m_renameAction = makeAction(QStringLiteral("重命名(&R)…"), QStringLiteral("rename"),
                                QKeySequence(), QStringLiteral("重命名 (F2)"));
    m_deleteAction = makeAction(QStringLiteral("删除(&D)"), QStringLiteral("delete"),
                                QKeySequence(), QStringLiteral("删除 (Del)"));
    m_propertiesAction = makeAction(QStringLiteral("属性(&P)…"), QStringLiteral("properties"),
                                    QKeySequence(), QStringLiteral("查看属性"));
    m_refreshAction = makeAction(QStringLiteral("刷新(&R)"), QStringLiteral("refresh"),
                                 QKeySequence(), QStringLiteral("刷新 (F5)"));
    m_preferencesAction = makeAction(QStringLiteral("首选项(&O)…"), QStringLiteral("settings"),
                                     QKeySequence(QStringLiteral("Ctrl+,")));
    m_exitAction = makeAction(QStringLiteral("退出(&X)"), QStringLiteral("close"),
                              QKeySequence(QStringLiteral("Ctrl+Q")));

    m_showHiddenAction = new QAction(QStringLiteral("显示隐藏文件(&H)"), this);
    m_showHiddenAction->setCheckable(true);
    m_showHiddenAction->setChecked(Settings::instance().showHiddenFiles);

    m_queueAction = new QAction(QStringLiteral("显示传输队列(&Q)"), this);
    m_queueAction->setIcon(IconProvider::instance().icon(QStringLiteral("queue")));
    m_queueAction->setCheckable(true);
    m_clearQueueAction = makeAction(QStringLiteral("清空已完成任务"), QStringLiteral("clear"));
    m_cancelTransfersAction = makeAction(QStringLiteral("取消所有传输"), QStringLiteral("stop"));

    m_themeDarkAction = new QAction(QStringLiteral("深色主题"), this);
    m_themeDarkAction->setCheckable(true);
    m_themeDarkAction->setChecked(Settings::instance().theme != QLatin1String("light"));
    m_themeLightAction = new QAction(QStringLiteral("浅色主题"), this);
    m_themeLightAction->setCheckable(true);
    m_themeLightAction->setChecked(Settings::instance().theme == QLatin1String("light"));
    auto *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    themeGroup->addAction(m_themeDarkAction);
    themeGroup->addAction(m_themeLightAction);

    m_shortcutsAction = makeAction(QStringLiteral("键盘快捷键(&K)"), QStringLiteral("terminal"));
    m_logAction = makeAction(QStringLiteral("打开日志文件(&L)"), QStringLiteral("text"));

    connect(m_newSessionAction, &QAction::triggered, this, &SessionWorkspace::newSession);
    connect(m_siteManagerAction, &QAction::triggered, this, &SessionWorkspace::newSession);
    connect(m_connectAction, &QAction::triggered, this, [this] {
        if (m_session.isValid() && m_session.protocol != Protocol::Local)
            connectToSession(m_session);
        else
            newSession();
    });
    connect(m_disconnectAction, &QAction::triggered, this, &SessionWorkspace::disconnectSession);
    connect(m_terminalAction, &QAction::triggered, this, [this] { openTerminal(); });
    connect(m_uploadAction, &QAction::triggered, this, &SessionWorkspace::uploadSelection);
    connect(m_downloadAction, &QAction::triggered, this, &SessionWorkspace::downloadSelection);
    connect(m_syncAction, &QAction::triggered, this, &SessionWorkspace::openSynchronize);
    connect(m_newFolderAction, &QAction::triggered, this, [this] {
        FilePane *pane = activePane();
        if (pane)
            createFolder(pane->currentPath(), pane->isRemote());
    });
    connect(m_renameAction, &QAction::triggered, this, [this] {
        FilePane *pane = activePane();
        if (!pane)
            return;
        const auto selected = pane->selectedEntries();
        if (selected.size() == 1)
            renameEntry(selected.first(), pane->isRemote());
    });
    connect(m_deleteAction, &QAction::triggered, this, [this] {
        FilePane *pane = activePane();
        if (!pane)
            return;
        const auto selected = pane->selectedEntries();
        if (!selected.isEmpty())
            deleteEntries(selected, pane->isRemote());
    });
    connect(m_propertiesAction, &QAction::triggered, this, [this] {
        FilePane *pane = activePane();
        if (!pane)
            return;
        const auto selected = pane->selectedEntries();
        if (selected.size() == 1)
            showProperties(selected.first(), pane->isRemote());
    });
    connect(m_refreshAction, &QAction::triggered, this, [this] {
        if (FilePane *pane = activePane())
            pane->refresh();
    });
    connect(m_preferencesAction, &QAction::triggered, this, &SessionWorkspace::openPreferences);
    connect(m_exitAction, &QAction::triggered, this, &SessionWorkspace::exitRequested);
    connect(m_showHiddenAction, &QAction::toggled, this, [this](bool checked) {
        Settings::instance().showHiddenFiles = checked;
        Settings::instance().save();
        m_localPane->setShowHidden(checked);
        m_remotePane->setShowHidden(checked);
    });
    connect(m_queueAction, &QAction::toggled, this, &SessionWorkspace::showTransferQueue);
    connect(m_clearQueueAction, &QAction::triggered, this, [this] {
        m_transferManager->clearFinished();
        updateTransferSummary();
    });
    connect(m_cancelTransfersAction, &QAction::triggered, this, [this] {
        m_transferManager->cancelAll();
        showStatus(QStringLiteral("已请求取消所有传输。"));
    });
    connect(m_themeDarkAction, &QAction::triggered, this, [this] {
        Settings::instance().theme = QStringLiteral("dark");
        Settings::instance().save();
        refreshTheme();
        emit preferencesChanged();
    });
    connect(m_themeLightAction, &QAction::triggered, this, [this] {
        Settings::instance().theme = QStringLiteral("light");
        Settings::instance().save();
        refreshTheme();
        emit preferencesChanged();
    });
    connect(m_shortcutsAction, &QAction::triggered, this, &SessionWorkspace::showShortcuts);
    connect(m_logAction, &QAction::triggered, this, &SessionWorkspace::openLogFile);
}

void SessionWorkspace::buildUi()
{
    m_remoteFs = new FileSystemController(this);
    m_remoteFs->setHostKeyPrompt([this](const QString &host, int port, const QString &algorithm,
                                        const QString &fingerprint, QString *error) {
        return askHostKey(host, port, algorithm, fingerprint, error);
    });

    m_localPane = new FilePane(FilePane::Role::Local, this);
    m_remotePane = new FilePane(FilePane::Role::Remote, this);
    m_remotePane->attachController(m_remoteFs);
    m_localPane->setShowHidden(Settings::instance().showHiddenFiles);
    m_remotePane->setShowHidden(Settings::instance().showHiddenFiles);

    m_commanderSplitter = new QSplitter(Qt::Horizontal, this);
    m_commanderSplitter->setObjectName(QStringLiteral("CommanderSplitter"));
    m_commanderSplitter->setChildrenCollapsible(false);
    m_commanderSplitter->addWidget(m_localPane);
    m_commanderSplitter->addWidget(m_remotePane);
    m_commanderSplitter->setStretchFactor(0, 1);
    m_commanderSplitter->setStretchFactor(1, 1);
    m_commanderSplitter->setSizes({640, 640});

    m_terminalPanel = new TerminalPanel(this);
    m_terminalPanel->setFileSystemController(m_remoteFs);
    m_terminalPanel->setSession(SessionData());

    m_mainSplitter = new QSplitter(Qt::Vertical, this);
    m_mainSplitter->setObjectName(QStringLiteral("MainSplitter"));
    m_mainSplitter->setChildrenCollapsible(false);
    m_mainSplitter->addWidget(m_commanderSplitter);
    m_mainSplitter->addWidget(m_terminalPanel);
    m_mainSplitter->setStretchFactor(0, 4);
    m_mainSplitter->setStretchFactor(1, 1);
    m_mainSplitter->setSizes({680, 240});
    setCentralWidget(m_mainSplitter);

    m_fileOpener = new RemoteFileOpener(m_remoteFs, m_transferManager, this);
}

void SessionWorkspace::buildMenus()
{
    QMenu *sessionMenu = menuBar()->addMenu(QStringLiteral("会话(&S)"));
    sessionMenu->addAction(m_newSessionAction);
    sessionMenu->addAction(m_siteManagerAction);
    m_recentMenu = sessionMenu->addMenu(QStringLiteral("最近会话(&R)"));
    sessionMenu->addSeparator();
    sessionMenu->addAction(m_connectAction);
    sessionMenu->addAction(m_disconnectAction);
    sessionMenu->addAction(m_terminalAction);
    sessionMenu->addSeparator();
    sessionMenu->addAction(m_preferencesAction);
    sessionMenu->addAction(m_exitAction);

    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(m_uploadAction);
    fileMenu->addAction(m_downloadAction);
    fileMenu->addAction(m_syncAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_newFolderAction);
    fileMenu->addAction(m_renameAction);
    fileMenu->addAction(m_deleteAction);
    fileMenu->addAction(m_propertiesAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_refreshAction);

    QMenu *transferMenu = menuBar()->addMenu(QStringLiteral("传输(&T)"));
    transferMenu->addAction(m_queueAction);
    transferMenu->addAction(m_clearQueueAction);
    transferMenu->addAction(m_cancelTransfersAction);

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    viewMenu->addAction(m_showHiddenAction);
    viewMenu->addAction(m_queueAction);
    QMenu *themeMenu = viewMenu->addMenu(QStringLiteral("主题"));
    themeMenu->addAction(m_themeDarkAction);
    themeMenu->addAction(m_themeLightAction);

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(m_shortcutsAction);
    helpMenu->addAction(m_logAction);
}

void SessionWorkspace::buildToolbar()
{
    m_toolBar = addToolBar(QStringLiteral("主工具栏"));
    m_toolBar->setObjectName(QStringLiteral("MainToolBar"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setIconSize(QSize(16, 16));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_toolBar->addAction(m_newSessionAction);
    m_toolBar->addAction(m_siteManagerAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_connectAction);
    m_toolBar->addAction(m_disconnectAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_uploadAction);
    m_toolBar->addAction(m_downloadAction);
    m_toolBar->addAction(m_syncAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_newFolderAction);
    m_toolBar->addAction(m_deleteAction);
    m_toolBar->addAction(m_refreshAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_terminalAction);
    m_toolBar->addAction(m_queueAction);

    auto *spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);
    m_toolBar->addAction(m_preferencesAction);
}

void SessionWorkspace::buildStatusBar()
{
    statusBar()->setSizeGripEnabled(true);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setMinimumWidth(200);
    statusBar()->addWidget(m_statusLabel, 1);

    m_transferProgress = new QProgressBar(this);
    m_transferProgress->setRange(0, 100);
    m_transferProgress->setFixedWidth(130);
    m_transferProgress->setTextVisible(false);
    m_transferProgress->setFixedHeight(7);
    m_transferProgress->hide();
    statusBar()->addPermanentWidget(m_transferProgress);

    m_transferLabel = new QLabel(this);
    m_transferLabel->setStyleSheet(QStringLiteral("padding: 0 8px;"));
    statusBar()->addPermanentWidget(m_transferLabel);

    m_siteLabel = new QLabel(this);
    m_siteLabel->setStyleSheet(QStringLiteral("padding: 0 10px; color: %1;").arg(Theme::muted().name()));
    statusBar()->addPermanentWidget(m_siteLabel);
}


void SessionWorkspace::connectSignals()
{
    connect(m_localPane, &FilePane::uploadRequested, this,
            [this](const QList<FileInfo> &entries) {
                uploadEntries(entries, m_remotePane->currentPath());
            });
    connect(m_remotePane, &FilePane::downloadRequested, this,
            [this](const QList<FileInfo> &entries) {
                downloadEntries(entries, m_localPane->currentPath());
            });
    for (FilePane *pane : {m_localPane, m_remotePane}) {
        connect(pane, &FilePane::openRequested, this, &SessionWorkspace::openFile);
        connect(pane, &FilePane::editRequested, this, [this](const FileInfo &info) {
            if (m_connected)
                m_remoteFs->readFile(info.path);
        });
        connect(pane, &FilePane::deleteRequested, this, &SessionWorkspace::deleteEntries);
        connect(pane, &FilePane::renameRequested, this, &SessionWorkspace::renameEntry);
        connect(pane, &FilePane::newFolderRequested, this, &SessionWorkspace::createFolder);
        connect(pane, &FilePane::propertiesRequested, this, &SessionWorkspace::showProperties);
        connect(pane, &FilePane::statusMessage, this, [this](const QString &message) {
            showStatus(message, true);
        });
        connect(pane, &FilePane::selectionChanged, this, &SessionWorkspace::updateActions);
        connect(pane, &FilePane::pathChanged, this, [this](const QString &, bool) {
            if (m_connected && !m_demo)
                Settings::instance().rememberDirectories(m_session, m_localPane->currentPath(), m_remotePane->currentPath());
            updateTitle();
            updateActions();
        });
        connect(pane, &FilePane::transferDropped, this, &SessionWorkspace::handleCrossPaneDrop);
        connect(pane, &FilePane::externalFilesDropped, this, &SessionWorkspace::handleExternalDrop);
    }
    connect(m_remotePane, &FilePane::connectRequested, this, &SessionWorkspace::newSession);
    connect(m_remotePane, &FilePane::disconnectRequested, this, &SessionWorkspace::disconnectSession);
    connect(m_remotePane, &FilePane::newSessionRequested, this, &SessionWorkspace::newSession);
    connect(m_remotePane, &FilePane::siteManagerRequested, this, &SessionWorkspace::newSession);
    connect(m_remotePane, &FilePane::terminalRequested, this, &SessionWorkspace::openTerminal);

    connect(m_remoteFs, &FileSystemController::connecting, this, [this](const SessionData &session) {
        m_connecting = true;
        m_connected = false;
        m_session = session;
        m_terminalPanel->setSession(session);
        showStatus(QStringLiteral("正在连接 %1 …").arg(session.endpoint()));
        updateTitle();
        updateActions();
    });
    connect(m_remoteFs, &FileSystemController::connected, this, &SessionWorkspace::onRemoteConnected);
    connect(m_remoteFs, &FileSystemController::connectFailed, this,
            &SessionWorkspace::onRemoteConnectFailed);
    connect(m_remoteFs, &FileSystemController::disconnected, this,
            &SessionWorkspace::onRemoteDisconnected);
    connect(m_remoteFs, &FileSystemController::directoryListed, this,
            &SessionWorkspace::onDirectoryListed);
    connect(m_remoteFs, &FileSystemController::operationFinished, this,
            &SessionWorkspace::onOperationFinished);
    connect(m_remoteFs, &FileSystemController::fileRead, this, &SessionWorkspace::onFileRead);
    connect(m_remoteFs, &FileSystemController::symlinkCreated, this,
            &SessionWorkspace::onSymlinkCreated);
    connect(m_remoteFs, &FileSystemController::busyChanged, this, [this](bool) { updateActions(); });

    connect(m_fileOpener, &RemoteFileOpener::statusMessage, this,
            [this](const QString &message) { showStatus(message, true); });
    connect(m_fileOpener, &RemoteFileOpener::textFileReady, this, &SessionWorkspace::showTextEditor);
    connect(m_terminalPanel, &TerminalPanel::connectRequested, this, [this] {
        newSession();
    });
    connect(m_terminalPanel, &TerminalPanel::statusMessage, this,
            [this](const QString &message) { showStatus(message, true); });
    connect(m_terminalPanel, &TerminalPanel::terminalCountChanged, this,
            [this](int) { updateActions(); });

    connect(m_transferManager, &TransferManager::taskAdded, this, [this](int) {
        updateTransferSummary();
        const int active = m_transferManager->model()->activeCount();
        showStatus(active > 0 ? QStringLiteral("已加入传输队列，共 %1 个活动任务。").arg(active)
                              : QStringLiteral("已加入传输队列。"));
    });
    connect(m_transferManager, &TransferManager::allTransfersFinished, this, [this] {
        updateTransferSummary();
        showStatus(QStringLiteral("所有传输任务已完成。"));
        m_localPane->refresh();
        if (m_connected)
            m_remotePane->refresh();
    });
    connect(m_transferManager, &TransferManager::statusMessage, this,
            [this](const QString &message) { showStatus(message, true); });
    connect(m_transferManager, &TransferManager::queueChanged, this,
            &SessionWorkspace::updateTransferSummary);
    connect(m_transferManager->model(), &TransferQueueModel::rowsChanged, this,
            &SessionWorkspace::updateTransferSummary);

}

void SessionWorkspace::restoreWindowSettings()
{
    Settings &settings = Settings::instance();
    if (!settings.commanderSplitterState.isEmpty())
        m_commanderSplitter->restoreState(settings.commanderSplitterState);
    if (!settings.bottomSplitterState.isEmpty())
        m_mainSplitter->restoreState(settings.bottomSplitterState);
}

void SessionWorkspace::saveLayoutSettings()
{
    Settings &settings = Settings::instance();
    settings.commanderSplitterState = m_commanderSplitter->saveState();
    settings.bottomSplitterState = m_mainSplitter->saveState();
    settings.showHiddenFiles = m_localPane->showHidden();
    settings.save();
}

void SessionWorkspace::setInitialSession(const SessionData &session, bool connectNow)
{
    m_session = session;
    if (!session.localDirectory.isEmpty() && QFileInfo(session.localDirectory).isDir())
        m_localPane->navigateLocal(session.localDirectory);
    m_terminalPanel->setSession(session);
    updateTitle();
    updateActions();
    if (!connectNow)
        return;
    QTimer::singleShot(100, this, [this, session] { connectToSession(session); });
}

void SessionWorkspace::newSession()
{
    emit loginRequested();
}

void SessionWorkspace::connectToSession(const SessionData &requestedSession)
{
    const SessionData session = Settings::instance().restoreDirectories(requestedSession);
    if (!session.isValid()) {
        QMessageBox::warning(this, QStringLiteral("无法连接"), QStringLiteral("会话信息不完整。"));
        return;
    }
    m_terminalPanel->closeAll();
    m_session = session;
    m_connecting = true;
    m_connected = false;
    if (!session.localDirectory.isEmpty() && QFileInfo(session.localDirectory).isDir())
        m_localPane->navigateLocal(session.localDirectory);
    m_remotePane->setRemoteConnecting(session);
    m_terminalPanel->setSession(session);
    m_remoteFs->connectToHost(session);
    updateTitle();
    updateActions();
    showStatus(QStringLiteral("正在连接 %1 …").arg(session.endpoint()));
}

void SessionWorkspace::disconnectSession()
{
    if (!m_connected && !m_connecting)
        return;
    m_terminalPanel->closeAll();
    if (m_demo) {
        m_connected = false;
        m_remotePane->showRemoteDisconnected(QStringLiteral("演示会话已断开。"));
        updateTitle();
        updateActions();
        return;
    }
    m_remoteFs->disconnectFromHost();
    m_loginAfterDisconnect = Settings::instance().showLoginDialog;
    showStatus(QStringLiteral("正在断开连接…"));
}

void SessionWorkspace::onRemoteConnected(const SessionData &session, const QString &home,
                                   const QString &initialPath)
{
    if (m_closing)
        return;
    Q_UNUSED(home);
    m_connected = true;
    m_connecting = false;
    m_session = session;
    m_demo = false;
    m_terminalPanel->setSession(session);
    Settings &settings = Settings::instance();
    settings.addRecentSession(session);
    settings.lastSessionUrl = session.url(false);
    settings.lastSessionName = session.displayName();
    settings.save();
    updateRecentMenu();
    updateTitle();
    updateActions();
    updateTransferSummary();
    m_siteLabel->setText(QStringLiteral("%1 · %2")
                             .arg(protocolDisplayName(session.protocol), session.endpoint()));
    showStatus(QStringLiteral("已连接到 %1").arg(session.endpoint()));
    Logger::instance().success(QStringLiteral("会话已就绪：%1").arg(session.url()));
    if (protocolUsesSsh(session.protocol))
        openTerminal(initialPath);
}

void SessionWorkspace::onRemoteConnectFailed(const QString &message)
{
    if (m_closing)
        return;
    m_connected = false;
    m_connecting = false;
    updateTitle();
    updateActions();
    m_siteLabel->setText(QStringLiteral("未连接"));
    showStatus(QStringLiteral("连接失败：%1").arg(message.section(QLatin1Char('\n'), 0, 0)), true);
    Logger::instance().error(QStringLiteral("连接失败：%1").arg(message));
    QMessageBox::critical(this, QStringLiteral("连接失败"), message);
}

void SessionWorkspace::onRemoteDisconnected(const QString &reason)
{
    m_terminalPanel->closeAll();
    m_terminalPanel->setSession(SessionData());
    m_connected = false;
    m_connecting = false;
    updateTitle();
    updateActions();
    m_siteLabel->setText(QStringLiteral("未连接"));
    showStatus(reason.isEmpty() ? QStringLiteral("连接已断开。") : reason);
    if (m_loginAfterDisconnect && !m_closing) {
        QTimer::singleShot(0, this, [this] {
            if (!m_closing && !m_connected && !m_connecting)
                newSession();
        });
    }
    m_loginAfterDisconnect = false;
}

void SessionWorkspace::onDirectoryListed(const QString &path, const QList<FileInfo> &entries)
{
    updateTitle();
    updateActions();
    showStatus(QStringLiteral("%1 · %2 个项目").arg(path).arg(entries.size()));
}

void SessionWorkspace::onOperationFinished(const QString &action, bool success, const QString &message)
{
    if (m_closing)
        return;
    showStatus(message, !success);
    if (!success) {
        Logger::instance().error(QStringLiteral("%1失败：%2").arg(action, message));
        QMessageBox::warning(this, QStringLiteral("%1失败").arg(action), message);
    } else {
        Logger::instance().success(QStringLiteral("%1：%2").arg(action, message));
    }
    updateActions();
}

void SessionWorkspace::onFileRead(const QString &path, const QByteArray &data)
{
    showTextEditor(path, data, false);
}

void SessionWorkspace::showTextEditor(const QString &path, const QByteArray &data, bool readOnly)
{
    if (m_closing)
        return;
    if (data.size() > MaxEditorBytes) {
        QMessageBox::information(this, QStringLiteral("文件过大"),
                                 QStringLiteral("内置编辑器支持不超过 32 MB 的文本。"));
        return;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder.decode(data);
    Q_UNUSED(decoded);
    if (data.contains('\0') || decoder.hasError()) {
        QMessageBox::information(this, QStringLiteral("无法编辑"),
                                 QStringLiteral("内置编辑器仅支持 UTF-8 文本，无法编辑该文件。"));
        return;
    }
    TextEditorDialog dialog(path, data, this, readOnly);
    const quint64 connectionId = m_remoteFs->connectionId();
    connect(&dialog, &TextEditorDialog::saveRequested, &dialog, [&, connectionId](const QByteArray &content) {
        if (!m_remoteFs->isConnected() || m_remoteFs->connectionId() != connectionId) {
            dialog.finishSave(false, QStringLiteral("连接已断开或会话已切换，请保留内容并重新打开文件。"));
            return;
        }
        m_remoteFs->writeFile(path, content);
    });
    connect(m_remoteFs, &FileSystemController::fileWriteFinished, &dialog,
            [&](const QString &savedPath, bool success, const QString &message) {
        if (savedPath == path) {
            dialog.finishSave(success, message);
            showStatus(message, !success);
        }
    });
    const auto interrupted = [&] {
        dialog.finishSave(false, QStringLiteral("连接已断开或会话已切换，编辑内容已保留。"));
    };
    connect(m_remoteFs, &FileSystemController::connecting, &dialog, interrupted);
    connect(m_remoteFs, &FileSystemController::disconnected, &dialog, interrupted);
    dialog.exec();
}

void SessionWorkspace::onSymlinkCreated(const QString &linkPath, bool success, const QString &message)
{
    showStatus(message, !success);
    if (!success)
        QMessageBox::warning(this, QStringLiteral("创建符号链接失败"), message);
    Q_UNUSED(linkPath);
}

void SessionWorkspace::openPreferences()
{
    PreferencesDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    applyPreferences();
    emit preferencesChanged();
    showStatus(QStringLiteral("首选项已保存。"));
}

void SessionWorkspace::applyPreferences()
{
    Settings &settings = Settings::instance();
    m_transferManager->setParallelLimit(settings.parallelTransfers);
    m_localPane->setShowHidden(settings.showHiddenFiles);
    m_remotePane->setShowHidden(settings.showHiddenFiles);
    {
        const QSignalBlocker blocker(m_showHiddenAction);
        m_showHiddenAction->setChecked(settings.showHiddenFiles);
    }
    refreshTheme();
    m_terminalPanel->applySettings();
    m_fileOpener->cleanup();
    updateRecentMenu();
    updateActions();
}

void SessionWorkspace::activate()
{
    m_localPane->setShowHidden(Settings::instance().showHiddenFiles);
    m_remotePane->setShowHidden(Settings::instance().showHiddenFiles);
    updateRecentMenu();
    updateActions();
}

void SessionWorkspace::openSynchronize()
{
    if (!remoteReady()) {
        showStatus(QStringLiteral("请先连接远程服务器再同步目录。"), true);
        return;
    }
    const QString localRoot = m_localPane->currentPath();
    const QString remoteRoot = m_remotePane->currentPath();
    if (!QFileInfo(localRoot).isDir()) {
        showStatus(QStringLiteral("本地目录无效：%1").arg(localRoot), true);
        return;
    }
    SyncDialog dialog(
        m_session, localRoot, remoteRoot,
        [this](const QString &host, int port, const QString &algorithm, const QString &fingerprint,
               QString *error) { return askHostKey(host, port, algorithm, fingerprint, error); },
        this);
    connect(&dialog, &SyncDialog::uploadRequested, this,
            [this](const QList<FileInfo> &files, const QString &remoteDirectory) {
                uploadEntries(files, remoteDirectory);
            });
    connect(&dialog, &SyncDialog::downloadRequested, this,
            [this](const QList<FileInfo> &files, const QString &localDirectory) {
                downloadEntries(files, localDirectory);
            });
    connect(&dialog, &SyncDialog::remoteDeleteRequested, this,
            [this](const QList<FileInfo> &files) {
                if (!m_remoteFs || files.isEmpty())
                    return;
                m_remoteFs->deleteEntries(files);
                showStatus(QStringLiteral("正在删除 %1 个远程多余文件…").arg(files.size()));
            });
    connect(&dialog, &SyncDialog::localDeleteRequested, this,
            &SessionWorkspace::deleteLocalSilently);
    dialog.exec();
}

void SessionWorkspace::deleteLocalSilently(const QList<FileInfo> &entries)
{
    if (entries.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList errors;
    for (const FileInfo &entry : entries) {
        bool ok = false;
        if (entry.isDir && !entry.isSymlink)
            ok = QDir(entry.path).removeRecursively();
        else
            ok = QFile::remove(entry.path);
        if (!ok)
            errors.append(entry.path);
    }
    QApplication::restoreOverrideCursor();
    m_localPane->refresh();
    if (!errors.isEmpty())
        showStatus(QStringLiteral("部分本地文件删除失败：%1").arg(errors.join(QStringLiteral("，"))),
                   true);
    else
        showStatus(QStringLiteral("已删除 %1 个本地多余文件。").arg(entries.size()));
}

void SessionWorkspace::openTerminal(const QString &path)
{
    if (!m_connected || !protocolUsesSsh(m_session.protocol)) {
        if (m_connecting) {
            showStatus(QStringLiteral("正在连接，请稍候再打开终端。"), true);
            return;
        }
        if (m_session.hostName.isEmpty()) {
            newSession();
            return;
        }
        QMessageBox::information(
            this, QStringLiteral("终端不可用"),
            QStringLiteral("SSH 终端仅支持 SFTP 和 SCP 会话。当前协议：%1")
                .arg(protocolDisplayName(m_session.protocol)));
        return;
    }
    m_terminalPanel->setSession(m_session);
    m_terminalPanel->openForSession(m_session, path);
    showStatus(QStringLiteral("已打开 SSH 终端。"));
}

void SessionWorkspace::uploadSelection()
{
    const auto entries = m_localPane->selectedEntries();
    if (entries.isEmpty()) {
        showStatus(QStringLiteral("请先在本地面板选择要上传的文件。"), true);
        return;
    }
    uploadEntries(entries, m_remotePane->currentPath());
}

void SessionWorkspace::downloadSelection()
{
    const auto entries = m_remotePane->selectedEntries();
    if (entries.isEmpty()) {
        showStatus(QStringLiteral("请先在远程面板选择要下载的文件。"), true);
        return;
    }
    downloadEntries(entries, m_localPane->currentPath());
}

void SessionWorkspace::uploadEntries(const QList<FileInfo> &entries, const QString &targetDirectory)
{
    if (entries.isEmpty())
        return;
    if (!m_connected || !m_remotePane->isConnected()) {
        showStatus(QStringLiteral("请先连接远程服务器。"), true);
        return;
    }
    const QString target = targetDirectory.isEmpty() ? m_remotePane->currentPath()
                                                     : targetDirectory;
    m_transferManager->enqueueUpload(m_session, entries, target);
}

void SessionWorkspace::downloadEntries(const QList<FileInfo> &entries, const QString &targetDirectory)
{
    if (entries.isEmpty())
        return;
    if (!m_connected || !m_remotePane->isConnected()) {
        showStatus(QStringLiteral("请先连接远程服务器。"), true);
        return;
    }
    QString target = targetDirectory.isEmpty() ? m_localPane->currentPath() : targetDirectory;
    QDir().mkpath(target);
    m_transferManager->enqueueDownload(m_session, entries, target);
}

void SessionWorkspace::deleteEntries(const QList<FileInfo> &entries, bool remote)
{
    if (entries.isEmpty())
        return;
    QStringList names;
    for (int index = 0; index < std::min<int>(5, entries.size()); ++index)
        names.append(entries.at(index).name);
    QString message = entries.size() == 1
        ? QStringLiteral("确定要删除“%1”吗？").arg(entries.first().name)
        : QStringLiteral("确定要删除选中的 %1 个项目吗？\n%2%3")
              .arg(entries.size())
              .arg(names.join(QStringLiteral("、")),
                   entries.size() > 5 ? QStringLiteral(" …") : QString());
    if (remote)
        message += QStringLiteral("\n\n远程删除不可恢复。");
    if (Settings::instance().confirmDelete) {
        if (QMessageBox::question(this, QStringLiteral("确认删除"), message) != QMessageBox::Yes)
            return;
    }

    if (remote) {
        if (!m_connected) {
            showStatus(QStringLiteral("远程连接已断开。"), true);
            return;
        }
        m_remoteFs->deleteEntries(entries);
        showStatus(QStringLiteral("正在删除 %1 个项目…").arg(entries.size()));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList errors;
    for (const FileInfo &entry : entries) {
        bool ok = false;
        if (entry.isDir && !entry.isSymlink)
            ok = QDir(entry.path).removeRecursively();
        else
            ok = QFile::remove(entry.path);
        if (!ok)
            errors.append(entry.path);
    }
    QApplication::restoreOverrideCursor();
    m_localPane->refresh();
    if (!errors.isEmpty()) {
        const QString error = QStringLiteral("以下项目无法删除：\n%1").arg(errors.join(QLatin1Char('\n')));
        showStatus(error, true);
        QMessageBox::warning(this, QStringLiteral("删除失败"), error);
    } else {
        showStatus(QStringLiteral("已删除 %1 个项目。").arg(entries.size()));
    }
}

void SessionWorkspace::renameEntry(const FileInfo &info, bool remote)
{
    if (!info.isValid())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("重命名"),
                                               QStringLiteral("新名称："), QLineEdit::Normal,
                                               info.name, &ok);
    if (!ok)
        return;
    const QString newName = name.trimmed();
    if (newName.isEmpty() || newName == info.name)
        return;
    if (newName == QLatin1String(".") || newName == QLatin1String("..") ||
        newName.contains(QLatin1Char('/')) || newName.contains(QLatin1Char('\\'))) {
        QMessageBox::warning(this, QStringLiteral("名称无效"), QStringLiteral("名称不能包含路径分隔符。"));
        return;
    }
    if (remote) {
        if (!m_connected)
            return;
        const QString target = path::remoteJoin(path::remoteParent(info.path), newName);
        m_remoteFs->renameEntry(info.path, target);
        return;
    }
    const QString parent = QFileInfo(info.path).absolutePath();
    const QString target = QDir(parent).filePath(newName);
    LocalBackend backend;
    QString error;
    if (!backend.rename(info.path, target, &error)) {
        QMessageBox::warning(this, QStringLiteral("重命名失败"), error);
    } else {
        m_localPane->refresh();
        showStatus(QStringLiteral("已重命名为 %1。").arg(newName));
    }
}

void SessionWorkspace::createFolder(const QString &parentPath, bool remote)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"),
                                               QStringLiteral("文件夹名称："), QLineEdit::Normal,
                                               QStringLiteral("新建文件夹"), &ok);
    if (!ok)
        return;
    const QString folderName = name.trimmed();
    if (folderName.isEmpty())
        return;
    if (folderName.contains(QLatin1Char('/')) || folderName.contains(QLatin1Char('\\'))) {
        QMessageBox::warning(this, QStringLiteral("名称无效"), QStringLiteral("名称不能包含路径分隔符。"));
        return;
    }
    if (remote) {
        if (!m_connected)
            return;
        m_remoteFs->createDirectory(path::remoteJoin(parentPath, folderName));
    } else {
        if (!QDir(parentPath).mkdir(folderName)) {
            QMessageBox::warning(this, QStringLiteral("无法创建文件夹"),
                                 QStringLiteral("无法在 %1 创建文件夹。").arg(parentPath));
        } else {
            m_localPane->refresh();
            showStatus(QStringLiteral("已创建文件夹 %1。").arg(folderName));
        }
    }
}

void SessionWorkspace::showProperties(const FileInfo &info, bool remote)
{
    if (!info.isValid())
        return;
    const bool canEdit = remote && m_connected && m_remotePane->isConnected() &&
        (m_session.protocol == Protocol::Sftp || m_session.protocol == Protocol::Ftp ||
         m_session.protocol == Protocol::Ftps);
    PropertiesDialog dialog(info, remote, canEdit, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (canEdit && dialog.permissionsEdited()) {
        m_remoteFs->setPermissions(info.path, dialog.mode());
        showStatus(QStringLiteral("正在更新权限…"));
    }
}

void SessionWorkspace::openFile(const FileInfo &info, bool remote)
{
    if (!info.isValid())
        return;
    if (!remote) {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.path))) {
            QMessageBox::warning(this, QStringLiteral("无法打开"),
                                 QStringLiteral("没有可用的程序打开此文件。"));
        }
        return;
    }
    if (!m_connected)
        return;
    m_fileOpener->open(m_session, info);
}

void SessionWorkspace::handleExternalDrop(const QList<QUrl> &urls, const QString &targetDirectory,
                                    bool targetRemote)
{
    QList<FileInfo> entries;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        const FileInfo info = LocalBackend::infoForPath(path);
        if (info.isValid())
            entries.append(info);
    }
    if (entries.isEmpty())
        return;
    if (targetRemote) {
        uploadEntries(entries, targetDirectory);
    } else {
        QStringList paths;
        for (const FileInfo &info : entries)
            paths.append(info.path);
        copyLocalPaths(paths, targetDirectory);
    }
}

void SessionWorkspace::handleCrossPaneDrop(const QList<FileInfo> &entries, bool sourceRemote,
                                     const QString &targetDirectory, bool targetRemote)
{
    if (entries.isEmpty())
        return;
    if (sourceRemote && targetRemote) {
        showStatus(QStringLiteral("暂不支持在远程服务器之间直接复制文件。"), true);
        return;
    }
    if (!sourceRemote && !targetRemote) {
        showStatus(QStringLiteral("请将文件拖放到远程面板，或使用资源管理器复制。"), true);
        return;
    }
    if (sourceRemote)
        downloadEntries(entries, targetDirectory);
    else
        uploadEntries(entries, targetDirectory);
}

void SessionWorkspace::copyLocalPaths(const QStringList &paths, const QString &targetDirectory)
{
    if (paths.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList errors;
    for (const QString &path : paths) {
        const QString target = QDir(targetDirectory).filePath(QFileInfo(path).fileName());
        const QFileInfo source(path);
        const QString destination = QDir(QFileInfo(targetDirectory).canonicalFilePath())
                                        .filePath(source.fileName());
        const QString relative = QDir(source.canonicalFilePath()).relativeFilePath(destination);
        if (source.isDir() && !QDir::isAbsolutePath(relative) &&
            relative != QLatin1String("..") && !relative.startsWith(QStringLiteral("../"))) {
            errors.append(QStringLiteral("不能将目录复制到自身或其子目录：%1").arg(path));
            continue;
        }
        QString error;
        if (!copyLocalRecursively(path, target, &error))
            errors.append(error);
    }
    QApplication::restoreOverrideCursor();
    m_localPane->refresh();
    if (!errors.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("复制失败"), errors.join(QLatin1Char('\n')));
    } else {
        showStatus(QStringLiteral("已复制 %1 个项目到 %2。").arg(paths.size()).arg(targetDirectory));
    }
}

bool SessionWorkspace::askHostKey(const QString &host, int port, const QString &algorithm,
                            const QString &fingerprint, QString *error)
{
    if (!Settings::instance().askUnknownHostKey)
        return true;
    if (QThread::currentThread() == thread()) {
        return showHostKeyDialog(host, port, algorithm, fingerprint, error);
    }
    bool accepted = false;
    QString message;
    QMetaObject::invokeMethod(
        this,
        [this, host, port, algorithm, fingerprint, &accepted, &message] {
            accepted = showHostKeyDialog(host, port, algorithm, fingerprint, &message);
        },
        Qt::BlockingQueuedConnection);
    if (!accepted && error)
        *error = message;
    return accepted;
}

bool SessionWorkspace::showHostKeyDialog(const QString &host, int port, const QString &algorithm,
                                   const QString &fingerprint, QString *error)
{
    if (m_closing) {
        if (error)
            *error = QStringLiteral("应用正在关闭。");
        return false;
    }
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("确认 SSH 主机密钥"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("首次连接到 %1:%2").arg(host).arg(port));
    box.setInformativeText(
        QStringLiteral("服务器主机密钥指纹：\n\n%1\n\n算法：%2\n\n"
                       "请核对指纹与服务端显示的一致。信任后会把密钥保存到已知主机列表。")
            .arg(fingerprint, algorithm));
    box.setDetailedText(QStringLiteral("主机：%1\n端口：%2\n算法：%3\nSHA256 指纹：%4")
                            .arg(host)
                            .arg(port)
                            .arg(algorithm, fingerprint));
    QPushButton *trustButton = box.addButton(QStringLiteral("信任并保存"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != trustButton) {
        if (error)
            *error = QStringLiteral("用户取消了主机密钥验证。");
        return false;
    }
    QString knownError;
    if (!KnownHosts::instance().addOrUpdate(host, port, algorithm, fingerprint, &knownError)) {
        if (error)
            *error = knownError;
        return false;
    }
    return true;
}


FilePane *SessionWorkspace::focusedPane() const
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus)
        return nullptr;
    if (m_localPane->isAncestorOf(focus))
        return m_localPane;
    if (m_remotePane->isAncestorOf(focus))
        return m_remotePane;
    return nullptr;
}

FilePane *SessionWorkspace::activePane() const
{
    if (FilePane *pane = focusedPane())
        return pane;
    if (m_remotePane->isConnected())
        return m_remotePane;
    return m_localPane;
}

bool SessionWorkspace::remoteReady() const
{
    return m_connected && m_remotePane->isConnected();
}

void SessionWorkspace::updateActions()
{
    if (m_closing)
        return;
    const bool connected = remoteReady();
    const bool connecting = m_connecting;
    FilePane *pane = activePane();
    const bool paneRemote = pane && pane->isRemote();
    const QList<FileInfo> selected = pane ? pane->selectedEntries() : QList<FileInfo>();

    m_connectAction->setEnabled(!connected && !connecting);
    m_disconnectAction->setEnabled(connected || connecting);
    m_terminalAction->setEnabled(connected && protocolUsesSsh(m_session.protocol));
    m_uploadAction->setEnabled(connected && !m_localPane->selectedEntries().isEmpty());
    m_downloadAction->setEnabled(connected && !m_remotePane->selectedEntries().isEmpty());
    m_syncAction->setEnabled(connected);

    const bool paneReady = pane && (!paneRemote || connected);
    m_newFolderAction->setEnabled(paneReady);
    m_renameAction->setEnabled(paneReady && selected.size() == 1);
    m_deleteAction->setEnabled(paneReady && !selected.isEmpty());
    m_propertiesAction->setEnabled(paneReady && selected.size() == 1);
    m_refreshAction->setEnabled(pane != nullptr);
    m_clearQueueAction->setEnabled(m_transferManager && m_transferManager->model()->rowCount() > 0);
    m_cancelTransfersAction->setEnabled(m_transferManager &&
                                        m_transferManager->model()->activeCount() > 0);

    const bool showHidden = Settings::instance().showHiddenFiles;
    if (m_showHiddenAction->isChecked() != showHidden) {
        const QSignalBlocker blocker(m_showHiddenAction);
        m_showHiddenAction->setChecked(showHidden);
    }
    m_themeDarkAction->setChecked(Settings::instance().theme != QLatin1String("light"));
    m_themeLightAction->setChecked(Settings::instance().theme == QLatin1String("light"));
}

void SessionWorkspace::updateTitle()
{
    QString title;
    if (m_connecting)
        title = QStringLiteral("正在连接 %1 — WinSCP Qt").arg(m_session.endpoint());
    else if (m_connected && !m_session.hostName.isEmpty())
        title = QStringLiteral("%1 — WinSCP Qt").arg(m_session.rootLabel());
    else
        title = QStringLiteral("本地文件 — WinSCP Qt");
    setWindowTitle(title);
    emit sessionChanged();
    if (m_siteLabel) {
        if (m_connected)
            m_siteLabel->setText(QStringLiteral("%1 · %2")
                                     .arg(protocolDisplayName(m_session.protocol),
                                          m_session.endpoint()));
        else if (m_connecting)
            m_siteLabel->setText(QStringLiteral("连接中 · %1").arg(m_session.endpoint()));
        else
            m_siteLabel->setText(QStringLiteral("未连接"));
    }
}

void SessionWorkspace::updateTransferSummary()
{
    if (!m_transferManager || !m_transferLabel)
        return;
    int running = 0;
    int queued = 0;
    double progressSum = 0;
    int progressCount = 0;
    for (const TransferRow &row : m_transferManager->model()->rows()) {
        if (row.status == TransferStatus::Running) {
            ++running;
            progressSum += row.progress();
            ++progressCount;
        } else if (row.status == TransferStatus::Queued) {
            ++queued;
        }
    }
    if (running > 0) {
        m_transferLabel->setText(QStringLiteral("传输 %1 · 排队 %2").arg(running).arg(queued));
        m_transferProgress->setValue(static_cast<int>(100.0 * progressSum / progressCount));
        m_transferProgress->show();
    } else {
        m_transferLabel->setText(queued > 0 ? QStringLiteral("排队 %1").arg(queued)
                                            : QStringLiteral("传输空闲"));
        m_transferProgress->hide();
    }
    updateActions();
}

void SessionWorkspace::updateRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();
    const auto recents = Settings::instance().recentSessions;
    if (recents.isEmpty()) {
        QAction *empty = m_recentMenu->addAction(QStringLiteral("（无）"));
        empty->setEnabled(false);
        return;
    }
    for (const SessionData &session : recents) {
        QAction *action = m_recentMenu->addAction(
            IconProvider::instance().icon(QStringLiteral("site")),
            QStringLiteral("%1 — %2").arg(session.displayName(), session.endpoint()));
        connect(action, &QAction::triggered, this, [this, session] {
            emit sessionRequested(session);
        });
    }
    m_recentMenu->addSeparator();
    QAction *clearAction = m_recentMenu->addAction(QStringLiteral("清除最近会话"));
    connect(clearAction, &QAction::triggered, this, [this] {
        Settings::instance().clearRecentSessions();
        Settings::instance().save();
        updateRecentMenu();
    });
}

void SessionWorkspace::refreshTheme()
{
    Theme::apply(Settings::instance().theme);
    updateActionIcons();
    m_localPane->updateFileIconTheme();
    m_remotePane->updateFileIconTheme();
    m_terminalPanel->applySettings();
    m_fileOpener->cleanup();
    if (m_siteLabel) {
        m_siteLabel->setStyleSheet(
            QStringLiteral("padding: 0 10px; color: %1;").arg(Theme::muted().name()));
    }
    update();
}

void SessionWorkspace::updateActionIcons()
{
    auto setIcon = [](QAction *action, const QString &name) {
        action->setIcon(IconProvider::instance().icon(name));
    };
    setIcon(m_newSessionAction, QStringLiteral("connect"));
    setIcon(m_siteManagerAction, QStringLiteral("site"));
    setIcon(m_connectAction, QStringLiteral("connect"));
    setIcon(m_disconnectAction, QStringLiteral("disconnect"));
    setIcon(m_terminalAction, QStringLiteral("terminal"));
    setIcon(m_uploadAction, QStringLiteral("upload"));
    setIcon(m_downloadAction, QStringLiteral("download"));
    setIcon(m_syncAction, QStringLiteral("sync"));
    setIcon(m_newFolderAction, QStringLiteral("newfolder"));
    setIcon(m_renameAction, QStringLiteral("rename"));
    setIcon(m_deleteAction, QStringLiteral("delete"));
    setIcon(m_propertiesAction, QStringLiteral("properties"));
    setIcon(m_refreshAction, QStringLiteral("refresh"));
    setIcon(m_preferencesAction, QStringLiteral("settings"));
    setIcon(m_exitAction, QStringLiteral("close"));
    setIcon(m_clearQueueAction, QStringLiteral("clear"));
    setIcon(m_cancelTransfersAction, QStringLiteral("stop"));
    setIcon(m_queueAction, QStringLiteral("queue"));
    setIcon(m_shortcutsAction, QStringLiteral("terminal"));
    setIcon(m_logAction, QStringLiteral("text"));
}

void SessionWorkspace::showStatus(const QString &message, bool error)
{
    if (!m_statusLabel)
        return;
    m_statusLabel->setText(message);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: %1; padding: 0 6px;")
            .arg(error ? Theme::danger().name() : Theme::muted().name()));
    m_statusLabel->setToolTip(message);
}

void SessionWorkspace::showTransferQueue(bool visible)
{
    emit transferQueueVisibilityRequested(visible);
}

void SessionWorkspace::setTransferQueueVisible(bool visible)
{
    if (m_queueAction) {
        const QSignalBlocker blocker(m_queueAction);
        m_queueAction->setChecked(visible);
    }
}

void SessionWorkspace::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    if (m_terminalPanel)
        m_terminalPanel->closeAll();
    if (m_remoteFs)
        m_remoteFs->shutdown();
}

void SessionWorkspace::showShortcuts()
{
    const QString text = QStringLiteral(
        "快捷键\n\n"
        "Ctrl+N          新建会话\n"
        "Ctrl+Shift+S    站点管理器\n"
        "Ctrl+T          打开 SSH 终端\n"
        "Ctrl+U          上传选中的本地文件\n"
        "Ctrl+D          下载选中的远程文件\n"
        "Ctrl+,          首选项\n"
        "Ctrl+Shift+C    终端复制\n"
        "Ctrl+Shift+V    终端粘贴\n\n"
        "文件面板\n"
        "F5              刷新\n"
        "F7              新建文件夹\n"
        "F2              重命名\n"
        "Delete          删除\n"
        "Backspace       上一级\n"
        "Alt+← / Alt+→   后退 / 前进\n"
        "Ctrl+L          定位到路径栏\n"
        "F3              筛选\n"
        "Shift+PageUp    终端向上滚动");
    QMessageBox::information(this, QStringLiteral("键盘快捷键"), text);
}

void SessionWorkspace::openLogFile()
{
    const QString path = Logger::instance().logFilePath();
    if (!QFileInfo::exists(path)) {
        QMessageBox::information(this, QStringLiteral("日志"), QStringLiteral("当前还没有日志文件。"));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        QMessageBox::information(this, QStringLiteral("日志文件"), path);
}

void SessionWorkspace::enableDemoMode()
{
    if (m_demo)
        return;
    m_demo = true;

    // Create a deterministic local sample tree for screenshots and demos.
    const QString localRoot = QDir(AppPaths::tempDir()).filePath(QStringLiteral("demo-local"));
    QDir().mkpath(QDir(localRoot).filePath(QStringLiteral("documents")));
    QDir().mkpath(QDir(localRoot).filePath(QStringLiteral("images")));
    QDir().mkpath(QDir(localRoot).filePath(QStringLiteral("src")));
    QDir().mkpath(QDir(localRoot).filePath(QStringLiteral("downloads")));
    const auto writeFile = [&localRoot](const QString &name, const QByteArray &content) {
        QFile file(QDir(localRoot).filePath(name));
        if (file.open(QIODevice::WriteOnly))
            file.write(content);
    };
    writeFile(QStringLiteral("README.md"), QByteArrayLiteral("# Demo workspace\n\nWinSCP Qt demo files.\n"));
    writeFile(QStringLiteral("notes.txt"), QByteArrayLiteral("Deployment notes\n- build\n- test\n- ship\n"));
    writeFile(QStringLiteral("documents/report.pdf"), QByteArray(128 * 1024, 'P'));
    writeFile(QStringLiteral("documents/roadmap.docx"), QByteArray(64 * 1024, 'D'));
    writeFile(QStringLiteral("images/logo.png"), QByteArray(48 * 1024, 'I'));
    writeFile(QStringLiteral("images/banner.jpg"), QByteArray(96 * 1024, 'J'));
    writeFile(QStringLiteral("src/main.cpp"), QByteArrayLiteral("#include <iostream>\nint main() { return 0; }\n"));
    writeFile(QStringLiteral("src/CMakeLists.txt"), QByteArrayLiteral("cmake_minimum_required(VERSION 3.21)\n"));
    writeFile(QStringLiteral("downloads/archive.zip"), QByteArray(256 * 1024, 'Z'));
    m_localPane->navigateLocal(localRoot);

    SessionData demo;
    demo.siteName = QStringLiteral("演示服务器");
    demo.protocol = Protocol::Sftp;
    demo.hostName = QStringLiteral("winscp.local");
    demo.port = 22;
    demo.userName = QStringLiteral("demo");
    demo.remoteDirectory = QStringLiteral("/home/demo");
    m_session = demo;
    m_connected = true;
    m_connecting = false;

    FileList entries;
    const QString root = QStringLiteral("/home/demo");
    entries.append(makeDemoEntry(root, QStringLiteral("projects"), true, 0, 040755u, 8));
    entries.append(makeDemoEntry(root, QStringLiteral("downloads"), true, 0, 040755u, 26));
    entries.append(makeDemoEntry(root, QStringLiteral("public_html"), true, 0, 040755u, 1440));
    entries.append(makeDemoEntry(root, QStringLiteral("README.md"), false, 12 * 1024, 0100644u, 18));
    entries.append(makeDemoEntry(root, QStringLiteral("deploy.sh"), false, 2140, 0100755u, 42));
    entries.append(makeDemoEntry(root, QStringLiteral("nginx.conf"), false, 3320, 0100644u, 90));
    entries.append(makeDemoEntry(root, QStringLiteral("backup-2026-10-18.tar.gz"), false, 18432 * 1024,
                                 0100644u, 310));
    entries.append(makeDemoEntry(root, QStringLiteral("screenshot.png"), false, 428 * 1024,
                                 0100644u, 620));
    entries.append(makeDemoEntry(root, QStringLiteral("server.log"), false, 2 * 1024 * 1024,
                                 0100644u, 3));
    m_remotePane->setRemoteSession(demo);
    m_remotePane->showRemoteConnected(QStringLiteral("/home/demo"), QStringLiteral("/home/demo"));
    m_remotePane->applyRemoteListing(QStringLiteral("/home/demo"), entries);

    m_terminalPanel->setSession(demo);
    m_terminalPanel->openDemoTerminal();
    m_mainSplitter->setSizes({520, 380});
    m_commanderSplitter->setSizes({680, 680});
    updateTitle();
    updateActions();
    m_siteLabel->setText(QStringLiteral("SFTP · demo@winscp.local"));
    showStatus(QStringLiteral("演示模式 · SFTP 会话已连接 · 终端位于界面下方"));
    m_terminalPanel->focusTerminal();
}

} // namespace wsc
