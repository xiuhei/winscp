// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"

class QSslSocket;

namespace wsc {

/// FTP and FTPS (explicit / implicit TLS) backend.
///
/// The implementation is synchronous and must be used from a worker thread.
/// It prefers MLSD for listings and falls back to Unix/DOS style LIST output.
class FtpBackend final : public Backend {
public:
    explicit FtpBackend(Protocol protocol);
    ~FtpBackend() override;

    Protocol protocol() const override { return m_protocol; }
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
    bool canSetModifiedTime() const override { return false; }
    void keepAlive() override;

    QString backendName() const override { return m_protocol == Protocol::Ftps ? "FTPS" : "FTP"; }

private:
    struct Reply {
        int code = 0;
        QStringList lines;

        QString text() const { return lines.join(QLatin1Char('\n')); }
        bool ok() const { return code >= 200 && code < 300; }
    };

    bool openControl(QString *error);
    bool upgradeToTls(bool implicit, QString *error);
    bool login(const SessionData &session, QString *error);
    bool readReply(Reply *reply, QString *error);
    bool sendCommand(const QString &command, Reply *reply, QString *error);
    bool expect(const QString &command, int expectedCode, Reply *reply, QString *error);
    bool cwd(const QString &path, QString *error);
    bool openDataConnection(QSslSocket **dataSocket, QString *error, bool preferTls12 = false);
    void closeDataConnection(QSslSocket *socket);
    bool readDataAll(QSslSocket *dataSocket, QByteArray *out, QString *error);
    bool parseMlsdLine(const QString &line, const QString &directory, FileInfo *info);
    bool parseUnixLine(const QString &line, const QString &directory, FileInfo *info);
    bool parseDosLine(const QString &line, const QString &directory, FileInfo *info);
    bool parseList(const QByteArray &data, const QString &directory, FileList *out, QString *error);
    bool removeDirRecursive(const QString &path, int depth, QString *error);
    bool retrToSink(const QString &remotePath, const std::function<bool(const char *, qint64)> &sink,
                    qint64 totalHint, const ProgressCallback &progress, CancelToken *cancel,
                    QString *error);
    bool uploadStream(const QString &localPath, const QString &remotePath,
                      const ProgressCallback &progress, CancelToken *cancel, QString *error);
    bool readTransferReply(Reply *reply, QString *error);
    bool runListCommand(const QString &command, QByteArray *data, int *replyCode, QString *error);
    QString errorForReply(const Reply &reply, const QString &context) const;
    QString quote(const QString &path) const;

    Protocol m_protocol = Protocol::Ftp;
    QSslSocket *m_control = nullptr;
    QString m_host;
    int m_port = 21;
    QString m_user;
    QString m_cwd = QStringLiteral("/");
    QString m_home = QStringLiteral("/");
    bool m_connected = false;
    bool m_secure = false;
    bool m_verifyTls = true;
    bool m_useMlsd = false;
    bool m_useMlst = false;
    bool m_useEpsv = false;
    bool m_useUtf8 = false;
    int m_timeoutMs = 20000;
    int m_dataTimeoutMs = 60000;
    bool m_mlsdFailed = false;
    QStringList m_tlsErrors;
    QByteArray m_sessionTicket;
};

} // namespace wsc
