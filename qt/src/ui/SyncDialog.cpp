#include "ui/SyncDialog.h"

#include "core/Logger.h"
#include "core/PathUtils.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace wsc {

namespace {

QString formatTime(const QDateTime &dateTime)
{
    return dateTime.isValid() ? dateTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                              : QStringLiteral("—");
}

} // namespace

class SyncResultModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ActionColumn = 0,
        PathColumn,
        SizeColumn,
        ModifiedColumn,
        ReasonColumn,
        ColumnCount,
    };

    explicit SyncResultModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    void setItems(const QList<SyncItem> &items)
    {
        beginResetModel();
        m_items = items;
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_items.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
            return {};
        const SyncItem &item = m_items.at(index.row());
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case ActionColumn:
                return syncActionName(item.action);
            case PathColumn:
                return item.relativePath;
            case SizeColumn:
                return item.file.isDir ? QString() : path::formatSize(item.file.size);
            case ModifiedColumn:
                return formatTime(item.file.modified);
            case ReasonColumn:
                return item.reason;
            default:
                break;
            }
        } else if (role == Qt::ForegroundRole && index.column() == ActionColumn) {
            switch (item.action) {
            case SyncAction::Upload:
                return Theme::accent();
            case SyncAction::Download:
                return Theme::isDark() ? QColor(QStringLiteral("#7cc4ff"))
                                       : QColor(QStringLiteral("#2f6ea8"));
            case SyncAction::DeleteRemote:
            case SyncAction::DeleteLocal:
                return Theme::danger();
            }
        } else if (role == Qt::TextAlignmentRole && index.column() == SizeColumn) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        } else if (role == Qt::ToolTipRole) {
            return QStringLiteral("%1\n%2\n目标：%3")
                .arg(item.relativePath, item.reason, item.targetDirectory);
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            switch (section) {
            case ActionColumn:
                return QStringLiteral("操作");
            case PathColumn:
                return QStringLiteral("文件");
            case SizeColumn:
                return QStringLiteral("大小");
            case ModifiedColumn:
                return QStringLiteral("修改时间");
            case ReasonColumn:
                return QStringLiteral("原因");
            default:
                break;
            }
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }

private:
    QList<SyncItem> m_items;
};

