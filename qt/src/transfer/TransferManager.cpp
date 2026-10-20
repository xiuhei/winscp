#include "transfer/TransferManager.h"

#include "core/Backend.h"
#include "core/Logger.h"
#include "core/PathUtils.h"
#include "core/Settings.h"
#include "proto/LocalBackend.h"
#include "transfer/TransferQueueModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <QtConcurrentRun>

#include <algorithm>

namespace wsc {

struct TransferManager::Task {
    int id = 0;
    TransferDirection direction = TransferDirection::Upload;
    QString name;
    QString targetRoot;
    SessionData session;
    QList<FileInfo> entries;
    qint64 totalBytes = -1;
    std::atomic<qint64> doneBytes{0};
    std::atomic_bool cancel{false};
    std::atomic_bool finished{false};
    TransferStatus status = TransferStatus::Queued;
    QString message;
    QString currentFile;
    double speed = 0;
    int skipped = 0;
};

namespace {

QString safeName(const QString &name)
{
    QString result = path::safeRemoteFileName(name);
    if (result == QLatin1String(".") || result == QLatin1String("..") || result.isEmpty())
        result = QStringLiteral("_");
    return result;
}

QString localTarget(const QString &directory, const QString &name)
{
    return QDir::cleanPath(QDir(directory).filePath(safeName(name)));
}

QDateTime localModificationTime(const QFileInfo &info)
{
    return info.lastModified();
}

quint32 localMode(const QFileInfo &info)
{
    return LocalBackend::infoForPath(info.absoluteFilePath()).mode;
}

bool ensureRemoteDirectory(Backend *backend, const QString &pathValue, QString *error)
{
    const QString path = path::normalizeRemote(pathValue);
    if (path == QLatin1String("/") || path.isEmpty())
        return true;
    FileInfo existing;
    if (backend->stat(path, &existing, nullptr) && existing.isDir)
        return true;
    const QString parent = path::remoteParent(path);
    if (parent == path)
        return false;
    if (!ensureRemoteDirectory(backend, parent, error))
        return false;
    if (backend->mkdir(path, nullptr))
        return true;
    if (backend->stat(path, &existing, nullptr) && existing.isDir)
        return true;
    if (error)
        *error = QStringLiteral("无法创建远程目录 %1。").arg(path);
    return false;
}

} // namespace

TransferManager::TransferManager(QObject *parent)
    : QObject(parent)
    , m_model(new TransferQueueModel(this))
    , m_parallelLimit(qBound(1, Settings::instance().parallelTransfers, 8))
{
}

TransferManager::~TransferManager()
{
    shutdown();
}

void TransferManager::setParallelLimit(int limit)
{
    m_parallelLimit = qBound(1, limit, 8);
    startQueued();
}

int TransferManager::pendingCount() const
{
    int count = 0;
    for (const TaskPtr &task : m_tasks) {
        if (task->status == TransferStatus::Queued)
            ++count;
    }
    return count;
}

void TransferManager::enqueueUpload(const SessionData &session, const QList<FileInfo> &localEntries,
                                    const QString &remoteDirectory)
{
    if (localEntries.isEmpty())
        return;
    auto task = std::make_shared<Task>();
    task->id = m_nextId++;
    task->direction = TransferDirection::Upload;
    task->name = localEntries.size() == 1 ? localEntries.first().name
                                          : QStringLiteral("%1 个项目").arg(localEntries.size());
    task->targetRoot = path::normalizeRemote(remoteDirectory);
    task->session = session;
    task->entries = localEntries;
    task->status = TransferStatus::Queued;
    m_tasks.append(task);
    m_byId.insert(task->id, task);

    TransferRow row = rowForTask(task);
    m_model->addRow(row);
    emit taskAdded(task->id);
    emit queueChanged();
    Logger::instance().info(QStringLiteral("已加入上传队列：%1 → %2").arg(task->name, task->targetRoot));
    startQueued();
}

int TransferManager::enqueueDownload(const SessionData &session,
                                     const QList<FileInfo> &remoteEntries,
                                     const QString &localDirectory)
{
    if (remoteEntries.isEmpty())
        return -1;
    auto task = std::make_shared<Task>();
    task->id = m_nextId++;
    task->direction = TransferDirection::Download;
    task->name = remoteEntries.size() == 1 ? remoteEntries.first().name
                                           : QStringLiteral("%1 个项目").arg(remoteEntries.size());
    task->targetRoot = QDir::cleanPath(localDirectory);
    task->session = session;
    task->entries = remoteEntries;
    task->status = TransferStatus::Queued;
    m_tasks.append(task);
    m_byId.insert(task->id, task);

    m_model->addRow(rowForTask(task));
    emit taskAdded(task->id);
    emit queueChanged();
    Logger::instance().info(QStringLiteral("已加入下载队列：%1 → %2").arg(task->name, task->targetRoot));
    startQueued();
    return task->id;
}

TransferRow TransferManager::rowForTask(const TaskPtr &task) const
{
    TransferRow row;
    row.id = task->id;
    row.name = task->name;
    row.direction = task->direction;
    row.status = task->status;
    row.bytesDone = task->doneBytes.load();
    row.bytesTotal = task->totalBytes;
    row.speed = task->speed;
    row.currentFile = task->currentFile;
    row.target = task->targetRoot;
    row.message = task->message;
    row.canCancel = task->status == TransferStatus::Queued || task->status == TransferStatus::Running;
    if (task->status == TransferStatus::Completed && task->skipped > 0 && task->message.isEmpty())
        row.message = QStringLiteral("已完成（跳过 %1 项）").arg(task->skipped);
    return row;
}

void TransferManager::updateTaskRow(TaskPtr task)
{
    m_model->updateRow(rowForTask(task));
}

void TransferManager::startQueued()
{
    if (m_shuttingDown)
        return;
    for (const TaskPtr &task : m_tasks) {
        if (m_activeCount >= m_parallelLimit)
            break;
        if (task->status != TransferStatus::Queued)
            continue;
        task->status = TransferStatus::Running;
        ++m_activeCount;
        updateTaskRow(task);
        emit taskStateChanged(task->id);
        const QFuture<void> future =
            QtConcurrent::run([this, task]() { runTask(task); });
        m_futures.append(future);
    }
}

void TransferManager::runTask(TaskPtr task)
{
    const auto complete = [this, task](TransferStatus status, const QString &message) {
        task->finished.store(true);
        QMetaObject::invokeMethod(
            this, [this, task, status, message]() { finishTask(task, status, message); },
            Qt::QueuedConnection);
    };

    if (m_shuttingDown || task->cancel.load()) {
        complete(TransferStatus::Cancelled, QStringLiteral("传输已取消。"));
        return;
    }

    BackendPtr backend = createBackend(task->session.protocol);
    if (!backend) {
        complete(TransferStatus::Failed,
                 QStringLiteral("暂不支持协议：%1").arg(protocolDisplayName(task->session.protocol)));
        return;
    }
    QString error;
    if (!backend->connect(task->session, &error)) {
        complete(TransferStatus::Failed, QStringLiteral("无法连接服务器：%1").arg(error));
        return;
    }

    if (task->direction == TransferDirection::Upload && !task->targetRoot.isEmpty()) {
        QString mkdirError;
        if (!ensureRemoteDirectory(backend.get(), task->targetRoot, &mkdirError)) {
            backend->disconnect();
            complete(TransferStatus::Failed,
                     QStringLiteral("无法创建目标目录 %1：%2").arg(task->targetRoot, mkdirError));
            return;
        }
    }

    Planner planner;
    bool planned = false;
    if (task->direction == TransferDirection::Upload) {
        planned = planUpload(task->entries, task->targetRoot, &planner, &error);
    } else {
        planned = planDownload(task->entries, task->targetRoot, backend.get(), &planner, &error);
    }
    if (!planned) {
        backend->disconnect();
        complete(TransferStatus::Failed, error.isEmpty() ? QStringLiteral("无法创建传输计划。") : error);
        return;
    }

    task->totalBytes = planner.totalBytes;
    task->skipped = 0;
    QMetaObject::invokeMethod(this, [this, task]() { updateTaskRow(task); }, Qt::QueuedConnection);

    if (m_shuttingDown || task->cancel.load()) {
        backend->disconnect();
        complete(TransferStatus::Cancelled, QStringLiteral("传输已取消。"));
        return;
    }

    QString executeError;
    const bool success = executePlan(task, &planner, backend.get(), &executeError);
    backend->disconnect();

    if (task->cancel.load() || m_shuttingDown) {
        complete(TransferStatus::Cancelled, QStringLiteral("传输已取消。"));
    } else if (success) {
        QString message;
        if (planner.symlinkCount > 0)
            message = QStringLiteral("已完成，跳过 %1 个符号链接。").arg(planner.symlinkCount);
        complete(TransferStatus::Completed, message);
    } else {
        complete(TransferStatus::Failed, executeError);
    }
}

bool TransferManager::planUpload(const QList<FileInfo> &entries, const QString &remoteDirectory,
                                 Planner *planner, QString *error)
{
    for (const FileInfo &entry : entries) {
        const QString target = path::remoteJoin(remoteDirectory, safeName(entry.name));
        if (entry.isSymlink) {
            PlanItem item;
            item.isSymlink = true;
            item.source = entry.path;
            item.target = target;
            item.size = 0;
            planner->items.append(item);
            ++planner->symlinkCount;
            continue;
        }
        if (entry.isDir) {
            PlanItem item;
            item.isDir = true;
            item.source = entry.path;
            item.target = target;
            item.mode = entry.mode;
            item.modified = entry.modified;
            planner->items.append(item);
            if (!planUploadDirectory(entry.path, target, 1, planner, error))
                return false;
            continue;
        }
        QFileInfo info(entry.path);
        if (!info.exists() || !info.isFile()) {
            if (error)
                *error = QStringLiteral("源文件不存在：%1").arg(entry.path);
            return false;
        }
        PlanItem item;
        item.source = entry.path;
        item.target = target;
        item.size = info.size();
        item.mode = entry.mode != 0 ? entry.mode : localMode(info);
        item.modified = localModificationTime(info);
        planner->items.append(item);
        planner->totalBytes += item.size;
    }
    return true;
}

bool TransferManager::planUploadDirectory(const QString &localDirectory,
                                          const QString &remoteDirectory, int depth,
                                          Planner *planner, QString *error)
{
    if (depth > 128) {
        if (error)
            *error = QStringLiteral("目录层级过深：%1").arg(localDirectory);
        return false;
    }
    QDir directory(localDirectory);
    if (!directory.exists()) {
        if (error)
            *error = QStringLiteral("目录不存在：%1").arg(localDirectory);
        return false;
    }
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : entries) {
        const QString target =
            path::remoteJoin(remoteDirectory, safeName(info.fileName()));
        if (info.isSymLink()) {
            PlanItem item;
            item.isSymlink = true;
            item.source = info.absoluteFilePath();
            item.target = target;
            planner->items.append(item);
            ++planner->symlinkCount;
            continue;
        }
        if (info.isDir()) {
            PlanItem item;
            item.isDir = true;
            item.source = info.absoluteFilePath();
            item.target = target;
            planner->items.append(item);
            if (!planUploadDirectory(info.absoluteFilePath(), target, depth + 1, planner, error))
                return false;
            continue;
        }
        PlanItem item;
        item.source = info.absoluteFilePath();
        item.target = target;
        item.size = info.size();
        item.mode = localMode(info);
        item.modified = info.lastModified();
        planner->items.append(item);
        planner->totalBytes += item.size;
    }
    return true;
}

