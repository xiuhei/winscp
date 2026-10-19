#include "proto/WebDavBackend.h"

#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/PathUtils.h"

#include <QAuthenticator>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimeZone>
#include <QTimer>
#include <QUuid>
#include <QXmlStreamReader>

#include <algorithm>

namespace wsc {

namespace {

constexpr auto kPropFindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
    "<d:displayname/><d:resourcetype/><d:getcontentlength/>"
    "<d:getlastmodified/><d:getcontenttype/>"
    "</d:prop></d:propfind>";

QDateTime parseHttpDate(const QString &value)
{
    QDateTime result = QDateTime::fromString(value, Qt::RFC2822Date);
    if (!result.isValid())
        result = QLocale::c().toDateTime(value, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (!result.isValid())
        return {};
    if (result.timeSpec() == Qt::LocalTime)
        result.setTimeZone(QTimeZone::UTC);
    return result.toTimeZone(QTimeZone::systemTimeZone());
}

struct DavItem {
    QString href;
    QString name;
    bool isDir = false;
    qint64 size = 0;
    QDateTime modified;
};

QList<DavItem> parseResponses(const QByteArray &xml)
{
    QList<DavItem> items;
    QXmlStreamReader reader(xml);
    DavItem current;
    bool inResponse = false;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (name == QLatin1String("response")) {
                current = DavItem();
                inResponse = true;
            } else if (inResponse && name == QLatin1String("href")) {
                current.href = reader.readElementText().trimmed();
            } else if (inResponse && name == QLatin1String("displayname")) {
                current.name = reader.readElementText().trimmed();
            } else if (inResponse && name == QLatin1String("getcontentlength")) {
                current.size = reader.readElementText().trimmed().toLongLong();
            } else if (inResponse && name == QLatin1String("getlastmodified")) {
                current.modified = parseHttpDate(reader.readElementText().trimmed());
            } else if (inResponse && name == QLatin1String("collection")) {
                current.isDir = true;
            }
        } else if (reader.isEndElement() && reader.name() == QLatin1String("response")) {
            if (!current.href.isEmpty())
                items.append(current);
            inResponse = false;
        }
    }
    return items;
}

} // namespace

void WebDavBackend::applySslConfiguration(QNetworkRequest *request) const
{
    if (!request)
        return;
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    if (!m_verifyTls)
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    request->setSslConfiguration(configuration);
}

void WebDavBackend::attachAuthentication(QNetworkAccessManager *manager) const
{
    if (!manager)
        return;
    QObject::connect(manager, &QNetworkAccessManager::authenticationRequired, manager,
                     [this](QNetworkReply *, QAuthenticator *authenticator) {
                         if (!authenticator)
                             return;
                         if (!m_user.isEmpty())
                             authenticator->setUser(m_user);
                         authenticator->setPassword(m_password);
                     });
}

