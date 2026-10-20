// SPDX-License-Identifier: MIT
#pragma once

#include "core/Types.h"

#include <QAbstractTableModel>
#include <QCollator>
#include <QSortFilterProxyModel>

namespace wsc {

class FileTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        NameColumn = 0,
        SizeColumn,
        TypeColumn,
        ModifiedColumn,
        PermissionsColumn,
        ColumnCount,
    };

    explicit FileTableModel(bool local, QObject *parent = nullptr);

    void setEntries(const FileList &entries);
    void updateEntries(const FileList &entries);
    const FileList &entries() const { return m_entries; }
    FileInfo entryAt(int row) const;
    bool isLocal() const { return m_local; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;

    qint64 totalFileSize() const;
    int folderCount() const;
    int fileCount() const;

private:
    FileList m_entries;
    bool m_local = false;
};

class FileFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit FileFilterProxyModel(QObject *parent = nullptr);

    void setShowHidden(bool show);
    bool showHidden() const { return m_showHidden; }
    void setFilterText(const QString &text);
    QString filterText() const { return m_filter; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    bool m_showHidden = false;
    QString m_filter;
    QCollator m_collator;
};

} // namespace wsc
