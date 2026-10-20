#include "core/FileSystemController.h"

#include "core/Backend.h"
#include "core/Logger.h"
#include "core/PathUtils.h"
#include "terminal/SshShellSession.h"

#include <QMetaObject>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <functional>

namespace wsc {

class FileSystemWorker : public QObject {
    Q_OBJECT
public:
    explicit FileSystemWorker(QObject *parent = nullptr)
        : QObject(parent)
        , m_keepAlive(new QTimer(this))
    {
        m_keepAlive->setInterval(30000);
        connect(m_keepAlive, &QTimer::timeout, this, [this] {
            if (m_backend && m_backend->isConnected())
                m_backend->keepAlive();
        });
    }

    void setHostKeyPrompt(HostKeyPrompt prompt) { m_prompt = std::move(prompt); }

    void connectToHost(const SessionData &session)
    {
        stopBackend();
        m_session = session;
        if (session.protocol == Protocol::Local) {
            emit connectFailed(QStringLiteral("无法为本地会话创建远程连接。"));
            return;
        }
        m_backend = createBackend(session.protocol);
        if (!m_backend) {
            emit connectFailed(QStringLiteral("暂不支持协议：%1")
                                   .arg(protocolDisplayName(session.protocol)));
            return;
        }
        m_backend->setHostKeyPrompt(m_prompt);
        QString error;
        if (!m_backend->connect(session, &error)) {
            m_backend.reset();
            emit connectFailed(error.isEmpty() ? QStringLiteral("连接失败。") : error);
            return;
        }
        m_home = path::normalizeRemote(m_backend->homePath());
        m_currentPath = session.remoteDirectory.trimmed().isEmpty()
            ? m_home
            : path::resolveRemoteInitial(m_home, session.remoteDirectory);
        if (session.keepAliveSeconds > 0) {
            m_keepAlive->setInterval(std::max(5, session.keepAliveSeconds) * 1000);
            m_keepAlive->start();
        }
        emit connected(session, m_home, m_currentPath);
        if (!doList(m_currentPath) && m_currentPath != m_home)
            doList(m_home);
    }

    void disconnectFromHost()
    {
        m_keepAlive->stop();
        stopBackend();
        emit disconnected(QStringLiteral("会话已断开。"));
    }

    void shutdown()
    {
        m_keepAlive->stop();
        stopBackend();
    }

    void startShell(SshShellSession *shell)
    {
        shell->setParent(this);
        shell->start(m_backend ? m_backend->sshConnection() : nullptr);
    }

    void listDirectory(const QString &path)
    {
        if (!m_backend || !m_backend->isConnected()) {
            emit directoryFailed(path, QStringLiteral("当前没有活动的远程连接。"));
            return;
        }
        doList(path);
    }

    void createDirectory(const QString &path)
    {
        if (!ensureBackend())
            return;
        QString error;
        const bool ok = m_backend->mkdir(path, &error);
        emit operationFinished(QStringLiteral("新建文件夹"), ok,
                               ok ? QStringLiteral("已创建文件夹：%1").arg(path) : error);
        if (ok)
            doList(m_currentPath);
    }

    void deleteEntries(const QList<FileInfo> &entries)
    {
        if (!ensureBackend())
            return;
        int failed = 0;
        QStringList errors;
        for (const FileInfo &entry : entries) {
            QString error;
            const bool ok = entry.isDir && !entry.isSymlink
                ? m_backend->removeDir(entry.path, &error)
                : m_backend->removeFile(entry.path, &error);
            if (!ok) {
                ++failed;
                errors.append(QStringLiteral("%1：%2").arg(entry.name, error));
            }
        }
        const bool ok = failed == 0;
        QString message;
        if (ok) {
            message = QStringLiteral("已删除 %1 项。").arg(entries.size());
        } else {
            message = QStringLiteral("%1 项删除成功，%2 项失败。\n%3")
                          .arg(entries.size() - failed)
                          .arg(failed)
                          .arg(errors.join(QLatin1Char('\n')));
        }
        emit operationFinished(QStringLiteral("删除"), ok, message);
        if (failed < entries.size())
            doList(m_currentPath);
    }

    void renameEntry(const QString &from, const QString &to)
    {
        if (!ensureBackend())
            return;
        QString error;
        const bool ok = m_backend->rename(from, to, &error);
        emit operationFinished(QStringLiteral("重命名"), ok,
                               ok ? QStringLiteral("已重命名为：%1").arg(path::remoteName(to)) : error);
        if (ok)
            doList(m_currentPath);
    }

    void setPermissions(const QString &path, quint32 mode)
    {
        if (!ensureBackend())
            return;
        QString error;
        const bool ok = m_backend->chmod(path, mode, &error);
        emit operationFinished(QStringLiteral("修改权限"), ok,
                               ok ? QStringLiteral("权限已更新：%1").arg(path) : error);
        if (ok)
            doList(m_currentPath);
    }

