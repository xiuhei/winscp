// SPDX-License-Identifier: MIT
#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

#include <mutex>

namespace wsc {

enum class LogLevel {
    Debug = 0,
    Info,
    Success,
    Warning,
    Error,
};

QString logLevelName(LogLevel level);
QString logLevelColor(LogLevel level);

/// Thread-safe application log. Messages are appended to a rotating text file
/// and broadcast to the UI through a queued signal.
class Logger : public QObject {
    Q_OBJECT
public:
    static Logger &instance();

    void debug(const QString &message);
    void info(const QString &message);
    void success(const QString &message);
    void warning(const QString &message);
    void error(const QString &message);
    void log(LogLevel level, const QString &message);

    QString logFilePath() const;
    void setMinimumLevel(LogLevel level) { m_minimumLevel = level; }

signals:
    void messageLogged(int level, const QString &message, const QDateTime &timestamp);

private:
    explicit Logger(QObject *parent = nullptr);
    void openLogFile();
    void rotateIfNeeded();

    mutable std::mutex m_mutex;
    QString m_logFilePath;
    LogLevel m_minimumLevel = LogLevel::Debug;
};

} // namespace wsc
