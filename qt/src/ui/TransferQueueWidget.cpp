#include "ui/TransferQueueWidget.h"

#include "core/PathUtils.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QAction>
#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace wsc {

namespace {

class ProgressDelegate : public QStyledItemDelegate {
public:
    explicit ProgressDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const QVariant progress = index.data(TransferQueueModel::ProgressRole);
        const int width = option.rect.width() - option.fontMetrics.horizontalAdvance(QStringLiteral("100%")) - 20;
        if (!progress.isValid() || width <= 0)
            return;
        QRect track(option.rect.left() + 6, option.rect.center().y() - 2, width, 5);
        painter->fillRect(track, option.palette.midlight());
        track.setWidth(qRound(width * progress.toDouble()));
        painter->fillRect(track, Theme::accent());
    }
};

} // namespace

TransferQueueWidget::TransferQueueWidget(TransferManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
    , m_model(manager->model())
{
    setObjectName(QStringLiteral("TransferQueue"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 16);
    layout->setSpacing(8);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(4);
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setObjectName(QStringLiteral("DialogHeading"));
    toolbar->addWidget(m_summaryLabel);
    toolbar->addStretch(1);

    auto makeButton = [this](const QString &icon, const QString &text) {
        auto *button = new QToolButton(this);
        button->setIcon(IconProvider::instance().icon(icon));
        button->setIconSize(QSize(16, 16));
        button->setText(text);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::StrongFocus);
        return button;
    };
    m_cancelButton = makeButton(QStringLiteral("stop"), QStringLiteral("取消所选"));
    m_cancelAllButton = makeButton(QStringLiteral("close"), QStringLiteral("全部取消"));
    toolbar->addWidget(m_cancelButton);
    toolbar->addWidget(m_cancelAllButton);
    layout->addLayout(toolbar);

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("TransferTable"));
    m_table->setAccessibleName(QStringLiteral("本轮传输任务"));
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(28);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->setItemDelegateForColumn(TransferQueueModel::ProgressColumn,
                                      new ProgressDelegate(m_table));
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->horizontalHeader()->setSectionResizeMode(TransferQueueModel::NameColumn, QHeaderView::Stretch);
    m_table->setColumnWidth(TransferQueueModel::DirectionColumn, 60);
    m_table->setColumnWidth(TransferQueueModel::ProgressColumn, 116);
    m_table->setColumnWidth(TransferQueueModel::SizeColumn, 82);
    m_table->setColumnWidth(TransferQueueModel::SpeedColumn, 86);
    m_table->setColumnWidth(TransferQueueModel::StatusColumn, 96);

    m_emptyLabel = new QLabel(QStringLiteral("没有传输任务"), this);
    m_emptyLabel->setObjectName(QStringLiteral("EmptyState"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_content = new QStackedLayout();
    m_content->addWidget(m_table);
    m_content->addWidget(m_emptyLabel);
    layout->addLayout(m_content, 1);

    connect(m_cancelButton, &QToolButton::clicked, this, &TransferQueueWidget::cancelSelected);
    connect(m_cancelAllButton, &QToolButton::clicked, m_manager, &TransferManager::cancelAll);
    connect(m_model, &TransferQueueModel::rowsChanged, this, &TransferQueueWidget::refresh);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &TransferQueueWidget::updateCancelAction);
    connect(m_table, &QTableView::customContextMenuRequested, this,
            &TransferQueueWidget::showContextMenu);
    connect(m_table, &QTableView::activated, this, [this](const QModelIndex &index) {
        showDetails(index.data(TransferQueueModel::TaskIdRole).toInt());
    });

    refresh();
}

void TransferQueueWidget::refresh()
{
    int running = 0;
    int queued = 0;
    int completed = 0;
    int failed = 0;
    int cancelled = 0;
    for (const TransferRow &row : m_model->rows()) {
        switch (row.status) {
        case TransferStatus::Running:
            ++running;
            break;
        case TransferStatus::Queued:
            ++queued;
            break;
        case TransferStatus::Completed:
            ++completed;
            break;
        case TransferStatus::Failed:
            ++failed;
            break;
        case TransferStatus::Cancelled:
            ++cancelled;
            break;
        }
    }
    const bool hasBatch = running + queued + failed + cancelled > 0;
    QStringList counts;
    if (running > 0)
        counts.append(QStringLiteral("传输中 %1").arg(running));
    if (queued > 0)
        counts.append(QStringLiteral("排队 %1").arg(queued));
    if (completed > 0)
        counts.append(QStringLiteral("完成 %1").arg(completed));
    if (failed > 0)
        counts.append(QStringLiteral("失败 %1").arg(failed));
    if (cancelled > 0)
        counts.append(QStringLiteral("取消 %1").arg(cancelled));
    m_summaryLabel->setText(hasBatch ? counts.join(QStringLiteral(" · ")) : QString());
    m_content->setCurrentWidget(hasBatch ? static_cast<QWidget *>(m_table) : m_emptyLabel);
    m_cancelButton->setVisible(hasBatch);
    m_cancelAllButton->setVisible(hasBatch);
    m_cancelAllButton->setEnabled(running + queued > 0);
    updateCancelAction();
}

void TransferQueueWidget::updateCancelAction()
{
    for (const auto &index : m_table->selectionModel()->selectedRows()) {
        if (index.data(TransferQueueModel::CanCancelRole).toBool()) {
            m_cancelButton->setEnabled(true);
            return;
        }
    }
    m_cancelButton->setEnabled(false);
}

void TransferQueueWidget::cancelSelected()
{
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        const int id = index.data(TransferQueueModel::TaskIdRole).toInt();
        m_manager->cancelTask(id);
    }
}

void TransferQueueWidget::showContextMenu(const QPoint &position)
{
    const QModelIndex index = m_table->indexAt(position);
    if (!index.isValid())
        return;
    const int id = index.data(TransferQueueModel::TaskIdRole).toInt();
    QMenu menu(this);
    QAction *cancelAction = menu.addAction(IconProvider::instance().icon(QStringLiteral("stop")),
                                           QStringLiteral("取消任务"));
    cancelAction->setEnabled(index.data(TransferQueueModel::CanCancelRole).toBool());
    QAction *detailsAction = menu.addAction(IconProvider::instance().icon(QStringLiteral("properties")),
                                            QStringLiteral("查看详情"));
    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (chosen == cancelAction)
        m_manager->cancelTask(id);
    else if (chosen == detailsAction)
        showDetails(id);
}

void TransferQueueWidget::showDetails(int id)
{
    const int rowIndex = m_model->rowForId(id);
    if (rowIndex < 0)
        return;
    const TransferRow row = m_model->rowAt(rowIndex);
    QString details = QStringLiteral("任务：%1\n方向：%2\n状态：%3\n大小：%4\n目标：%5")
                          .arg(row.name, transferDirectionName(row.direction),
                               transferStatusName(row.status), path::formatSize(row.bytesTotal),
                               row.target);
    if (!row.currentFile.isEmpty())
        details += QStringLiteral("\n当前文件：%1").arg(row.currentFile);
    if (!row.message.isEmpty())
        details += QStringLiteral("\n消息：%1").arg(row.message);
    QMessageBox box(QMessageBox::Information, QStringLiteral("传输详情"), details,
                    QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

} // namespace wsc
