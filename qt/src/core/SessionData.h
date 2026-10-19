// SPDX-License-Identifier: MIT
#pragma once

#include "core/Types.h"

#include <QJsonObject>
#include <QMetaType>
#include <QStringList>

namespace wsc {

/// Everything needed to open a session. The struct is intentionally
/// serializable so it can be used for sites, recent sessions and command-line
/// parsing.
struct SessionData {
    QString siteName;
    Protocol protocol = Protocol::Sftp;
    QString hostName;
    int port = 22;
    QString userName;
    QString password;
    QString privateKeyFile;
    QString passphrase;
    QString remoteDirectory;
    QString localDirectory;
    QString note;
    QString color; // optional accent color for a site

    bool savePassword = true;
    bool passiveMode = true;
    bool implicitTls = false;
    bool tlsVerifyCertificate = true;
    bool webDavHttps = false;
    bool compression = true;
    bool agentForwarding = false;
    int timeoutSeconds = 20;
    int keepAliveSeconds = 30;
    int maxConnections = 2;

    /// Additional raw settings, kept for WinSCP compatible site imports.
    QStringList rawSettings;

    bool isValid() const;
    bool isLocal() const { return protocol == Protocol::Local; }
    QString displayName() const;
    QString endpoint() const;          // user@host:port
    QString rootLabel() const;         // "user@host" or "本地"
    QString url(bool includePassword = false) const;
    QJsonObject toJson() const;

    static SessionData fromJson(const QJsonObject &object);
    static SessionData localSession();
    static bool parseUrl(const QString &url, SessionData *session, QString *error = nullptr);

    bool operator==(const SessionData &other) const;
};

/// Parse a WinSCP-style session URL (sftp://user:pass@host:22/path).
QString sessionUrlEncode(const QString &value);

} // namespace wsc

Q_DECLARE_METATYPE(wsc::SessionData)
