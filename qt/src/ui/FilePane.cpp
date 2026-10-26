#include "ui/FilePane.h"

#include "core/FileSystemController.h"
#include "core/Logger.h"
#include "core/PathUtils.h"
#include "core/Settings.h"
#include "proto/LocalBackend.h"
#include "ui/FileTableModel.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace wsc {

namespace {

constexpr auto kEntryMimeType = "application/x-winscpqt-entries";

class FileTableView : public QTableView {
public:
    explicit FileTableView(QWidget *parent = nullptr)
        : QTableView(parent)
    {
    }

    std::function<bool(const QMimeData *)> canDropHandler;
    std::function<void(const QMimeData *, const QModelIndex &)> dropHandler;
    std::function<void()> dragHandler;
    std::function<bool(QKeyEvent *)> keyHandler;

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions);
        if (dragHandler)
            dragHandler();
        else
            QTableView::startDrag(supportedActions);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (canDropHandler && canDropHandler(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (canDropHandler && canDropHandler(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            QTableView::dragMoveEvent(event);
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        if (dropHandler && canDropHandler && canDropHandler(event->mimeData())) {
            const QModelIndex index = indexAt(event->position().toPoint());
            dropHandler(event->mimeData(), index);
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        event->ignore();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (keyHandler && keyHandler(event))
            return;
        QTableView::keyPressEvent(event);
    }


};

QToolButton *makeToolButton(QWidget *parent, const QString &iconName, const QString &tooltip)
{
    auto *button = new QToolButton(parent);
    button->setIcon(IconProvider::instance().icon(iconName));
    button->setIconSize(QSize(16, 16));
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setAutoRaise(true);
    button->setFixedSize(26, 26);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QString localDisplay(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

FileInfo infoFromJson(const QJsonObject &object)
{
    FileInfo info;
    info.name = object.value(QStringLiteral("name")).toString();
    info.path = object.value(QStringLiteral("path")).toString();
    info.size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
    info.isDir = object.value(QStringLiteral("isDir")).toBool();
    info.isSymlink = object.value(QStringLiteral("isSymlink")).toBool();
    info.linkTarget = object.value(QStringLiteral("linkTarget")).toString();
    info.mode = static_cast<quint32>(object.value(QStringLiteral("mode")).toDouble());
    info.modified = QDateTime::fromString(object.value(QStringLiteral("modified")).toString(),
                                          Qt::ISODate);
    return info;
}

QJsonObject infoToJson(const FileInfo &info)
{
    QJsonObject object;
    object.insert(QStringLiteral("name"), info.name);
    object.insert(QStringLiteral("path"), info.path);
    object.insert(QStringLiteral("size"), static_cast<double>(info.size));
    object.insert(QStringLiteral("isDir"), info.isDir);
    object.insert(QStringLiteral("isSymlink"), info.isSymlink);
    object.insert(QStringLiteral("linkTarget"), info.linkTarget);
    object.insert(QStringLiteral("mode"), static_cast<int>(info.mode));
    object.insert(QStringLiteral("modified"), info.modified.toString(Qt::ISODate));
    return object;
}

FileList withParent(FileList entries, const QString &directory, bool remote)
{
    const QString parent = remote ? path::remoteParent(directory)
                                  : QDir::cleanPath(QDir(directory).filePath(QStringLiteral("..")));
    if (parent != directory && !(remote ? path::remoteIsRoot(directory) : QDir(directory).isRoot())) {
        FileInfo entry;
        entry.name = QStringLiteral("..");
        entry.path = parent;
        entry.isDir = true;
        entry.isParent = true;
        entries.prepend(entry);
    }
    return entries;
}

QString normalizedLocalInput(const QString &text)
{
    QString value = text.trimmed();
    if (value.isEmpty())
        return {};
    if (value == QLatin1String("~"))
        return QDir::homePath();
    if (value.startsWith(QStringLiteral("~/")))
        value = QDir::homePath() + value.mid(1);
    value = QDir::fromNativeSeparators(value);
    return QDir::cleanPath(value);
}

} // namespace

FilePane::FilePane(Role role, QWidget *parent)
    : QFrame(parent)
    , m_role(role)
{
    setObjectName(QStringLiteral("Pane"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(280);
    setAcceptDrops(true);
    m_showHidden = Settings::instance().showHiddenFiles;
    buildUi();
    applyRoleStyle();
    if (m_role == Role::Local) {
        const QString initial = Settings::instance().lastLocalDirectory.isEmpty()
            ? QDir::homePath()
            : Settings::instance().lastLocalDirectory;
        navigateLocal(initial);
    } else {
        showRemoteDisconnected(QString());
    }
    updateNavigationButtons();
}

void FilePane::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("PaneHeader"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(6, 4, 6, 3);
    headerLayout->setSpacing(2);

    auto *titleRow = new QHBoxLayout();
    titleRow->setSpacing(4);
    m_badgeLabel = new QLabel(header);
    m_badgeLabel->setObjectName(QStringLiteral("PaneBadge"));
    m_badgeLabel->setText(m_role == Role::Local ? QStringLiteral("本地") : QStringLiteral("远程"));
    if (m_role == Role::Remote)
        m_badgeLabel->setProperty("role", QStringLiteral("remote"));
    m_titleLabel = new QLabel(header);
    m_titleLabel->setObjectName(QStringLiteral("PaneTitle"));
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_titleLabel->setText(m_role == Role::Local ? QStringLiteral("本地文件")
                                                : QStringLiteral("远程文件"));

    titleRow->addWidget(m_badgeLabel);
    titleRow->addWidget(m_titleLabel, 1);

    m_terminalButton = makeToolButton(header, QStringLiteral("terminal"), QStringLiteral("在终端中打开"));
    m_hiddenButton = makeToolButton(header, QStringLiteral("eye-off"), QStringLiteral("显示隐藏文件"));
    m_connectButton = makeToolButton(header, QStringLiteral("connect"), QStringLiteral("连接到服务器"));
    m_disconnectButton = makeToolButton(header, QStringLiteral("disconnect"), QStringLiteral("断开连接"));
    if (m_role == Role::Local) {
        m_terminalButton->hide();
        m_connectButton->hide();
        m_disconnectButton->hide();
    } else {
        m_terminalButton->hide();
        m_disconnectButton->hide();
    }
    if (isRemote()) {
        m_autoRefreshButton = new QToolButton(header);
        m_autoRefreshButton->setObjectName(QStringLiteral("AutoRefreshButton"));
        m_autoRefreshButton->setFixedHeight(26);
        m_autoRefreshButton->setAccessibleName(QStringLiteral("自动刷新远程目录"));
        m_autoRefreshButton->setCheckable(true);
        m_autoRefreshButton->setAutoRaise(true);
        m_autoRefreshButton->setChecked(Settings::instance().autoRefresh);
        titleRow->addWidget(m_autoRefreshButton);
        m_autoRefreshTimer = new QTimer(this);
        m_autoRefreshTimer->setInterval(2000);
        m_autoRefreshTimer->setTimerType(Qt::PreciseTimer);
        connect(m_autoRefreshTimer, &QTimer::timeout, this, [this] { refreshRemote(true); });
        connect(m_autoRefreshButton, &QToolButton::toggled, this, [this](bool enabled) {
            Settings::instance().autoRefresh = enabled;
            Settings::instance().save();
            updateAutoRefreshButton();
            if (enabled)
                refreshRemote(true);
        });
    }
    titleRow->addWidget(m_terminalButton);
    titleRow->addWidget(m_hiddenButton);
    titleRow->addWidget(m_connectButton);
    titleRow->addWidget(m_disconnectButton);
    headerLayout->addLayout(titleRow);

    auto *pathRow = new QHBoxLayout();
    pathRow->setSpacing(2);
    m_backButton = makeToolButton(header, QStringLiteral("arrow-left"), QStringLiteral("后退 (Alt+←)"));
    m_forwardButton = makeToolButton(header, QStringLiteral("arrow-right"), QStringLiteral("前进 (Alt+→)"));
    m_upButton = makeToolButton(header, QStringLiteral("up"), QStringLiteral("上一级 (Backspace)"));
    m_homeButton = makeToolButton(header, QStringLiteral("home"), QStringLiteral("主目录"));
    m_refreshButton = makeToolButton(header, QStringLiteral("refresh"), QStringLiteral("刷新 (F5)"));

    m_pathCombo = new QComboBox(header);
    m_pathCombo->setEditable(true);
    m_pathCombo->setInsertPolicy(QComboBox::NoInsert);
    m_pathCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_pathCombo->setMinimumWidth(120);
    m_pathCombo->setMaxVisibleItems(8);
    m_pathCombo->lineEdit()->setPlaceholderText(QStringLiteral("输入路径后按回车"));
    m_pathCombo->lineEdit()->setClearButtonEnabled(false);

    pathRow->addWidget(m_backButton);
    pathRow->addWidget(m_forwardButton);
    pathRow->addWidget(m_upButton);
    pathRow->addWidget(m_homeButton);
    m_pathStack = new QStackedWidget(header);
    m_pathStack->setFixedHeight(26);
    m_breadcrumbs = new QLabel(m_pathStack);
    m_breadcrumbs->setObjectName(QStringLiteral("PathBreadcrumbs"));
    m_breadcrumbs->setTextFormat(Qt::RichText);
    m_breadcrumbs->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_breadcrumbs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_breadcrumbs->setMargin(3);
    m_pathStack->addWidget(m_breadcrumbs);
    m_pathStack->addWidget(m_pathCombo);
    connect(m_breadcrumbs, &QLabel::linkActivated, this, [this](const QString &link) {
        navigate(QUrl::fromPercentEncoding(link.toLatin1()));
    });
    auto *editPath = makeToolButton(header, QStringLiteral("edit"), QStringLiteral("输入路径 (Ctrl+L)"));
    connect(editPath, &QToolButton::clicked, this, &FilePane::focusPathEditor);
    connect(m_pathCombo->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        m_pathStack->setCurrentIndex(0);
    });
    pathRow->addWidget(m_pathStack, 1);
    pathRow->addWidget(editPath);
    pathRow->addWidget(m_refreshButton);
    headerLayout->addLayout(pathRow);

    m_loadingBar = new QProgressBar(header);
    m_loadingBar->setRange(0, 0);
    m_loadingBar->setTextVisible(false);
    m_loadingBar->setFixedHeight(3);
    auto loadingSizePolicy = m_loadingBar->sizePolicy();
    loadingSizePolicy.setRetainSizeWhenHidden(true);
    m_loadingBar->setSizePolicy(loadingSizePolicy);
    m_loadingBar->hide();
    headerLayout->addWidget(m_loadingBar);

    outer->addWidget(header);

    m_stack = new QStackedWidget(this);
    auto *tablePage = new QWidget(m_stack);
    auto *tableLayout = new QVBoxLayout(tablePage);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    tableLayout->setSpacing(0);
    auto *view = new FileTableView(tablePage);
    m_table = view;
    m_table->setObjectName(QStringLiteral("FileTable"));
    m_table->setAlternatingRowColors(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSortingEnabled(true);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->setShowGrid(false);
    m_table->setDragEnabled(true);
    m_table->setAcceptDrops(true);
    m_table->setDropIndicatorShown(true);
    m_table->setDefaultDropAction(Qt::CopyAction);
    m_table->setDragDropMode(QAbstractItemView::DragDrop);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setIconSize(QSize(16, 16));
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setMinimumSectionSize(20);
    m_table->verticalHeader()->setDefaultSectionSize(20);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionsMovable(true);
    tableLayout->addWidget(m_table);
    m_stack->addWidget(tablePage);

    m_emptyLabel = new QLabel(QStringLiteral("此文件夹为空"), m_table->viewport());
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_emptyLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent; font-size: 9pt;")
                                  .arg(Theme::muted().name()));
    m_emptyLabel->hide();

    buildWelcomePage();
    outer->addWidget(m_stack, 1);

    auto *footerLine = new QFrame(this);
    footerLine->setObjectName(QStringLiteral("PaneFooterLine"));
    footerLine->setFrameShape(QFrame::NoFrame);
    outer->addWidget(footerLine);

    auto *footer = new QWidget(this);
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(6, 4, 6, 4);
    footerLayout->setSpacing(8);
    m_filterEdit = new QLineEdit(footer);
    m_filterEdit->setObjectName(QStringLiteral("FilterEdit"));
    m_filterEdit->setAccessibleName(QStringLiteral("筛选文件"));
    m_filterEdit->setPlaceholderText(QStringLiteral("筛选，例如 *.log"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setFixedWidth(150);
    m_filterEdit->addAction(IconProvider::instance().icon(QStringLiteral("search"), Theme::muted()),
                            QLineEdit::LeadingPosition);
    m_statusLabel = new QLabel(footer);
    m_statusLabel->setObjectName(QStringLiteral("PaneFooter"));
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    footerLayout->addWidget(m_filterEdit);
    footerLayout->addWidget(m_statusLabel, 1);
    outer->addWidget(footer);

    m_model = new FileTableModel(!isRemote(), this);
    m_proxy = new FileFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setShowHidden(m_showHidden);
    m_table->setModel(m_proxy);
    m_table->sortByColumn(FileTableModel::NameColumn, Qt::AscendingOrder);

    auto *headerView = m_table->horizontalHeader();
    headerView->setSectionResizeMode(FileTableModel::NameColumn, QHeaderView::Stretch);
    for (int column = FileTableModel::SizeColumn; column < FileTableModel::ColumnCount; ++column) {
        headerView->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    headerView->resizeSection(FileTableModel::SizeColumn, 65);
    headerView->resizeSection(FileTableModel::TypeColumn, 75);
    headerView->resizeSection(FileTableModel::ModifiedColumn, 125);
    headerView->resizeSection(FileTableModel::PermissionsColumn, 85);

    connect(m_pathCombo, &QComboBox::activated, this, [this](int index) {
        const QString text = m_pathCombo->itemText(index);
        if (!text.isEmpty())
            navigate(text);
    });
    connect(m_pathCombo->lineEdit(), &QLineEdit::returnPressed, this,
            [this] { navigate(m_pathCombo->currentText()); });
    connect(m_backButton, &QToolButton::clicked, this, &FilePane::goBack);
    connect(m_forwardButton, &QToolButton::clicked, this, &FilePane::goForward);
    connect(m_upButton, &QToolButton::clicked, this, &FilePane::goUp);
    connect(m_homeButton, &QToolButton::clicked, this, &FilePane::goHome);
    connect(m_refreshButton, &QToolButton::clicked, this, &FilePane::refresh);
    connect(m_connectButton, &QToolButton::clicked, this, &FilePane::connectRequested);
    connect(m_disconnectButton, &QToolButton::clicked, this, &FilePane::disconnectRequested);
    connect(m_terminalButton, &QToolButton::clicked, this, [this] {
        QString path = currentPath();
        const auto selected = selectedEntries();
        if (selected.size() == 1 && selected.first().isDir)
            path = selected.first().path;
        emit terminalRequested(path);
    });
    connect(m_hiddenButton, &QToolButton::clicked, this, [this] {
        setShowHidden(!m_showHidden);
        Settings::instance().showHiddenFiles = m_showHidden;
        Settings::instance().save();
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_proxy->setFilterText(text);
        updateStatus();
        updateEmptyState();
    });
    connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const FileInfo info = entryAtViewRow(index.row());
        if (!info.isValid())
            return;
        if (info.isDir) {
            navigate(info.path);
        } else {
            emit openRequested(info, isRemote());
        }
    });
    connect(m_table, &QTableView::customContextMenuRequested, this, &FilePane::showContextMenu);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] {
                updateStatus();
                emit selectionChanged();
            });
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this,
            [this](int) { updateStatus(); });

    auto *viewImpl = static_cast<FileTableView *>(m_table);
    viewImpl->canDropHandler = [this](const QMimeData *mime) {
        if (isRemote() && !isConnected())
            return false;
        return mime->hasFormat(QLatin1String(kEntryMimeType)) || mime->hasUrls();
    };
    viewImpl->dropHandler = [this](const QMimeData *mime, const QModelIndex &index) {
        const FileInfo target = entryAtViewRow(index.row());
        const QString targetDirectory = target.isValid() && target.isDir ? target.path : currentPath();
        if (mime->hasFormat(QLatin1String(kEntryMimeType))) {
            const QJsonDocument document =
                QJsonDocument::fromJson(mime->data(QLatin1String(kEntryMimeType)));
            if (!document.isObject())
                return;
            const QJsonObject root = document.object();
            const bool sourceRemote = root.value(QStringLiteral("sourceRemote")).toBool();
            QList<FileInfo> entries;
            for (const QJsonValue &value : root.value(QStringLiteral("entries")).toArray()) {
                if (value.isObject())
                    entries.append(infoFromJson(value.toObject()));
            }
            if (entries.isEmpty())
                return;
            if (sourceRemote == isRemote() && isRemote()) {
                emit statusMessage(QStringLiteral("暂不支持直接在服务器内复制文件。"));
                return;
            }
            emit transferDropped(entries, sourceRemote, targetDirectory, isRemote());
        } else if (mime->hasUrls()) {
            emit externalFilesDropped(mime->urls(), targetDirectory, isRemote());
        }
    };
    viewImpl->dragHandler = [this] { startDrag(); };
    viewImpl->keyHandler = [this](QKeyEvent *event) -> bool {
        const int key = event->key();
        const Qt::KeyboardModifiers modifiers = event->modifiers();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            activateCurrent();
            return true;
        }
        if (key == Qt::Key_Backspace && modifiers == Qt::NoModifier) {
            goUp();
            return true;
        }
        if (key == Qt::Key_Delete) {
            const auto entries = selectedEntries();
            if (!entries.isEmpty())
                emit deleteRequested(entries, isRemote());
            return true;
        }
        if (key == Qt::Key_F2) {
            const auto entries = selectedEntries();
            if (entries.size() == 1)
                emit renameRequested(entries.first(), isRemote());
            return true;
        }
        if (key == Qt::Key_F5) {
            refresh();
            return true;
        }
        if (key == Qt::Key_F7) {
            emit newFolderRequested(currentPath(), isRemote());
            return true;
        }
        if (key == Qt::Key_F3) {
            m_filterEdit->setFocus();
            m_filterEdit->selectAll();
            return true;
        }
        if (key == Qt::Key_L && modifiers.testFlag(Qt::ControlModifier)) {
            focusPathEditor();
            return true;
        }
        if (key == Qt::Key_Left && modifiers.testFlag(Qt::AltModifier)) {
            goBack();
            return true;
        }
        if (key == Qt::Key_Right && modifiers.testFlag(Qt::AltModifier)) {
            goForward();
            return true;
        }
        if (key == Qt::Key_Up && modifiers.testFlag(Qt::AltModifier)) {
            goUp();
            return true;
        }
        return false;
    };

    updateStatus();
    updateNavigationButtons();
}

void FilePane::buildWelcomePage()
{
    m_welcomePage = new QWidget(m_stack);
    m_welcomePage->setObjectName(QStringLiteral("WelcomePage"));
    auto *layout = new QVBoxLayout(m_welcomePage);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->setSpacing(10);
    layout->addStretch(1);

    m_welcomeIcon = new QLabel(m_welcomePage);
    m_welcomeIcon->setPixmap(
        IconProvider::instance().icon(QStringLiteral("connect"), Theme::accent()).pixmap(24, 24));
    m_welcomeIcon->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_welcomeIcon);

    m_welcomeTitle = new QLabel(QStringLiteral("连接到远程服务器"), m_welcomePage);
    m_welcomeTitle->setObjectName(QStringLiteral("WelcomeTitle"));
    m_welcomeTitle->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_welcomeTitle);

    m_welcomeText = new QLabel(
        QStringLiteral("使用 SFTP、SCP、FTP、FTPS 或 WebDAV 管理远程文件。\n连接后可在下方打开 SSH 终端。"),
        m_welcomePage);
    m_welcomeText->setObjectName(QStringLiteral("WelcomeText"));
    m_welcomeText->setAlignment(Qt::AlignCenter);
    m_welcomeText->setWordWrap(true);
    layout->addWidget(m_welcomeText);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    buttonRow->addStretch(1);
    m_newSessionButton = new QPushButton(QStringLiteral("新建会话"), m_welcomePage);
    m_newSessionButton->setProperty("accent", true);
    m_newSessionButton->setIcon(IconProvider::instance().icon(QStringLiteral("connect")));
    m_siteManagerButton = new QPushButton(QStringLiteral("站点管理器"), m_welcomePage);
    m_siteManagerButton->setIcon(IconProvider::instance().icon(QStringLiteral("site")));
    buttonRow->addWidget(m_newSessionButton);
    buttonRow->addWidget(m_siteManagerButton);
    buttonRow->addStretch(1);
    layout->addSpacing(4);
    layout->addLayout(buttonRow);
    layout->addStretch(2);

    connect(m_newSessionButton, &QPushButton::clicked, this, &FilePane::newSessionRequested);
    connect(m_siteManagerButton, &QPushButton::clicked, this, &FilePane::siteManagerRequested);
    m_stack->addWidget(m_welcomePage);
}

void FilePane::applyRoleStyle()
{
    if (m_role == Role::Remote) {
        m_badgeLabel->setProperty("role", QStringLiteral("remote"));
        m_welcomePage->setVisible(true);
    } else {
        m_welcomePage->setVisible(false);
    }
    m_hiddenButton->setIcon(IconProvider::instance().icon(
        m_showHidden ? QStringLiteral("eye") : QStringLiteral("eye-off")));
    m_hiddenButton->setToolTip(m_showHidden ? QStringLiteral("隐藏隐藏文件")
                                            : QStringLiteral("显示隐藏文件"));
}

void FilePane::attachController(FileSystemController *controller)
{
    if (m_role != Role::Remote || !controller)
        return;
    m_controller = controller;
    connect(controller, &FileSystemController::connected, this,
            [this](const SessionData &session, const QString &home, const QString &initial) {
                setRemoteSession(session);
                showRemoteConnected(home, initial);
            });
    connect(controller, &FileSystemController::connectFailed, this,
            [this](const QString &message) { showRemoteConnectFailed(message); });
    connect(controller, &FileSystemController::disconnected, this,
            [this](const QString &reason) { showRemoteDisconnected(reason); });
    connect(controller, &FileSystemController::directoryLoading, this,
            [this](const QString &path) { setLoading(true, path); });
    connect(controller, &FileSystemController::directoryListed, this,
            &FilePane::applyRemoteListing);
    connect(controller, &FileSystemController::directoryFailed, this,
            [this](const QString &path, const QString &message) {
                setLoading(false);
                m_refreshError = message;
                updateAutoRefreshButton();
                m_pendingHistory = false;
                m_pendingRemotePath.clear();
                m_statusLabel->setText(QStringLiteral("无法读取 %1：%2")
                                           .arg(path, message.section(QLatin1Char('\n'), 0, 0)));
                m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::danger().name()));
                emit statusMessage(message);
            });
    connect(controller, &FileSystemController::operationFinished, this,
            [this](const QString &action, bool success, const QString &message) {
                if (!success)
                    emit statusMessage(QStringLiteral("%1失败：%2").arg(action, message));
            });
}