SyncDialog::SyncDialog(const SessionData &session, const QString &localRoot,
                       const QString &remoteRoot, HostKeyPrompt hostKeyPrompt, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
    , m_localRoot(QDir::cleanPath(localRoot))
    , m_remoteRoot(path::normalizeRemote(remoteRoot))
    , m_hostKeyPrompt(std::move(hostKeyPrompt))
{
    setWindowTitle(QStringLiteral("同步目录 - WinSCP Qt"));
    setWindowIcon(IconProvider::instance().appIcon());
    resize(940, 660);
    setMinimumSize(720, 500);
    buildUi();
    updateControls();
}

void SyncDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto *header = new QHBoxLayout();
    auto *icon = new QLabel(this);
    icon->setPixmap(IconProvider::instance().icon(QStringLiteral("sync"), Theme::accent())
                        .pixmap(42, 42));
    auto *title = new QLabel(QStringLiteral("同步目录"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(15);
    title->setObjectName(QStringLiteral("DialogHeading"));
    title->setFont(titleFont);
    auto *subtitle = new QLabel(
        QStringLiteral("比较本地与远程目录，预览后把差异加入传输队列。"), this);
    subtitle->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    auto *titleColumn = new QVBoxLayout();
    titleColumn->setSpacing(2);
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitle);
    header->addWidget(icon);
    header->addSpacing(8);
    header->addLayout(titleColumn, 1);
    layout->addLayout(header);

    auto *paths = new QWidget(this);
    auto *pathLayout = new QGridLayout(paths);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->setHorizontalSpacing(10);
    pathLayout->setVerticalSpacing(4);
    m_localLabel = new QLabel(m_localRoot, this);
    m_remoteLabel = new QLabel(QStringLiteral("%1（%2）").arg(m_remoteRoot, m_session.endpoint()),
                               this);
    m_localLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_remoteLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_localLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    m_remoteLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    pathLayout->addWidget(new QLabel(QStringLiteral("本地目录："), this), 0, 0);
    pathLayout->addWidget(m_localLabel, 0, 1);
    pathLayout->addWidget(new QLabel(QStringLiteral("远程目录："), this), 1, 0);
    pathLayout->addWidget(m_remoteLabel, 1, 1);
    pathLayout->setColumnStretch(1, 1);
    layout->addWidget(paths);

    auto *options = new QHBoxLayout();
    options->setSpacing(12);
    m_directionCombo = new QComboBox(this);
    m_directionCombo->addItem(QStringLiteral("本地 → 远程"),
                              static_cast<int>(SyncDirection::LocalToRemote));
    m_directionCombo->addItem(QStringLiteral("远程 → 本地"),
                              static_cast<int>(SyncDirection::RemoteToLocal));
    m_directionCombo->addItem(QStringLiteral("双向（较新覆盖）"),
                              static_cast<int>(SyncDirection::Both));
    m_recursiveCheck = new QCheckBox(QStringLiteral("包含子目录"), this);
    m_recursiveCheck->setChecked(true);
    m_deleteExtrasCheck = new QCheckBox(QStringLiteral("删除目标端多余文件"), this);
    m_compareTimeCheck = new QCheckBox(QStringLiteral("比较修改时间"), this);
    m_compareTimeCheck->setChecked(true);
    options->addWidget(new QLabel(QStringLiteral("方向："), this));
    options->addWidget(m_directionCombo);
    options->addWidget(m_recursiveCheck);
    options->addWidget(m_deleteExtrasCheck);
    options->addWidget(m_compareTimeCheck);
    options->addStretch(1);
    layout->addLayout(options);

    auto *actionRow = new QHBoxLayout();
    m_compareButton = new QPushButton(QStringLiteral("开始比较"), this);
    m_compareButton->setIcon(IconProvider::instance().icon(QStringLiteral("sync")));
    m_compareButton->setProperty("accent", true);
    m_syncButton = new QPushButton(QStringLiteral("同步"), this);
    m_syncButton->setIcon(IconProvider::instance().icon(QStringLiteral("upload"), Qt::white));
    m_syncButton->setProperty("accent", true);
    m_closeButton = new QPushButton(QStringLiteral("关闭"), this);
    actionRow->addWidget(m_compareButton);
    actionRow->addWidget(m_syncButton);
    actionRow->addStretch(1);
    actionRow->addWidget(m_closeButton);
    layout->addLayout(actionRow);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(5);
    m_progress->hide();
    layout->addWidget(m_progress);

    m_table = new QTableView(this);
    m_model = new SyncResultModel(this);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(SyncResultModel::ActionColumn, 100);
    m_table->setColumnWidth(SyncResultModel::PathColumn, 360);
    m_table->setColumnWidth(SyncResultModel::SizeColumn, 100);
    m_table->setColumnWidth(SyncResultModel::ModifiedColumn, 170);
    layout->addWidget(m_table, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    layout->addWidget(m_statusLabel);

    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_compareButton, &QPushButton::clicked, this, &SyncDialog::startCompare);
    connect(m_syncButton, &QPushButton::clicked, this, &SyncDialog::performSync);
    connect(m_directionCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        m_items.clear();
        m_model->setItems({});
        m_statusLabel->clear();
        updateControls();
    });
    for (QCheckBox *check : {m_recursiveCheck, m_deleteExtrasCheck, m_compareTimeCheck}) {
        connect(check, &QCheckBox::toggled, this, [this](bool) {
            m_items.clear();
            m_model->setItems({});
            m_statusLabel->clear();
            updateControls();
        });
    }
}

SyncDirection SyncDialog::direction() const
{
    return static_cast<SyncDirection>(m_directionCombo->currentData().toInt());
}

QString SyncDialog::directionName() const
{
    switch (direction()) {
    case SyncDirection::LocalToRemote:
        return QStringLiteral("本地 → 远程");
    case SyncDirection::RemoteToLocal:
        return QStringLiteral("远程 → 本地");
    case SyncDirection::Both:
        return QStringLiteral("双向（较新覆盖）");
    }
    return {};
}

void SyncDialog::updateControls()
{
    const bool both = direction() == SyncDirection::Both;
    m_deleteExtrasCheck->setEnabled(!both);
    if (both && m_deleteExtrasCheck->isChecked()) {
        const QSignalBlocker blocker(m_deleteExtrasCheck);
        m_deleteExtrasCheck->setChecked(false);
    }
    m_compareButton->setEnabled(!m_running);
    m_syncButton->setEnabled(!m_running && !m_items.isEmpty());
    m_syncButton->setText(m_items.isEmpty() ? QStringLiteral("同步")
                                            : QStringLiteral("同步 %1 项").arg(m_items.size()));
    m_directionCombo->setEnabled(!m_running);
    m_recursiveCheck->setEnabled(!m_running);
    m_compareTimeCheck->setEnabled(!m_running);
    m_deleteExtrasCheck->setEnabled(!m_running && !both);
}

