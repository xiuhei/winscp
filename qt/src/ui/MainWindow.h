// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"
#include "core/SessionData.h"
#include "core/Types.h"
#include "transfer/TransferTypes.h"

#include <QList>
#include <QMainWindow>
#include <QUrl>

class QAction;
class QDialog;
class QLabel;
class QMenu;
class QProgressBar;
class QSplitter;
class QToolBar;

namespace wsc {

class RemoteFileOpener;
class FilePane;
class FileSystemController;
class TerminalPanel;
class TransferManager;
class TransferQueueWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void setInitialSession(const SessionData &session, bool connectNow);
    void enableDemoMode();
    void newSession();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void buildActions();
    void buildMenus();
    void buildToolbar();
    void buildStatusBar();
    void buildTransferWindow();
    void connectSignals();
    void restoreWindowSettings();
    void saveWindowSettings();

    void connectToSession(const SessionData &session);
    void disconnectSession();
    void openPreferences();
    void openSynchronize();
    void deleteLocalSilently(const QList<FileInfo> &entries);
    void showShortcuts();
    void openLogFile();
    void updateRecentMenu();
    void updateActions();
    void updateTitle();
    void updateTransferSummary();
    void updateActionIcons();
    void refreshTheme();
    void showStatus(const QString &message, bool error = false);
    void showTransferQueue(bool visible);

    void uploadSelection();
    void downloadSelection();
    void uploadEntries(const QList<FileInfo> &entries, const QString &targetDirectory);
    void downloadEntries(const QList<FileInfo> &entries, const QString &targetDirectory,
                         bool openWhenDone = false);
    void deleteEntries(const QList<FileInfo> &entries, bool remote);
    void renameEntry(const FileInfo &info, bool remote);
    void createFolder(const QString &parentPath, bool remote);
    void showProperties(const FileInfo &info, bool remote);
    void openFile(const FileInfo &info, bool remote);
    void showTextEditor(const QString &path, const QByteArray &data, bool readOnly);
    void handleExternalDrop(const QList<QUrl> &urls, const QString &targetDirectory,
                            bool targetRemote);
    void handleCrossPaneDrop(const QList<FileInfo> &entries, bool sourceRemote,
                             const QString &targetDirectory, bool targetRemote);
    void copyLocalPaths(const QStringList &paths, const QString &targetDirectory);

    void openTerminal(const QString &path = {});
    void onRemoteConnected(const SessionData &session, const QString &home,
                           const QString &initialPath);
    void onRemoteConnectFailed(const QString &message);
    void onRemoteDisconnected(const QString &reason);
    void onDirectoryListed(const QString &path, const QList<FileInfo> &entries);
    void onOperationFinished(const QString &action, bool success, const QString &message);
    void onFileRead(const QString &path, const QByteArray &data);
    void onFileWriteFinished(const QString &path, bool success, const QString &message);
    void onSymlinkCreated(const QString &linkPath, bool success, const QString &message);

    bool askHostKey(const QString &host, int port, const QString &algorithm,
                    const QString &fingerprint, QString *error);
    bool showHostKeyDialog(const QString &host, int port, const QString &algorithm,
                           const QString &fingerprint, QString *error);
    ConflictAction askConflict(const QString &targetPath, TransferDirection direction,
                               qint64 sourceSize, qint64 targetSize);

    FilePane *activePane() const;
    FilePane *focusedPane() const;
    bool remoteReady() const;

    FilePane *m_localPane = nullptr;
    FilePane *m_remotePane = nullptr;
    QSplitter *m_commanderSplitter = nullptr;
    QSplitter *m_mainSplitter = nullptr;
    TerminalPanel *m_terminalPanel = nullptr;
    FileSystemController *m_remoteFs = nullptr;
    RemoteFileOpener *m_fileOpener = nullptr;
    TransferManager *m_transferManager = nullptr;
    TransferQueueWidget *m_queueWidget = nullptr;
    QDialog *m_queueDialog = nullptr;
    QToolBar *m_toolBar = nullptr;

    QAction *m_newSessionAction = nullptr;
    QAction *m_siteManagerAction = nullptr;
    QAction *m_connectAction = nullptr;
    QAction *m_disconnectAction = nullptr;
    QAction *m_terminalAction = nullptr;
    QAction *m_uploadAction = nullptr;
    QAction *m_downloadAction = nullptr;
    QAction *m_syncAction = nullptr;
    QAction *m_newFolderAction = nullptr;
    QAction *m_renameAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_propertiesAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_preferencesAction = nullptr;
    QAction *m_exitAction = nullptr;
    QAction *m_showHiddenAction = nullptr;
    QAction *m_queueAction = nullptr;
    QAction *m_clearQueueAction = nullptr;
    QAction *m_cancelTransfersAction = nullptr;
    QAction *m_shortcutsAction = nullptr;
    QAction *m_logAction = nullptr;
    QAction *m_themeDarkAction = nullptr;
    QAction *m_themeLightAction = nullptr;
    QMenu *m_recentMenu = nullptr;

    QLabel *m_statusLabel = nullptr;
    QLabel *m_transferLabel = nullptr;
    QLabel *m_siteLabel = nullptr;
    QProgressBar *m_transferProgress = nullptr;

    SessionData m_session;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_demo = false;
    bool m_closing = false;
    bool m_loginAfterDisconnect = false;
};

} // namespace wsc
