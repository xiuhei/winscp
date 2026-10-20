#include "core/FileSystemController.h"

#include "core/Backend.h"
#include "core/Logger.h"
#include "core/PathUtils.h"
#include "terminal/SshShellSession.h"

#include <QMetaObject>
#include <QEventLoop>
#include <QTimer>
#include <QPointer>
#include <deque>

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
            if (!m_running && m_backend) {
                m_backend->keepAlive();
                if (!m_backend->isConnected()) {
                    m_keepAlive->stop();
                    stopBackend();
                    emit disconnected(m_generation, QStringLiteral("服务器连接已断开，请重新连接。"));
                }
            }
        });
    }

    void enqueue(quint64 generation, std::function<void()> operation)
    {
        m_latestGeneration = std::max(m_latestGeneration, generation);
        m_operations.emplace_back(generation, std::move(operation));
        if (m_running)
            return;
        m_running = true;
        while (!m_operations.empty()) {
            auto [id, next] = std::move(m_operations.front());
            m_operations.pop_front();
            if (id != m_latestGeneration)
                continue;
            m_generation = id;
            emit busyChanged(m_generation, true);
            next();
        }
        m_running = false;
        emit busyChanged(m_generation, false);
    }

    void setHostKeyPrompt(HostKeyPrompt prompt) { m_prompt = std::move(prompt); }

    void connectToHost(const SessionData &session)
    {
        stopBackend();
        m_session = session;
        if (session.protocol == Protocol::Local) {
            emit connectFailed(m_generation, QStringLiteral("无法为本地会话创建远程连接。"));
            return;
        }
        m_backend = createBackend(session.protocol);
        if (!m_backend) {
            emit connectFailed(m_generation, QStringLiteral("暂不支持协议：%1")
                                   .arg(protocolDisplayName(session.protocol)));
            return;
        }
        m_backend->setHostKeyPrompt(m_prompt);
        QString error;
        if (!m_backend->connect(session, &error)) {
            m_backend.reset();
            emit connectFailed(m_generation, error.isEmpty() ? QStringLiteral("连接失败。") : error);
            return;
        }
        if (m_generation != m_latestGeneration) {
            stopBackend();
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
        emit connected(m_generation, session, m_home, m_currentPath);
        if (!doList(m_currentPath) && m_currentPath != m_home)
            doList(m_home);
    }

    void disconnectFromHost()
    {
        m_keepAlive->stop();
        stopBackend();
        emit disconnected(m_generation, QStringLiteral("会话已断开。"));
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
            emit directoryFailed(m_generation, path, QStringLiteral("当前没有活动的远程连接。"));
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
        emit operationFinished(m_generation, QStringLiteral("新建文件夹"), ok,
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
        emit operationFinished(m_generation, QStringLiteral("删除"), ok, message);
        if (failed < entries.size())
            doList(m_currentPath);
    }

    void renameEntry(const QString &from, const QString &to)
    {
        if (!ensureBackend())
            return;
        QString error;
        const bool ok = m_backend->rename(from, to, &error);
        emit operationFinished(m_generation, QStringLiteral("重命名"), ok,
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
        emit operationFinished(m_generation, QStringLiteral("修改权限"), ok,
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
        emit fileStated(m_generation, requestId, m_session, path, info, error);
    }

    void readFile(const QString &path)
    {
        if (!ensureBackend())
            return;
        QByteArray data;
        QString error;
        if (!m_backend->readFile(path, &data, &error)) {
            emit operationFinished(m_generation, QStringLiteral("读取文件"), false, error);
            return;
        }
        emit fileRead(m_generation, path, data);
    }

    void writeFile(const QString &path, const QByteArray &data)
    {
        if (!ensureBackend())
            return;
        QString error;
        const bool ok = m_backend->writeFile(path, data, &error);
        emit fileWriteFinished(m_generation, path, ok,
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
        emit symlinkCreated(m_generation, linkPath, ok,
                            ok ? QStringLiteral("已创建符号链接：%1").arg(linkPath) : error);
        if (ok)
            doList(m_currentPath);
    }

signals:
    void busyChanged(quint64 generation, bool busy);
    void connected(quint64 generation, const SessionData &session, const QString &home, const QString &initialPath);
    void connectFailed(quint64 generation, const QString &message);
    void disconnected(quint64 generation, const QString &reason);
    void directoryLoading(quint64 generation, const QString &path);
    void directoryListed(quint64 generation, const QString &path, const QList<FileInfo> &entries);
    void directoryFailed(quint64 generation, const QString &path, const QString &message);
    void operationFinished(quint64 generation, const QString &action, bool success, const QString &message);
    void fileRead(quint64 generation, const QString &path, const QByteArray &data);
    void fileStated(quint64 generation, quint64 requestId, const wsc::SessionData &session, const QString &path, const wsc::FileInfo &info, const QString &error);
    void fileWriteFinished(quint64 generation, const QString &path, bool success, const QString &message);
    void symlinkCreated(quint64 generation, const QString &linkPath, bool success, const QString &message);

private:
    bool ensureBackend()
    {
        if (m_backend && m_backend->isConnected())
            return true;
        emit operationFinished(m_generation, QStringLiteral("操作"), false,
                               QStringLiteral("当前没有活动的远程连接。"));
        return false;
    }

    void stopBackend()
    {
        m_keepAlive->stop();
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
            emit directoryFailed(m_generation, pathValue, QStringLiteral("当前没有活动的远程连接。"));
            return false;
        }
        const QString path = path::normalizeRemote(pathValue);
        emit directoryLoading(m_generation, path);
        FileList entries;
        QString error;
        if (!m_backend->list(path, &entries, &error)) {
            emit directoryFailed(m_generation, path, error);
            return false;
        }
        m_currentPath = path;
        std::sort(entries.begin(), entries.end(), [](const FileInfo &left, const FileInfo &right) {
            if (left.isDir != right.isDir)
                return left.isDir;
            return QString::localeAwareCompare(left.name, right.name) < 0;
        });
        emit directoryListed(m_generation, path, entries);
        return true;
    }

    std::deque<std::pair<quint64, std::function<void()>>> m_operations;
    bool m_running = false;
    quint64 m_generation = 0;
    quint64 m_latestGeneration = 0;
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
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &FileSystemWorker::busyChanged, this,
            [this](quint64 generation, bool busy) {
                if (generation == m_generation)
                    setBusy(busy);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::connected, this,
            [this](quint64 generation, const SessionData &session, const QString &home, const QString &initialPath) {
                if (generation == m_generation)
                    onConnected(session, home, initialPath);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::connectFailed, this,
            [this](quint64 generation, const QString &message) {
                if (generation == m_generation)
                    onConnectFailed(message);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::disconnected, this,
            [this](quint64 generation, const QString &reason) {
                if (generation == m_generation)
                    onDisconnected(reason);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::directoryLoading, this,
            [this](quint64 generation, const QString &path) {
                if (generation == m_generation)
                    onDirectoryLoading(path);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::directoryListed, this,
            [this](quint64 generation, const QString &path, const QList<FileInfo> &entries) {
                if (generation == m_generation)
                    onDirectoryListed(path, entries);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::directoryFailed, this,
            [this](quint64 generation, const QString &path, const QString &message) {
                if (generation == m_generation)
                    onDirectoryFailed(path, message);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::operationFinished, this,
            [this](quint64 generation, const QString &action, bool success, const QString &message) {
                if (generation == m_generation)
                    onOperationFinished(action, success, message);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::fileRead, this,
            [this](quint64 generation, const QString &path, const QByteArray &data) {
                if (generation == m_generation)
                    onFileRead(path, data);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::fileStated, this,
            [this](quint64 generation, quint64 requestId, const wsc::SessionData &session, const QString &path, const wsc::FileInfo &info, const QString &error) {
                if (generation == m_generation)
                    fileStated(requestId, session, path, info, error);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::fileWriteFinished, this,
            [this](quint64 generation, const QString &path, bool success, const QString &message) {
                if (generation == m_generation)
                    onFileWriteFinished(path, success, message);
            }, Qt::QueuedConnection);
    connect(m_worker, &FileSystemWorker::symlinkCreated, this,
            [this](quint64 generation, const QString &linkPath, bool success, const QString &message) {
                if (generation == m_generation)
                    onSymlinkCreated(linkPath, success, message);
            }, Qt::QueuedConnection);
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
    dispatch([shell = QPointer<SshShellSession>(shell)](FileSystemWorker *worker) {
        if (shell)
            worker->startShell(shell);
    });
}

void FileSystemController::setHostKeyPrompt(HostKeyPrompt prompt)
{
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, prompt = std::move(prompt)]() { worker->setHostKeyPrompt(prompt); },
        Qt::QueuedConnection);
}

void FileSystemController::connectToHost(const SessionData &session)
{
    ++m_generation;
    m_connecting = true;
    m_connected = false;
    m_busy = true;
    emit connecting(session);
    emit busyChanged(true);
    dispatch([session](FileSystemWorker *worker) { worker->connectToHost(session); });
}

void FileSystemController::disconnectFromHost()
{
    if (!m_worker)
        return;
    ++m_generation;
    m_connected = false;
    m_connecting = false;
    dispatch([](FileSystemWorker *worker) { worker->disconnectFromHost(); });
}

void FileSystemController::shutdown()
{
    if (!m_worker || !m_thread.isRunning())
        return;
    QEventLoop loop;
    // A connection can be awaiting a host-key decision on the UI thread.
    ++m_generation;
    dispatch([&loop](FileSystemWorker *worker) {
        worker->shutdown();
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    });
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

void FileSystemController::listDirectory(const QString &path)
{
    dispatch([path](FileSystemWorker *worker) { worker->listDirectory(path); });
}

void FileSystemController::refresh()
{
    if (!m_currentPath.isEmpty())
        listDirectory(m_currentPath);
}

void FileSystemController::createDirectory(const QString &path)
{
    dispatch([path](FileSystemWorker *worker) { worker->createDirectory(path); });
}

void FileSystemController::deleteEntries(const QList<FileInfo> &entries)
{
    dispatch([entries](FileSystemWorker *worker) { worker->deleteEntries(entries); });
}

void FileSystemController::renameEntry(const QString &from, const QString &to)
{
    dispatch([from, to](FileSystemWorker *worker) { worker->renameEntry(from, to); });
}

void FileSystemController::setPermissions(const QString &path, quint32 mode)
{
    dispatch([path, mode](FileSystemWorker *worker) { worker->setPermissions(path, mode); });
}

void FileSystemController::statFile(const QString &path, quint64 requestId)
{
    dispatch([path, requestId](FileSystemWorker *worker) { worker->statFile(path, requestId); });
}

void FileSystemController::readFile(const QString &path)
{
    dispatch([path](FileSystemWorker *worker) { worker->readFile(path); });
}

void FileSystemController::writeFile(const QString &path, const QByteArray &data)
{
    dispatch([path, data](FileSystemWorker *worker) { worker->writeFile(path, data); });
}

void FileSystemController::createSymlink(const QString &target, const QString &linkPath)
{
    dispatch([target, linkPath](FileSystemWorker *worker) { worker->createSymlink(target, linkPath); });
}

void FileSystemController::dispatch(std::function<void(FileSystemWorker *)> operation)
{
    const quint64 generation = m_generation;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, generation, operation = std::move(operation)] {
        worker->enqueue(generation, [worker, operation] { operation(worker); });
    }, Qt::QueuedConnection);
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
