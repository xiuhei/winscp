#include "sync/SyncPlanner.h"

#include "core/PathUtils.h"
#include "proto/LocalBackend.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace wsc {

namespace {

bool scanLocal(const QString &root, const QString &relative, bool recursive, int depth,
               QMap<QString, FileInfo> *entries, QString *error)
{
    if (depth > 128) {
        if (error)
            *error = QStringLiteral("本地目录层级过深：%1").arg(relative);
        return false;
    }
    const QString path = relative.isEmpty() ? root : QDir(root).filePath(relative);
    QDir directory(path);
    if (!directory.exists()) {
        if (error)
            *error = QStringLiteral("本地目录不存在：%1").arg(path);
        return false;
    }
    const QFileInfoList list = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : list) {
        const QString childRelative =
            relative.isEmpty() ? info.fileName() : relative + QLatin1Char('/') + info.fileName();
        FileInfo entry = LocalBackend::infoForPath(info.absoluteFilePath());
        if (!entry.isValid())
            continue;
        entries->insert(childRelative, entry);
        if (entry.isDir && !entry.isSymlink && recursive) {
            if (!scanLocal(root, childRelative, true, depth + 1, entries, error))
                return false;
        }
    }
    return true;
}

bool scanRemote(Backend *backend, const QString &root, const QString &relative, bool recursive,
                int depth, QMap<QString, FileInfo> *entries, QString *error)
{
    if (depth > 128) {
        if (error)
            *error = QStringLiteral("远程目录层级过深：%1").arg(relative);
        return false;
    }
    const QString directory =
        relative.isEmpty() ? path::normalizeRemote(root) : path::remoteJoin(root, relative);
    FileList list;
    if (!backend->list(directory, &list, error))
        return false;
    for (const FileInfo &info : list) {
        if (info.name == QLatin1String(".") || info.name == QLatin1String(".."))
            continue;
        const QString childRelative =
            relative.isEmpty() ? info.name : relative + QLatin1Char('/') + info.name;
        entries->insert(childRelative, info);
        if (info.isDir && !info.isSymlink && recursive) {
            if (!scanRemote(backend, root, childRelative, true, depth + 1, entries, error))
                return false;
        }
    }
    return true;
}

QString parentRelative(const QString &relative)
{
    const int slash = relative.lastIndexOf(QLatin1Char('/'));
    return slash > 0 ? relative.left(slash) : QString();
}

} // namespace

QString syncActionName(SyncAction action)
{
    switch (action) {
    case SyncAction::Upload:
        return QStringLiteral("上传");
    case SyncAction::Download:
        return QStringLiteral("下载");
    case SyncAction::DeleteRemote:
        return QStringLiteral("删除远程");
    case SyncAction::DeleteLocal:
        return QStringLiteral("删除本地");
    }
    return QString();
}

