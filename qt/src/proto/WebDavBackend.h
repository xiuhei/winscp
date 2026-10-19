// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"

#include <QUrl>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkRequest;

namespace wsc {

/// WebDAV backend. Supports Basic/Digest authentication through
/// QNetworkAccessManager and HTTP(S) endpoints.
class WebDavBackend final : public Backend {
public:
    WebDavBackend() = default;
    ~WebDavBackend() override = default;

    Protocol protocol() const override { return Protocol::WebDav; }
    bool connect(const SessionData &session, QString *error) override;
    void disconnect() override { m_connected = false; }
    bool isConnected() const override { return m_connected; }

    QString homePath() override { return m_home; }
    bool list(const QString &path, FileList *out, QString *error) override;
    bool stat(const QString &path, FileInfo *out, QString *error) override;
    bool mkdir(const QString &path, QString *error) override;
    bool removeFile(const QString &path, QString *error) override;
    bool removeDir(const QString &path, QString *error) override;
    bool rename(const QString &from, const QString &to, QString *error) override;
    bool chmod(const QString &, quint32, QString *error) override;
    bool readFile(const QString &path, QByteArray *data, QString *error) override;
    bool writeFile(const QString &path, const QByteArray &data, QString *error) override;
    bool downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                      const ProgressCallback &progress, CancelToken *cancel,
                      QString *error) override;
    bool uploadFile(const QString &localPath, const QString &remotePath,
                    const ProgressCallback &progress, CancelToken *cancel,
                    QString *error) override;

    bool supportsUnixPermissions() const override { return false; }
    bool supportsSymlinks() const override { return false; }
    bool canSetModifiedTime() const override { return false; }
    void keepAlive() override;
    bool setModifiedTime(const QString &path, const QDateTime &time, QString *error) override;

    QString backendName() const override { return QStringLiteral("WebDAV"); }

private:
    struct HttpResult {
        int status = 0;
        QByteArray body;
        QString error;
        QUrl effectiveUrl;
        bool ok() const { return status >= 200 && status < 300; }
    };

    HttpResult request(const QByteArray &verb, const QString &path, const QByteArray &body = {},
                       const QByteArray &contentType = {}, const QString &depth = {},
                       const QUrl &destination = {}, bool allowStatusCodes = false);
    HttpResult propFind(const QString &path, int depth, QString *error);
    bool parsePropFind(const QByteArray &xml, const QUrl &requestUrl, FileList *out,
                       QString *error);
    bool parsePropFindSingle(const QByteArray &xml, const QUrl &requestUrl, FileInfo *out,
                             QString *error);
    bool deleteRecursive(const QString &path, int depth, QString *error);
    QUrl urlForPath(const QString &path) const;
    QString pathFromHref(const QUrl &base, const QString &href) const;
    QString httpErrorText(const HttpResult &result, const QString &context) const;
    void applySslConfiguration(QNetworkRequest *request) const;
    void attachAuthentication(QNetworkAccessManager *manager) const;

    bool m_connected = false;
    bool m_verifyTls = true;
    QString m_host;
    int m_port = 80;
    QString m_scheme = QStringLiteral("http");
    QUrl m_baseOrigin;
    QString m_home = QStringLiteral("/");
    QString m_user;
    QString m_password;
    QString m_basePath = QStringLiteral("/");
    int m_timeoutMs = 20000;
};

} // namespace wsc