bool TransferManager::planDownload(const QList<FileInfo> &entries, const QString &localDirectory,
                                   Backend *backend, Planner *planner, QString *error)
{
    if (!QDir().mkpath(localDirectory)) {
        if (error)
            *error = QStringLiteral("无法创建本地目录：%1").arg(localDirectory);
        return false;
    }
    for (const FileInfo &entry : entries) {
        const QString target = localTarget(localDirectory, entry.name);
        if (entry.isSymlink) {
            PlanItem item;
            item.isSymlink = true;
            item.source = entry.path;
            item.target = target;
            planner->items.append(item);
            ++planner->symlinkCount;
            continue;
        }
        if (entry.isDir) {
            PlanItem item;
            item.isDir = true;
            item.source = entry.path;
            item.target = target;
            planner->items.append(item);
            if (!planDownloadDirectory(entry.path, target, 1, backend, planner, error))
                return false;
            continue;
        }
        PlanItem item;
        item.source = entry.path;
        item.target = target;
        item.size = entry.size;
        item.mode = entry.mode;
        item.modified = entry.modified;
        planner->items.append(item);
        planner->totalBytes += item.size;
    }
    return true;
}

bool TransferManager::planDownloadDirectory(const QString &remoteDirectory,
                                            const QString &localDirectory, int depth,
                                            Backend *backend, Planner *planner, QString *error)
{
    if (depth > 128) {
        if (error)
            *error = QStringLiteral("目录层级过深：%1").arg(remoteDirectory);
        return false;
    }
    FileList entries;
    if (!backend->list(remoteDirectory, &entries, error))
        return false;
    for (const FileInfo &entry : entries) {
        if (entry.name == QLatin1String(".") || entry.name == QLatin1String(".."))
            continue;
        const QString target = localTarget(localDirectory, entry.name);
        if (entry.isSymlink) {
            PlanItem item;
            item.isSymlink = true;
            item.source = entry.path;
            item.target = target;
            planner->items.append(item);
            ++planner->symlinkCount;
            continue;
        }
        if (entry.isDir) {
            PlanItem item;
            item.isDir = true;
            item.source = entry.path;
            item.target = target;
            planner->items.append(item);
            if (!planDownloadDirectory(entry.path, target, depth + 1, backend, planner, error))
                return false;
            continue;
        }
        PlanItem item;
        item.source = entry.path;
        item.target = target;
        item.size = entry.size;
        item.mode = entry.mode;
        item.modified = entry.modified;
        planner->items.append(item);
        planner->totalBytes += item.size;
    }
    return true;
}