    void statFile(const QString &path, quint64 requestId)
    {
        FileInfo info;
        QString error;
        if (!m_backend || !m_backend->isConnected())
            error = QStringLiteral("连接已断开。");
        else if (!m_backend->stat(path, &info, &error) && error.isEmpty())
            error = QStringLiteral("无法读取文件属性。");
        emit fileStated(requestId, m_session, path, info, error);
    }

    void readFile(const QString &path)
    {
        if (!ensureBackend())
            return;
        QByteArray data;
        QString error;
        if (!m_backend->readFile(path, &data, &error)) {
            emit operationFinished(QStringLiteral("读取文件"), false, error);
            return;
        }
        emit fileRead(path, data);
    }

    void writeFile(const QString &path, const QByteArray &data)
    {
        if (!ensureBackend())
            return;
        QString error;
        const bool ok = m_backend->writeFile(path, data, &error);
        emit fileWriteFinished(path, ok,
                               ok ? QStringLiteral("已保存：%1").arg(path) : error);
        if (ok)
            doList(m_currentPath);
    }

    void createSymlink(const QString &target, const QString &linkPath)
    {
        if (!ensureBackend())
            return;
        QString error;
        const bool ok = m_backend->createSymlink(target, linkPath, &error);
        emit symlinkCreated(linkPath, ok,
                            ok ? QStringLiteral("已创建符号链接：%1").arg(linkPath) : error);
        if (ok)
            doList(m_currentPath);
    }

signals:
    void connected(const SessionData &session, const QString &home, const QString &initialPath);
    void connectFailed(const QString &message);
    void disconnected(const QString &reason);
    void directoryLoading(const QString &path);
    void directoryListed(const QString &path, const QList<FileInfo> &entries);
    void directoryFailed(const QString &path, const QString &message);
    void operationFinished(const QString &action, bool success, const QString &message);
    void fileRead(const QString &path, const QByteArray &data);
    void fileStated(quint64 requestId, const wsc::SessionData &session, const QString &path, const wsc::FileInfo &info, const QString &error);
    void fileWriteFinished(const QString &path, bool success, const QString &message);
    void symlinkCreated(const QString &linkPath, bool success, const QString &message);

private:
    bool ensureBackend()
    {
        if (m_backend && m_backend->isConnected())
            return true;
        emit operationFinished(QStringLiteral("操作"), false,
                               QStringLiteral("当前没有活动的远程连接。"));
        return false;
    }

    void stopBackend()
    {
        for (auto *shell : findChildren<SshShellSession *>(QString(), Qt::FindDirectChildrenOnly))
            shell->stop();
        if (m_backend) {
            m_backend->disconnect();
            m_backend.reset();
        }
    }

    bool doList(const QString &pathValue)
    {
        if (!m_backend || !m_backend->isConnected()) {
            emit directoryFailed(pathValue, QStringLiteral("当前没有活动的远程连接。"));
            return false;
        }
        const QString path = path::normalizeRemote(pathValue);
        emit directoryLoading(path);
        FileList entries;
        QString error;
        if (!m_backend->list(path, &entries, &error)) {
            emit directoryFailed(path, error);
            return false;
        }
        m_currentPath = path;
        std::sort(entries.begin(), entries.end(), [](const FileInfo &left, const FileInfo &right) {
            if (left.isDir != right.isDir)
                return left.isDir;
            return QString::localeAwareCompare(left.name, right.name) < 0;
        });
        emit directoryListed(path, entries);
        return true;
    }

    BackendPtr m_backend;
    SessionData m_session;
    HostKeyPrompt m_prompt;
    QTimer *m_keepAlive = nullptr;
    QString m_home = QStringLiteral("/");
    QString m_currentPath = QStringLiteral("/");
};

FileSystemController::FileSystemController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<FileInfo>("wsc::FileInfo");
    qRegisterMetaType<FileList>("wsc::FileList");
    qRegisterMetaType<SessionData>("wsc::SessionData");
    m_worker = new FileSystemWorker();
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);    connect(m_worker, &FileSystemWorker::connected, this, &FileSystemController::onConnected,
            Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::connectFailed, this,
            &FileSystemController::onConnectFailed, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::disconnected, this,
            &FileSystemController::onDisconnected, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::directoryLoading, this,
            &FileSystemController::onDirectoryLoading, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::directoryListed, this,
            &FileSystemController::onDirectoryListed, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::directoryFailed, this,
            &FileSystemController::onDirectoryFailed, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::operationFinished, this,
            &FileSystemController::onOperationFinished, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::fileRead, this, &FileSystemController::onFileRead,
            Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::fileWriteFinished, this,
            &FileSystemController::onFileWriteFinished, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::symlinkCreated, this,
            &FileSystemController::onSymlinkCreated, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::fileStated, this, &FileSystemController::fileStated,
            Qt::QueuedConnection);
    m_thread.setObjectName(QStringLiteral("RemoteFileSystem"));
    m_thread.start();
}