void SyncDialog::startCompare()
{
    if (m_running)
        return;
    m_items.clear();
    m_model->setItems({});
    m_running = true;
    m_progress->show();
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    m_statusLabel->setText(QStringLiteral("正在扫描本地和远程目录…"));
    updateControls();

    SyncOptions options;
    options.direction = direction();
    options.recursive = m_recursiveCheck->isChecked();
    options.deleteExtras = m_deleteExtrasCheck->isChecked();
    options.compareTime = m_compareTimeCheck->isChecked();
    const SessionData session = m_session;
    const QString localRoot = m_localRoot;
    const QString remoteRoot = m_remoteRoot;
    const HostKeyPrompt prompt = m_hostKeyPrompt;

    auto *watcher = new QFutureWatcher<SyncPlan>(this);
    connect(watcher, &QFutureWatcher<SyncPlan>::finished, this, [this, watcher] {
        const SyncPlan plan = watcher->result();
        m_items = plan.items;
        m_model->setItems(m_items);
        m_running = false;
        m_progress->hide();
        if (!plan.error.isEmpty()) {
            m_statusLabel->setStyleSheet(
                QStringLiteral("color: %1;").arg(Theme::danger().name()));
            m_statusLabel->setText(plan.error);
        } else {
            int uploads = 0;
            int downloads = 0;
            int deletes = 0;
            for (const SyncItem &item : std::as_const(m_items)) {
                if (item.action == SyncAction::Upload)
                    ++uploads;
                else if (item.action == SyncAction::Download)
                    ++downloads;
                else
                    ++deletes;
            }
            m_statusLabel->setStyleSheet(
                QStringLiteral("color: %1;").arg(Theme::muted().name()));
            m_statusLabel->setText(
                QStringLiteral("比较完成：%1 个上传、%2 个下载、%3 个删除（方向：%4）")
                    .arg(uploads)
                    .arg(downloads)
                    .arg(deletes)
                    .arg(directionName()));
            Logger::instance().info(QStringLiteral("同步比较完成：上传 %1，下载 %2，删除 %3")
                                        .arg(uploads)
                                        .arg(downloads)
                                        .arg(deletes));
        }
        watcher->deleteLater();
        updateControls();
    });
    watcher->setFuture(QtConcurrent::run([=] {
        return buildSyncPlan(session, localRoot, remoteRoot, options, prompt);
    }));
}

void SyncDialog::performSync()
{
    if (m_items.isEmpty() || m_running)
        return;
    QHash<QString, QList<FileInfo>> uploads;
    QHash<QString, QList<FileInfo>> downloads;
    QList<FileInfo> remoteDeletes;
    QList<FileInfo> localDeletes;
    int uploadCount = 0;
    int downloadCount = 0;
    for (const SyncItem &item : std::as_const(m_items)) {
        switch (item.action) {
        case SyncAction::Upload:
            uploads[item.targetDirectory].append(item.file);
            ++uploadCount;
            break;
        case SyncAction::Download:
            downloads[item.targetDirectory].append(item.file);
            ++downloadCount;
            break;
        case SyncAction::DeleteRemote:
            remoteDeletes.append(item.file);
            break;
        case SyncAction::DeleteLocal:
            localDeletes.append(item.file);
            break;
        }
    }
    for (auto it = uploads.constBegin(); it != uploads.constEnd(); ++it)
        emit uploadRequested(it.value(), it.key());
    for (auto it = downloads.constBegin(); it != downloads.constEnd(); ++it)
        emit downloadRequested(it.value(), it.key());
    if (!remoteDeletes.isEmpty())
        emit remoteDeleteRequested(remoteDeletes);
    if (!localDeletes.isEmpty())
        emit localDeleteRequested(localDeletes);

    m_statusLabel->setText(QStringLiteral("已加入队列：%1 个上传、%2 个下载、%3 个删除。")
                               .arg(uploadCount)
                               .arg(downloadCount)
                               .arg(remoteDeletes.size() + localDeletes.size()));
    m_syncButton->setEnabled(false);
}

} // namespace wsc

#include "SyncDialog.moc"

