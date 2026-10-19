#include "core/Logger.h"

#include "core/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace wsc {

QString logLevelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return QStringLiteral("DEBUG");
    case LogLevel::Info:
        return QStringLiteral("INFO");
    case LogLevel::Success:
        return QStringLiteral("OK");
    case LogLevel::Warning:
        return QStringLiteral("WARN");
    case LogLevel::Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("INFO");
}

QString logLevelColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return QStringLiteral("#8b95a5");
    case LogLevel::Info:
        return QStringLiteral("#c8d0dc");
    case LogLevel::Success:
        return QStringLiteral("#56c596");
    case LogLevel::Warning:
        return QStringLiteral("#e8b34b");
    case LogLevel::Error:
        return QStringLiteral("#f06a6a");
    }
    return QStringLiteral("#c8d0dc");
}

Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::Logger(QObject *parent)
    : QObject(parent)
{
    openLogFile();
}

void Logger::openLogFile()
{
    AppPaths::ensureDirectories();
    const QString date = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd"));
    m_logFilePath = QDir(AppPaths::logsDir()).filePath(QStringLiteral("winscpqt-%1.log").arg(date));
    rotateIfNeeded();
}

void Logger::rotateIfNeeded()
{
    QFileInfo info(m_logFilePath);
    if (info.exists() && info.size() > 8 * 1024 * 1024) {
        const QString backup = m_logFilePath + QStringLiteral(".1");
        QFile::remove(backup);
        QFile::rename(m_logFilePath, backup);
    }
}

void Logger::debug(const QString &message)
{
    log(LogLevel::Debug, message);
}

void Logger::info(const QString &message)
{
    log(LogLevel::Info, message);
}

void Logger::success(const QString &message)
{
    log(LogLevel::Success, message);
}

void Logger::warning(const QString &message)
{
    log(LogLevel::Warning, message);
}

void Logger::error(const QString &message)
{
    log(LogLevel::Error, message);
}

void Logger::log(LogLevel level, const QString &message)
{
    if (static_cast<int>(level) < static_cast<int>(m_minimumLevel))
        return;
    const QDateTime now = QDateTime::currentDateTime();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_logFilePath.isEmpty())
            openLogFile();
        QFile file(m_logFilePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&file);
            stream.setEncoding(QStringConverter::Utf8);
            stream << now.toString(Qt::ISODateWithMs) << " [" << logLevelName(level) << "] "
                   << message << '\n';
        }
    }

    QMetaObject::invokeMethod(
        this,
        [this, level, message, now] {
            emit messageLogged(static_cast<int>(level), message, now);
        },
        Qt::QueuedConnection);
}

QString Logger::logFilePath() const
{
    return m_logFilePath;
}

} // namespace wsc
