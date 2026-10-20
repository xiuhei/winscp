#include "proto/LocalBackend.h"

#include "core/Logger.h"
#include "core/PathUtils.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <memory>

namespace wsc {

namespace {

QString canonical(const QString &path)
{
    if (path.isEmpty())
        return QDir::homePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

quint32 modeFor(const QFileInfo &info)
{
    quint32 mode = 0;
    if (info.isDir())
        mode |= 0040000;
    else if (info.isSymLink())
        mode |= 0120000;
    else
        mode |= 0100000;
    const QFile::Permissions permissions = info.permissions();
    if (permissions.testFlag(QFileDevice::ReadOwner))
        mode |= 0400;
    if (permissions.testFlag(QFileDevice::WriteOwner))
        mode |= 0200;
    if (permissions.testFlag(QFileDevice::ExeOwner))
        mode |= 0100;
    if (permissions.testFlag(QFileDevice::ReadGroup))
        mode |= 0040;
    if (permissions.testFlag(QFileDevice::WriteGroup))
        mode |= 0020;
    if (permissions.testFlag(QFileDevice::ExeGroup))
        mode |= 0010;
    if (permissions.testFlag(QFileDevice::ReadOther))
        mode |= 0004;
    if (permissions.testFlag(QFileDevice::WriteOther))
        mode |= 0002;
    if (permissions.testFlag(QFileDevice::ExeOther))
        mode |= 0001;
    if (mode & 0777u)
        return mode;
    return info.isDir() ? 040755u : 0100644u;
}

bool isExecutableSuffix(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == QLatin1String("exe") || suffix == QLatin1String("com") ||
           suffix == QLatin1String("bat") || suffix == QLatin1String("cmd") ||
           suffix == QLatin1String("ps1") || suffix == QLatin1String("sh") ||
           suffix == QLatin1String("py");
}

} // namespace

bool LocalBackend::connect(const SessionData &, QString *error)
{
    m_connected = true;
    if (error)
        error->clear();
    return true;
}

QString LocalBackend::homePath()
{
    return QDir::homePath();
}

FileInfo LocalBackend::infoForPath(const QString &path, bool includeHidden)
{
    QFileInfo info(path);
    FileInfo result;
    if (!info.exists() && !info.isSymLink())
        return result;
    result.name = info.fileName();
    if (result.name.isEmpty())
        result.name = QDir::toNativeSeparators(info.absoluteFilePath());
    result.path = QDir::cleanPath(info.absoluteFilePath());
    result.size = info.isDir() ? 0 : info.size();
    result.modified = info.lastModified();
    result.accessed = info.lastRead();
    result.mode = modeFor(info);
    result.isDir = info.isDir();
    result.isSymlink = info.isSymLink();
    result.isHidden = info.isHidden() && includeHidden;
    result.isExecutable = isExecutableSuffix(result.name);
    result.owner = info.owner();
    result.group = info.group();
    if (result.isSymlink)
        result.linkTarget = info.symLinkTarget();
    return result;
}

bool LocalBackend::list(const QString &path, FileList *out, QString *error)
{
    if (!out)
        return false;
    out->clear();
    const QString directory = canonical(path);
    QDir dir(directory);
    if (!dir.exists()) {
        if (error)
            *error = QStringLiteral("目录不存在：%1").arg(QDir::toNativeSeparators(directory));
        return false;
    }
    if (!dir.isReadable()) {
        if (error)
            *error = QStringLiteral("没有权限读取目录：%1").arg(QDir::toNativeSeparators(directory));
        return false;
    }
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    out->reserve(entries.size());
    for (const QFileInfo &entry : entries) {
        FileInfo info = infoForPath(entry.absoluteFilePath());
        if (!info.isValid())
            continue;
        out->append(info);
    }
    if (error)
        error->clear();
    return true;
}

bool LocalBackend::stat(const QString &path, FileInfo *out, QString *error)
{
    if (!out)
        return false;
    const FileInfo info = infoForPath(canonical(path));
    if (!info.isValid()) {
        if (error)
            *error = QStringLiteral("路径不存在：%1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    *out = info;
    if (error)
        error->clear();
    return true;
}

bool LocalBackend::mkdir(const QString &path, QString *error)
{
    QDir dir;
    if (dir.mkpath(canonical(path))) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = QStringLiteral("无法创建文件夹：%1").arg(QDir::toNativeSeparators(path));
    return false;
}

bool LocalBackend::removeFile(const QString &path, QString *error)
{
    QFile file(canonical(path));
    if (file.remove()) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = QStringLiteral("无法删除文件：%1\n%2")
                     .arg(QDir::toNativeSeparators(path), file.errorString());
    return false;
}

bool LocalBackend::removeDir(const QString &path, QString *error)
{
    QDir dir(canonical(path));
    if (!dir.exists()) {
        if (error)
            *error = QStringLiteral("目录不存在：%1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    if (dir.removeRecursively()) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = QStringLiteral("无法删除文件夹：%1").arg(QDir::toNativeSeparators(path));
    return false;
}

bool LocalBackend::rename(const QString &from, const QString &to, QString *error)
{
    const QString source = canonical(from);
    const QString target = canonical(to);
    const QFileInfo sourceInfo(source);
    const QFileInfo targetInfo(target);
    if ((targetInfo.exists() || targetInfo.isSymLink()) && sourceInfo != targetInfo) {
        if (error)
            *error = QStringLiteral("目标已存在：%1").arg(QDir::toNativeSeparators(target));
        return false;
    }
    if ((source == target && (sourceInfo.exists() || sourceInfo.isSymLink())) ||
        QFile::rename(source, target)) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = QStringLiteral("无法重命名 %1 为 %2")
                     .arg(QDir::toNativeSeparators(from), QDir::toNativeSeparators(to));
    return false;
}

bool LocalBackend::chmod(const QString &path, quint32 mode, QString *error)
{
    QFile file(canonical(path));
    QFile::Permissions permissions;
    if (mode & 0400)
        permissions |= QFileDevice::ReadOwner;
    if (mode & 0200)
        permissions |= QFileDevice::WriteOwner;
    if (mode & 0100)
        permissions |= QFileDevice::ExeOwner;
    if (mode & 0040)
        permissions |= QFileDevice::ReadGroup;
    if (mode & 0020)
        permissions |= QFileDevice::WriteGroup;
    if (mode & 0010)
        permissions |= QFileDevice::ExeGroup;
    if (mode & 0004)
        permissions |= QFileDevice::ReadOther;
    if (mode & 0002)
        permissions |= QFileDevice::WriteOther;
    if (mode & 0001)
        permissions |= QFileDevice::ExeOther;
    if (!permissions)
        permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (file.setPermissions(permissions)) {
        if (error)
            error->clear();
        return true;
    }
    if (error)
        *error = QStringLiteral("无法修改权限：%1").arg(file.errorString());
    return false;
}

bool LocalBackend::readFile(const QString &path, QByteArray *data, QString *error)
{
    QFile file(canonical(path));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取文件：%1\n%2")
                         .arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    constexpr qint64 kMaxEditor = 32 * 1024 * 1024;
    if (file.size() > kMaxEditor) {
        if (error)
            *error = QStringLiteral("文件过大（%1），无法在编辑器中打开。")
                         .arg(path::formatSize(file.size()));
        return false;
    }
    *data = file.readAll();
    if (error)
        error->clear();
    return true;
}

bool LocalBackend::writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QSaveFile file(canonical(path));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入文件：%1\n%2")
                         .arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("写入文件失败：%1\n%2")
                         .arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool LocalBackend::downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                                const ProgressCallback &progress, CancelToken *cancel,
                                QString *error)
{
    Q_UNUSED(totalHint);
    return uploadFile(remotePath, localPath, progress, cancel, error);
}

bool LocalBackend::uploadFile(const QString &localPath, const QString &remotePath,
                              const ProgressCallback &progress, CancelToken *cancel,
                              QString *error)
{
    QFile source(canonical(localPath));
    if (!source.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取源文件：%1\n%2")
                         .arg(QDir::toNativeSeparators(localPath), source.errorString());
        return false;
    }
    const QString targetPath = canonical(remotePath);
    QDir().mkpath(QFileInfo(targetPath).absolutePath());
    QSaveFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入目标文件：%1\n%2")
                         .arg(QDir::toNativeSeparators(targetPath), target.errorString());
        return false;
    }
    const qint64 total = source.size();
    qint64 done = 0;
    QByteArray buffer;
    buffer.resize(256 * 1024);
    while (!source.atEnd()) {
        if (cancel && cancel->load())
            break;
        const qint64 read = source.read(buffer.data(), buffer.size());
        if (read < 0) {
            if (error)
                *error = QStringLiteral("读取源文件失败：%1").arg(source.errorString());
            target.cancelWriting();
            return false;
        }
        if (read == 0)
            break;
        if (target.write(buffer.constData(), read) != read) {
            if (error)
                *error = QStringLiteral("写入目标文件失败：%1").arg(target.errorString());
            target.cancelWriting();
            return false;
        }
        done += read;
        if (progress)
            progress(TransferProgress{done, total, QFileInfo(localPath).fileName()});
    }
    if (cancel && cancel->load()) {
        target.cancelWriting();
        if (error)
            *error = QStringLiteral("传输已取消。");
        return false;
    }
    if (!target.commit()) {
        if (error)
            *error = QStringLiteral("写入目标文件失败：%1").arg(target.errorString());
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool LocalBackend::setModifiedTime(const QString &path, const QDateTime &time, QString *error)
{
    QFile file(canonical(path));
    if (!file.open(QIODevice::ReadWrite)) {
        if (error)
            *error = QStringLiteral("无法设置修改时间：%1").arg(file.errorString());
        return false;
    }
    const bool ok = file.setFileTime(time, QFileDevice::FileModificationTime);
    if (!ok && error)
        *error = QStringLiteral("无法设置修改时间：%1").arg(file.errorString());
    return ok;
}

} // namespace wsc
