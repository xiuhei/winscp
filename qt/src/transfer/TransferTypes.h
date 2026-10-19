// SPDX-License-Identifier: MIT
#pragma once

#include <QColor>
#include <QString>

namespace wsc {

enum class TransferDirection {
    Upload,
    Download,
    LocalCopy,
};

enum class TransferStatus {
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled,
};

enum class ConflictAction {
    Overwrite,
    Skip,
    Cancel,
};

QString transferDirectionName(TransferDirection direction);
QString transferStatusName(TransferStatus status);
QColor transferStatusColor(TransferStatus status);

struct TransferRow {
    int id = 0;
    QString name;
    TransferDirection direction = TransferDirection::Upload;
    TransferStatus status = TransferStatus::Queued;
    qint64 bytesDone = 0;
    qint64 bytesTotal = -1;
    double speed = 0;
    QString currentFile;
    QString target;
    QString message;
    bool canCancel = true;

    double progress() const
    {
        if (bytesTotal <= 0)
            return status == TransferStatus::Completed ? 1.0 : 0.0;
        return qBound(0.0, static_cast<double>(bytesDone) / static_cast<double>(bytesTotal), 1.0);
    }
};

} // namespace wsc
