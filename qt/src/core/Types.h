// SPDX-License-Identifier: MIT
// WinSCP Qt - a modern, cross-platform reimplementation of WinSCP's core
// file-management workflows using C++20 and Qt Widgets.
#pragma once

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

namespace wsc {

enum class Protocol {
    Local = 0,
    Sftp,
    Scp,
    Ftp,
    Ftps,
    WebDav,
    S3,
};

QString protocolId(Protocol protocol);
QString protocolDisplayName(Protocol protocol);
Protocol protocolFromId(const QString &id, bool *ok = nullptr);
int defaultPort(Protocol protocol);
bool protocolUsesSsh(Protocol protocol);
bool protocolUsesTls(Protocol protocol);

/// A protocol independent description of one file-system entry.
struct FileInfo {
    QString name;
    QString path;
    qint64 size = 0;
    QDateTime modified;
    QDateTime accessed;
    quint32 mode = 0; // POSIX mode bits when available
    bool isDir = false;
    bool isParent = false;
    bool isSymlink = false;
    bool isHidden = false;
    bool isExecutable = false;
    QString linkTarget;
    QString owner;
    QString group;

    bool operator==(const FileInfo &) const = default;

    bool isValid() const { return !name.isEmpty(); }
    bool isFile() const { return !isDir; }

    /// Renders POSIX permissions, e.g. "drwxr-xr-x". Returns an empty string
    /// when the backend did not provide a mode.
    QString permissionsString() const;
    /// "Folder", "File", "Symbolic link"... localized type description.
    QString typeDescription() const;
    QString iconKey() const;
};

using FileList = QList<FileInfo>;

struct DriveInfo {
    QString name;
    QString path;
    QString displayName;
    qint64 totalBytes = 0;
    qint64 freeBytes = 0;
};

} // namespace wsc

Q_DECLARE_METATYPE(wsc::FileInfo)
Q_DECLARE_METATYPE(wsc::FileList)
