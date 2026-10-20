// SPDX-License-Identifier: MIT
#pragma once

#include "core/SessionData.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace wsc {

QJsonObject sessionToStoredJson(const SessionData &session, bool savePasswords);
SessionData sessionFromStoredJson(const QJsonObject &object);

/// Application preferences, persisted as an INI file.
class Settings {
public:
    static Settings &instance();

    void load();
    void save();

    QString theme = QStringLiteral("light");
    bool showHiddenFiles = false;
    bool confirmDelete = true;
    bool confirmOverwrite = true;
    QString conflictPolicy = QStringLiteral("ask"); // ask | overwrite | skip | resume
    int parallelTransfers = 2;
    int cacheRetentionDays = 7;
    bool autoRefresh = false;
    bool showLoginDialog = true;
    bool preserveTimestamps = true;
    bool preservePermissions = true;
    bool showTransferQueue = true;
    bool showLogPanel = false;
    bool savePasswords = true;
    bool askUnknownHostKey = true;
    bool confirmExit = false;
    int connectionTimeout = 20;
    int keepAlive = 30;

    QString terminalFontFamily = QStringLiteral("Cascadia Mono");
    int terminalFontSize = 10;
    int terminalScrollback = 5000;
    QString terminalShell; // empty -> server default

    QString editorCommand;
    QString lastLocalDirectory;
    QString lastSessionUrl;
    QString lastSessionName;

    QByteArray windowGeometry;
    QByteArray windowState;
    QByteArray commanderSplitterState;
    QByteArray bottomSplitterState;

    QList<SessionData> recentSessions;

    void addRecentSession(const SessionData &session);
    void removeRecentSession(const SessionData &session);
    void clearRecentSessions();
    void rememberDirectories(const SessionData &session, const QString &local, const QString &remote);
    SessionData restoreDirectories(const SessionData &session) const;

private:
    Settings() = default;
    void loadRecent();
    void saveRecent();
};

/// Saved sites (site manager). Stored separately from recent sessions.
class SiteStore {
public:
    static SiteStore &instance();

    QList<SessionData> sites;

    void load();
    void save() const;
    int indexOf(const QString &name) const;
    void addOrUpdate(const SessionData &session);
    bool remove(const QString &name);
    SessionData find(const QString &name) const;

private:
    SiteStore() = default;
};

QString defaultLocalDirectory();

} // namespace wsc
