#include "core/Types.h"

#include <QFileInfo>

namespace wsc {

QString protocolId(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Local:
        return QStringLiteral("local");
    case Protocol::Sftp:
        return QStringLiteral("sftp");
    case Protocol::Scp:
        return QStringLiteral("scp");
    case Protocol::Ftp:
        return QStringLiteral("ftp");
    case Protocol::Ftps:
        return QStringLiteral("ftps");
    case Protocol::WebDav:
        return QStringLiteral("webdav");
    case Protocol::S3:
        return QStringLiteral("s3");
    }
    return QStringLiteral("sftp");
}

QString protocolDisplayName(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Local:
        return QStringLiteral("本地");
    case Protocol::Sftp:
        return QStringLiteral("SFTP");
    case Protocol::Scp:
        return QStringLiteral("SCP");
    case Protocol::Ftp:
        return QStringLiteral("FTP");
    case Protocol::Ftps:
        return QStringLiteral("FTPS (TLS/SSL)");
    case Protocol::WebDav:
        return QStringLiteral("WebDAV");
    case Protocol::S3:
        return QStringLiteral("S3");
    }
    return QStringLiteral("SFTP");
}

Protocol protocolFromId(const QString &id, bool *ok)
{
    const QString value = id.trimmed().toLower();
    if (ok)
        *ok = true;
    if (value == QLatin1String("local"))
        return Protocol::Local;
    if (value == QLatin1String("sftp") || value == QLatin1String("ssh"))
        return Protocol::Sftp;
    if (value == QLatin1String("scp"))
        return Protocol::Scp;
    if (value == QLatin1String("ftp"))
        return Protocol::Ftp;
    if (value == QLatin1String("ftps") || value == QLatin1String("ftp-tls"))
        return Protocol::Ftps;
    if (value == QLatin1String("webdav") || value == QLatin1String("dav") ||
        value == QLatin1String("https") || value == QLatin1String("http"))
        return Protocol::WebDav;
    if (value == QLatin1String("s3"))
        return Protocol::S3;
    if (ok)
        *ok = false;
    return Protocol::Sftp;
}

int defaultPort(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Local:
        return 0;
    case Protocol::Sftp:
    case Protocol::Scp:
        return 22;
    case Protocol::Ftp:
    case Protocol::Ftps:
        return 21;
    case Protocol::WebDav:
        return 80;
    case Protocol::S3:
        return 443;
    }
    return 22;
}

bool protocolUsesSsh(Protocol protocol)
{
    return protocol == Protocol::Sftp || protocol == Protocol::Scp;
}

bool protocolUsesTls(Protocol protocol)
{
    return protocol == Protocol::Ftps || protocol == Protocol::WebDav ||
           protocol == Protocol::S3;
}

QString FileInfo::permissionsString() const
{
    if (mode == 0 && !isDir && !isSymlink)
        return QString();

    QString result;
    result.reserve(10);
    if (isSymlink && (mode & 0170000) == 0120000)
        result.append(QLatin1Char('l'));
    else
        result.append(isDir ? QLatin1Char('d') : QLatin1Char('-'));

    static const char *bits[] = {"---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"};
    for (int shift = 6; shift >= 0; shift -= 3) {
        const quint32 triple = (mode >> shift) & 7;
        result.append(QLatin1String(bits[triple]));
    }
    if ((mode & 04000) && result.size() > 3)
        result[3] = QLatin1Char('s');
    if ((mode & 02000) && result.size() > 6)
        result[6] = QLatin1Char('s');
    return result;
}

QString FileInfo::typeDescription() const
{
    if (isSymlink)
        return QStringLiteral("符号链接");
    if (isDir)
        return QStringLiteral("文件夹");
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (suffix.isEmpty())
        return QStringLiteral("文件");
    return suffix.toUpper() + QStringLiteral(" 文件");
}

QString FileInfo::iconKey() const
{
    if (isSymlink)
        return QStringLiteral("link");
    if (isDir)
        return QStringLiteral("folder");
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (isExecutable)
        return QStringLiteral("executable");
    if (QStringList{"png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "ico"}.contains(suffix))
        return QStringLiteral("image");
    if (QStringList{"zip", "7z", "rar", "gz", "tgz", "bz2", "xz", "tar"}.contains(suffix))
        return QStringLiteral("archive");
    if (QStringList{"mp3", "wav", "flac", "ogg", "mp4", "mkv", "avi", "mov"}.contains(suffix))
        return QStringLiteral("media");
    if (QStringList{"c", "cc", "cpp", "h", "hpp", "cs", "py", "js", "ts", "html", "css",
                    "json", "xml", "yml", "yaml", "sh", "ps1", "php", "java", "rs", "go"}
            .contains(suffix))
        return QStringLiteral("code");
    if (QStringList{"txt", "md", "log", "ini", "conf", "cfg"}.contains(suffix))
        return QStringLiteral("text");
    return QStringLiteral("file");
}

} // namespace wsc