void FilePane::setRemoteSession(const SessionData &session)
{
    m_session = session;
    if (m_role != Role::Remote)
        return;
    m_badgeLabel->setText(protocolDisplayName(session.protocol));
    m_titleLabel->setText(session.rootLabel());
    m_titleLabel->setToolTip(session.url());
    m_connectButton->hide();
    m_disconnectButton->show();
    m_terminalButton->show();
    m_connectButton->setEnabled(false);
    updateHeader();
}

void FilePane::setRemoteConnecting(const SessionData &session)
{
    if (m_role != Role::Remote)
        return;
    m_session = session;
    m_lastRemoteRefresh = {};
    m_refreshError.clear();
    m_remotePath.clear();
    m_history.clear();
    m_historyIndex = -1;
    m_connecting = true;
    m_remoteConnected = false;
    m_badgeLabel->setText(QStringLiteral("连接中"));
    m_titleLabel->setText(session.displayName());
    m_connectButton->setEnabled(false);
    m_connectButton->show();
    m_disconnectButton->hide();
    m_terminalButton->hide();
    m_pathCombo->setEnabled(false);
    showWelcome(true, QStringLiteral("正在连接 %1 …").arg(session.endpoint()));
    setLoading(true, QString());
}

void FilePane::showRemoteConnected(const QString &home, const QString &initialPath)
{
    Q_UNUSED(initialPath);
    if (m_role != Role::Remote)
        return;
    m_remoteHome = home;
    m_remoteConnected = true;
    m_connecting = false;
    m_pathCombo->setEnabled(true);
    m_terminalButton->show();
    m_connectButton->hide();
    m_disconnectButton->show();
    setLoading(true, m_pendingRemotePath);
    updateHeader();
    updateNavigationButtons();
}

