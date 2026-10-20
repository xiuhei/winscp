#include "core/KnownHosts.h"

#include "core/AppPaths.h"
#include "core/Logger.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>

namespace wsc {

KnownHosts &KnownHosts::instance()
{
    static KnownHosts hosts;
    return hosts;
}

KnownHosts::KnownHosts()
{
    loadLocked();
    m_loaded = true;
}

QString KnownHosts::keyFor(const QString &host, int port)
{
    return QStringLiteral("%1:%2").arg(host.toLower()).arg(port);
}

void KnownHosts::reload()
{
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
    loadLocked();
}

void KnownHosts::loadLocked()
{
    QFile file(AppPaths::knownHostsFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return;
    const QJsonArray array = document.object().value(QStringLiteral("hosts")).toArray();
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        KnownHostKey key;
        key.host = object.value(QStringLiteral("host")).toString();
        key.port = object.value(QStringLiteral("port")).toInt(22);
        key.algorithm = object.value(QStringLiteral("algorithm")).toString();
        key.fingerprint = object.value(QStringLiteral("fingerprint")).toString();
        key.added = QDateTime::fromString(object.value(QStringLiteral("added")).toString(),
                                          Qt::ISODate);
        if (!key.host.isEmpty() && !key.fingerprint.isEmpty())
            m_entries.append(key);
    }
}

HostKeyStatus KnownHosts::verify(const QString &host, int port, const QString &algorithm,
                                 const QString &fingerprint, KnownHostKey *known, QString *error)
{
    QMutexLocker locker(&m_mutex);
    if (!m_loaded)
        loadLocked();
    for (const KnownHostKey &entry : std::as_const(m_entries)) {
        if (entry.host.compare(host, Qt::CaseInsensitive) == 0 && entry.port == port) {
            if (known)
                *known = entry;
            if (entry.fingerprint == fingerprint)
                return HostKeyStatus::Match;
            if (error) {
                *error = QStringLiteral("主机密钥不匹配！\n\n主机：%1:%2\n算法：%3\n"
                                        "已保存：%4\n服务器：%5\n\n"
                                        "这可能表示服务器已重装，也可能表示中间人攻击。")
                             .arg(host)
                             .arg(port)
                             .arg(algorithm.isEmpty() ? entry.algorithm : algorithm)
                             .arg(entry.fingerprint, fingerprint);
            }
            return HostKeyStatus::Mismatch;
        }
    }
    if (known)
        *known = KnownHostKey{};
    return HostKeyStatus::Unknown;
}

bool KnownHosts::addOrUpdate(const QString &host, int port, const QString &algorithm,
                             const QString &fingerprint, QString *error)
{
    {
        QMutexLocker locker(&m_mutex);
        const auto previous = m_entries;
        bool found = false;
        for (KnownHostKey &entry : m_entries) {
            if (entry.host.compare(host, Qt::CaseInsensitive) == 0 && entry.port == port) {
                entry.algorithm = algorithm;
                entry.fingerprint = fingerprint;
                entry.added = QDateTime::currentDateTime();
                found = true;
                break;
            }
        }
        if (!found) {
            KnownHostKey entry;
            entry.host = host;
            entry.port = port;
            entry.algorithm = algorithm;
            entry.fingerprint = fingerprint;
            entry.added = QDateTime::currentDateTime();
            m_entries.append(entry);
        }
        if (!saveLocked(error)) {
            m_entries = previous;
            return false;
        }
    }
    if (error)
        error->clear();
    Logger::instance().info(QStringLiteral("已保存主机密钥 %1:%2 (%3)")
                                .arg(host)
                                .arg(port)
                                .arg(fingerprint));
    return true;
}

bool KnownHosts::remove(const QString &host, int port)
{
    QMutexLocker locker(&m_mutex);
    for (int index = m_entries.size() - 1; index >= 0; --index) {
        const KnownHostKey &entry = m_entries.at(index);
        if (entry.host.compare(host, Qt::CaseInsensitive) == 0 && entry.port == port) {
            const auto previous = m_entries;
            m_entries.removeAt(index);
            if (!saveLocked()) {
                m_entries = previous;
                return false;
            }
            return true;
        }
    }
    return false;
}

bool KnownHosts::contains(const QString &host, int port) const
{
    QMutexLocker locker(&m_mutex);
    for (const KnownHostKey &entry : m_entries) {
        if (entry.host.compare(host, Qt::CaseInsensitive) == 0 && entry.port == port)
            return true;
    }
    return false;
}

QList<KnownHostKey> KnownHosts::entries() const
{
    QMutexLocker locker(&m_mutex);
    return m_entries;
}

void KnownHosts::save() const
{
    QMutexLocker locker(&m_mutex);
    saveLocked();
}

bool KnownHosts::saveLocked(QString *error) const
{
    AppPaths::ensureDirectories();
    QJsonArray array;
    for (const KnownHostKey &entry : m_entries) {
        QJsonObject object;
        object.insert(QStringLiteral("host"), entry.host);
        object.insert(QStringLiteral("port"), entry.port);
        object.insert(QStringLiteral("algorithm"), entry.algorithm);
        object.insert(QStringLiteral("fingerprint"), entry.fingerprint);
        object.insert(QStringLiteral("added"), entry.added.toString(Qt::ISODate));
        array.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("hosts"), array);
    QSaveFile file(AppPaths::knownHostsFile());
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("无法保存 SSH 主机密钥：%1").arg(file.errorString());
        return false;
    }
    return true;
}

} // namespace wsc
