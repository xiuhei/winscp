#include "core/PathUtils.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QUrl>
#include <QVector>

namespace wsc::path {

namespace {

QString normalizePosix(const QString &input, bool absolute)
{
    QString value = input;
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (value.contains(QStringLiteral("//")))
        value.replace(QStringLiteral("//"), QStringLiteral("/"));

    const bool trailingSlash = value.size() > 1 && value.endsWith(QLatin1Char('/'));
    const bool wasAbsolute = value.startsWith(QLatin1Char('/'));
    const QStringList parts = value.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    QVector<QString> stack;
    for (const QString &part : parts) {
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            if (!stack.isEmpty() && stack.last() != QLatin1String(".."))
                stack.removeLast();
            else if (!absolute && !wasAbsolute)
                stack.append(part);
            continue;
        }
        stack.append(part);
    }

    QString result = stack.join(QLatin1Char('/'));
    if (absolute || wasAbsolute) {
        result.prepend(QLatin1Char('/'));
    }
    if (result.isEmpty())
        result = absolute ? QStringLiteral("/") : QString();
    if (trailingSlash && result != QLatin1String("/") && !result.endsWith(QLatin1Char('/')))
        result.append(QLatin1Char('/'));
    return result;
}

QString humanSize(qint64 bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(static_cast<qint64>(value));
    return QStringLiteral("%1 %2").arg(value, 0, 'f', value >= 100 ? 0 : (value >= 10 ? 1 : 2))
        .arg(QLatin1String(units[unit]));
}

} // namespace

QString normalizeRemote(const QString &input)
{
    QString value = input.trimmed();
    if (value.isEmpty())
        return QStringLiteral("/");
    return normalizePosix(value, true);
}

QString remoteJoin(const QString &directory, const QString &child)
{
    if (child.isEmpty())
        return normalizeRemote(directory);
    if (remoteIsAbsolute(child) || child.startsWith(QLatin1Char('\\')))
        return normalizeRemote(child);
    QString base = normalizeRemote(directory);
    if (base == QLatin1String("/"))
        return normalizeRemote(base + child);
    return normalizeRemote(base + QLatin1Char('/') + child);
}

QString remoteParent(const QString &pathValue)
{
    const QString normalized = normalizeRemote(pathValue);
    if (normalized == QLatin1String("/"))
        return normalized;
    const int index = normalized.lastIndexOf(QLatin1Char('/'));
    if (index <= 0)
        return QStringLiteral("/");
    return normalized.left(index);
}

QString remoteName(const QString &pathValue)
{
    QString normalized = normalizeRemote(pathValue);
    if (normalized == QLatin1String("/"))
        return QStringLiteral("/");
    if (normalized.endsWith(QLatin1Char('/')))
        normalized.chop(1);
    const int index = normalized.lastIndexOf(QLatin1Char('/'));
    return index < 0 ? normalized : normalized.mid(index + 1);
}

bool remoteIsRoot(const QString &pathValue)
{
    return normalizeRemote(pathValue) == QLatin1String("/");
}

bool remoteIsAbsolute(const QString &pathValue)
{
    return pathValue.startsWith(QLatin1Char('/'));
}

QString resolveRemoteInitial(const QString &home, const QString &initial)
{
    const QString normalizedHome = normalizeRemote(home);
    QString value = initial.trimmed();
    if (value.isEmpty() || value == QLatin1String("~") || value == QLatin1String("."))
        return normalizedHome;
    if (value.startsWith(QStringLiteral("~/")))
        return remoteJoin(normalizedHome, value.mid(2));
    if (value.startsWith(QLatin1Char('/')))
        return normalizeRemote(value);
    return remoteJoin(normalizedHome, value);
}

QString remoteRelative(const QString &base, const QString &target)
{
    const QString basePath = normalizeRemote(base);
    const QString targetPath = normalizeRemote(target);
    if (basePath == targetPath)
        return QStringLiteral(".");
    QString prefix = basePath;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
    if (targetPath.startsWith(prefix))
        return targetPath.mid(prefix.size());
    return targetPath;
}

QString normalizeLocal(const QString &input)
{
    if (input.isEmpty())
        return QDir::homePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(input));
}

QString localJoin(const QString &directory, const QString &child)
{
    return QDir::cleanPath(QDir(directory).filePath(child));
}

QString localParent(const QString &pathValue)
{
    QString normalized = normalizeLocal(pathValue);
    if (normalized.isEmpty())
        return QDir::rootPath();
    QDir dir(normalized);
    if (dir.cdUp())
        return dir.absolutePath();
    return normalized;
}