void FilePane::showRemoteConnectFailed(const QString &message)
{
    if (m_role != Role::Remote)
        return;
    m_connecting = false;
    m_remoteConnected = false;
    m_pathCombo->setEnabled(false);
    m_terminalButton->hide();
    m_connectButton->show();
    m_connectButton->setEnabled(true);
    m_disconnectButton->hide();
    m_badgeLabel->setText(QStringLiteral("连接失败"));
    m_titleLabel->setText(QStringLiteral("远程文件"));
    m_stack->setCurrentWidget(m_welcomePage);
    showWelcome(false, message);
    setLoading(false);
    m_statusLabel->setText(message.section(QLatin1Char('\n'), 0, 0));
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::danger().name()));
    updateNavigationButtons();
    emit statusMessage(message);
}

void FilePane::showRemoteDisconnected(const QString &reason)
{
    if (m_role != Role::Remote)
        return;
    m_remoteConnected = false;
    m_connecting = false;
    m_pendingHistory = false;
    m_pendingRemotePath.clear();
    m_remotePath.clear();
    m_model->setEntries({});
    m_stack->setCurrentWidget(m_welcomePage);
    m_pathCombo->setEditText(QString());
    m_breadcrumbs->clear();
    m_pathCombo->setEnabled(false);
    m_badgeLabel->setText(QStringLiteral("远程"));
    m_titleLabel->setText(QStringLiteral("远程文件"));
    m_titleLabel->setToolTip(QString());
    m_connectButton->show();
    m_connectButton->setEnabled(true);
    m_disconnectButton->hide();
    m_terminalButton->hide();
    showWelcome(false, reason.isEmpty()
                             ? QStringLiteral("选择“新建会话”，或使用站点管理器连接服务器。")
                             : reason);
    setLoading(false);
    updateNavigationButtons();
    if (!reason.isEmpty())
        emit statusMessage(reason);
}