bool WebDavBackend::connect(const SessionData &session, QString *error)
{
    disconnect();
    m_host = session.hostName;
    m_scheme = (session.webDavHttps || session.port == 443) ? QStringLiteral("https")
                                                            : QStringLiteral("http");
    m_port = session.port > 0 ? session.port : (m_scheme == QLatin1String("https") ? 443 : 80);
    m_user = session.userName;
    m_password = session.password;
    m_verifyTls = session.tlsVerifyCertificate;
    m_timeoutMs = std::max(5, session.timeoutSeconds) * 1000;
    m_baseOrigin = QUrl();
    m_baseOrigin.setScheme(m_scheme);
    m_baseOrigin.setHost(m_host);
    m_baseOrigin.setPort(m_port);
    m_home = path::normalizeRemote(session.remoteDirectory.isEmpty() ? QStringLiteral("/")
                                                                     : session.remoteDirectory);
    HttpResult result = propFind(m_home, 0, error);
    if (result.status == 207 || result.status == 200) {
        m_connected = true;
        Logger::instance().success(
            QStringLiteral("WebDAV 会话已建立：%1（根目录 %2）").arg(session.endpoint(), m_home));
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = httpErrorText(result, QStringLiteral("无法连接 WebDAV 服务器 %1").arg(m_host));
    return false;
}

QUrl WebDavBackend::urlForPath(const QString &pathValue) const
{
    QUrl url = m_baseOrigin;
    url.setPath(path::normalizeRemote(pathValue));
    return url;
}

QString WebDavBackend::pathFromHref(const QUrl &base, const QString &href) const
{
    QUrl url(href, QUrl::TolerantMode);
    if (url.isRelative())
        url = base.resolved(url);
    QString path = url.path(QUrl::FullyDecoded);
    if (path.isEmpty())
        path = QStringLiteral("/");
    QString normalized = path::normalizeRemote(path);
    if (normalized.size() > 1 && normalized.endsWith(QLatin1Char('/')))
        normalized.chop(1);
    return normalized;
}

WebDavBackend::HttpResult WebDavBackend::request(const QByteArray &verb, const QString &path,
                                                const QByteArray &body, const QByteArray &contentType,
                                                const QString &depth, const QUrl &destination,
                                                bool allowStatusCodes)
{
    Q_UNUSED(allowStatusCodes);
    HttpResult result;
    QNetworkAccessManager manager;
    attachAuthentication(&manager);
    QNetworkRequest networkRequest(urlForPath(path));
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                               QNetworkRequest::NoLessSafeRedirectPolicy);
    applySslConfiguration(&networkRequest);
    if (!contentType.isEmpty())
        networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    if (!depth.isEmpty())
        networkRequest.setRawHeader("Depth", depth.toLatin1());
    if (!destination.isEmpty()) {
        networkRequest.setRawHeader("Destination", destination.toString(QUrl::FullyEncoded).toLatin1());
        networkRequest.setRawHeader("Overwrite", "T");
    }

    QNetworkReply *reply = nullptr;
    if (verb == "GET")
        reply = manager.get(networkRequest);
    else if (verb == "DELETE")
        reply = manager.deleteResource(networkRequest);
    else if (verb == "PUT")
        reply = manager.put(networkRequest, body);
    else
        reply = manager.sendCustomRequest(networkRequest, verb, body);

    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [this, reply](const QList<QSslError> &errors) {
                         if (!m_verifyTls)
                             reply->ignoreSslErrors();
                         else
                             for (const QSslError &sslError : errors)
                                 Logger::instance().warning(
                                     QStringLiteral("WebDAV TLS 错误：%1").arg(sslError.errorString()));
                     });

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(m_timeoutMs);
    loop.exec();
    timer.stop();

    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.effectiveUrl = reply->url();
    result.body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        result.error = reply->errorString();
        if (result.status == 0)
            Logger::instance().warning(QStringLiteral("WebDAV 请求失败：%1").arg(result.error));
    }
    const QByteArray reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toByteArray();
    if (!reason.isEmpty() && result.error.isEmpty() && result.status >= 400)
        result.error = QString::fromUtf8(reason);
    reply->deleteLater();
    return result;
}

WebDavBackend::HttpResult WebDavBackend::propFind(const QString &pathValue, int depth, QString *error)
{
    HttpResult result = request(QByteArrayLiteral("PROPFIND"), pathValue,
                                QByteArray(kPropFindBody), QByteArrayLiteral("application/xml; charset=utf-8"),
                                QString::number(depth));
    if (result.status != 207 && result.status != 200 && error)
        *error = httpErrorText(result, QStringLiteral("读取 WebDAV 属性失败"));
    return result;
}

QString WebDavBackend::httpErrorText(const HttpResult &result, const QString &context) const
{
    QString meaning;
    switch (result.status) {
    case 0:
        meaning = result.error.isEmpty() ? QStringLiteral("网络请求失败。") : result.error;
        break;
    case 401:
        meaning = QStringLiteral("认证失败，请检查用户名和密码。");
        break;
    case 403:
        meaning = QStringLiteral("没有权限执行此操作。");
        break;
    case 404:
        meaning = QStringLiteral("资源不存在。");
        break;
    case 405:
        meaning = QStringLiteral("服务器不允许此方法。");
        break;
    case 409:
        meaning = QStringLiteral("父目录不存在或存在冲突。");
        break;
    case 423:
        meaning = QStringLiteral("资源被锁定。");
        break;
    case 507:
        meaning = QStringLiteral("存储空间不足。");
        break;
    default:
        if (result.status >= 400)
            meaning = QStringLiteral("HTTP %1 %2").arg(result.status).arg(result.error);
        else if (!result.error.isEmpty())
            meaning = result.error;
        break;
    }
    if (meaning.isEmpty())
        meaning = result.error;
    return QStringLiteral("%1：%2").arg(context, meaning);
}

