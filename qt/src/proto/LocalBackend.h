// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"

namespace wsc {

class LocalBackend final : public Backend {
public:
    LocalBackend() = default;
    ~LocalBackend() override = default;

    Protocol protocol() const override { return Protocol::Local; }
    bool connect(const SessionData &session, QString *error) override;
    void disconnect() override { m_connected = false; }
    bool isConnected() const override { return m_connected; }

    QString homePath() override;
    bool list(const QString &path, FileList *out, QString *error) override;
    bool stat(const QString &path, FileInfo *out, QString *error) override;
    bool mkdir(const QString &path, QString *error) override;
    bool removeFile(const QString &path, QString *error) override;
    bool removeDir(const QString &path, QString *error) override;
    bool rename(const QString &from, const QString &to, QString *error) override;
    bool chmod(const QString &path, quint32 mode, QString *error) override;

    bool readFile(const QString &path, QByteArray *data, QString *error) override;
    bool writeFile(const QString &path, const QByteArray &data, QString *error) override;

    bool downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                      const ProgressCallback &progress, CancelToken *cancel,
                      QString *error) override;
    bool uploadFile(const QString &localPath, const QString &remotePath,
                    const ProgressCallback &progress, CancelToken *cancel,
                    QString *error) override;

    bool supportsUnixPermissions() const override { return false; }
    bool supportsSymlinks() const override { return true; }
    bool canSetModifiedTime() const override { return true; }
    bool setModifiedTime(const QString &path, const QDateTime &time, QString *error) override;

    QString backendName() const override { return QStringLiteral("Local"); }

    static FileInfo infoForPath(const QString &path, bool includeHidden = true);

private:
    bool m_connected = false;
};

} // namespace wsc
