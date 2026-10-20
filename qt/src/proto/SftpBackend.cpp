#include "proto/SftpBackend.h"

#include "core/KnownHosts.h"
#include "core/Logger.h"
#include "core/PathUtils.h"
#include "ssh/SshConnection.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QScopeGuard>
#include <QUuid>
#include <QTimeZone>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <memory>

namespace wsc {

namespace {

bool isDirectoryMode(quint32 mode)
{
    return (mode & 0170000) == 0040000;
}

bool isSymlinkMode(quint32 mode)
{
    return (mode & 0170000) == 0120000;
}

QByteArray remoteBytes(const QString &path)
{
    return path.toUtf8();
}

QDateTime dateFromSeconds(long seconds)
{
    if (seconds <= 0)
        return {};
    return QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::systemTimeZone());
}

} // namespace

SftpBackend::SftpBackend() = default;

SftpBackend::~SftpBackend()
{
    disconnect();
}

bool SftpBackend::isConnected() const
{
    return m_ssh && m_ssh->isConnected() && m_sftp != nullptr;
}

bool SftpBackend::connect(const SessionData &session, QString *error)
{
    disconnect();
    m_ssh = std::make_unique<SshConnection>();
    m_ssh->setTimeoutSeconds(session.timeoutSeconds);
    if (m_hostKeyPrompt) {
        m_ssh->setHostKeyVerifier([prompt = m_hostKeyPrompt](const HostKeyInfo &info, QString *error) {
            KnownHostKey existing;
            const HostKeyStatus status = KnownHosts::instance().verify(
                info.host, info.port, info.algorithm, info.fingerprint, &existing, error);
            if (status == HostKeyStatus::Match)
                return true;
            if (status == HostKeyStatus::Mismatch) {
                if (error && error->isEmpty()) {
                    *error = QStringLiteral("主机密钥不匹配：%1:%2").arg(info.host).arg(info.port);
                }
                return false;
            }
            QString promptError;
            const bool accepted =
                prompt(info.host, info.port, info.algorithm, info.fingerprint, &promptError);
            if (!accepted) {
                if (error)
                    *error = promptError.isEmpty() ? QStringLiteral("用户取消了主机密钥验证。")
                                                   : promptError;
                return false;
            }
            return KnownHosts::instance().addOrUpdate(info.host, info.port, info.algorithm,
                                                      info.fingerprint, error);
        });
    }

    if (!m_ssh->connectToHost(session.hostName, session.port, error))
        return false;
    if (!authenticate(session, error)) {
        m_ssh->disconnect();
        return false;
    }
    if (!openSftp(error)) {
        m_ssh->disconnect();
        return false;
    }
    m_ssh->enableKeepAlive(session.keepAliveSeconds);

    char buffer[4096] = {};
    const QByteArray dot = QByteArrayLiteral(".");
    if (libssh2_sftp_realpath(static_cast<LIBSSH2_SFTP *>(m_sftp), dot.constData(), buffer,
                              sizeof(buffer)) > 0) {
        m_home = path::normalizeRemote(QString::fromUtf8(buffer));
    } else {
        m_home = QStringLiteral("/");
    }
    Logger::instance().success(QStringLiteral("SFTP 会话已建立：%1（主目录 %2）")
                                   .arg(session.endpoint(), m_home));
    return true;
}

bool SftpBackend::authenticate(const SessionData &session, QString *error)
{
    QString keyError;
    if (!session.privateKeyFile.trimmed().isEmpty()) {
        if (m_ssh->authenticateKeyFile(session.userName, session.privateKeyFile,
                                       session.passphrase, &keyError)) {
            return true;
        }
        Logger::instance().warning(QStringLiteral("私钥认证失败：%1").arg(keyError));
        if (session.password.isEmpty()) {
            if (error)
                *error = keyError;
            return false;
        }
    }

    if (!session.password.isEmpty()) {
        QString passwordError;
        if (m_ssh->authenticatePassword(session.userName, session.password, &passwordError))
            return true;
        QString interactiveError;
        if (m_ssh->authenticateKeyboardInteractive(session.userName, session.password,
                                                   &interactiveError)) {
            return true;
        }
        if (error) {
            *error = passwordError;
            if (!interactiveError.isEmpty() && interactiveError != passwordError)
                *error += QStringLiteral("\n") + interactiveError;
        }
        return false;
    }

    if (m_ssh->authenticateAgent(session.userName, error))
        return true;
    if (error && error->isEmpty())
        *error = QStringLiteral("未提供密码或私钥。");
    return false;
}

