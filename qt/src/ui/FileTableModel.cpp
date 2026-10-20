#include "ui/FileTableModel.h"

#include "core/PathUtils.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QCollator>
#include <QLocale>

namespace wsc {

FileTableModel::FileTableModel(bool local, QObject *parent)
    : QAbstractTableModel(parent)
    , m_local(local)
{
}

void FileTableModel::setEntries(const FileList &entries)
{
    beginResetModel();
    m_entries = entries;
    endResetModel();
}

FileInfo FileTableModel::entryAt(int row) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    return m_entries.at(row);
}

int FileTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

int FileTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant FileTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const FileInfo &info = m_entries.at(index.row());
    const int column = index.column();

    switch (role) {
    case Qt::DisplayRole:
        if (info.isParent && column != NameColumn)
            return {};
        switch (column) {
        case NameColumn:
            return info.name;
        case SizeColumn:
            return info.isDir ? QString() : path::formatSize(info.size);
        case TypeColumn:
            return info.typeDescription();
        case ModifiedColumn:
            return path::formatDateTime(info.modified);
        case PermissionsColumn:
            return info.permissionsString();
        default:
            break;
        }
        break;
    case Qt::DecorationRole:
        if (column == NameColumn)
            return IconProvider::instance().fileIcon(info, m_local);
        break;
    case Qt::TextAlignmentRole:
        if (column == SizeColumn)
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        if (column == ModifiedColumn)
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        break;
    case Qt::ForegroundRole:
        if (info.isHidden)
            return Theme::muted();
        if (info.isSymlink)
            return Theme::isDark() ? QColor(QStringLiteral("#7cc4ff"))
                                   : QColor(QStringLiteral("#2f6ea8"));
        if (info.isExecutable && !info.isDir)
            return Theme::success();
        break;
    case Qt::ToolTipRole: {
        QString tooltip = info.path;
        if (!info.linkTarget.isEmpty())
            tooltip += QStringLiteral("\n→ ") + info.linkTarget;
        if (!info.owner.isEmpty())
            tooltip += QStringLiteral("\n所有者：%1").arg(info.owner);
        if (!info.permissionsString().isEmpty())
            tooltip += QStringLiteral("\n权限：%1").arg(info.permissionsString());
        return tooltip;
    }
    case Qt::SizeHintRole:
        if (column == NameColumn)
            return QSize(320, 24);
        break;
    default:
        break;
    }
    return {};
}

QVariant FileTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case NameColumn:
            return QStringLiteral("名称");
        case SizeColumn:
            return QStringLiteral("大小");
        case TypeColumn:
            return QStringLiteral("类型");
        case ModifiedColumn:
            return QStringLiteral("修改时间");
        case PermissionsColumn:
            return QStringLiteral("权限");
        default:
            break;
        }
    }
    return QAbstractTableModel::headerData(section, orientation, role);
}

QStringList FileTableModel::mimeTypes() const
{
    return {QStringLiteral("application/x-winscpqt-entries"), QStringLiteral("text/uri-list")};
}

Qt::ItemFlags FileTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
    if (!entryAt(index.row()).isParent)
        flags |= Qt::ItemIsDragEnabled;
    return flags;
}

qint64 FileTableModel::totalFileSize() const
{
    qint64 total = 0;
    for (const FileInfo &info : m_entries) {
        if (!info.isDir)
            total += info.size;
    }
    return total;
}

int FileTableModel::folderCount() const
{
    int count = 0;
    for (const FileInfo &info : m_entries) {
        if (info.isDir && !info.isParent)
            ++count;
    }
    return count;
}

int FileTableModel::fileCount() const
{
    int count = 0;
    for (const FileInfo &info : m_entries)
        if (!info.isDir)
            ++count;
    return count;
}

FileFilterProxyModel::FileFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
    , m_collator(QLocale())
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    setSortCaseSensitivity(Qt::CaseInsensitive);
    setSortRole(Qt::DisplayRole);
    setDynamicSortFilter(true);
}

void FileFilterProxyModel::setShowHidden(bool show)
{
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    invalidateFilter();
}

void FileFilterProxyModel::setFilterText(const QString &text)
{
    if (m_filter == text)
        return;
    m_filter = text.trimmed();
    invalidateFilter();
}

bool FileFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QAbstractItemModel *model = sourceModel();
    if (!model)
        return false;
    const QModelIndex index = model->index(sourceRow, FileTableModel::NameColumn, sourceParent);
    const QString name = model->data(index, Qt::DisplayRole).toString();
    const auto *fileModel = qobject_cast<const FileTableModel *>(sourceModel());
    if (fileModel && fileModel->entryAt(sourceRow).isParent)
        return true;
    if (fileModel && fileModel->entryAt(sourceRow).isHidden && !m_showHidden)
        return false;
    if (!m_filter.isEmpty() && !path::wildcardMatch(m_filter, name))
        return false;
    return true;
}

bool FileFilterProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const auto *fileModel = qobject_cast<const FileTableModel *>(sourceModel());
    if (!fileModel)
        return QSortFilterProxyModel::lessThan(left, right);

    const FileInfo leftInfo = fileModel->entryAt(left.row());
    const FileInfo rightInfo = fileModel->entryAt(right.row());
    if (!leftInfo.isValid() || !rightInfo.isValid())
        return QSortFilterProxyModel::lessThan(left, right);

    if (leftInfo.isParent != rightInfo.isParent)
        return sortOrder() == Qt::AscendingOrder ? leftInfo.isParent : rightInfo.isParent;
    if (leftInfo.isDir != rightInfo.isDir)
        return sortOrder() == Qt::AscendingOrder ? leftInfo.isDir : rightInfo.isDir;

    const int column = left.column();
    int comparison = 0;
    switch (column) {
    case FileTableModel::NameColumn: {
        comparison = m_collator.compare(leftInfo.name, rightInfo.name);
        break;
    }
    case FileTableModel::SizeColumn:
        comparison = leftInfo.size < rightInfo.size ? -1 : (leftInfo.size > rightInfo.size ? 1 : 0);
        break;
    case FileTableModel::ModifiedColumn:
        comparison = leftInfo.modified < rightInfo.modified
            ? -1
            : (leftInfo.modified > rightInfo.modified ? 1 : 0);
        break;
    case FileTableModel::TypeColumn:
        comparison = QString::compare(leftInfo.typeDescription(), rightInfo.typeDescription(),
                                      Qt::CaseInsensitive);
        break;
    case FileTableModel::PermissionsColumn:
        comparison = QString::compare(leftInfo.permissionsString(), rightInfo.permissionsString(),
                                      Qt::CaseInsensitive);
        break;
    default:
        break;
    }
    if (comparison == 0)
        comparison = QString::localeAwareCompare(leftInfo.name, rightInfo.name);
    return comparison < 0;
}

} // namespace wsc
