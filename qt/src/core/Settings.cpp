#include "core/Settings.h"

#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/PasswordStore.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace wsc {

QJsonObject sessionToStoredJson(const SessionData &session, bool savePasswords)
{
    SessionData copy = session;
    if (!savePasswords || !copy.savePassword) {
        copy.password.clear();
        copy.passphrase.clear();
    }
    if (!copy.password.isEmpty())
        copy.password = PasswordStore::protect(copy.password);
    if (!copy.passphrase.isEmpty())
        copy.passphrase = PasswordStore::protect(copy.passphrase);
    return copy.toJson();
}

SessionData sessionFromStoredJson(const QJsonObject &object)
{
    SessionData session = SessionData::fromJson(object);
    session.password = PasswordStore::unprotect(session.password);
    session.passphrase = PasswordStore::unprotect(session.passphrase);
    return session;
}

QString defaultLocalDirectory()
{
    QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (home.isEmpty())
        home = QDir::homePath();
    return QDir::cleanPath(home);
}

Settings &Settings::instance()
{
    static Settings settings;
    return settings;
}

void Settings::load()
{
    QSettings store(AppPaths::settingsFile(), QSettings::IniFormat);
    cacheRetentionDays = qBound(1, store.value(QStringLiteral("transfer/cacheRetentionDays"), 7).toInt(), 3650);
    autoRefresh = store.value(QStringLiteral("ui/autoRefresh"), false).toBool();
    showLoginDialog = store.value(QStringLiteral("ui/showLoginDialog"), true).toBool();
    theme = store.value(QStringLiteral("ui/theme"), theme).toString();
    showHiddenFiles = store.value(QStringLiteral("ui/showHidden"), showHiddenFiles).toBool();
    confirmDelete = store.value(QStringLiteral("ui/confirmDelete"), confirmDelete).toBool();
    confirmOverwrite = store.value(QStringLiteral("ui/confirmOverwrite"), confirmOverwrite).toBool();
    conflictPolicy = store.value(QStringLiteral("transfer/conflict"), conflictPolicy).toString();
    parallelTransfers = store.value(QStringLiteral("transfer/parallel"), parallelTransfers).toInt();
    preserveTimestamps =
        store.value(QStringLiteral("transfer/preserveTimestamps"), preserveTimestamps).toBool();
    preservePermissions =
        store.value(QStringLiteral("transfer/preservePermissions"), preservePermissions).toBool();
    terminalFontFamily =
        store.value(QStringLiteral("terminal/fontFamily"), terminalFontFamily).toString();
    terminalFontSize = store.value(QStringLiteral("terminal/fontSize"), terminalFontSize).toInt();
    terminalScrollback =
        store.value(QStringLiteral("terminal/scrollback"), terminalScrollback).toInt();
    terminalShell = store.value(QStringLiteral("terminal/shell"), terminalShell).toString();
    savePasswords = store.value(QStringLiteral("security/savePasswords"), savePasswords).toBool();
    askUnknownHostKey =
        store.value(QStringLiteral("security/askUnknownHostKey"), askUnknownHostKey).toBool();
    confirmExit = store.value(QStringLiteral("ui/confirmExit"), confirmExit).toBool();
    editorCommand = store.value(QStringLiteral("tools/editor"), editorCommand).toString();
    lastLocalDirectory =
        store.value(QStringLiteral("session/localDirectory"), lastLocalDirectory).toString();
    if (lastLocalDirectory.isEmpty() || !QDir(lastLocalDirectory).exists())
        lastLocalDirectory = defaultLocalDirectory();
    lastSessionUrl = store.value(QStringLiteral("session/lastUrl")).toString();
    lastSessionName = store.value(QStringLiteral("session/lastName")).toString();
    windowGeometry = store.value(QStringLiteral("window/geometry")).toByteArray();
    windowState = store.value(QStringLiteral("window/state")).toByteArray();
    commanderSplitterState = store.value(QStringLiteral("window/commanderSplitter")).toByteArray();
    bottomSplitterState = store.value(QStringLiteral("window/bottomSplitter")).toByteArray();
    connectionTimeout = store.value(QStringLiteral("session/timeout"), connectionTimeout).toInt();
    keepAlive = store.value(QStringLiteral("session/keepAlive"), keepAlive).toInt();
    if (parallelTransfers < 1)
        parallelTransfers = 1;
    if (parallelTransfers > 8)
        parallelTransfers = 8;
    if (terminalScrollback < 500)
        terminalScrollback = 500;
    if (terminalFontSize < 6)
        terminalFontSize = 6;
    if (terminalFontSize > 32)
        terminalFontSize = 32;
    loadRecent();
}