bool TransferManager::executePlan(TaskPtr task, Planner *planner, Backend *backend, QString *error)
{
    qint64 baseDone = 0;
    QElapsedTimer speedTimer;
    speedTimer.start();
    qint64 lastSpeedTime = 0;
    qint64 lastSpeedBytes = 0;
    qint64 lastEmitTime = 0;

    const auto emitProgress = [&](qint64 overall, const QString &currentFile, bool force) {
        const qint64 now = speedTimer.elapsed();
        if (!force && now - lastEmitTime < 100)
            return;
        double speed = task->speed;
        if (now > lastSpeedTime) {
            speed = static_cast<double>(overall - lastSpeedBytes) * 1000.0 /
                    static_cast<double>(now - lastSpeedTime);
            lastSpeedTime = now;
            lastSpeedBytes = overall;
        }
        lastEmitTime = now;
        task->doneBytes.store(std::min(overall, task->totalBytes >= 0 ? task->totalBytes : overall));
        task->speed = speed;
        task->currentFile = currentFile;
        QMetaObject::invokeMethod(this, [this, task]() { updateTaskRow(task); }, Qt::QueuedConnection);
    };

    for (const PlanItem &item : planner->items) {
        if (task->cancel.load() || m_shuttingDown) {
            if (error)
                *error = QStringLiteral("传输已取消。");
            return false;
        }

        if (item.isDir) {
            bool ok = false;
            if (task->direction == TransferDirection::Upload) {
                QString mkdirError;
                if (backend->mkdir(item.target, &mkdirError)) {
                    ok = true;
                } else {
                    FileInfo existing;
                    QString statError;
                    ok = backend->stat(item.target, &existing, &statError) && existing.isDir;
                }
            } else {
                ok = QDir().mkpath(item.target);
            }
            if (!ok) {
                if (error)
                    *error = QStringLiteral("无法创建目录：%1").arg(item.target);
                return false;
            }
            continue;
        }

        if (item.isSymlink) {
            ++task->skipped;
            continue;
        }

        if (task->direction == TransferDirection::Upload) {
            FileInfo existing;
            const bool exists = backend->stat(item.target, &existing, nullptr);
            if (exists) {
                const ConflictAction action =
                    resolveConflict(item.target, task->direction, item.size, existing.size);
                if (action == ConflictAction::Cancel) {
                    task->cancel.store(true);
                    if (error)
                        *error = QStringLiteral("传输已取消。");
                    return false;
                }
                if (action == ConflictAction::Skip) {
                    ++task->skipped;
                    baseDone += item.size;
                    emitProgress(baseDone, item.source, true);
                    continue;
                }
            }
        } else {
            const QFileInfo targetInfo(item.target);
            if (targetInfo.exists()) {
                const ConflictAction action =
                    resolveConflict(item.target, task->direction, item.size, targetInfo.size());
                if (action == ConflictAction::Cancel) {
                    task->cancel.store(true);
                    if (error)
                        *error = QStringLiteral("传输已取消。");
                    return false;
                }
                if (action == ConflictAction::Skip) {
                    ++task->skipped;
                    baseDone += item.size;
                    emitProgress(baseDone, item.source, true);
                    continue;
                }
                if (targetInfo.isDir()) {
                    if (error)
                        *error = QStringLiteral("目标是一个目录：%1").arg(item.target);
                    return false;
                }
            }
        }

        emitProgress(baseDone, item.source, true);
        QString transferError;
        bool ok = false;
        if (task->direction == TransferDirection::Upload) {
            const auto callback = [&](const TransferProgress &progress) {
                emitProgress(baseDone + progress.bytesDone, item.source, false);
            };
            ok = backend->uploadFile(item.source, item.target, callback, &task->cancel,
                                     &transferError);
        } else {
            const auto callback = [&](const TransferProgress &progress) {
                emitProgress(baseDone + progress.bytesDone, item.source, false);
            };
            ok = backend->downloadFile(item.source, item.target, item.size, callback,
                                       &task->cancel, &transferError);
        }
        if (!ok) {
            if (task->cancel.load() || m_shuttingDown) {
                if (error)
                    *error = QStringLiteral("传输已取消。");
                return false;
            }
            if (error)
                *error = transferError.isEmpty()
                    ? QStringLiteral("传输失败：%1").arg(item.source)
                    : transferError;
            return false;
        }

        if (task->direction == TransferDirection::Upload) {
            if (Settings::instance().preservePermissions && backend->supportsUnixPermissions() &&
                item.mode != 0) {
                QString ignored;
                backend->chmod(item.target, item.mode & 0777, &ignored);
            }
            if (Settings::instance().preserveTimestamps && item.modified.isValid() &&
                backend->canSetModifiedTime()) {
                QString ignored;
                backend->setModifiedTime(item.target, item.modified, &ignored);
            }
        } else if (item.modified.isValid()) {
            QFile localFile(item.target);
            if (localFile.open(QIODevice::ReadWrite))
                localFile.setFileTime(item.modified, QFileDevice::FileModificationTime);
        }

        baseDone += item.size;
        emitProgress(baseDone, item.source, true);
    }
    return true;
}