SyncPlan buildSyncPlan(const SessionData &session, const QString &localRoot,
                       const QString &remoteRoot, const SyncOptions &options,
                       HostKeyPrompt hostKeyPrompt)
{
    SyncPlan result;
    QMap<QString, FileInfo> localEntries;
    QMap<QString, FileInfo> remoteEntries;
    if (!scanLocal(localRoot, QString(), options.recursive, 0, &localEntries, &result.error))
        return result;

    BackendPtr backend = createBackend(session.protocol);
    if (!backend) {
        result.error = QStringLiteral("当前协议不支持同步：%1")
                           .arg(protocolDisplayName(session.protocol));
        return result;
    }
    backend->setHostKeyPrompt(hostKeyPrompt);
    if (!backend->connect(session, &result.error))
        return result;
    if (!scanRemote(backend.get(), remoteRoot, QString(), options.recursive, 0, &remoteEntries,
                    &result.error)) {
        backend->disconnect();
        return result;
    }
    backend->disconnect();

    const qint64 toleranceSeconds = 2;
    QSet<QString> allPaths;
    for (auto it = localEntries.constBegin(); it != localEntries.constEnd(); ++it)
        allPaths.insert(it.key());
    for (auto it = remoteEntries.constBegin(); it != remoteEntries.constEnd(); ++it)
        allPaths.insert(it.key());

    QStringList sortedPaths = allPaths.values();
    std::sort(sortedPaths.begin(), sortedPaths.end());

    for (const QString &relative : sortedPaths) {
        const auto localIt = localEntries.constFind(relative);
        const auto remoteIt = remoteEntries.constFind(relative);
        const bool hasLocal = localIt != localEntries.constEnd();
        const bool hasRemote = remoteIt != remoteEntries.constEnd();
        const FileInfo localInfo = hasLocal ? localIt.value() : FileInfo();
        const FileInfo remoteInfo = hasRemote ? remoteIt.value() : FileInfo();

        if (hasLocal && localInfo.isSymlink)
            continue;
        if (hasRemote && remoteInfo.isSymlink)
            continue;
        if (hasLocal && hasRemote && localInfo.isDir != remoteInfo.isDir)
            continue;
        if ((hasLocal && localInfo.isDir) && (hasRemote && remoteInfo.isDir))
            continue;
        if (hasLocal && localInfo.isDir && !hasRemote)
            continue;
        if (hasRemote && remoteInfo.isDir && !hasLocal)
            continue;

        const QString parent = parentRelative(relative);
        const auto makeUpload = [&](const QString &reason) {
            SyncItem item;
            item.action = SyncAction::Upload;
            item.relativePath = relative;
            item.file = localInfo;
            item.targetDirectory = parent.isEmpty()
                ? path::normalizeRemote(remoteRoot)
                : path::remoteJoin(remoteRoot, parent);
            item.reason = reason;
            result.items.append(item);
        };
        const auto makeDownload = [&](const QString &reason) {
            SyncItem item;
            item.action = SyncAction::Download;
            item.relativePath = relative;
            item.file = remoteInfo;
            item.targetDirectory = parent.isEmpty() ? QDir::cleanPath(localRoot)
                                                    : QDir(localRoot).filePath(parent);
            item.reason = reason;
            result.items.append(item);
        };

        if (hasLocal && !hasRemote) {
            if (options.direction == SyncDirection::LocalToRemote ||
                options.direction == SyncDirection::Both) {
                makeUpload(QStringLiteral("远程端不存在"));
            } else if (options.deleteExtras) {
                SyncItem item;
                item.action = SyncAction::DeleteLocal;
                item.relativePath = relative;
                item.file = localInfo;
                item.reason = QStringLiteral("本地端多余");
                result.items.append(item);
            }
            continue;
        }
        if (!hasLocal && hasRemote) {
            if (options.direction == SyncDirection::RemoteToLocal ||
                options.direction == SyncDirection::Both) {
                makeDownload(QStringLiteral("本地端不存在"));
            } else if (options.deleteExtras) {
                SyncItem item;
                item.action = SyncAction::DeleteRemote;
                item.relativePath = relative;
                item.file = remoteInfo;
                item.reason = QStringLiteral("远程端多余");
                result.items.append(item);
            }
            continue;
        }

        if (localInfo.isDir || remoteInfo.isDir)
            continue;

        const bool localNewer =
            localInfo.modified.isValid() && remoteInfo.modified.isValid() &&
            localInfo.modified.toSecsSinceEpoch() >
                remoteInfo.modified.toSecsSinceEpoch() + toleranceSeconds;
        const bool remoteNewer =
            localInfo.modified.isValid() && remoteInfo.modified.isValid() &&
            remoteInfo.modified.toSecsSinceEpoch() >
                localInfo.modified.toSecsSinceEpoch() + toleranceSeconds;
        const bool sizeDiff = localInfo.size != remoteInfo.size;

        if (options.direction == SyncDirection::LocalToRemote) {
            if (sizeDiff || (options.compareTime && localNewer))
                makeUpload(sizeDiff ? QStringLiteral("大小不同") : QStringLiteral("本地文件较新"));
        } else if (options.direction == SyncDirection::RemoteToLocal) {
            if (sizeDiff || (options.compareTime && remoteNewer))
                makeDownload(sizeDiff ? QStringLiteral("大小不同")
                                      : QStringLiteral("远程文件较新"));
        } else if (localNewer && !remoteNewer) {
            makeUpload(QStringLiteral("本地文件较新"));
        } else if (remoteNewer && !localNewer) {
            makeDownload(QStringLiteral("远程文件较新"));
        } else if (sizeDiff) {
            makeUpload(QStringLiteral("大小不同（双向同步默认上传）"));
        }
    }

    std::sort(result.items.begin(), result.items.end(),
              [](const SyncItem &left, const SyncItem &right) {
                  if (left.relativePath != right.relativePath)
                      return QString::localeAwareCompare(left.relativePath, right.relativePath) < 0;
                  return static_cast<int>(left.action) < static_cast<int>(right.action);
              });
    return result;
}

} // namespace wsc

