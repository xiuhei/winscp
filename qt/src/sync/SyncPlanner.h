// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"
#include "core/SessionData.h"
#include "core/Types.h"

#include <QDateTime>
#include <QList>
#include <QString>

namespace wsc {

enum class SyncAction {
    Upload,
    Download,
    DeleteRemote,
    DeleteLocal,
};

enum class SyncDirection {
    LocalToRemote = 0,
    RemoteToLocal,
    Both,
};

struct SyncOptions {
    SyncDirection direction = SyncDirection::LocalToRemote;
    bool recursive = true;
    bool deleteExtras = false;
    bool compareTime = true;
};

struct SyncItem {
    SyncAction action = SyncAction::Upload;
    QString relativePath;
    FileInfo file;
    QString targetDirectory;
    QString reason;
};

struct SyncPlan {
    QList<SyncItem> items;
    QString error;
};

QString syncActionName(SyncAction action);

/// Compares a local and a remote directory and returns the operations needed
/// to make them match according to the options. This function performs network
/// I/O and is meant to be called from a worker thread.
SyncPlan buildSyncPlan(const SessionData &session, const QString &localRoot,
                       const QString &remoteRoot, const SyncOptions &options,
                       HostKeyPrompt hostKeyPrompt);

} // namespace wsc