bool FilePane::isConnected() const
{
    return m_role == Role::Local || m_remoteConnected;
}

QString FilePane::currentPath() const
{
    return isRemote() ? m_remotePath : m_localPath;
}

QString FilePane::title() const
{
    return m_titleLabel ? m_titleLabel->text() : QString();
}

void FilePane::showWelcome(bool connecting, const QString &message)
{
    m_stack->setCurrentWidget(m_welcomePage);
    if (connecting) {
        m_welcomeTitle->setText(QStringLiteral("正在连接…"));
        m_welcomeText->setText(message.isEmpty() ? QStringLiteral("正在建立安全连接，请稍候。") : message);
        m_newSessionButton->hide();
        m_siteManagerButton->hide();
        m_welcomeIcon->setPixmap(
            IconProvider::instance().icon(QStringLiteral("sync"), Theme::accent()).pixmap(24, 24));
    } else {
        m_welcomeTitle->setText(QStringLiteral("连接到远程服务器"));
        m_welcomeText->setText(message.isEmpty()
                                   ? QStringLiteral("使用 SFTP、SCP、FTP、FTPS 或 WebDAV 管理远程文件。\n"
                                                    "连接后可在下方打开 SSH 终端。")
                                   : message);
        m_newSessionButton->show();
        m_siteManagerButton->show();
        m_welcomeIcon->setPixmap(
            IconProvider::instance().icon(QStringLiteral("connect"), Theme::accent()).pixmap(24, 24));
    }
}