void TransferManager::finishTask(TaskPtr task, TransferStatus status, const QString &message)
{
    if (task->status == TransferStatus::Completed || task->status == TransferStatus::Failed ||
        task->status == TransferStatus::Cancelled)
        return;
    const bool wasActive = task->status == TransferStatus::Running;
    task->status = status;
    task->message = message;
    task->speed = 0;
    if (status == TransferStatus::Completed)
        task->doneBytes.store(task->totalBytes >= 0 ? task->totalBytes : task->doneBytes.load());
    if (wasActive && m_activeCount > 0)
        --m_activeCount;
    updateTaskRow(task);
    emit taskStateChanged(task->id);
    emit queueChanged();
    if (status == TransferStatus::Completed) {
        Logger::instance().success(QStringLiteral("传输完成：%1").arg(task->name));
    } else if (status == TransferStatus::Failed) {
        Logger::instance().error(QStringLiteral("传输失败：%1：%2").arg(task->name, message));
        emit statusMessage(QStringLiteral("传输失败：%1").arg(message));
    } else if (status == TransferStatus::Cancelled) {
        Logger::instance().warning(QStringLiteral("传输已取消：%1").arg(task->name));
    }
    startQueued();
    if (m_activeCount == 0 && pendingCount() == 0)
        emit allTransfersFinished();
}

