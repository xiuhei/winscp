#include "ui/SiteManagerDialog.h"

#include "core/Settings.h"
#include "ui/IconProvider.h"
#include "ui/SessionEditor.h"
#include "ui/Theme.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

namespace wsc {

SiteManagerDialog::SiteManagerDialog(const SessionData &initial, QWidget *parent)
    : QDialog(parent)
{
    Q_UNUSED(initial);
    setWindowTitle(QStringLiteral("站点管理器 - WinSCP Qt"));
    setWindowIcon(IconProvider::instance().appIcon());
    resize(960, 680);
    setMinimumSize(780, 540);

    SiteStore::instance().load();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(12);

    auto *header = new QHBoxLayout();
    auto *icon = new QLabel(this);
    icon->setPixmap(IconProvider::instance().icon(QStringLiteral("site"), Theme::accent()).pixmap(24, 24));
    auto *title = new QLabel(QStringLiteral("站点管理器"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(10);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto *subtitle = new QLabel(QStringLiteral("保存常用服务器，快速连接并自动打开终端"), this);
    subtitle->setStyleSheet(QStringLiteral("color: #8492a6;"));
    auto *titleColumn = new QVBoxLayout();
    titleColumn->setSpacing(2);
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitle);
    header->addWidget(icon);
    header->addSpacing(8);
    header->addLayout(titleColumn, 1);
    layout->addLayout(header);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto *leftPanel = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 8, 0);
    leftLayout->setSpacing(6);
    m_searchEdit = new QLineEdit(leftPanel);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索站点…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->addAction(IconProvider::instance().icon(QStringLiteral("search"), Theme::muted()),
                            QLineEdit::LeadingPosition);
    m_list = new QListWidget(leftPanel);
    m_list->setIconSize(QSize(16, 16));
    m_list->setAlternatingRowColors(true);
    auto *leftButtons = new QHBoxLayout();
    auto *newButton = new QPushButton(QStringLiteral("新建"), leftPanel);
    newButton->setIcon(IconProvider::instance().icon(QStringLiteral("newfolder")));
    auto *deleteButton = new QPushButton(QStringLiteral("删除"), leftPanel);
    deleteButton->setIcon(IconProvider::instance().icon(QStringLiteral("delete")));
    leftButtons->addWidget(newButton);
    leftButtons->addWidget(deleteButton);
    leftButtons->addStretch(1);
    leftLayout->addWidget(m_searchEdit);
    leftLayout->addWidget(m_list, 1);
    leftLayout->addLayout(leftButtons);

    auto *rightScroll = new QScrollArea(splitter);
    rightScroll->setWidgetResizable(true);
    rightScroll->setFrameShape(QFrame::NoFrame);
    rightScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor = new SessionEditor(rightScroll);
    m_editor->setSiteNameVisible(true);
    rightScroll->setWidget(m_editor);

    splitter->addWidget(leftPanel);
    splitter->addWidget(rightScroll);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 640});
    layout->addWidget(splitter, 1);

    auto *footer = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    footer->addWidget(m_statusLabel, 1);
    auto *saveButton = new QPushButton(QStringLiteral("保存"), this);
    saveButton->setIcon(IconProvider::instance().icon(QStringLiteral("file")));
    auto *connectButton = new QPushButton(QStringLiteral("连接"), this);
    connectButton->setProperty("accent", true);
    connectButton->setIcon(IconProvider::instance().icon(QStringLiteral("connect"), Qt::white));
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    footer->addWidget(saveButton);
    footer->addWidget(connectButton);
    footer->addWidget(closeButton);
    layout->addLayout(footer);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] { refreshList(); });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            loadSite(row);
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        connectSelected();
    });
    connect(newButton, &QPushButton::clicked, this, &SiteManagerDialog::newSite);
    connect(deleteButton, &QPushButton::clicked, this, &SiteManagerDialog::deleteSite);
    connect(saveButton, &QPushButton::clicked, this, &SiteManagerDialog::saveCurrent);
    connect(connectButton, &QPushButton::clicked, this, &SiteManagerDialog::connectSelected);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    refreshList();
    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    } else {
        SessionData session;
        session.protocol = Protocol::Sftp;
        session.port = 22;
        session.siteName = QStringLiteral("新站点");
        m_editor->setSession(session);
        m_editor->setSiteNameVisible(true);
        m_statusLabel->setText(QStringLiteral("尚无保存的站点，填写信息后点击“保存”。"));
    }
}

