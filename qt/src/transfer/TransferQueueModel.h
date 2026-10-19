// SPDX-License-Identifier: MIT
#pragma once

#include "transfer/TransferTypes.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QVector>

namespace wsc {

class TransferQueueModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        NameColumn = 0,
        DirectionColumn,
        ProgressColumn,
        SizeColumn,
        SpeedColumn,
        StatusColumn,
        TargetColumn,
        ColumnCount,
    };
    enum Role {
        TaskIdRole = Qt::UserRole + 1,
        ProgressRole,
        StatusRole,
        CanCancelRole,
    };

    explicit TransferQueueModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void addRow(const TransferRow &row);
    void updateRow(const TransferRow &row);
    void removeRow(int id);
    void clearFinished();
    void clearAll();
    int rowForId(int id) const;
    TransferRow rowAt(int index) const;
    QList<TransferRow> rows() const;
    int activeCount() const;

signals:
    void rowsChanged();

private:
    QVector<TransferRow> m_rows;
    QHash<int, int> m_indexById;
    void rebuildIndex();
};

} // namespace wsc
