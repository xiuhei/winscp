// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"

#include <memory>

namespace wsc {

class SshConnection;

/// SCP transport implemented over SSH exec channels. Listings and metadata use
/// POSIX shell commands, file contents use binary-safe `cat` streams (the same
/// data path as the SCP protocol) so it works with minimal SCP servers.
class ScpBackend final : public Backend {
public:
    ScpBackend();
    ~ScpBackend() override;

    Protocol protocol() const override { return Protocol::Scp; }
    bool connect(const SessionData &session, QString *error) override;
    void disconnect() override;
    bool isConnected() const override;

    QString homePath() override { return m_home; }
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
    bool canSetModifiedTime() const override { return true; }
    bool setModifiedTime(const QString &path, const QDateTime &time, QString *error) override;
    void keepAlive() override;
    void setHostKeyPrompt(HostKeyPrompt prompt) override { m_hostKeyPrompt = std::move(prompt); }

    QString backendName() const override { return QStringLiteral("SCP"); }
    SshConnection *sshConnection() const override { return m_ssh.get(); }

private:
    bool authenticate(const SessionData &session, QString *error);
    bool exec(const QString &command, QByteArray *output, QByteArray *errorOutput, int *exitCode,
              QString *error);
    bool execOk(const QString &command, QByteArray *output, QString *error);
    bool downloadStream(const QString &remotePath, const QString &localPath, qint64 totalHint,
                        const ProgressCallback &progress, CancelToken *cancel, QString *error);
    bool uploadStream(const QString &localPath, const QString &remotePath,
                      const ProgressCallback &progress, CancelToken *cancel, QString *error);
    bool parseListLine(const QString &line, const QString &directory, FileInfo *info,
                       QString *error);
    QString shellQuote(const QString &value) const;
    QString commandError(const QString &command, const QByteArray &stderrData, int exitCode) const;

    std::unique_ptr<SshConnection> m_ssh;
    QString m_home = QStringLiteral("/");
    HostKeyPrompt m_hostKeyPrompt;
    bool m_connected = false;
};

} // namespace wsc
