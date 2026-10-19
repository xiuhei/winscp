// SPDX-License-Identifier: MIT
#pragma once

#include <QWidget>

class QLabel;
class QTableView;

namespace wsc {

class TransferManager;
class TransferQueueModel;

class TransferQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferQueueWidget(TransferManager *manager, QWidget *parent = nullptr);

    void refreshSummary();

private:
    void showContextMenu(const QPoint &position);
    void cancelSelected();
    void updateEmptyState();

    TransferManager *m_manager = nullptr;
    TransferQueueModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_emptyLabel = nullptr;
};

} // namespace wsc
