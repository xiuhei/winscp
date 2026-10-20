#include "core/SessionData.h"

#include <QJsonValue>
#include <QUrl>
#include <QUrlQuery>

namespace wsc {

bool SessionData::isValid() const
{
    if (protocol == Protocol::Local)
        return true;
    return !hostName.trimmed().isEmpty() && port > 0 && port < 65536;
}

QString SessionData::displayName() const
{
    if (!siteName.trimmed().isEmpty())
        return siteName;
    if (protocol == Protocol::Local)
        return QStringLiteral("本地");
    return endpoint();
}

QString SessionData::endpoint() const
{
    if (protocol == Protocol::Local)
        return QStringLiteral("本地");
    QString user = userName;
    if (user.isEmpty())
        return QStringLiteral("%1:%2").arg(hostName).arg(port);
    return QStringLiteral("%1@%2:%3").arg(user, hostName).arg(port);
}

QString SessionData::rootLabel() const
{
    if (protocol == Protocol::Local)
        return QStringLiteral("本地文件");
    QString label = hostName;
    if (!userName.isEmpty())
        label = userName + QStringLiteral("@") + hostName;
    if (port != defaultPort(protocol) && port > 0)
        label += QStringLiteral(":") + QString::number(port);
    return label;
}

QString SessionData::url(bool includePassword) const
{
    if (protocol == Protocol::Local)
        return QStringLiteral("local://");
    QUrl url;
    url.setScheme(protocolId(protocol));
    url.setHost(hostName);
    url.setPort(port);
    url.setUserName(userName);
    if (includePassword && !password.isEmpty())
        url.setPassword(password);
    if (!remoteDirectory.isEmpty())
        url.setPath(remoteDirectory);
    return url.toString(QUrl::FullyEncoded);
}

QJsonObject SessionData::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), siteName);
    obj.insert(QStringLiteral("protocol"), protocolId(protocol));
    obj.insert(QStringLiteral("host"), hostName);
    obj.insert(QStringLiteral("port"), port);
    obj.insert(QStringLiteral("user"), userName);
    if (savePassword && !password.isEmpty())
        obj.insert(QStringLiteral("password"), password);
    obj.insert(QStringLiteral("privateKey"), privateKeyFile);
    if (!passphrase.isEmpty())
        obj.insert(QStringLiteral("passphrase"), passphrase);
    obj.insert(QStringLiteral("remoteDirectory"), remoteDirectory);
    obj.insert(QStringLiteral("localDirectory"), localDirectory);
    obj.insert(QStringLiteral("savePassword"), savePassword);
    obj.insert(QStringLiteral("implicitTls"), implicitTls);
    obj.insert(QStringLiteral("tlsVerify"), tlsVerifyCertificate);
    obj.insert(QStringLiteral("webDavHttps"), webDavHttps);
    obj.insert(QStringLiteral("timeout"), timeoutSeconds);
    obj.insert(QStringLiteral("keepAlive"), keepAliveSeconds);
    return obj;
}

SessionData SessionData::fromJson(const QJsonObject &object)
{
    SessionData data;
    data.siteName = object.value(QStringLiteral("name")).toString();
    bool ok = true;
    data.protocol = protocolFromId(object.value(QStringLiteral("protocol")).toString(), &ok);
    if (!ok)
        data.protocol = Protocol::Sftp;
    data.hostName = object.value(QStringLiteral("host")).toString();
    data.port = object.value(QStringLiteral("port")).toInt(defaultPort(data.protocol));
    data.userName = object.value(QStringLiteral("user")).toString();
    data.password = object.value(QStringLiteral("password")).toString();
    data.privateKeyFile = object.value(QStringLiteral("privateKey")).toString();
    data.passphrase = object.value(QStringLiteral("passphrase")).toString();
    data.remoteDirectory = object.value(QStringLiteral("remoteDirectory")).toString();
    data.localDirectory = object.value(QStringLiteral("localDirectory")).toString();
    data.savePassword = object.value(QStringLiteral("savePassword")).toBool(true);
    data.implicitTls = object.value(QStringLiteral("implicitTls")).toBool(false);
    data.tlsVerifyCertificate = object.value(QStringLiteral("tlsVerify")).toBool(true);
    data.webDavHttps = object.value(QStringLiteral("webDavHttps")).toBool(false);
    data.timeoutSeconds = object.value(QStringLiteral("timeout")).toInt(20);
    data.keepAliveSeconds = object.value(QStringLiteral("keepAlive")).toInt(30);
    return data;
}

