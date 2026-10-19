// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>

typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;

namespace wsc {

struct HostKeyInfo {
    QString host;
    int port = 22;
    QString algorithm;
    QString fingerprint; // SHA256:<base64 without padding>
};

/// Called after the SSH handshake. Returning false aborts the connection.
using HostKeyVerifier = std::function<bool(const HostKeyInfo &, QString *error)>;

/// A blocking libssh2 connection with a native socket. Instances are meant to
/// be used from a single worker thread at a time.
class SshConnection {
public:
    SshConnection();
    ~SshConnection();

    SshConnection(const SshConnection &) = delete;
    SshConnection &operator=(const SshConnection &) = delete;

    void setHostKeyVerifier(HostKeyVerifier verifier) { m_verifier = std::move(verifier); }
    void setTimeoutSeconds(int seconds) { m_timeoutSeconds = qMax(1, seconds); }
    int timeoutSeconds() const { return m_timeoutSeconds; }

    bool connectToHost(const QString &host, int port, QString *error);
    bool authenticatePassword(const QString &user, const QString &password, QString *error);
    bool authenticateKeyFile(const QString &user, const QString &privateKeyPath,
                             const QString &passphrase, QString *error);
    bool authenticateKeyboardInteractive(const QString &user, const QString &password,
                                         QString *error);
    bool authenticateAgent(const QString &user, QString *error);
    bool isAuthenticated() const;
    bool isConnected() const { return m_session != nullptr; }

    LIBSSH2_SESSION *session() const { return m_session; }
    HostKeyInfo hostKey() const { return m_hostKey; }
    QString serverBanner() const { return m_banner; }
    QString lastError() const;

    /// Configures keep-alive and sends one keep-alive packet if needed.
    void enableKeepAlive(int seconds);
    void sendKeepAlive();

    void disconnect();

private:
    bool verifyHostKey(QString *error);
    bool fail(const QString &message, QString *error);

    LIBSSH2_SESSION *m_session = nullptr;
    qintptr m_socket = -1;
    int m_timeoutSeconds = 20;
    HostKeyInfo m_hostKey;
    QString m_banner;
    HostKeyVerifier m_verifier;
};

} // namespace wsc
