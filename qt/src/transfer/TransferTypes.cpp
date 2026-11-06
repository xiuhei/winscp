#include "transfer/TransferTypes.h"

namespace wsc {

QString transferDirectionName(TransferDirection direction)
{
    switch (direction) {
    case TransferDirection::Upload:
        return QStringLiteral("上传");
    case TransferDirection::Download:
        return QStringLiteral("下载");
    case TransferDirection::LocalCopy:
        return QStringLiteral("复制");
    }
    return QStringLiteral("传输");
}

QString transferStatusName(TransferStatus status)
{
    switch (status) {
    case TransferStatus::Queued:
        return QStringLiteral("排队中");
    case TransferStatus::Running:
        return QStringLiteral("传输中");
    case TransferStatus::Completed:
        return QStringLiteral("已完成");
    case TransferStatus::Failed:
        return QStringLiteral("失败");
    case TransferStatus::Cancelled:
        return QStringLiteral("已取消");
    }
    return QString();
}

} // namespace wsc
