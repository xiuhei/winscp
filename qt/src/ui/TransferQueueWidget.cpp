#include "ui/TransferQueueWidget.h"

#include "core/PathUtils.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QAction>
#include <QAbstractItemView>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

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
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF outer = option.rect.adjusted(6, 9, -6, -9);
        const QColor background = Theme::isDark() ? QColor(QStringLiteral("#1c1c1c"))
                                                  : QColor(QStringLiteral("#eef2f7"));
        painter->setPen(Qt::NoPen);
        painter->setBrush(background);
        painter->drawRoundedRect(outer, 4, 4);

        const double progress = index.data(TransferQueueModel::ProgressRole).toDouble();
        if (progress > 0.0) {
            QRectF fill = outer;
            fill.setWidth(outer.width() * progress);
            const QColor accent = progress >= 1.0 ? Theme::success() : Theme::accent();
            painter->setBrush(accent);
            painter->drawRoundedRect(fill, 4, 4);
        }
        const QString text = QStringLiteral("%1%").arg(qRound(progress * 100.0));
        painter->setPen(Theme::text());
        QFont font = option.font;
        font.setPointSizeF(std::max(7.5, font.pointSizeF() - 1.0));
        painter->setFont(font);
        painter->drawText(option.rect, Qt::AlignCenter, text);
        painter->restore();
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
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(3);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("传输队列"), this);
    QFont titleFont = title->font();
    title->setObjectName(QStringLiteral("DialogHeading"));
    title->setFont(titleFont);
    toolbar->addWidget(title);
    toolbar->addStretch(1);

    auto makeButton = [this](const QString &icon, const QString &text) {
        auto *button = new QToolButton(this);
        button->setIcon(IconProvider::instance().icon(icon));
        button->setIconSize(QSize(16, 16));
        button->setText(text);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        return button;
    };
    auto *cancelButton = makeButton(QStringLiteral("stop"), QStringLiteral("取消"));
    auto *clearButton = makeButton(QStringLiteral("clear"), QStringLiteral("清空已完成"));
    auto *cancelAllButton = makeButton(QStringLiteral("close"), QStringLiteral("全部取消"));
    toolbar->addWidget(cancelButton);
    toolbar->addWidget(clearButton);
    toolbar->addWidget(cancelAllButton);
    layout->addLayout(toolbar);

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("TransferTable"));
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setMinimumSectionSize(22);
    m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setItemDelegateForColumn(TransferQueueModel::ProgressColumn,
                                      new ProgressDelegate(m_table));
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setColumnWidth(TransferQueueModel::NameColumn, 170);
    m_table->setColumnWidth(TransferQueueModel::DirectionColumn, 60);
    m_table->setColumnWidth(TransferQueueModel::ProgressColumn, 140);
    m_table->setColumnWidth(TransferQueueModel::SizeColumn, 82);
    m_table->setColumnWidth(TransferQueueModel::SpeedColumn, 86);
    m_table->setColumnWidth(TransferQueueModel::StatusColumn, 150);
    m_table->horizontalHeader()->setSectionResizeMode(TransferQueueModel::TargetColumn,
                                                      QHeaderView::Stretch);
    layout->addWidget(m_table, 1);

    m_emptyLabel = new QLabel(QStringLiteral("没有传输任务\n从本地或远程面板拖放文件即可开始传输"),
                              m_table->viewport());
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_emptyLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                    .arg(Theme::muted().name()));
    m_emptyLabel->hide();

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    layout->addWidget(m_summaryLabel);

    connect(cancelButton, &QToolButton::clicked, this, &TransferQueueWidget::cancelSelected);
    connect(clearButton, &QToolButton::clicked, this, [this] {
        m_manager->clearFinished();
        updateEmptyState();
    });
    connect(cancelAllButton, &QToolButton::clicked, this, [this] { m_manager->cancelAll(); });
    connect(m_model, &TransferQueueModel::rowsChanged, this, [this] {
        refreshSummary();
        updateEmptyState();
    });
    connect(m_manager, &TransferManager::taskAdded, this, [this](int id) {
        const QModelIndex index = m_model->index(m_model->rowForId(id), 0);
        m_table->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_table->scrollTo(index);
        refreshSummary();
        updateEmptyState();
    });
    connect(m_manager, &TransferManager::allTransfersFinished, this, [this] {
        refreshSummary();
    });
    connect(m_table, &QTableView::customContextMenuRequested, this,
            &TransferQueueWidget::showContextMenu);

    refreshSummary();
    updateEmptyState();
}

void TransferQueueWidget::refreshSummary()
{
    int running = 0;
    int queued = 0;
    int completed = 0;
    int failed = 0;
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
        default:
            break;
        }
    }
    QString text = QStringLiteral("运行 %1 · 排队 %2 · 完成 %3").arg(running).arg(queued).arg(completed);
    if (failed > 0)
        text += QStringLiteral(" · 失败 %1").arg(failed);
    m_summaryLabel->setText(text);
}

void TransferQueueWidget::updateEmptyState()
{
    if (!m_emptyLabel || !m_table)
        return;
    if (m_model->rowCount() == 0) {
        m_emptyLabel->setGeometry(m_table->viewport()->rect());
        m_emptyLabel->raise();
        m_emptyLabel->show();
    } else {
        m_emptyLabel->hide();
    }
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
    QMenu menu(this);
    QAction *cancelAction = menu.addAction(IconProvider::instance().icon(QStringLiteral("stop")),
                                           QStringLiteral("取消任务"));
    cancelAction->setEnabled(index.isValid() &&
                             index.data(TransferQueueModel::CanCancelRole).toBool());
    QAction *detailsAction = menu.addAction(IconProvider::instance().icon(QStringLiteral("properties")),
                                            QStringLiteral("查看详情"));
    detailsAction->setEnabled(index.isValid());
    menu.addSeparator();
    QAction *clearAction = menu.addAction(IconProvider::instance().icon(QStringLiteral("clear")),
                                          QStringLiteral("清空已完成"));
    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (!chosen)
        return;
    if (chosen == cancelAction) {
        const int id = index.data(TransferQueueModel::TaskIdRole).toInt();
        m_manager->cancelTask(id);
    } else if (chosen == detailsAction) {
        const TransferRow row = m_model->rowAt(index.row());
        QString details = QStringLiteral("任务：%1\n方向：%2\n状态：%3\n大小：%4\n目标：%5")
                              .arg(row.name, transferDirectionName(row.direction),
                                   transferStatusName(row.status), path::formatSize(row.bytesTotal),
                                   row.target);
        if (!row.currentFile.isEmpty())
            details += QStringLiteral("\n当前文件：%1").arg(row.currentFile);
        if (!row.message.isEmpty())
            details += QStringLiteral("\n消息：%1").arg(row.message);
        QMessageBox::information(this, QStringLiteral("传输详情"), details);
    } else if (chosen == clearAction) {
        m_manager->clearFinished();
        updateEmptyState();
    }
}

} // namespace wsc
