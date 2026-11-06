// SPDX-License-Identifier: MIT
#pragma once

#include <QWidget>

class QLabel;
class QStackedLayout;
class QTableView;
class QToolButton;

namespace wsc {

class TransferManager;
class TransferQueueModel;

class TransferQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferQueueWidget(TransferManager *manager, QWidget *parent = nullptr);

private:
    void showContextMenu(const QPoint &position);
    void showDetails(int id);
    void cancelSelected();
    void refresh();
    void updateCancelAction();

    TransferManager *m_manager = nullptr;
    TransferQueueModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QStackedLayout *m_content = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QToolButton *m_cancelButton = nullptr;
    QToolButton *m_cancelAllButton = nullptr;
};

} // namespace wsc
