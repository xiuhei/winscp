#include "core/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace wsc {

namespace {
QString g_applicationRoot;

QString envHome()
{
    return qEnvironmentVariable("WINSCPQT_HOME").trimmed();
}

bool fileExists(const QString &path)
{
    return QFileInfo::exists(path);
}
} // namespace

void AppPaths::setApplicationRoot(const QString &root)
{
    g_applicationRoot = QDir::cleanPath(root);
}

bool AppPaths::isPortable()
{
    if (!envHome().isEmpty())
        return true;
    const QString exeDir = g_applicationRoot.isEmpty() ? QCoreApplication::applicationDirPath()
                                                       : g_applicationRoot;
    return fileExists(QDir(exeDir).filePath(QStringLiteral("portable.ini")));
}

QString AppPaths::configDir()
{
    const QString home = envHome();
    if (!home.isEmpty())
        return QDir::cleanPath(home);
    if (isPortable()) {
        const QString exeDir = g_applicationRoot.isEmpty() ? QCoreApplication::applicationDirPath()
                                                           : g_applicationRoot;
        return QDir(exeDir).filePath(QStringLiteral("data"));
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = QDir(QDir::homePath()).filePath(QStringLiteral(".winscpqt"));
    return dir;
}

QString AppPaths::dataDir()
{
    const QString home = envHome();
    if (!home.isEmpty())
        return QDir::cleanPath(home);
    if (isPortable()) {
        const QString exeDir = g_applicationRoot.isEmpty() ? QCoreApplication::applicationDirPath()
                                                           : g_applicationRoot;
        return QDir(exeDir).filePath(QStringLiteral("data"));
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir(QDir::homePath()).filePath(QStringLiteral(".winscpqt"));
    return dir;
}

QString AppPaths::logsDir()
{
    return QDir(dataDir()).filePath(QStringLiteral("logs"));
}

QString AppPaths::sessionsFile()
{
    return QDir(configDir()).filePath(QStringLiteral("sessions.json"));
}

QString AppPaths::sitesFile()
{
    return QDir(configDir()).filePath(QStringLiteral("sites.json"));
}

QString AppPaths::knownHostsFile()
{
    return QDir(configDir()).filePath(QStringLiteral("known_hosts.json"));
}

QString AppPaths::settingsFile()
{
    return QDir(configDir()).filePath(QStringLiteral("settings.ini"));
}

QString AppPaths::cacheDir()
{
    return QDir(dataDir()).filePath(QStringLiteral("cache"));
}

QString AppPaths::tempDir()
{
    QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                      .filePath(QStringLiteral("WinSCPQt"));
    return dir;
}

QString AppPaths::themeFile(const QString &name)
{
    return QStringLiteral(":/themes/%1.qss").arg(name);
}

void AppPaths::ensureDirectories()
{
    QDir().mkpath(configDir());
    QDir().mkpath(dataDir());
    QDir().mkpath(logsDir());
    QDir().mkpath(cacheDir());
    QDir().mkpath(tempDir());
}

} // namespace wsc