void FilePane::showTablePage()
{
    m_stack->setCurrentIndex(0);
}

void FilePane::setLoading(bool loading, const QString &path)
{
    m_loading = loading;
    if (loading && m_backgroundRefresh)
        return;
    if (!loading)
        m_backgroundRefresh = false;
    m_loadingBar->setVisible(loading);
    if (loading) {
        if (!path.isEmpty())
            m_statusLabel->setText(QStringLiteral("正在加载 %1 …").arg(path));
        else
            m_statusLabel->setText(QStringLiteral("正在连接…"));
        m_statusLabel->setStyleSheet(QString());
        m_emptyLabel->setText(QStringLiteral("正在加载…"));
        updateEmptyState();
    } else {
        updateStatus();
        updateEmptyState();
    }
}

void FilePane::applyRemoteListing(const QString &path, const QList<FileInfo> &entries)
{
    if (m_role != Role::Remote)
        return;
    const QString normalized = path::normalizeRemote(path);
    const bool sameDirectory = m_remotePath == normalized && !m_remotePath.isEmpty();
    const int scroll = m_table->verticalScrollBar()->value();
    m_remotePath = normalized;
    m_lastRemoteRefresh = QDateTime::currentDateTime();
    m_refreshError.clear();
    if (sameDirectory) {
        m_model->updateEntries(withParent(entries, normalized, true));
    } else {
        m_model->setEntries(withParent(entries, normalized, true));
        m_table->clearSelection();
        m_table->setCurrentIndex(QModelIndex());
    }
    m_proxy->setShowHidden(m_showHidden);
    m_stack->setCurrentIndex(0);
    updateHeader();
    if (!sameDirectory)
        updateCombo(normalized);
    updateStatus();
    updateEmptyState();
    setLoading(false);
    updateNavigationButtons();
    if (m_pendingHistory) {
        addHistory(normalized);
        m_pendingHistory = false;
        m_pendingRemotePath.clear();
    } else if (m_history.isEmpty()) {
        addHistory(normalized);
    }
    if (sameDirectory) {
        m_table->verticalScrollBar()->setValue(scroll);
    }
    else
        emit pathChanged(normalized, true);
}

void FilePane::navigate(const QString &pathValue)
{
    if (isRemote())
        navigateRemote(pathValue);
    else
        navigateLocal(pathValue);
}

void FilePane::navigateLocal(const QString &pathValue)
{
    loadLocalDirectory(pathValue, true);
}

FilePane::~FilePane()
{
    ++*m_localRequest;
}

void FilePane::loadLocalDirectory(const QString &pathValue, bool addToHistory)
{
    QString input = normalizedLocalInput(pathValue);
    if (input.isEmpty())
        input = QDir::homePath();
    const quint64 request = ++*m_localRequest;
    const auto latest = m_localRequest;
    setLoading(true, input);
    struct Listing {
        QString path;
        QString error;
        QString storage;
        FileList entries;
    };
    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcher<Listing>::finished, this, [this, watcher, latest, request, addToHistory] {
        Listing result = watcher->future().takeResult();
        watcher->deleteLater();
        if (latest->load() != request)
            return;
        if (!result.error.isEmpty()) {
            setLoading(false);
            m_pathCombo->setEditText(localDisplay(m_localPath));
            emit statusMessage(result.error);
            return;
        }
        m_localPath = result.path;
        m_storageStatus = result.storage;
        m_model->setEntries(withParent(std::move(result.entries), m_localPath, false));
        m_table->clearSelection();
        m_table->setCurrentIndex(QModelIndex());
        m_stack->setCurrentIndex(0);
        updateHeader();
        updateCombo(localDisplay(m_localPath));
        setLoading(false);
        if (addToHistory)
            addHistory(m_localPath);
        updateNavigationButtons();
        Settings::instance().lastLocalDirectory = m_localPath;
        emit pathChanged(m_localPath, false);
    });
    watcher->setFuture(QtConcurrent::run([input, latest, request] {
        Listing result;
        QFileInfo info(input);
        result.path = info.isFile() ? info.absolutePath() : QDir::cleanPath(info.absoluteFilePath());
        QDir directory(result.path);
        if (!directory.exists() || !directory.isReadable()) {
            result.error = QStringLiteral("目录不存在或无法读取：%1").arg(localDisplay(result.path));
            return result;
        }
        QDirIterator files(result.path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (files.hasNext()) {
            if (latest->load() != request)
                return Listing{};
            const FileInfo entry = LocalBackend::infoForPath(files.next());
            if (entry.isValid())
                result.entries.append(entry);
        }
        const QStorageInfo storage(result.path);
        if (storage.isValid())
            result.storage = QStringLiteral(" · 可用 %1 / %2")
                                 .arg(path::formatSize(storage.bytesAvailable()), path::formatSize(storage.bytesTotal()));
        return result;
    }));
}

void FilePane::navigateRemote(const QString &pathValue)
{
    if (!m_controller)
        return;
    if (!m_remoteConnected) {
        emit connectRequested();
        return;
    }
    QString target = pathValue;
    if (target.isEmpty())
        target = m_remoteHome;
    else if (!target.startsWith(QLatin1Char('/')))
        target = path::remoteJoin(path::normalizeRemote(m_remotePath), target);
    target = path::normalizeRemote(target);
    m_pendingHistory = true;
    m_pendingRemotePath = target;
    m_backgroundRefresh = false;
    setLoading(true, target);
    m_controller->listDirectory(target);
}

void FilePane::goUp()
{
    if (isRemote()) {
        if (!m_remoteConnected)
            return;
        if (path::remoteIsRoot(m_remotePath))
            return;
        navigateRemote(path::remoteParent(m_remotePath));
    } else {
        QDir dir(m_localPath);
        if (!dir.cdUp())
            return;
        navigateLocal(dir.absolutePath());
    }
}

