// SPDX-License-Identifier: MIT
#pragma once

#include "core/SessionData.h"
#include "core/Types.h"
#include "transfer/TransferTypes.h"

#include <QElapsedTimer>
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QTimer;

namespace wsc {

class TransferQueueModel;
class Backend;

/// Executes upload/download jobs on background threads. Each job owns its own
/// protocol connection, so transfers never interfere with the browser session.
class TransferManager : public QObject {
    Q_OBJECT
public:
    using ConflictResolver = std::function<ConflictAction(
        const QString &targetPath, TransferDirection direction, qint64 sourceSize,
        qint64 targetSize)>;

    explicit TransferManager(QObject *parent = nullptr);
    ~TransferManager() override;

    TransferQueueModel *model() const { return m_model; }

    void setParallelLimit(int limit);
    int parallelLimit() const { return m_parallelLimit; }
    int activeCount() const { return m_activeCount; }
    int pendingCount() const;

    void setConflictResolver(ConflictResolver resolver) { m_conflictResolver = std::move(resolver); }

    void enqueueUpload(const SessionData &session, const QList<FileInfo> &localEntries,
                       const QString &remoteDirectory);
    int enqueueDownload(const SessionData &session, const QList<FileInfo> &remoteEntries,
                        const QString &localDirectory);
    void cancelTask(int id);
    void cancelAll();
    void shutdown();

signals:
    void taskAdded(int id);
    void taskStateChanged(int id);
    void allTransfersFinished();
    void statusMessage(const QString &message);
    void queueChanged();

private:
    struct Task;
    using TaskPtr = std::shared_ptr<Task>;

    struct PlanItem {
        bool isDir = false;
        bool isSymlink = false;
        QString source;
        QString target;
        qint64 size = 0;
        quint32 mode = 0;
        QDateTime modified;
    };

    struct Planner {
        QVector<PlanItem> items;
        qint64 totalBytes = 0;
        int symlinkCount = 0;
        std::atomic_bool *cancel = nullptr;
    };

    void enqueue(const TaskPtr &task);
    void startQueued();
    void runTask(TaskPtr task);
    void finishTask(TaskPtr task, TransferStatus status, const QString &message);
    void updateTaskRow(TaskPtr task);
    TransferRow rowForTask(const TaskPtr &task) const;
    ConflictAction resolveConflict(const QString &targetPath, TransferDirection direction,
                                   qint64 sourceSize, qint64 targetSize);
    bool planUpload(const QList<FileInfo> &entries, const QString &remoteDirectory, Planner *planner,
                    QString *error);
    bool planUploadDirectory(const QString &localDirectory, const QString &remoteDirectory,
                             int depth, Planner *planner, QString *error);
    bool planDownload(const QList<FileInfo> &entries, const QString &localDirectory,
                      Backend *backend, Planner *planner, QString *error);
    bool planDownloadDirectory(const QString &remoteDirectory, const QString &localDirectory,
                               int depth, Backend *backend, Planner *planner, QString *error);
    bool executePlan(TaskPtr task, Planner *planner, Backend *backend, QString *error);

    TransferQueueModel *m_model = nullptr;
    QTimer *m_progressTimer = nullptr;
    QVector<TaskPtr> m_tasks;
    QHash<int, TaskPtr> m_byId;
    QVector<QFuture<void>> m_futures;
    ConflictResolver m_conflictResolver;
    int m_nextId = 1;
    int m_parallelLimit = 2;
    int m_activeCount = 0;
    std::atomic_bool m_shuttingDown{false};
};

} // namespace wsc
