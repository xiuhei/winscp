// SPDX-License-Identifier: MIT
#pragma once

#include "core/Types.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace wsc {

namespace path {

/// Normalizes a POSIX style remote path. Always returns an absolute path with
/// a leading '/', collapses duplicate separators and resolves "." and "..".
QString normalizeRemote(const QString &path);

/// Appends a child to a remote directory. If child is absolute it wins.
QString remoteJoin(const QString &directory, const QString &child);

/// Parent directory of a remote path. Root's parent is root.
QString remoteParent(const QString &path);

QString remoteName(const QString &path);
bool remoteIsRoot(const QString &path);
bool remoteIsAbsolute(const QString &path);

/// Resolves a session's initial directory ("~", relative, absolute) against
/// the server side home directory.
QString resolveRemoteInitial(const QString &home, const QString &initial);

/// Relative path from base to target (both must be normalized).
QString remoteRelative(const QString &base, const QString &target);

QString normalizeLocal(const QString &path);
QString localJoin(const QString &directory, const QString &child);
QString localParent(const QString &path);
QString localName(const QString &path);
QString localFromUrl(const QUrl &url);
bool isTextFileName(const QString &name);

/// Makes a path unique by appending " (2)", " (3)", ... to the file name.
QString uniqueLocalPath(const QString &path);

QString formatSize(qint64 bytes);
QString formatSpeed(double bytesPerSecond);
QString formatDuration(qint64 seconds);
QString formatDateTime(const QDateTime &dateTime);
QString formatCount(qint64 count);

QString safeRemoteFileName(const QString &name);
bool isSafeLocalTarget(const QString &root, const QString &target);
bool wildcardMatch(const QString &pattern, const QString &text);

} // namespace path

struct LocalDriveInfo {
    QString path;
    QString displayName;
    qint64 totalBytes = 0;
    qint64 freeBytes = 0;
};

QList<LocalDriveInfo> localDrives();

} // namespace wsc
