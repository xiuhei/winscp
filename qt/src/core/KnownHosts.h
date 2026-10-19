// SPDX-License-Identifier: MIT
#pragma once

#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QString>

namespace wsc {

struct KnownHostKey {
    QString host;
    int port = 22;
    QString algorithm;
    QString fingerprint; // SHA256:...
    QDateTime added;
    QString keyBase64() const { return fingerprint; }
};

enum class HostKeyStatus {
    Match,
    Unknown,
    Mismatch,
    Error,
};

class KnownHosts {
public:
    static KnownHosts &instance();

    HostKeyStatus verify(const QString &host, int port, const QString &algorithm,
                         const QString &fingerprint, KnownHostKey *known = nullptr,
                         QString *error = nullptr);
    bool addOrUpdate(const QString &host, int port, const QString &algorithm,
                     const QString &fingerprint, QString *error = nullptr);
    bool remove(const QString &host, int port);
    bool contains(const QString &host, int port) const;
    QList<KnownHostKey> entries() const;
    void reload();
    void save() const;

    static QString keyFor(const QString &host, int port);

private:
    KnownHosts();
    void loadLocked();
    void saveLocked() const;

    mutable QMutex m_mutex;
    QList<KnownHostKey> m_entries;
    bool m_loaded = false;
};

} // namespace wsc
