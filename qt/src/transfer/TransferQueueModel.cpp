#include "transfer/TransferQueueModel.h"

#include "core/PathUtils.h"

namespace wsc {

TransferQueueModel::TransferQueueModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int TransferQueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int TransferQueueModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant TransferQueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const TransferRow &row = m_rows.at(index.row());
    const int column = index.column();

    if (role == TaskIdRole)
        return row.id;
    const bool hasProgress = row.bytesTotal >= 0 || row.status == TransferStatus::Completed;
    if (role == ProgressRole)
        return hasProgress ? QVariant(row.progress()) : QVariant();
    if (role == CanCancelRole)
        return row.canCancel &&
            (row.status == TransferStatus::Queued || row.status == TransferStatus::Running);

    switch (role) {
    case Qt::DisplayRole:
        switch (column) {
        case NameColumn:
            return row.name;
        case DirectionColumn:
            return transferDirectionName(row.direction);
        case ProgressColumn:
            return hasProgress ? QStringLiteral("%1%").arg(qRound(row.progress() * 100.0))
                               : QStringLiteral("—");
        case SizeColumn:
            return path::formatSize(row.bytesTotal);
        case SpeedColumn:
            return row.status == TransferStatus::Running ? path::formatSpeed(row.speed)
                                                         : QStringLiteral("—");
        case StatusColumn:
            return row.status == TransferStatus::Running && !row.canCancel
                ? row.message : transferStatusName(row.status);
        default:
            break;
        }
        break;
    case Qt::ToolTipRole: {
        QString tooltip = row.name;
        if (!row.currentFile.isEmpty())
            tooltip += QStringLiteral("\n当前文件：%1").arg(row.currentFile);
        if (!row.target.isEmpty())
            tooltip += QStringLiteral("\n目标：%1").arg(row.target);
        if (!row.message.isEmpty())
            tooltip += QStringLiteral("\n%1").arg(row.message);
        tooltip += QStringLiteral("\n%1 / %2")
                       .arg(path::formatSize(row.bytesDone), path::formatSize(row.bytesTotal));
        return tooltip;
    }
    case Qt::TextAlignmentRole:
        if (column == ProgressColumn || column == SizeColumn || column == SpeedColumn)
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        break;
    default:
        break;
    }
    return {};
}

QVariant TransferQueueModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case NameColumn:
            return QStringLiteral("名称");
        case DirectionColumn:
            return QStringLiteral("方向");
        case ProgressColumn:
            return QStringLiteral("进度");
        case SizeColumn:
            return QStringLiteral("大小");
        case SpeedColumn:
            return QStringLiteral("速度");
        case StatusColumn:
            return QStringLiteral("状态");
        default:
            break;
        }
    }
    if (orientation == Qt::Horizontal && role == Qt::TextAlignmentRole) {
        const bool numeric = section == ProgressColumn || section == SizeColumn || section == SpeedColumn;
        return static_cast<int>((numeric ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    }
    return QAbstractTableModel::headerData(section, orientation, role);
}

void TransferQueueModel::addRow(const TransferRow &row)
{
    const int rowIndex = m_rows.size();
    beginInsertRows(QModelIndex(), rowIndex, rowIndex);
    m_rows.append(row);
    m_indexById.insert(row.id, rowIndex);
    endInsertRows();
    emit rowsChanged();
}

void TransferQueueModel::updateRow(const TransferRow &row)
{
    const int index = rowForId(row.id);
    if (index < 0)
        return;
    m_rows[index] = row;
    emit dataChanged(this->index(index, 0), this->index(index, ColumnCount - 1));
    emit rowsChanged();
}

void TransferQueueModel::clearAll()
{
    beginResetModel();
    m_rows.clear();
    m_indexById.clear();
    endResetModel();
    emit rowsChanged();
}

int TransferQueueModel::rowForId(int id) const
{
    return m_indexById.value(id, -1);
}

TransferRow TransferQueueModel::rowAt(int index) const
{
    if (index < 0 || index >= m_rows.size())
        return {};
    return m_rows.at(index);
}

QList<TransferRow> TransferQueueModel::rows() const
{
    return m_rows;
}

int TransferQueueModel::activeCount() const
{
    int count = 0;
    for (const TransferRow &row : m_rows) {
        if (row.status == TransferStatus::Queued || row.status == TransferStatus::Running)
            ++count;
    }
    return count;
}

} // namespace wsc