void FilePane::goHome()
{
    if (isRemote()) {
        if (!m_remoteConnected)
            return;
        navigateRemote(m_remoteHome.isEmpty() ? QStringLiteral("/") : m_remoteHome);
    } else {
        navigateLocal(QDir::homePath());
    }
}

void FilePane::refresh()
{
    if (isRemote()) {
        refreshRemote(false);
    } else {
        const QString path = m_localPath;
        navigateLocal(path);
    }
}

void FilePane::refreshRemote(bool background)
{
    if (!m_remoteConnected || m_loading || !m_controller || m_controller->isBusy() || m_remotePath.isEmpty())
        return;
    if (background && (QApplication::mouseButtons() != Qt::NoButton || QApplication::activePopupWidget()))
        return;
    m_pendingHistory = false;
    m_pendingRemotePath = m_remotePath;
    m_backgroundRefresh = background;
    setLoading(true, m_remotePath);
    m_controller->listDirectory(m_remotePath);
}

void FilePane::focusFileList()
{
    m_table->setFocus();
    if (!m_table->selectionModel()->hasSelection() && m_proxy->rowCount() > 0)
        m_table->selectRow(0);
}

void FilePane::focusPathEditor()
{
    m_pathStack->setCurrentIndex(1);
    m_pathCombo->setFocus();
    m_pathCombo->lineEdit()->selectAll();
}

void FilePane::setFilterText(const QString &text)
{
    m_filterEdit->setText(text);
}

QList<FileInfo> FilePane::selectedEntries() const
{
    QList<FileInfo> entries;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        const FileInfo info = entryAtViewRow(index.row());
        if (info.isValid() && !info.isParent)
            entries.append(info);
    }
    return entries;
}

QStringList FilePane::selectedPaths() const
{
    QStringList paths;
    for (const FileInfo &entry : selectedEntries())
        paths.append(isRemote() ? entry.path : localDisplay(entry.path));
    return paths;
}

FileInfo FilePane::currentEntry() const
{
    const QModelIndex index = m_table->currentIndex();
    return entryAtViewRow(index.row());
}

FileInfo FilePane::entryAtViewRow(int row) const
{
    if (!m_proxy || row < 0)
        return {};
    const QModelIndex proxyIndex = m_proxy->index(row, FileTableModel::NameColumn);
    if (!proxyIndex.isValid())
        return {};
    const QModelIndex sourceIndex = m_proxy->mapToSource(proxyIndex);
    return m_model->entryAt(sourceIndex.row());
}

void FilePane::selectByName(const QStringList &names)
{
    if (names.isEmpty())
        return;
    const QSet<QString> wanted(names.begin(), names.end());
    QItemSelection selection;
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        const FileInfo info = entryAtViewRow(row);
        if (!wanted.contains(info.name))
            continue;
        const QModelIndex index = m_proxy->index(row, FileTableModel::NameColumn);
        selection.select(index, index);
    }
    if (!selection.isEmpty())
        m_table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
}

void FilePane::updateFileIconTheme()
{
    m_hiddenButton->setIcon(IconProvider::instance().icon(
        m_showHidden ? QStringLiteral("eye") : QStringLiteral("eye-off")));
    if (m_model && m_model->rowCount() > 0) {
        emit m_model->dataChanged(m_model->index(0, 0),
                                  m_model->index(m_model->rowCount() - 1,
                                                 FileTableModel::ColumnCount - 1));
    }
    m_emptyLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent; font-size: 9pt;")
                                    .arg(Theme::muted().name()));
    m_filterEdit->actions().first()->setIcon(
        IconProvider::instance().icon(QStringLiteral("search"), Theme::muted()));
    m_welcomeIcon->setPixmap(IconProvider::instance().icon(QStringLiteral("connect"), Theme::accent())
                                .pixmap(QSize(24, 24), devicePixelRatioF()));
    updateBreadcrumbs(isRemote() ? m_remotePath : localDisplay(m_localPath));
}

void FilePane::setActive(bool active)
{
    if (property("active").isValid() && property("active").toBool() == active)
        return;
    setProperty("active", active);
    auto *header = findChild<QFrame *>(QStringLiteral("PaneHeader"));
    for (QWidget *widget : {static_cast<QWidget *>(this), static_cast<QWidget *>(header)}) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }
}

void FilePane::setShowHidden(bool show)
{
    m_showHidden = show;
    m_proxy->setShowHidden(show);
    m_hiddenButton->setIcon(IconProvider::instance().icon(
        show ? QStringLiteral("eye") : QStringLiteral("eye-off")));
    m_hiddenButton->setToolTip(show ? QStringLiteral("隐藏隐藏文件")
                                    : QStringLiteral("显示隐藏文件"));
    updateStatus();
    updateEmptyState();
}

bool FilePane::showHidden() const
{
    return m_showHidden;
}

void FilePane::updateHeader()
{
    if (isRemote()) {
        if (!m_remoteConnected) {
            m_pathCombo->setEnabled(false);
            return;
        }
        m_badgeLabel->setText(protocolDisplayName(m_session.protocol));
        m_titleLabel->setText(m_session.rootLabel());
        m_titleLabel->setToolTip(m_session.url());
        m_pathCombo->setEnabled(true);
    } else {
        m_badgeLabel->setText(QStringLiteral("本地"));
        m_titleLabel->setText(QStringLiteral("本地文件"));
        m_titleLabel->setToolTip(localDisplay(m_localPath));
    }
}

void FilePane::updateBreadcrumbs(const QString &path)
{
    QString normalized = QDir::fromNativeSeparators(path);
    QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString target = normalized.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString();
    QStringList links;
    const auto link = [](const QString &name, const QString &value) {
        return QStringLiteral("<a href=\"%1\" style=\"color:%3;text-decoration:none\">%2</a>")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(value)), name.toHtmlEscaped(), Theme::text().name());
    };
    if (!target.isEmpty())
        links.append(link(QStringLiteral("/"), target));
    if (normalized.startsWith(QStringLiteral("//"))) {
        target = QStringLiteral("//") + parts.takeFirst();
        if (!parts.isEmpty())
            target += QLatin1Char('/') + parts.takeFirst();
        links = {link(isRemote() ? target : QDir::toNativeSeparators(target), target)};
    }
    for (const QString &part : parts) {
        if (!target.isEmpty() && !target.endsWith(QLatin1Char('/')))
            target += QLatin1Char('/');
        target += part;
        if (target.endsWith(QLatin1Char(':')))
            target += QLatin1Char('/');
        const QString label = !isRemote() && parts.size() == 1 && part.endsWith(QLatin1Char(':'))
            ? part + QLatin1Char('\\') : part;
        links.append(link(label, target));
    }
    const QString separator = isRemote() ? QStringLiteral("/") : QStringLiteral("\\");
    const QString breadcrumb = normalized.startsWith(QLatin1Char('/')) && !normalized.startsWith(QStringLiteral("//"))
        ? links.value(0) + links.mid(1).join(separator) : links.join(separator);
    m_breadcrumbs->setText(breadcrumb);
    m_breadcrumbs->setToolTip(path);
}