void Settings::save()
{
    AppPaths::ensureDirectories();
    QSettings store(AppPaths::settingsFile(), QSettings::IniFormat);
    store.setValue(QStringLiteral("transfer/cacheRetentionDays"), cacheRetentionDays);
    store.setValue(QStringLiteral("ui/autoRefresh"), autoRefresh);
    store.setValue(QStringLiteral("ui/showLoginDialog"), showLoginDialog);
    store.remove(QStringLiteral("terminal/synchronizeDirectories"));
    store.setValue(QStringLiteral("ui/theme"), theme);
    store.setValue(QStringLiteral("ui/showHidden"), showHiddenFiles);
    store.setValue(QStringLiteral("ui/confirmDelete"), confirmDelete);
    store.setValue(QStringLiteral("ui/confirmOverwrite"), confirmOverwrite);
    store.setValue(QStringLiteral("transfer/conflict"), conflictPolicy);
    store.setValue(QStringLiteral("transfer/parallel"), parallelTransfers);
    store.setValue(QStringLiteral("transfer/preserveTimestamps"), preserveTimestamps);
    store.setValue(QStringLiteral("transfer/preservePermissions"), preservePermissions);
    store.remove(QStringLiteral("terminal/autoOpen"));
    store.setValue(QStringLiteral("terminal/fontFamily"), terminalFontFamily);
    store.setValue(QStringLiteral("terminal/fontSize"), terminalFontSize);
    store.setValue(QStringLiteral("terminal/scrollback"), terminalScrollback);
    store.setValue(QStringLiteral("terminal/shell"), terminalShell);
    store.setValue(QStringLiteral("security/savePasswords"), savePasswords);
    store.setValue(QStringLiteral("security/askUnknownHostKey"), askUnknownHostKey);
    store.setValue(QStringLiteral("ui/confirmExit"), confirmExit);
    store.setValue(QStringLiteral("tools/editor"), editorCommand);
    store.setValue(QStringLiteral("session/localDirectory"), lastLocalDirectory);
    store.setValue(QStringLiteral("session/lastUrl"), lastSessionUrl);
    store.setValue(QStringLiteral("session/lastName"), lastSessionName);
    store.setValue(QStringLiteral("window/geometry"), windowGeometry);
    store.setValue(QStringLiteral("window/state"), windowState);
    store.setValue(QStringLiteral("window/commanderSplitter"), commanderSplitterState);
    store.setValue(QStringLiteral("window/bottomSplitter"), bottomSplitterState);
    store.setValue(QStringLiteral("session/timeout"), connectionTimeout);
    store.setValue(QStringLiteral("session/keepAlive"), keepAlive);
    store.sync();
    saveRecent();
}