bool SftpBackend::openSftp(QString *error)
{
    m_sftp = libssh2_sftp_init(m_ssh->session());
    if (!m_sftp) {
        if (error)
            *error = sftpError(QStringLiteral("无法启动 SFTP 子系统。"));
        return false;
    }
    return true;
}

QString SftpBackend::sftpError(const QString &fallback) const
{
    if (!m_sftp)
        return fallback;
    const unsigned long code = libssh2_sftp_last_error(static_cast<LIBSSH2_SFTP *>(m_sftp));
    QString text;
    switch (code) {
    case LIBSSH2_FX_NO_SUCH_FILE:
        text = QStringLiteral("没有那个文件或目录。");
        break;
    case LIBSSH2_FX_PERMISSION_DENIED:
        text = QStringLiteral("权限不足。");
        break;
    case LIBSSH2_FX_NO_CONNECTION:
    case LIBSSH2_FX_CONNECTION_LOST:
        text = QStringLiteral("SFTP 连接已断开。");
        break;
    case LIBSSH2_FX_OP_UNSUPPORTED:
        text = QStringLiteral("服务器不支持此操作。");
        break;
    case LIBSSH2_FX_DIR_NOT_EMPTY:
        text = QStringLiteral("目录不为空。");
        break;
    case LIBSSH2_FX_NOT_A_DIRECTORY:
        text = QStringLiteral("不是目录。");
        break;
    case LIBSSH2_FX_FILE_ALREADY_EXISTS:
        text = QStringLiteral("文件已存在。");
        break;
    case LIBSSH2_FX_WRITE_PROTECT:
        text = QStringLiteral("文件系统为只读。");
        break;
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
        text = QStringLiteral("磁盘空间不足。");
        break;
    case LIBSSH2_FX_QUOTA_EXCEEDED:
        text = QStringLiteral("超出磁盘配额。");
        break;
    case LIBSSH2_FX_LINK_LOOP:
        text = QStringLiteral("符号链接循环。");
        break;
    case LIBSSH2_FX_INVALID_FILENAME:
        text = QStringLiteral("文件名无效。");
        break;
    default:
        break;
    }
    if (text.isEmpty()) {
        text = fallback;
        if (code != 0)
            text += QStringLiteral("（SFTP 状态 %1）").arg(code);
    }
    return text;
}

QString SftpBackend::homePath()
{
    return m_home;
}

void SftpBackend::applyAttributes(const QString &directory, const QString &name,
                                  const LIBSSH2_SFTP_ATTRIBUTES &attributes, FileInfo *info)
{
    info->name = name;
    info->path = path::remoteJoin(directory, name);
    info->isHidden = name.startsWith(QLatin1Char('.'));
    if (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)
        info->size = static_cast<qint64>(attributes.filesize);
    if (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
        info->mode = attributes.permissions;
        info->isDir = isDirectoryMode(attributes.permissions);
        info->isSymlink = isSymlinkMode(attributes.permissions);
        info->isExecutable = (attributes.permissions & 0111) != 0;
    }
    if (attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
        info->accessed = dateFromSeconds(attributes.atime);
        info->modified = dateFromSeconds(attributes.mtime);
    }
    if (attributes.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        info->owner = QString::number(attributes.uid);
        info->group = QString::number(attributes.gid);
    }
}