SessionData SessionData::localSession()
{
    SessionData data;
    data.siteName = QStringLiteral("本地");
    data.protocol = Protocol::Local;
    data.hostName.clear();
    data.port = 0;
    return data;
}

bool SessionData::parseUrl(const QString &input, SessionData *session, QString *error)
{
    if (!session)
        return false;
    const QString text = input.trimmed();
    if (text.isEmpty()) {
        if (error)
            *error = QStringLiteral("会话地址为空。");
        return false;
    }

    // WinSCP accepts a plain "user@host" or "host" as SFTP.
    if (!text.contains(QStringLiteral("://")))
        return parseUrl(QStringLiteral("sftp://") + text, session, error);

    QUrl url(text, QUrl::TolerantMode);
    if (!url.isValid()) {
        if (error)
            *error = QStringLiteral("无法解析会话地址：%1").arg(url.errorString());
        return false;
    }

    const QString scheme = url.scheme().toLower();
    SessionData data;
    bool ok = true;
    if (scheme == QLatin1String("ssh"))
        data.protocol = Protocol::Sftp;
    else
        data.protocol = protocolFromId(scheme, &ok);
    if (!ok || data.protocol == Protocol::Local) {
        if (error)
            *error = QStringLiteral("不支持的协议：%1").arg(scheme);
        return false;
    }
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        data.protocol = Protocol::WebDav;
    data.hostName = url.host(QUrl::FullyDecoded);
    data.port = url.port(data.protocol == Protocol::WebDav ? (scheme == QLatin1String("https") ? 443 : 80)
                                                           : defaultPort(data.protocol));
    data.userName = url.userName(QUrl::FullyDecoded);
    data.password = url.password(QUrl::FullyDecoded);
    data.remoteDirectory = url.path(QUrl::FullyDecoded);
    if (data.remoteDirectory.isEmpty())
        data.remoteDirectory = QStringLiteral("/");
    if (data.protocol == Protocol::Ftps)
        data.implicitTls = (data.port == 990);
    if (data.protocol == Protocol::WebDav) {
        data.webDavHttps = (scheme == QLatin1String("https") || data.port == 443);
        data.port = url.port(data.webDavHttps ? 443 : 80);
    }
    const QUrlQuery query(url);
    if (query.hasQueryItem(QStringLiteral("insecure"))) {
        const QString value = query.queryItemValue(QStringLiteral("insecure")).toLower();
        if (value.isEmpty() || value == QLatin1String("1") || value == QLatin1String("true") ||
            value == QLatin1String("yes"))
            data.tlsVerifyCertificate = false;
    }
    if (query.hasQueryItem(QStringLiteral("implicit"))) {
        const QString value = query.queryItemValue(QStringLiteral("implicit")).toLower();
        data.implicitTls = !(value == QLatin1String("0") || value == QLatin1String("false") ||
                             value == QLatin1String("no"));
    }
    if (data.hostName.isEmpty()) {
        if (error)
            *error = QStringLiteral("会话地址缺少主机名。");
        return false;
    }
    data.siteName = data.endpoint();
    *session = data;
    return true;
}

bool SessionData::operator==(const SessionData &other) const
{
    return protocol == other.protocol && hostName == other.hostName && port == other.port &&
           userName == other.userName && remoteDirectory == other.remoteDirectory;
}

} // namespace wsc