namespace {
QString directoryKey(const SessionData &session)
{
    const QJsonArray values{session.siteName, protocolId(session.protocol), session.hostName.toLower(),
                            session.port, session.userName, session.webDavHttps};
    return QStringLiteral("directories/") + QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(values).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
}

void Settings::rememberDirectories(const SessionData &session, const QString &local, const QString &remote)
{
    if (!session.isValid() || remote.isEmpty())
        return;
    QSettings store(AppPaths::settingsFile(), QSettings::IniFormat);
    store.beginGroup(directoryKey(session));
    store.setValue(QStringLiteral("local"), local);
    store.setValue(QStringLiteral("remote"), remote);
}

SessionData Settings::restoreDirectories(const SessionData &session) const
{
    QSettings store(AppPaths::settingsFile(), QSettings::IniFormat);
    store.beginGroup(directoryKey(session));
    SessionData result = session;
    const QString local = store.value(QStringLiteral("local")).toString();
    if (QDir(local).exists() && !local.isEmpty())
        result.localDirectory = local;
    const QString remote = store.value(QStringLiteral("remote")).toString();
    if (!remote.isEmpty())
        result.remoteDirectory = remote;
    return result;
}

void Settings::addRecentSession(const SessionData &session)
{
    if (!session.isValid() || session.protocol == Protocol::Local)
        return;
    SessionData copy = session;
    if (!savePasswords || !copy.savePassword)
        copy.password.clear();
    for (int index = recentSessions.size() - 1; index >= 0; --index) {
        const SessionData &existing = recentSessions.at(index);
        if (existing.protocol == copy.protocol && existing.hostName == copy.hostName &&
            existing.port == copy.port && existing.userName == copy.userName) {
            recentSessions.removeAt(index);
        }
    }
    recentSessions.prepend(copy);
    while (recentSessions.size() > 20)
        recentSessions.removeLast();
}

void Settings::removeRecentSession(const SessionData &session)
{
    for (int index = recentSessions.size() - 1; index >= 0; --index) {
        const SessionData &existing = recentSessions.at(index);
        if (existing.protocol == session.protocol && existing.hostName == session.hostName &&
            existing.port == session.port && existing.userName == session.userName)
            recentSessions.removeAt(index);
    }
}

void Settings::clearRecentSessions()
{
    recentSessions.clear();
}

void Settings::loadRecent()
{
    recentSessions.clear();
    QFile file(AppPaths::sessionsFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray())
        return;
    for (const QJsonValue &value : document.array()) {
        if (!value.isObject())
            continue;
        const SessionData session = sessionFromStoredJson(value.toObject());
        if (session.isValid())
            recentSessions.append(session);
    }
}

void Settings::saveRecent()
{
    AppPaths::ensureDirectories();
    QJsonArray array;
    for (const SessionData &session : std::as_const(recentSessions))
        array.append(sessionToStoredJson(session, savePasswords));
    QSaveFile file(AppPaths::sessionsFile());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    file.commit();
}

SiteStore &SiteStore::instance()
{
    static SiteStore store;
    return store;
}

void SiteStore::load()
{
    sites.clear();
    QFile file(AppPaths::sitesFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray())
        return;
    for (const QJsonValue &value : document.array()) {
        if (!value.isObject())
            continue;
        const SessionData session = sessionFromStoredJson(value.toObject());
        if (!session.siteName.isEmpty())
            sites.append(session);
    }
}

void SiteStore::save() const
{
    AppPaths::ensureDirectories();
    QJsonArray array;
    for (const SessionData &session : sites)
        array.append(sessionToStoredJson(session, true));
    QSaveFile file(AppPaths::sitesFile());
    if (!file.open(QIODevice::WriteOnly)) {
        Logger::instance().warning(QStringLiteral("无法写入站点文件：%1").arg(file.errorString()));
        return;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    if (!file.commit())
        Logger::instance().warning(QStringLiteral("无法保存站点文件：%1").arg(file.errorString()));
}

int SiteStore::indexOf(const QString &name) const
{
    for (int index = 0; index < sites.size(); ++index) {
        if (sites.at(index).siteName == name)
            return index;
    }
    return -1;
}

void SiteStore::addOrUpdate(const SessionData &session)
{
    const int index = indexOf(session.siteName);
    if (index >= 0)
        sites[index] = session;
    else
        sites.append(session);
}

bool SiteStore::remove(const QString &name)
{
    const int index = indexOf(name);
    if (index < 0)
        return false;
    sites.removeAt(index);
    return true;
}

SessionData SiteStore::find(const QString &name) const
{
    const int index = indexOf(name);
    return index >= 0 ? sites.at(index) : SessionData();
}

} // namespace wsc