void FilePane::updateCombo(const QString &path)
{
    if (!m_pathCombo)
        return;
    updateBreadcrumbs(path);
    m_pathStack->setCurrentIndex(0);
    const QSignalBlocker blocker(m_pathCombo);
    m_pathCombo->setEditText(path);
    int existing = -1;
    for (int index = 0; index < m_pathCombo->count(); ++index) {
        if (m_pathCombo->itemText(index) == path) {
            existing = index;
            break;
        }
    }
    if (existing >= 0)
        m_pathCombo->removeItem(existing);
    m_pathCombo->insertItem(0, path);
    m_pathCombo->setCurrentIndex(0);
    while (m_pathCombo->count() > 25)
        m_pathCombo->removeItem(m_pathCombo->count() - 1);
}

void FilePane::updateStatus()
{
    if (!m_statusLabel)
        return;
    if (m_loading)
        return;
    const int total = m_model->rowCount();
    const int visible = m_proxy->rowCount();
    const int folders = m_model->folderCount();
    const int files = m_model->fileCount();
    const auto selected = selectedEntries();
    qint64 selectedSize = 0;
    for (const FileInfo &entry : selected) {
        if (!entry.isDir)
            selectedSize += entry.size;
    }

    QString text;
    if (!selected.isEmpty()) {
        text = QStringLiteral("已选 %1 项").arg(selected.size());
        if (selectedSize > 0)
            text += QStringLiteral(" · %1").arg(path::formatSize(selectedSize));
    } else {
        text = QStringLiteral("%1 个文件夹，%2 个文件").arg(folders).arg(files);
        const qint64 size = m_model->totalFileSize();
        if (size > 0)
            text += QStringLiteral(" · %1").arg(path::formatSize(size));
        if (visible != total)
            text += QStringLiteral(" · 显示 %1/%2").arg(visible).arg(total);
    }

    if (!isRemote())
        text += m_storageStatus;
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(QString());
    m_statusLabel->setToolTip(text);
}

void FilePane::updateEmptyState()
{
    if (!m_emptyLabel || !m_table)
        return;
    const bool empty = m_proxy->rowCount() == 0 && m_stack->currentIndex() == 0;
    if (empty) {
        m_emptyLabel->setText(m_filterEdit && !m_filterEdit->text().trimmed().isEmpty()
                                  ? QStringLiteral("没有匹配的项目")
                                  : (m_loading ? QStringLiteral("正在加载…")
                                               : QStringLiteral("此文件夹为空")));
        m_emptyLabel->setGeometry(m_table->viewport()->rect());
        m_emptyLabel->raise();
        m_emptyLabel->show();
    } else {
        m_emptyLabel->hide();
    }
}

void FilePane::updateAutoRefreshButton()
{
    if (!m_autoRefreshButton)
        return;
    const bool enabled = m_autoRefreshButton->isChecked();
    m_autoRefreshButton->setText(!enabled ? QStringLiteral("自动刷新：关")
        : (m_refreshError.isEmpty() ? QStringLiteral("自动刷新：开") : QStringLiteral("自动刷新：重试")));
    QString tooltip = QStringLiteral("开启后每 2 秒读取服务器上的当前目录；关闭后停止自动刷新。");
    if (m_lastRemoteRefresh.isValid())
        tooltip += QStringLiteral("\n上次成功刷新：%1").arg(m_lastRemoteRefresh.toString(QStringLiteral("HH:mm:ss")));
    if (!m_refreshError.isEmpty())
        tooltip += QStringLiteral("\n最近错误：%1").arg(m_refreshError);
    m_autoRefreshButton->setToolTip(tooltip);
    if (enabled && m_remoteConnected) {
        if (!m_autoRefreshTimer->isActive())
            m_autoRefreshTimer->start();
    } else {
        m_autoRefreshTimer->stop();
    }
}

void FilePane::updateNavigationButtons()
{
    const bool canBack = m_historyIndex > 0;
    const bool canForward = m_historyIndex >= 0 && m_historyIndex + 1 < m_history.size();
    m_backButton->setEnabled(canBack);
    m_forwardButton->setEnabled(canForward);
    if (isRemote()) {
        m_upButton->setEnabled(m_remoteConnected && !path::remoteIsRoot(m_remotePath));
        m_homeButton->setEnabled(m_remoteConnected);
        m_refreshButton->setEnabled(m_remoteConnected);
        m_pathCombo->setEnabled(m_remoteConnected);
        m_pathStack->setEnabled(m_remoteConnected);
        m_autoRefreshButton->setEnabled(m_remoteConnected);
        updateAutoRefreshButton();
        m_disconnectButton->setVisible(m_remoteConnected);
        m_connectButton->setVisible(!m_remoteConnected && !m_connecting);
        m_connectButton->setEnabled(!m_connecting);
        m_terminalButton->setVisible(m_remoteConnected);
        m_terminalButton->setEnabled(m_remoteConnected && protocolUsesSsh(m_session.protocol));
    } else {
        m_upButton->setEnabled(m_localPath != QDir::rootPath());
        m_homeButton->setEnabled(true);
        m_refreshButton->setEnabled(true);
    }
}

void FilePane::addHistory(const QString &path)
{
    if (path.isEmpty())
        return;
    if (m_historyIndex >= 0 && m_historyIndex < m_history.size() &&
        m_history.at(m_historyIndex) == path) {
        updateNavigationButtons();
        return;
    }
    while (m_history.size() > m_historyIndex + 1)
        m_history.removeLast();
    m_history.append(path);
    if (m_history.size() > 60)
        m_history.removeFirst();
    m_historyIndex = m_history.size() - 1;
    updateNavigationButtons();
}

void FilePane::goBack()
{
    if (m_historyIndex <= 0)
        return;
    const QString target = m_history.at(m_historyIndex - 1);
    --m_historyIndex;
    if (isRemote()) {
        m_pendingHistory = false;
        setLoading(true, target);
        m_controller->listDirectory(target);
    } else {
        loadLocalDirectory(target, false);
    }
    updateNavigationButtons();
}

void FilePane::goForward()
{
    if (m_historyIndex < 0 || m_historyIndex + 1 >= m_history.size())
        return;
    const QString target = m_history.at(m_historyIndex + 1);
    ++m_historyIndex;
    if (isRemote()) {
        m_pendingHistory = false;
        setLoading(true, target);
        m_controller->listDirectory(target);
    } else {
        loadLocalDirectory(target, false);
    }
    updateNavigationButtons();
}

void FilePane::activateCurrent()
{
    const FileInfo info = currentEntry();
    if (!info.isValid())
        return;
    if (info.isDir) {
        navigate(info.path);
    } else {
        emit openRequested(info, isRemote());
    }
}

