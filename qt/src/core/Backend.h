// SPDX-License-Identifier: MIT
#pragma once

#include "core/SessionData.h"
#include "core/Types.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

namespace wsc {

class SshConnection;

struct TransferProgress {
    qint64 bytesDone = 0;
    qint64 bytesTotal = -1;
    QString currentFile;
};

using ProgressCallback = std::function<void(const TransferProgress &)>;
using CancelToken = std::atomic_bool;

/// UI hook used to confirm an unknown or changed SSH host key.
using HostKeyPrompt = std::function<bool(const QString &host, int port, const QString &algorithm,
                                        const QString &fingerprint, QString *error)>;

/// Synchronous file-system backend. Every protocol implements this interface;
/// the UI wraps it in FileSystemController (worker threads) and the transfer
/// queue creates short-lived instances for parallel transfers.
class Backend {
public:
    virtual ~Backend() = default;

    virtual Protocol protocol() const = 0;
    virtual bool connect(const SessionData &session, QString *error) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    virtual QString homePath() = 0;
    virtual bool list(const QString &path, FileList *out, QString *error) = 0;
    virtual bool stat(const QString &path, FileInfo *out, QString *error) = 0;
    virtual bool mkdir(const QString &path, QString *error) = 0;
    virtual bool removeFile(const QString &path, QString *error) = 0;
    virtual bool removeDir(const QString &path, QString *error) = 0;
    virtual bool rename(const QString &from, const QString &to, QString *error) = 0;
    virtual bool chmod(const QString &path, quint32 mode, QString *error) = 0;
    virtual bool createSymlink(const QString &, const QString &, QString *error)
    {
        if (error)
            *error = QStringLiteral("当前协议不支持创建符号链接。");
        return false;
    }

    virtual bool readFile(const QString &path, QByteArray *data, QString *error) = 0;
    virtual bool writeFile(const QString &path, const QByteArray &data, QString *error) = 0;

    virtual bool downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                              const ProgressCallback &progress, CancelToken *cancel,
                              QString *error) = 0;
    virtual bool uploadFile(const QString &localPath, const QString &remotePath,
                            const ProgressCallback &progress, CancelToken *cancel,
                            QString *error) = 0;

    virtual bool supportsUnixPermissions() const { return false; }
    virtual bool supportsSymlinks() const { return false; }
    virtual bool canSetModifiedTime() const { return false; }
    virtual bool setModifiedTime(const QString &, const QDateTime &, QString *error)
    {
        if (error)
            error->clear();
        return false;
    }

    virtual void keepAlive() {}
    virtual SshConnection *sshConnection() const { return nullptr; }
    virtual void setHostKeyPrompt(HostKeyPrompt prompt) { Q_UNUSED(prompt); }

    /// Human readable protocol name used in logs.
    virtual QString backendName() const { return protocolDisplayName(protocol()); }
};

using BackendPtr = std::shared_ptr<Backend>;

/// Creates a backend for the given protocol. Returns nullptr for unsupported
/// combinations. Local sessions are handled directly by the UI.
BackendPtr createBackend(Protocol protocol);

} // namespace wsc
