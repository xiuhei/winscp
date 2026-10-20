// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"
#include "core/SessionData.h"

#include <QObject>
#include <QThread>

class QTimer;

namespace wsc {

class FileSystemWorker;
class SshShellSession;

/// Asynchronous facade for a remote backend. All backend operations run on a
/// dedicated worker thread, which keeps QSslSocket/QNetworkAccessManager usage
/// thread-affine and serializes commands.
class FileSystemController : public QObject {
    Q_OBJECT
public:
    explicit FileSystemController(QObject *parent = nullptr);
    ~FileSystemController() override;

    void setHostKeyPrompt(HostKeyPrompt prompt);
    void connectToHost(const SessionData &session);
    void disconnectFromHost();
    void shutdown();
    SshShellSession *createShell(int columns, int rows, const QString &shell = {});
    void startShell(SshShellSession *shell);

    void listDirectory(const QString &path);
    void refresh();
    void createDirectory(const QString &path);
    void deleteEntries(const QList<FileInfo> &entries);
    void renameEntry(const QString &from, const QString &to);
    void setPermissions(const QString &path, quint32 mode);
    void readFile(const QString &path);
    void statFile(const QString &path, quint64 requestId = 0);
    void writeFile(const QString &path, const QByteArray &data);
    void createSymlink(const QString &target, const QString &linkPath);

    bool isConnected() const { return m_connected; }
    bool isConnecting() const { return m_connecting; }
    bool isBusy() const { return m_busy; }
    QString currentPath() const { return m_currentPath; }
    QString homePath() const { return m_home; }
    SessionData session() const { return m_session; }
    Protocol protocol() const { return m_session.protocol; }

signals:
    void connecting(const SessionData &session);
    void connected(const SessionData &session, const QString &home, const QString &initialPath);
    void connectFailed(const QString &message);
    void disconnected(const QString &reason);

    void directoryLoading(const QString &path);
    void directoryListed(const QString &path, const QList<FileInfo> &entries);
    void directoryFailed(const QString &path, const QString &message);
    void busyChanged(bool busy);

    void operationFinished(const QString &action, bool success, const QString &message);
    void fileRead(const QString &path, const QByteArray &data);
    void fileStated(quint64 requestId, const wsc::SessionData &session, const QString &path, const wsc::FileInfo &info, const QString &error);
    void fileWriteFinished(const QString &path, bool success, const QString &message);
    void symlinkCreated(const QString &linkPath, bool success, const QString &message);

private slots:
    void onConnected(const SessionData &session, const QString &home, const QString &initialPath);
    void onConnectFailed(const QString &message);
    void onDisconnected(const QString &reason);
    void onDirectoryLoading(const QString &path);
    void onDirectoryListed(const QString &path, const QList<FileInfo> &entries);
    void onDirectoryFailed(const QString &path, const QString &message);
    void onOperationFinished(const QString &action, bool success, const QString &message);
    void onFileRead(const QString &path, const QByteArray &data);
    void onFileWriteFinished(const QString &path, bool success, const QString &message);
    void onSymlinkCreated(const QString &linkPath, bool success, const QString &message);

private:
    void setBusy(bool busy);

    QThread m_thread;
    FileSystemWorker *m_worker = nullptr;
    SessionData m_session;
    QString m_home;
    QString m_currentPath;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_busy = false;
};

} // namespace wsc