bool SftpBackend::list(const QString &pathValue, FileList *out, QString *error)
{
    if (!out)
        return false;
    out->clear();
    if (!isConnected()) {
        if (error)
            *error = QStringLiteral("SFTP 会话未连接。");
        return false;
    }
    const QString directory = path::normalizeRemote(pathValue);
    const QByteArray encoded = remoteBytes(directory);
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(
        static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
        static_cast<unsigned int>(encoded.size()), 0, 0, LIBSSH2_SFTP_OPENDIR);
    if (!handle) {
        if (error)
            *error = sftpError(QStringLiteral("无法打开目录 %1。").arg(directory));
        return false;
    }

    char name[1024] = {};
    char longEntry[2048] = {};
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    bool ok = true;
    while (true) {
        const int result = libssh2_sftp_readdir_ex(handle, name, sizeof(name), longEntry,
                                                   sizeof(longEntry), &attributes);
        if (result == 0)
            break;
        if (result < 0) {
            if (error)
                *error = sftpError(QStringLiteral("读取目录 %1 失败。").arg(directory));
            ok = false;
            break;
        }
        const QString entryName = QString::fromUtf8(name, result);
        if (entryName == QLatin1String(".") || entryName == QLatin1String("..") ||
            entryName.isEmpty())
            continue;
        FileInfo info;
        applyAttributes(directory, entryName, attributes, &info);
        if (info.isSymlink) {
            QByteArray linkPath = info.path.toUtf8();
            char target[4096] = {};
            const int linkLength = libssh2_sftp_readlink(static_cast<LIBSSH2_SFTP *>(m_sftp),
                                                         linkPath.constData(), target,
                                                         sizeof(target));
            if (linkLength > 0)
                info.linkTarget = QString::fromUtf8(target, linkLength);
            // A symlink to a directory should be navigable as a directory, but
            // keep its link appearance in the UI.
            LIBSSH2_SFTP_ATTRIBUTES targetAttributes{};
            if (libssh2_sftp_stat_ex(static_cast<LIBSSH2_SFTP *>(m_sftp), linkPath.constData(),
                                     static_cast<unsigned int>(linkPath.size()), LIBSSH2_SFTP_STAT,
                                     &targetAttributes) == 0) {
                info.isDir = isDirectoryMode(targetAttributes.permissions);
                info.size = static_cast<qint64>(targetAttributes.filesize);
            }
        }
        out->append(info);
    }
    libssh2_sftp_closedir(handle);
    if (error && ok)
        error->clear();
    return ok;
}