void FilePane::showContextMenu(const QPoint &position)
{
    const QModelIndex clicked = m_table->indexAt(position);
    if (clicked.isValid() && !m_table->selectionModel()->isSelected(clicked))
        m_table->selectRow(clicked.row());
    const FileInfo clickedEntry = entryAtViewRow(clicked.row());
    if (clickedEntry.isParent) {
        QMenu menu(this);
        auto *up = menu.addAction(QStringLiteral("上一级目录"));
        if (menu.exec(m_table->viewport()->mapToGlobal(position)) == up)
            goUp();
        return;
    }
    const QList<FileInfo> entries = selectedEntries();
    const FileInfo single = entries.size() == 1 ? entries.first() : FileInfo();

    QMenu menu(this);
    auto addAction = [&menu](const QIcon &icon, const QString &text, const QString &shortcut = {}) {
        QAction *action = menu.addAction(icon, text);
        if (!shortcut.isEmpty())
            action->setShortcut(QKeySequence(shortcut));
        return action;
    };

    QAction *openAction = addAction(IconProvider::instance().icon(QStringLiteral("open")),
                                    single.isValid() && single.isDir ? QStringLiteral("打开文件夹")
                                                                     : QStringLiteral("打开"));
    openAction->setEnabled(single.isValid());
    QAction *editAction = nullptr;
    if (isRemote() && single.isValid() && !single.isDir) {
        editAction = addAction(IconProvider::instance().icon(QStringLiteral("edit")),
                               QStringLiteral("编辑"));
    }
    menu.addSeparator();
    QAction *transferAction = nullptr;
    if (isRemote()) {
        transferAction = addAction(IconProvider::instance().icon(QStringLiteral("download")),
                                   QStringLiteral("下载"));
        transferAction->setEnabled(!entries.isEmpty());
    } else {
        transferAction = addAction(IconProvider::instance().icon(QStringLiteral("upload")),
                                   QStringLiteral("上传"));
        transferAction->setEnabled(!entries.isEmpty());
    }
    QAction *newFolderAction = addAction(IconProvider::instance().icon(QStringLiteral("newfolder")),
                                         QStringLiteral("新建文件夹"));
    QAction *renameAction = addAction(IconProvider::instance().icon(QStringLiteral("rename")),
                                      QStringLiteral("重命名"), QStringLiteral("F2"));
    renameAction->setEnabled(entries.size() == 1);
    QAction *deleteAction = addAction(IconProvider::instance().icon(QStringLiteral("delete")),
                                      QStringLiteral("删除"), QStringLiteral("Del"));
    deleteAction->setEnabled(!entries.isEmpty());
    menu.addSeparator();
    QAction *propertiesAction = addAction(IconProvider::instance().icon(QStringLiteral("properties")),
                                          QStringLiteral("属性"));
    propertiesAction->setEnabled(entries.size() == 1);
    QAction *terminalAction = nullptr;
    QAction *explorerAction = nullptr;
    QAction *copyPathAction = addAction(IconProvider::instance().icon(QStringLiteral("copy")),
                                        QStringLiteral("复制路径"));
    copyPathAction->setEnabled(!entries.isEmpty());
    if (isRemote() && protocolUsesSsh(m_session.protocol)) {
        terminalAction = addAction(IconProvider::instance().icon(QStringLiteral("terminal")),
                                   QStringLiteral("在终端中打开"));
        terminalAction->setEnabled(m_remoteConnected);
    }
    if (!isRemote()) {
        explorerAction = addAction(IconProvider::instance().icon(QStringLiteral("folder")),
                                   QStringLiteral("在资源管理器中显示"));
        explorerAction->setEnabled(single.isValid());
    }
    menu.addSeparator();
    QAction *refreshAction = addAction(IconProvider::instance().icon(QStringLiteral("refresh")),
                                       QStringLiteral("刷新"), QStringLiteral("F5"));

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (!chosen)
        return;
    if (chosen == openAction) {
        activateCurrent();
    } else if (editAction && chosen == editAction) {
        emit editRequested(single);
    } else if (chosen == transferAction) {
        if (isRemote())
            emit downloadRequested(entries);
        else
            emit uploadRequested(entries);
    } else if (chosen == newFolderAction) {
        emit newFolderRequested(currentPath(), isRemote());
    } else if (chosen == renameAction) {
        emit renameRequested(single, isRemote());
    } else if (chosen == deleteAction) {
        emit deleteRequested(entries, isRemote());
    } else if (chosen == propertiesAction) {
        emit propertiesRequested(single, isRemote());
    } else if (terminalAction && chosen == terminalAction) {
        QString path = currentPath();
        if (single.isValid())
            path = single.path;
        emit terminalRequested(path);
    } else if (explorerAction && chosen == explorerAction) {
        const QStringList selection = selectedPaths();
        if (!selection.isEmpty()) {
            QProcess::startDetached(QStringLiteral("explorer.exe"),
                                    {QStringLiteral("/select,") +
                                     QDir::toNativeSeparators(selection.first())});
        } else {
            QDesktopServices::openUrl(QUrl::fromLocalFile(currentPath()));
        }
    } else if (chosen == copyPathAction) {
        QApplication::clipboard()->setText(selectedPaths().join(QLatin1Char('\n')));
    } else if (chosen == refreshAction) {
        refresh();
    }
}

void FilePane::startDrag()
{
    const QList<FileInfo> entries = selectedEntries();
    if (entries.isEmpty())
        return;
    auto *mime = new QMimeData();
    QJsonObject root;
    root.insert(QStringLiteral("sourceRemote"), isRemote());
    root.insert(QStringLiteral("sourceDirectory"), currentPath());
    root.insert(QStringLiteral("protocol"), protocolId(m_session.protocol));
    QJsonArray array;
    for (const FileInfo &entry : entries)
        array.append(infoToJson(entry));
    root.insert(QStringLiteral("entries"), array);
    mime->setData(QLatin1String(kEntryMimeType), QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!isRemote()) {
        QList<QUrl> urls;
        for (const FileInfo &entry : entries)
            urls.append(QUrl::fromLocalFile(entry.path));
        mime->setUrls(urls);
    } else {
        mime->setText(selectedPaths().join(QLatin1Char('\n')));
    }

    auto *drag = new QDrag(m_table);
    drag->setMimeData(mime);
    const QVariant decoration = m_model->data(
        m_proxy->mapToSource(m_proxy->index(m_table->currentIndex().row(), FileTableModel::NameColumn)),
        Qt::DecorationRole);
    if (decoration.canConvert<QIcon>()) {
        const QIcon icon = decoration.value<QIcon>();
        drag->setPixmap(icon.pixmap(36, 36));
    }
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

void FilePane::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    if (m_emptyLabel && m_table)
        m_emptyLabel->setGeometry(m_table->viewport()->rect());
}

void FilePane::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        updateFileIconTheme();
}

} // namespace wsc
