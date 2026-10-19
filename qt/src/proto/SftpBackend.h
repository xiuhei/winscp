// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"

#include <libssh2_sftp.h>

#include <memory>

namespace wsc {

class SshConnection;

/// SFTP backend built on libssh2. Listing uses the SFTP readdir extension so
/// symlinks, permissions and timestamps are preserved.
class SftpBackend final : public Backend {
public:
    SftpBackend();
    ~SftpBackend() override;

    Protocol protocol() const override { return Protocol::Sftp; }
    bool connect(const SessionData &session, QString *error) override;
    void disconnect() override;
    bool isConnected() const override;

    QString homePath() override;
    bool list(const QString &path, FileList *out, QString *error) override;
    bool stat(const QString &path, FileInfo *out, QString *error) override;
    bool mkdir(const QString &path, QString *error) override;
    bool removeFile(const QString &path, QString *error) override;
    bool removeDir(const QString &path, QString *error) override;
    bool rename(const QString &from, const QString &to, QString *error) override;
    bool chmod(const QString &path, quint32 mode, QString *error) override;
    bool createSymlink(const QString &target, const QString &linkPath, QString *error) override;

    bool readFile(const QString &path, QByteArray *data, QString *error) override;
    bool writeFile(const QString &path, const QByteArray &data, QString *error) override;

    bool downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                      const ProgressCallback &progress, CancelToken *cancel,
                      QString *error) override;
    bool uploadFile(const QString &localPath, const QString &remotePath,
                    const ProgressCallback &progress, CancelToken *cancel,
                    QString *error) override;

    bool supportsUnixPermissions() const override { return true; }
    bool supportsSymlinks() const override { return true; }
    bool supportsResume() const override { return true; }
    bool canSetModifiedTime() const override { return true; }
    bool setModifiedTime(const QString &path, const QDateTime &time, QString *error) override;
    void keepAlive() override;
    void setHostKeyPrompt(HostKeyPrompt prompt) override { m_hostKeyPrompt = std::move(prompt); }

    QString backendName() const override { return QStringLiteral("SFTP"); }
    SshConnection *sshConnection() const override { return m_ssh.get(); }

private:
    bool authenticate(const SessionData &session, QString *error);
    bool openSftp(QString *error);
    bool removeDirRecursive(const QString &path, int depth, QString *error);
    QString sftpError(const QString &fallback) const;
    void applyAttributes(const QString &directory, const QString &name,
                         const LIBSSH2_SFTP_ATTRIBUTES &attributes, FileInfo *info);

    std::unique_ptr<SshConnection> m_ssh;
    void *m_sftp = nullptr; // LIBSSH2_SFTP*
    QString m_home = QStringLiteral("/");
    HostKeyPrompt m_hostKeyPrompt;
};

} // namespace wsc
