// SPDX-License-Identifier: MIT
#pragma once

#include "core/SessionData.h"
#include "core/Types.h"

#include <QFrame>
#include <QList>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTableView;
class QToolButton;
class QUrl;

namespace wsc {

class FileFilterProxyModel;
class FileSystemController;
class FileTableModel;

/// One side of the commander interface. It works as a local file browser or as
/// the remote browser driven by a FileSystemController.
class FilePane : public QFrame {
    Q_OBJECT
public:
    enum class Role {
        Local,
        Remote,
    };

    explicit FilePane(Role role, QWidget *parent = nullptr);
    ~FilePane() override = default;

    Role role() const { return m_role; }
    bool isRemote() const { return m_role == Role::Remote; }
    bool isConnected() const;
    QString currentPath() const;
    QString title() const;
    SessionData session() const { return m_session; }

    void attachController(FileSystemController *controller);
    void setRemoteSession(const SessionData &session);
    void setRemoteConnecting(const SessionData &session);
    void applyRemoteListing(const QString &path, const QList<FileInfo> &entries);
    void showRemoteConnected(const QString &home, const QString &initialPath);
    void showRemoteConnectFailed(const QString &message);
    void showRemoteDisconnected(const QString &reason);

    void navigateLocal(const QString &path);
    void navigateRemote(const QString &path);
    void navigate(const QString &path);
    void goUp();
    void goHome();
    void refresh();
    void focusFileList();
    void focusPathEditor();
    void setFilterText(const QString &text);

    QList<FileInfo> selectedEntries() const;
    QStringList selectedPaths() const;
    FileInfo currentEntry() const;
    FileInfo entryAtViewRow(int row) const;
    void selectByName(const QStringList &names);
    void updateFileIconTheme();

    void setShowHidden(bool show);
    bool showHidden() const;

signals:
    void pathChanged(const QString &path, bool remote);
    void connectRequested();
    void disconnectRequested();
    void newSessionRequested();
    void siteManagerRequested();
    void terminalRequested(const QString &path);
    void openRequested(const wsc::FileInfo &info, bool remote);
    void uploadRequested(const QList<wsc::FileInfo> &entries);
    void downloadRequested(const QList<wsc::FileInfo> &entries);
    void deleteRequested(const QList<wsc::FileInfo> &entries, bool remote);
    void renameRequested(const wsc::FileInfo &info, bool remote);
    void newFolderRequested(const QString &parentPath, bool remote);
    void propertiesRequested(const wsc::FileInfo &info, bool remote);
    void externalFilesDropped(const QList<QUrl> &urls, const QString &targetDirectory,
                              bool targetRemote);
    void transferDropped(const QList<wsc::FileInfo> &entries, bool sourceRemote,
                         const QString &targetDirectory, bool targetRemote);
    void statusMessage(const QString &message);
    void selectionChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void buildUi();
    void buildHeader();
    void buildTable();
    void buildFooter();
    void buildWelcomePage();
    void applyRoleStyle();
    void updateNavigationButtons();
    void updateStatus();
    void updateEmptyState();
    void updateHeader();
    void setLoading(bool loading, const QString &path = {});
    void addHistory(const QString &path);
    void goBack();
    void goForward();
    void activateCurrent();
    bool loadLocalDirectory(const QString &path, bool addHistory);
    void updateCombo(const QString &path);
    void showContextMenu(const QPoint &position);
    void handleDrop(const QList<FileInfo> &sourceEntries, bool sourceRemote,
                    const QString &targetDirectory);
    void handleExternalDrop(const QList<QUrl> &urls, const QString &targetDirectory);
    void startDrag();
    void showWelcome(bool connecting, const QString &message = {});
    void showTablePage();
    void selectFirstRow();

    Role m_role = Role::Local;
    QComboBox *m_pathCombo = nullptr;
    QToolButton *m_backButton = nullptr;
    QToolButton *m_forwardButton = nullptr;
    QToolButton *m_upButton = nullptr;
    QToolButton *m_homeButton = nullptr;
    QToolButton *m_refreshButton = nullptr;
    QToolButton *m_connectButton = nullptr;
    QToolButton *m_disconnectButton = nullptr;
    QToolButton *m_terminalButton = nullptr;
    QToolButton *m_hiddenButton = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QLabel *m_badgeLabel = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QLabel *m_welcomeTitle = nullptr;
    QLabel *m_welcomeText = nullptr;
    QLabel *m_welcomeIcon = nullptr;
    QPushButton *m_newSessionButton = nullptr;
    QPushButton *m_siteManagerButton = nullptr;
    QProgressBar *m_loadingBar = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_welcomePage = nullptr;
    QTableView *m_table = nullptr;
    FileTableModel *m_model = nullptr;
    FileFilterProxyModel *m_proxy = nullptr;
    FileSystemController *m_controller = nullptr;

    SessionData m_session;
    QString m_localPath;
    QString m_remotePath;
    QString m_remoteHome;
    QString m_pendingRemotePath;
    QStringList m_history;
    int m_historyIndex = -1;
    bool m_pendingHistory = false;
    bool m_remoteConnected = false;
    bool m_connecting = false;
    bool m_loading = false;
    bool m_showHidden = false;
};

} // namespace wsc