FileSystemController::~FileSystemController()
{
    shutdown();
    m_thread.quit();
    m_thread.wait();
}

SshShellSession *FileSystemController::createShell(int columns, int rows, const QString &shell)
{
    auto *session = new SshShellSession(columns, rows, shell);
    session->moveToThread(&m_thread);
    return session;
}

void FileSystemController::startShell(SshShellSession *shell)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, shell] { worker->startShell(shell); },
                              Qt::QueuedConnection);
}

void FileSystemController::setHostKeyPrompt(HostKeyPrompt prompt)
{
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, prompt = std::move(prompt)]() { worker->setHostKeyPrompt(prompt); },
        Qt::QueuedConnection);
}

void FileSystemController::connectToHost(const SessionData &session)
{
    if (m_connecting)
        return;
    m_connecting = true;
    m_connected = false;
    m_busy = true;
    emit connecting(session);
    emit busyChanged(true);
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, session]() { worker->connectToHost(session); },
        Qt::QueuedConnection);
}

void FileSystemController::disconnectFromHost()
{
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker]() { worker->disconnectFromHost(); },
                              Qt::QueuedConnection);
}

void FileSystemController::shutdown()
{
    if (!m_worker || !m_thread.isRunning())
        return;
    QEventLoop loop;
    // A connection can be awaiting a host-key decision on the UI thread.
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, &loop] {
        worker->shutdown();
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

void FileSystemController::listDirectory(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path]() { worker->listDirectory(path); },
                              Qt::QueuedConnection);
}

void FileSystemController::refresh()
{
    if (!m_currentPath.isEmpty())
        listDirectory(m_currentPath);
}

void FileSystemController::createDirectory(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path]() { worker->createDirectory(path); },
                              Qt::QueuedConnection);
}

void FileSystemController::deleteEntries(const QList<FileInfo> &entries)
{
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, entries]() { worker->deleteEntries(entries); },
        Qt::QueuedConnection);
}

void FileSystemController::renameEntry(const QString &from, const QString &to)
{
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, from, to]() { worker->renameEntry(from, to); },
        Qt::QueuedConnection);
}

void FileSystemController::setPermissions(const QString &path, quint32 mode)
{
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, path, mode]() { worker->setPermissions(path, mode); },
        Qt::QueuedConnection);
}

void FileSystemController::statFile(const QString &path, quint64 requestId)
{
    QMetaObject::invokeMethod(m_worker, [this, path, requestId] { m_worker->statFile(path, requestId); }, Qt::QueuedConnection);
}

void FileSystemController::readFile(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path]() { worker->readFile(path); },
                              Qt::QueuedConnection);
}

void FileSystemController::writeFile(const QString &path, const QByteArray &data)
{
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, path, data]() { worker->writeFile(path, data); },
        Qt::QueuedConnection);
}

void FileSystemController::createSymlink(const QString &target, const QString &linkPath)
{
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, target, linkPath]() { worker->createSymlink(target, linkPath); },
        Qt::QueuedConnection);
}

void FileSystemController::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged(busy);
}

void FileSystemController::onConnected(const SessionData &session, const QString &home,
                                       const QString &initialPath)
{
    m_session = session;
    m_home = home;
    m_currentPath = initialPath;
    m_connected = true;
    m_connecting = false;
    setBusy(false);
    emit connected(session, home, initialPath);
}

void FileSystemController::onConnectFailed(const QString &message)
{
    m_connected = false;
    m_connecting = false;
    setBusy(false);
    emit connectFailed(message);
}

void FileSystemController::onDisconnected(const QString &reason)
{
    m_connected = false;
    m_connecting = false;
    m_currentPath.clear();
    setBusy(false);
    emit disconnected(reason);
}

void FileSystemController::onDirectoryLoading(const QString &path)
{
    emit directoryLoading(path);
}

void FileSystemController::onDirectoryListed(const QString &path, const QList<FileInfo> &entries)
{
    m_currentPath = path;
    emit directoryListed(path, entries);
}

void FileSystemController::onDirectoryFailed(const QString &path, const QString &message)
{
    emit directoryFailed(path, message);
}

void FileSystemController::onOperationFinished(const QString &action, bool success,
                                               const QString &message)
{
    emit operationFinished(action, success, message);
}

void FileSystemController::onFileRead(const QString &path, const QByteArray &data)
{
    emit fileRead(path, data);
}

void FileSystemController::onFileWriteFinished(const QString &path, bool success,
                                               const QString &message)
{
    emit fileWriteFinished(path, success, message);
}

void FileSystemController::onSymlinkCreated(const QString &linkPath, bool success,
                                            const QString &message)
{
    emit symlinkCreated(linkPath, success, message);
}

} // namespace wsc

#include "FileSystemController.moc"