bool WebDavBackend::parsePropFind(const QByteArray &xml, const QUrl &requestUrl, FileList *out,
                                  QString *error)
{
    if (!out)
        return false;
    out->clear();
    const QList<DavItem> items = parseResponses(xml);
    const QString requestPath = pathFromHref(requestUrl, requestUrl.toString());
    for (const DavItem &item : items) {
        const QString itemPath = pathFromHref(requestUrl, item.href);
        if (itemPath == requestPath)
            continue;
        FileInfo info;
        info.path = itemPath;
        info.name = !item.name.isEmpty() ? item.name : path::remoteName(itemPath);
        if (info.name.isEmpty() || info.name == QLatin1String("/"))
            info.name = path::remoteName(itemPath);
        info.isDir = item.isDir;
        info.size = item.isDir ? 0 : item.size;
        info.modified = item.modified;
        info.mode = item.isDir ? 040755u : 0100644u;
        info.isHidden = info.name.startsWith(QLatin1Char('.'));
        out->append(info);
    }
    if (error)
        error->clear();
    return true;
}

bool WebDavBackend::parsePropFindSingle(const QByteArray &xml, const QUrl &requestUrl, FileInfo *out,
                                        QString *error)
{
    const QList<DavItem> items = parseResponses(xml);
    const QString requestPath = pathFromHref(requestUrl, requestUrl.toString());
    for (const DavItem &item : items) {
        const QString itemPath = pathFromHref(requestUrl, item.href);
        if (itemPath != requestPath)
            continue;
        if (!out)
            break;
        out->path = itemPath;
        out->name = !item.name.isEmpty() ? item.name : path::remoteName(itemPath);
        out->isDir = item.isDir;
        out->size = item.isDir ? 0 : item.size;
        out->modified = item.modified;
        out->mode = item.isDir ? 040755u : 0100644u;
        out->isHidden = out->name.startsWith(QLatin1Char('.'));
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = QStringLiteral("服务器未返回请求资源的属性。");
    return false;
}

bool WebDavBackend::list(const QString &pathValue, FileList *out, QString *error)
{
    if (!m_connected) {
        if (error)
            *error = QStringLiteral("WebDAV 会话未连接。");
        return false;
    }
    const QString normalized = path::normalizeRemote(pathValue);
    HttpResult result = propFind(normalized, 1, error);
    if (result.status != 207 && result.status != 200) {
        if (error)
            *error = httpErrorText(result, QStringLiteral("无法列出目录 %1").arg(normalized));
        return false;
    }
    const QUrl requestUrl = urlForPath(normalized);
    return parsePropFind(result.body, requestUrl, out, error);
}

bool WebDavBackend::stat(const QString &pathValue, FileInfo *out, QString *error)
{
    if (!m_connected)
        return false;
    const QString normalized = path::normalizeRemote(pathValue);
    HttpResult result = propFind(normalized, 0, error);
    if (result.status != 207 && result.status != 200) {
        if (error)
            *error = httpErrorText(result, QStringLiteral("无法获取 %1 的属性").arg(normalized));
        return false;
    }
    return parsePropFindSingle(result.body, urlForPath(normalized), out, error);
}

bool WebDavBackend::mkdir(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    HttpResult result = request(QByteArrayLiteral("MKCOL"), normalized);
    if (result.status == 201 || result.status == 200) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = httpErrorText(result, QStringLiteral("无法创建目录 %1").arg(normalized));
    return false;
}

bool WebDavBackend::removeFile(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    HttpResult result = request(QByteArrayLiteral("DELETE"), normalized);
    if (result.status == 200 || result.status == 204 || result.status == 202) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = httpErrorText(result, QStringLiteral("无法删除 %1").arg(normalized));
    return false;
}

bool WebDavBackend::deleteRecursive(const QString &pathValue, int depth, QString *error)
{
    if (depth > 128) {
        if (error)
            *error = QStringLiteral("目录层级过深，已中止删除。");
        return false;
    }
    FileList entries;
    if (!list(pathValue, &entries, error))
        return false;
    for (const FileInfo &entry : entries) {
        if (entry.isDir) {
            if (!deleteRecursive(entry.path, depth + 1, error))
                return false;
        } else if (!removeFile(entry.path, error)) {
            return false;
        }
    }
    return removeFile(pathValue, error);
}

bool WebDavBackend::removeDir(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    const bool ok = deleteRecursive(normalized, 0, error);
    if (!ok)
        return false;
    if (error)
        error->clear();
    return true;
}

bool WebDavBackend::rename(const QString &from, const QString &to, QString *error)
{
    const QString source = path::normalizeRemote(from);
    const QString target = path::normalizeRemote(to);
    const QUrl destination = urlForPath(target);
    HttpResult result = request(QByteArrayLiteral("MOVE"), source, {}, {}, {}, destination);
    if (result.status == 201 || result.status == 204 || result.status == 200) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = httpErrorText(result, QStringLiteral("无法重命名 %1").arg(source));
    return false;
}

bool WebDavBackend::chmod(const QString &, quint32, QString *error)
{
    if (error)
        *error = QStringLiteral("WebDAV 不支持 Unix 权限。");
    return false;
}

bool WebDavBackend::readFile(const QString &pathValue, QByteArray *data, QString *error)
{
    if (!data)
        return false;
    AppPaths::ensureDirectories();
    const QString tempPath = QDir(AppPaths::tempDir())
                                 .filePath(QStringLiteral("dav-read-%1.tmp")
                                               .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!downloadFile(pathValue, tempPath, -1, nullptr, nullptr, error))
        return false;
    QFile file(tempPath);
    const bool opened = file.open(QIODevice::ReadOnly);
    if (opened)
        *data = file.readAll();
    else if (error)
        *error = QStringLiteral("无法读取临时文件：%1").arg(file.errorString());
    file.close();
    QFile::remove(tempPath);
    return opened;
}

bool WebDavBackend::writeFile(const QString &pathValue, const QByteArray &data, QString *error)
{
    AppPaths::ensureDirectories();
    const QString tempPath = QDir(AppPaths::tempDir())
                                 .filePath(QStringLiteral("dav-write-%1.tmp")
                                               .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法创建临时文件：%1").arg(file.errorString());
        return false;
    }
    file.write(data);
    file.close();
    const bool ok = uploadFile(tempPath, pathValue, nullptr, nullptr, error);
    QFile::remove(tempPath);
    return ok;
}

bool WebDavBackend::downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                                 const ProgressCallback &progress, CancelToken *cancel,
                                 QString *error)
{
    const QString remote = path::normalizeRemote(remotePath);
    QNetworkAccessManager manager;
    attachAuthentication(&manager);
    QNetworkRequest request(urlForPath(remote));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    applySslConfiguration(&request);
    QNetworkReply *reply = manager.get(request);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [this, reply](const QList<QSslError> &errors) {
        Q_UNUSED(errors);
        if (!m_verifyTls)
            reply->ignoreSslErrors();
    });

    QFileInfo localInfo(localPath);
    QDir().mkpath(localInfo.absolutePath());
    QSaveFile output(localPath);
    if (!output.open(QIODevice::WriteOnly)) {
        delete reply;
        if (error)
            *error = QStringLiteral("无法写入本地文件 %1：%2")
                         .arg(QDir::toNativeSeparators(localPath), output.errorString());
        return false;
    }

    bool writeFailed = false;
    bool cancelled = false;
    QEventLoop loop;
    QTimer inactivity;
    inactivity.setSingleShot(true);
    QObject::connect(&inactivity, &QTimer::timeout, &loop, [&] {
        reply->abort();
        loop.quit();
    });
    QTimer cancelTimer;
    cancelTimer.setInterval(120);
    QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&] {
        if (cancel && cancel->load()) {
            cancelled = true;
            reply->abort();
            loop.quit();
        }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            if (output.write(chunk) != chunk.size()) {
                writeFailed = true;
                reply->abort();
                loop.quit();
            }
            inactivity.start(m_timeoutMs * 3);
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                     [&](qint64 received, qint64 total) {
                         if (progress)
                             progress(TransferProgress{received, total > 0 ? total : totalHint,
                                                       path::remoteName(remote)});
                         inactivity.start(m_timeoutMs * 3);
                     });
    inactivity.start(m_timeoutMs);
    cancelTimer.start();
    loop.exec();
    inactivity.stop();
    cancelTimer.stop();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString networkError = reply->errorString();
    if (!cancelled && !writeFailed && reply->error() == QNetworkReply::NoError) {
        // Flush any remaining data.
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty())
            output.write(tail);
    }
    const bool networkOk = (status >= 200 && status < 300) && !cancelled && !writeFailed &&
                           (reply->error() == QNetworkReply::NoError ||
                            reply->error() == QNetworkReply::OperationCanceledError);
    reply->deleteLater();
    if (cancelled) {
        output.cancelWriting();
        if (error)
            *error = QStringLiteral("传输已取消。");
        return false;
    }
    if (writeFailed) {
        output.cancelWriting();
        if (error)
            *error = QStringLiteral("写入本地文件失败。");
        return false;
    }
    if (!networkOk) {
        output.cancelWriting();
        if (error) {
            *error = QStringLiteral("下载 %1 失败：HTTP %2 %3")
                         .arg(remote)
                         .arg(status)
                         .arg(networkError);
        }
        return false;
    }
    if (!output.commit()) {
        if (error)
            *error = QStringLiteral("无法保存下载文件：%1").arg(output.errorString());
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool WebDavBackend::uploadFile(const QString &localPath, const QString &remotePath,
                               const ProgressCallback &progress, CancelToken *cancel, QString *error)
{
    QFile input(localPath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取本地文件 %1：%2")
                         .arg(QDir::toNativeSeparators(localPath), input.errorString());
        return false;
    }
    const QString remote = path::normalizeRemote(remotePath);
    QNetworkAccessManager manager;
    attachAuthentication(&manager);
    QNetworkRequest request(urlForPath(remote));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    applySslConfiguration(&request);
    QNetworkReply *reply = manager.put(request, &input);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [this, reply](const QList<QSslError> &errors) {
        Q_UNUSED(errors);
        if (!m_verifyTls)
            reply->ignoreSslErrors();
    });

    bool cancelled = false;
    QEventLoop loop;
    QTimer inactivity;
    inactivity.setSingleShot(true);
    QObject::connect(&inactivity, &QTimer::timeout, &loop, [&] {
        reply->abort();
        loop.quit();
    });
    QTimer cancelTimer;
    cancelTimer.setInterval(120);
    QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&] {
        if (cancel && cancel->load()) {
            cancelled = true;
            reply->abort();
            loop.quit();
        }
    });
    QObject::connect(reply, &QNetworkReply::uploadProgress, &loop,
                     [&](qint64 sent, qint64 total) {
                         if (progress)
                             progress(TransferProgress{sent, total, QFileInfo(localPath).fileName()});
                         inactivity.start(m_timeoutMs * 3);
                     });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    inactivity.start(m_timeoutMs);
    cancelTimer.start();
    loop.exec();
    inactivity.stop();
    cancelTimer.stop();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString networkError = reply->errorString();
    const bool networkOk = status >= 200 && status < 300 && !cancelled &&
                           (reply->error() == QNetworkReply::NoError ||
                            reply->error() == QNetworkReply::OperationCanceledError);
    reply->deleteLater();
    if (cancelled) {
        if (error)
            *error = QStringLiteral("传输已取消。");
        return false;
    }
    if (!networkOk) {
        if (error) {
            *error = QStringLiteral("上传 %1 失败：HTTP %2 %3")
                         .arg(remote)
                         .arg(status)
                         .arg(networkError);
        }
        return false;
    }
    if (error)
        error->clear();
    return true;
}

void WebDavBackend::keepAlive()
{
    // HTTP connections are kept alive by QNetworkAccessManager; nothing to do.
}

bool WebDavBackend::setModifiedTime(const QString &, const QDateTime &, QString *error)
{
    if (error)
        *error = QStringLiteral("WebDAV 后端暂不支持修改时间。");
    return false;
}

} // namespace wsc