bool SftpBackend::stat(const QString &pathValue, FileInfo *out, QString *error)
{
    if (!out)
        return false;
    if (!isConnected()) {
        if (error)
            *error = QStringLiteral("SFTP 会话未连接。");
        return false;
    }
    const QString normalized = path::normalizeRemote(pathValue);
    const QByteArray encoded = remoteBytes(normalized);
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    if (libssh2_sftp_stat_ex(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
                             static_cast<unsigned int>(encoded.size()), LIBSSH2_SFTP_STAT,
                             &attributes) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法获取 %1 的信息。").arg(normalized));
        return false;
    }
    FileInfo info;
    const int slash = normalized.lastIndexOf(QLatin1Char('/'));
    const QString parent = slash <= 0 ? QStringLiteral("/") : normalized.left(slash);
    applyAttributes(parent, path::remoteName(normalized), attributes, &info);
    info.path = normalized;
    *out = info;
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::mkdir(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    const QByteArray encoded = remoteBytes(normalized);
    if (libssh2_sftp_mkdir(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(), 0755) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法创建目录 %1。").arg(normalized));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::removeFile(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    const QByteArray encoded = remoteBytes(normalized);
    if (libssh2_sftp_unlink(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData()) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法删除文件 %1。").arg(normalized));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::removeDir(const QString &pathValue, QString *error)
{
    return removeDirRecursive(path::normalizeRemote(pathValue), 0, error);
}

bool SftpBackend::removeDirRecursive(const QString &pathValue, int depth, QString *error)
{
    if (depth > 128) {
        if (error)
            *error = QStringLiteral("目录层级过深，已中止删除。");
        return false;
    }
    const QString normalized = path::normalizeRemote(pathValue);
    const QByteArray encoded = remoteBytes(normalized);
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    if (libssh2_sftp_lstat(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
                           &attributes) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法访问 %1。").arg(normalized));
        return false;
    }
    if (isSymlinkMode(attributes.permissions)) {
        if (libssh2_sftp_unlink(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData()) != 0) {
            if (error)
                *error = sftpError(QStringLiteral("无法删除符号链接 %1。").arg(normalized));
            return false;
        }
        return true;
    }
    if (!isDirectoryMode(attributes.permissions)) {
        if (libssh2_sftp_unlink(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData()) != 0) {
            if (error)
                *error = sftpError(QStringLiteral("无法删除文件 %1。").arg(normalized));
            return false;
        }
        return true;
    }

    FileList entries;
    if (!list(normalized, &entries, error))
        return false;
    for (const FileInfo &entry : entries) {
        const bool ok = entry.isDir && !entry.isSymlink
            ? removeDirRecursive(entry.path, depth + 1, error)
            : removeFile(entry.path, error);
        if (!ok)
            return false;
    }
    if (libssh2_sftp_rmdir(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData()) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法删除目录 %1。").arg(normalized));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::rename(const QString &from, const QString &to, QString *error)
{
    const QString source = path::normalizeRemote(from);
    const QString target = path::normalizeRemote(to);
    const QByteArray sourceBytes = remoteBytes(source);
    const QByteArray targetBytes = remoteBytes(target);
    int result = libssh2_sftp_rename_ex(
        static_cast<LIBSSH2_SFTP *>(m_sftp), sourceBytes.constData(),
        static_cast<unsigned int>(sourceBytes.size()), targetBytes.constData(),
        static_cast<unsigned int>(targetBytes.size()),
        LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE);
    if (result != 0) {
        result = libssh2_sftp_rename_ex(
            static_cast<LIBSSH2_SFTP *>(m_sftp), sourceBytes.constData(),
            static_cast<unsigned int>(sourceBytes.size()), targetBytes.constData(),
            static_cast<unsigned int>(targetBytes.size()), 0);
    }
    if (result != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法重命名 %1 为 %2。").arg(source, target));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::chmod(const QString &pathValue, quint32 mode, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    const QByteArray encoded = remoteBytes(normalized);
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    attributes.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    attributes.permissions = mode;
    if (libssh2_sftp_stat_ex(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
                             static_cast<unsigned int>(encoded.size()), LIBSSH2_SFTP_SETSTAT,
                             &attributes) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法修改 %1 的权限。").arg(normalized));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::createSymlink(const QString &target, const QString &linkPath, QString *error)
{
    const QString link = path::normalizeRemote(linkPath);
    const QByteArray linkBytes = remoteBytes(link);
    const QByteArray targetBytes = target.toUtf8();
    if (libssh2_sftp_symlink_ex(static_cast<LIBSSH2_SFTP *>(m_sftp), targetBytes.constData(),
                                static_cast<unsigned int>(targetBytes.size()),
                                const_cast<char *>(linkBytes.constData()),
                                static_cast<unsigned int>(linkBytes.size()),
                                LIBSSH2_SFTP_SYMLINK) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法创建符号链接 %1。").arg(link));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::readFile(const QString &pathValue, QByteArray *data, QString *error)
{
    if (!data)
        return false;
    const QString normalized = path::normalizeRemote(pathValue);
    const QByteArray encoded = remoteBytes(normalized);
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(
        static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
        static_cast<unsigned int>(encoded.size()), LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!handle) {
        if (error)
            *error = sftpError(QStringLiteral("无法打开文件 %1。").arg(normalized));
        return false;
    }
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    qint64 size = -1;
    if (libssh2_sftp_fstat_ex(handle, &attributes, 0) == 0 &&
        (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
        size = static_cast<qint64>(attributes.filesize);
    }
    constexpr qint64 kMaxEditor = MaxEditorBytes;
    if (size > kMaxEditor) {
        libssh2_sftp_close(handle);
        if (error)
            *error = QStringLiteral("文件过大（%1），无法在编辑器中打开。").arg(path::formatSize(size));
        return false;
    }
    data->clear();
    data->reserve(static_cast<int>(size > 0 ? size : 0));
    char buffer[64 * 1024];
    bool ok = true;
    while (true) {
        const ssize_t read = libssh2_sftp_read(handle, buffer, sizeof(buffer));
        if (read == 0)
            break;
        if (read < 0) {
            if (error)
                *error = sftpError(QStringLiteral("读取文件 %1 失败。").arg(normalized));
            ok = false;
            break;
        }
        data->append(buffer, static_cast<int>(read));
        if (data->size() > kMaxEditor) {
            if (error)
                *error = QStringLiteral("文件超过编辑器限制。");
            ok = false;
            break;
        }
    }
    libssh2_sftp_close(handle);
    if (error && ok)
        error->clear();
    return ok;
}

bool SftpBackend::writeFile(const QString &pathValue, const QByteArray &data, QString *error)
{
    QTemporaryFile input;
    if (!input.open() || input.write(data) != data.size() || !input.flush()) {
        if (error)
            *error = QStringLiteral("无法创建待上传的临时文件：%1").arg(input.errorString());
        return false;
    }
    input.close();
    return uploadFile(input.fileName(), pathValue, {}, nullptr, error);
}

bool SftpBackend::downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                               const ProgressCallback &progress, CancelToken *cancel,
                               QString *error)
{
    const QString remote = path::normalizeRemote(remotePath);
    const QByteArray encoded = remoteBytes(remote);
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(
        static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
        static_cast<unsigned int>(encoded.size()), LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!handle) {
        if (error)
            *error = sftpError(QStringLiteral("无法读取 %1。").arg(remote));
        return false;
    }

    QFileInfo localInfo(localPath);
    QDir().mkpath(localInfo.absolutePath());
    QSaveFile output(localPath);
    if (!output.open(QIODevice::WriteOnly)) {
        libssh2_sftp_close(handle);
        if (error)
            *error = QStringLiteral("无法写入本地文件 %1：%2")
                         .arg(QDir::toNativeSeparators(localPath), output.errorString());
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    if (libssh2_sftp_fstat_ex(handle, &attributes, 0) == 0 &&
        (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) && totalHint < 0)
        totalHint = static_cast<qint64>(attributes.filesize);

    qint64 done = 0;
    bool cancelled = false;
    char buffer[128 * 1024];
    while (true) {
        if (cancel && cancel->load()) {
            cancelled = true;
            break;
        }
        const ssize_t read = libssh2_sftp_read(handle, buffer, sizeof(buffer));
        if (read == 0)
            break;
        if (read < 0) {
            if (error)
                *error = sftpError(QStringLiteral("读取 %1 失败。").arg(remote));
            output.cancelWriting();
            libssh2_sftp_close(handle);
            return false;
        }
        if (output.write(buffer, read) != read) {
            if (error)
                *error = QStringLiteral("写入本地文件失败：%1").arg(output.errorString());
            output.cancelWriting();
            libssh2_sftp_close(handle);
            return false;
        }
        done += read;
        if (progress)
            progress(TransferProgress{done, totalHint, path::remoteName(remote)});
    }
    libssh2_sftp_close(handle);
    if (cancelled) {
        output.cancelWriting();
        if (error)
            *error = QStringLiteral("传输已取消。");
        return false;
    }
    if (!output.commit()) {
        if (error)
            *error = QStringLiteral("无法完成本地文件写入：%1").arg(output.errorString());
        return false;
    }

    if ((attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) && attributes.mtime > 0) {
        QFile local(localPath);
        if (local.open(QIODevice::ReadWrite))
            local.setFileTime(dateFromSeconds(attributes.mtime), QFileDevice::FileModificationTime);
    }
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::uploadFile(const QString &localPath, const QString &remotePath,
                             const ProgressCallback &progress, CancelToken *cancel, QString *error)
{
    QFile input(localPath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取本地文件 %1：%2")
                         .arg(QDir::toNativeSeparators(localPath), input.errorString());
        return false;
    }
    QString remote = path::normalizeRemote(remotePath);
    const QByteArray originalBytes = remote.toUtf8();
    char resolved[4096];
    const int resolvedSize = libssh2_sftp_realpath(static_cast<LIBSSH2_SFTP *>(m_sftp),
                                                  originalBytes.constData(), resolved, sizeof(resolved));
    if (resolvedSize > 0)
        remote = QString::fromUtf8(resolved, resolvedSize);
    else {
        LIBSSH2_SFTP_ATTRIBUTES link{};
        if (libssh2_sftp_lstat(static_cast<LIBSSH2_SFTP *>(m_sftp), originalBytes.constData(), &link) == 0 &&
            isSymlinkMode(link.permissions)) {
            if (error)
                *error = QStringLiteral("无法解析上传目标的符号链接。");
            return false;
        }
    }
    FileInfo existing;
    const bool exists = stat(remote, &existing, nullptr);
    const QString temporary = path::remoteJoin(path::remoteParent(remote),
        QStringLiteral(".winscp-upload-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QByteArray encoded = temporary.toUtf8();
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(
        static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
        static_cast<unsigned int>(encoded.size()),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
        exists ? existing.mode & 0777 : 0644, LIBSSH2_SFTP_OPENFILE);
    if (!handle) {
        if (error)
            *error = sftpError(QStringLiteral("无法创建临时上传文件。"));
        return false;
    }
    auto cleanup = qScopeGuard([&] {
        libssh2_sftp_unlink(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData());
    });

    const qint64 total = input.size();
    qint64 done = 0;
    bool ok = true;
    QByteArray buffer;
    buffer.resize(128 * 1024);
    while (!input.atEnd()) {
        if (cancel && cancel->load()) {
            if (error)
                *error = QStringLiteral("传输已取消。");
            ok = false;
            break;
        }
        const qint64 read = input.read(buffer.data(), buffer.size());
        if (read < 0) {
            if (error)
                *error = input.errorString();
            ok = false;
            break;
        }
        if (read == 0)
            break;
        qint64 offset = 0;
        while (offset < read) {
            const ssize_t written = libssh2_sftp_write(handle, buffer.constData() + offset,
                                                       static_cast<size_t>(read - offset));
            if (written <= 0) {
                if (error)
                    *error = sftpError(QStringLiteral("上传 %1 失败。").arg(remote));
                ok = false;
                break;
            }
            offset += written;
        }
        if (!ok)
            break;
        done += read;
        if (progress)
            progress(TransferProgress{done, total, QFileInfo(localPath).fileName()});
    }
    if (libssh2_sftp_close(handle) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法完成远程文件写入。"));
        ok = false;
    }
    if (!ok || (cancel && cancel->load()))
        return false;
    if (exists && !chmod(temporary, existing.mode & 0777, error))
        return false;
    const QByteArray target = remote.toUtf8();
    if (libssh2_sftp_posix_rename_ex(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
                                    encoded.size(), target.constData(), target.size()) != 0 &&
        !rename(temporary, remote, error))
        return false;
    cleanup.dismiss();
    if (error)
        error->clear();
    return true;
}

bool SftpBackend::setModifiedTime(const QString &pathValue, const QDateTime &time, QString *error)
{
    if (!time.isValid())
        return true;
    const QString normalized = path::normalizeRemote(pathValue);
    const QByteArray encoded = remoteBytes(normalized);
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    attributes.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
    attributes.atime = static_cast<unsigned long>(time.toSecsSinceEpoch());
    attributes.mtime = static_cast<unsigned long>(time.toSecsSinceEpoch());
    if (libssh2_sftp_stat_ex(static_cast<LIBSSH2_SFTP *>(m_sftp), encoded.constData(),
                             static_cast<unsigned int>(encoded.size()), LIBSSH2_SFTP_SETSTAT,
                             &attributes) != 0) {
        if (error)
            *error = sftpError(QStringLiteral("无法设置 %1 的修改时间。").arg(normalized));
        return false;
    }
    return true;
}

void SftpBackend::keepAlive()
{
    if (m_ssh)
        m_ssh->sendKeepAlive();
}

void SftpBackend::disconnect()
{
    if (m_sftp) {
        libssh2_sftp_shutdown(static_cast<LIBSSH2_SFTP *>(m_sftp));
        m_sftp = nullptr;
    }
    if (m_ssh) {
        m_ssh->disconnect();
        m_ssh.reset();
    }
}

} // namespace wsc