void SiteManagerDialog::refreshList(const QString &selectName)
{
    const QString search = m_searchEdit->text().trimmed();
    const QString previouslySelected = !selectName.isEmpty()
        ? selectName
        : (m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString());
    m_loaded = false;
    m_list->clear();
    for (const SessionData &site : SiteStore::instance().sites) {
        if (!search.isEmpty() &&
            !site.siteName.contains(search, Qt::CaseInsensitive) &&
            !site.hostName.contains(search, Qt::CaseInsensitive) &&
            !site.endpoint().contains(search, Qt::CaseInsensitive))
            continue;
        auto *item = new QListWidgetItem(IconProvider::instance().icon(QStringLiteral("site")),
                                         site.siteName, m_list);
        item->setData(Qt::UserRole, site.siteName);
        item->setToolTip(QStringLiteral("%1\n%2").arg(protocolDisplayName(site.protocol),
                                                      site.endpoint()));
    }
    if (!previouslySelected.isEmpty()) {
        for (int row = 0; row < m_list->count(); ++row) {
            if (m_list->item(row)->data(Qt::UserRole).toString() == previouslySelected) {
                m_list->setCurrentRow(row);
                break;
            }
        }
    }
    m_loaded = true;
    m_statusLabel->setText(QStringLiteral("%1 个站点").arg(m_list->count()));
}

void SiteManagerDialog::loadSite(int index)
{
    if (!m_loaded || !m_list->item(index))
        return;
    const QString name = m_list->item(index)->data(Qt::UserRole).toString();
    const SessionData site = SiteStore::instance().find(name);
    if (site.siteName.isEmpty())
        return;
    m_editor->setSession(site);
    m_editor->setSiteNameVisible(true);
    m_statusLabel->setText(QStringLiteral("%1 · %2").arg(protocolDisplayName(site.protocol),
                                                         site.endpoint()));
}

void SiteManagerDialog::newSite()
{
    SessionData session;
    session.protocol = Protocol::Sftp;
    session.port = 22;
    session.siteName = QStringLiteral("新站点");
    m_editor->setSession(session);
    m_editor->setSiteNameVisible(true);
    m_list->clearSelection();
    m_list->setCurrentRow(-1);
    m_statusLabel->setText(QStringLiteral("填写站点信息后点击“保存”。"));
    m_editor->focusFirstField();
}

void SiteManagerDialog::deleteSite()
{
    if (!currentIndexSelected())
        return;
    const QString name = m_list->currentItem()->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, QStringLiteral("删除站点"),
                              QStringLiteral("确定要删除站点“%1”吗？").arg(name))
        != QMessageBox::Yes)
        return;
    SiteStore::instance().remove(name);
    SiteStore::instance().save();
    refreshList();
    m_statusLabel->setText(QStringLiteral("已删除站点“%1”。").arg(name));
}

void SiteManagerDialog::saveCurrent()
{
    const QString error = m_editor->validate();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法保存站点"), error);
        return;
    }
    SessionData session = m_editor->session();
    if (session.siteName.trimmed().isEmpty() || session.siteName == session.endpoint()) {
        QMessageBox::warning(this, QStringLiteral("需要站点名称"),
                             QStringLiteral("请为站点填写一个名称。"));
        return;
    }
    SiteStore::instance().addOrUpdate(session);
    SiteStore::instance().save();
    refreshList(session.siteName);
    m_statusLabel->setText(QStringLiteral("已保存站点“%1”。").arg(session.siteName));
}

void SiteManagerDialog::connectSelected()
{
    const QString error = m_editor->validate();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法连接"), error);
        return;
    }
    m_selectedSession = m_editor->session();
    if (m_selectedSession.siteName.isEmpty())
        m_selectedSession.siteName = m_selectedSession.endpoint();
    QDialog::accept();
}

bool SiteManagerDialog::currentIndexSelected() const
{
    return m_list->currentRow() >= 0 && m_list->currentItem();
}

void SiteManagerDialog::accept()
{
    connectSelected();
}

} // namespace wsc