ConflictAction TransferManager::resolveConflict(const QString &targetPath,
                                                TransferDirection direction, qint64 sourceSize,
                                                qint64 targetSize)
{
    const QString policy = Settings::instance().conflictPolicy;
    if (policy == QLatin1String("overwrite"))
        return ConflictAction::Overwrite;
    if (policy == QLatin1String("skip"))
        return ConflictAction::Skip;
    if (policy == QLatin1String("resume"))
        return ConflictAction::Overwrite;
    if (m_shuttingDown || !m_conflictResolver)
        return ConflictAction::Overwrite;
    ConflictAction action = ConflictAction::Overwrite;
    QMetaObject::invokeMethod(
        this,
        [this, targetPath, direction, sourceSize, targetSize, &action]() {
            if (m_conflictResolver)
                action = m_conflictResolver(targetPath, direction, sourceSize, targetSize);
        },
        Qt::BlockingQueuedConnection);
    return action;
}

void TransferManager::cancelTask(int id)
{
    const TaskPtr task = m_byId.value(id);
    if (!task)
        return;
    if (task->status == TransferStatus::Queued) {
        task->status = TransferStatus::Cancelled;
        task->message = QStringLiteral("已取消。");
        updateTaskRow(task);
        emit taskStateChanged(id);
        emit queueChanged();
        if (m_activeCount == 0 && pendingCount() == 0)
            emit allTransfersFinished();
        return;
    }
    if (task->status == TransferStatus::Running) {
        task->cancel.store(true);
        task->message = QStringLiteral("正在取消…");
        updateTaskRow(task);
        emit taskStateChanged(id);
    }
}

void TransferManager::cancelAll()
{
    for (const TaskPtr &task : m_tasks) {
        if (task->status == TransferStatus::Queued || task->status == TransferStatus::Running)
            cancelTask(task->id);
    }
}

void TransferManager::clearFinished()
{
    for (int index = m_tasks.size() - 1; index >= 0; --index) {
        const TaskPtr &task = m_tasks.at(index);
        if (task->status == TransferStatus::Completed || task->status == TransferStatus::Failed ||
            task->status == TransferStatus::Cancelled) {
            m_byId.remove(task->id);
            m_tasks.removeAt(index);
        }
    }
    m_model->clearFinished();
    emit queueChanged();
}

void TransferManager::clearAll()
{
    cancelAll();
    m_tasks.clear();
    m_byId.clear();
    m_model->clearAll();
    emit queueChanged();
}

void TransferManager::shutdown()
{
    if (m_shuttingDown.exchange(true))
        return;
    cancelAll();
    QElapsedTimer timer;
    timer.start();
    while (m_activeCount > 0 && timer.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    for (QFuture<void> &future : m_futures) {
        if (future.isValid())
            future.waitForFinished();
    }
    m_futures.clear();
}

} // namespace wsc