QString localName(const QString &pathValue)
{
    const QFileInfo info(pathValue);
    const QString name = info.fileName();
    if (!name.isEmpty())
        return name;
    return QDir::toNativeSeparators(info.absoluteFilePath());
}

QString localFromUrl(const QUrl &url)
{
    if (url.isLocalFile())
        return QDir::cleanPath(url.toLocalFile());
    return {};
}

bool isTextFileName(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    static const QSet<QString> textSuffixes = {
        QStringLiteral("txt"),  QStringLiteral("md"),   QStringLiteral("log"),  QStringLiteral("ini"),
        QStringLiteral("conf"), QStringLiteral("cfg"),  QStringLiteral("json"), QStringLiteral("xml"),
        QStringLiteral("yml"),  QStringLiteral("yaml"), QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("css"),  QStringLiteral("js"),   QStringLiteral("ts"),   QStringLiteral("cpp"),
        QStringLiteral("h"),    QStringLiteral("hpp"),  QStringLiteral("c"),    QStringLiteral("cc"),
        QStringLiteral("py"),   QStringLiteral("sh"),   QStringLiteral("ps1"),  QStringLiteral("bat"),
        QStringLiteral("cmd"),  QStringLiteral("php"),  QStringLiteral("java"), QStringLiteral("cs"),
        QStringLiteral("rs"),   QStringLiteral("go"),   QStringLiteral("sql"),  QStringLiteral("csv"),
        QStringLiteral("svg"),  QStringLiteral("gitignore"), QStringLiteral("editorconfig")};
    if (textSuffixes.contains(suffix))
        return true;
    return QFileInfo(name).fileName().startsWith(QLatin1Char('.'));
}

QString uniqueLocalPath(const QString &input)
{
    if (!QFileInfo::exists(input))
        return input;
    const QFileInfo info(input);
    const QDir dir = info.absoluteDir();
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    const bool hasSuffix = !suffix.isEmpty() && !info.isDir();
    for (int index = 2; index < 10000; ++index) {
        const QString name = hasSuffix
            ? QStringLiteral("%1 (%2).%3").arg(base).arg(index).arg(suffix)
            : QStringLiteral("%1 (%2)").arg(base).arg(index);
        const QString candidate = dir.filePath(name);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return input;
}

QString formatSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("—");
    return humanSize(bytes);
}

QString formatSpeed(double bytesPerSecond)
{
    if (bytesPerSecond < 0)
        return QStringLiteral("—");
    return humanSize(static_cast<qint64>(bytesPerSecond)) + QStringLiteral("/s");
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0)
        return QStringLiteral("—");
    if (seconds < 60)
        return QStringLiteral("%1 秒").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1 小时 %2 分").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString formatDateTime(const QDateTime &dateTime)
{
    if (!dateTime.isValid())
        return QStringLiteral("—");
    return dateTime.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString formatCount(qint64 count)
{
    return QLocale().toString(count);
}

QString safeRemoteFileName(const QString &name)
{
    QString result = name;
    result.replace(QLatin1Char('/'), QLatin1Char('_'));
    result.replace(QLatin1Char('\\'), QLatin1Char('_'));
    result.replace(QLatin1Char(':'), QLatin1Char('_'));
    result = result.trimmed();
    if (result == QLatin1String(".") || result == QLatin1String(".."))
        result = QStringLiteral("_");
    return result;
}

bool wildcardMatch(const QString &pattern, const QString &text)
{
    if (pattern.isEmpty() || pattern == QLatin1String("*"))
        return true;
    QString regex = QRegularExpression::escape(pattern);
    regex.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
    regex.replace(QStringLiteral("\\?"), QStringLiteral("."));
    const QRegularExpression re(QStringLiteral("^%1$").arg(regex),
                                QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

} // namespace wsc::path

namespace wsc {

QList<LocalDriveInfo> localDrives()
{
    QList<LocalDriveInfo> result;
    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes) {
        if (!volume.isValid() || !volume.isReady())
            continue;
        LocalDriveInfo info;
        info.path = QDir::cleanPath(volume.rootPath());
        info.displayName = volume.displayName();
        if (info.displayName.isEmpty())
            info.displayName = info.path;
        info.totalBytes = volume.bytesTotal();
        info.freeBytes = volume.bytesAvailable();
        result.append(info);
    }
    if (result.isEmpty()) {
        for (const QFileInfo &drive : QDir::drives()) {
            LocalDriveInfo info;
            info.path = QDir::cleanPath(drive.absoluteFilePath());
            info.displayName = info.path;
            result.append(info);
        }
    }
    return result;
}

} // namespace wsc
