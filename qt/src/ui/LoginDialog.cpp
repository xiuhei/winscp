#include "ui/LoginDialog.h"

#include "core/Settings.h"
#include "ui/IconProvider.h"
#include "ui/SessionEditor.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

namespace wsc {

LoginDialog::LoginDialog(const SessionData &initial, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("LoginDialog"));
    setWindowTitle(QStringLiteral("登录 - WinSCP Qt"));
    setWindowIcon(IconProvider::instance().appIcon());
    resize(900, 560);
    setMinimumSize(760, 480);
    SiteStore::instance().load();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 12, 10, 10);
    layout->setSpacing(10);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 8, 0);
    leftLayout->setSpacing(6);
    m_sites = new QListWidget(left);
    m_sites->setObjectName(QStringLiteral("LoginSites"));
    m_sites->setIconSize(QSize(20, 20));
    m_sites->setFrameShape(QFrame::StyledPanel);
    leftLayout->addWidget(m_sites, 1);
    m_deleteButton = new QPushButton(QStringLiteral("删除站点(&D)"), left);
    m_deleteButton->setAutoDefault(false);
    leftLayout->addWidget(m_deleteButton, 0, Qt::AlignLeft);
    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor = new SessionEditor(scroll);
    scroll->setWidget(m_editor);
    splitter->addWidget(left);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 590});
    layout->addWidget(splitter, 1);
    auto *buttons = new QDialogButtonBox(this);
    auto *login = buttons->addButton(QStringLiteral("登录(&L)"), QDialogButtonBox::AcceptRole);
    login->setObjectName(QStringLiteral("LoginButton"));
    login->setIcon(IconProvider::instance().icon(QStringLiteral("connect")));
    login->setDefault(true);
    buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    auto *startup = new QCheckBox(QStringLiteral("在启动与最后会话被关闭时显示登录对话框(&S)"), this);
    startup->setChecked(Settings::instance().showLoginDialog);
    layout->addWidget(startup);
    connect(startup, &QCheckBox::toggled, this, [](bool checked) {
        Settings::instance().showLoginDialog = checked;
        Settings::instance().save();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &LoginDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_editor, &SessionEditor::saveRequested, this, &LoginDialog::saveSite);
    connect(m_deleteButton, &QPushButton::clicked, this, &LoginDialog::deleteSite);
    connect(m_sites, &QListWidget::currentRowChanged, this, [this](int row) {
        const auto *item = m_sites->item(row);
        if (!item)
            return;
        const QString name = item->data(Qt::UserRole).toString();
        SessionData site;
        if (!name.isEmpty()) {
            site = SiteStore::instance().find(name);
        } else {
            site.savePassword = false;
            site.timeoutSeconds = Settings::instance().connectionTimeout;
            site.keepAliveSeconds = Settings::instance().keepAlive;
        }
        m_editor->setSession(site);
        m_deleteButton->setEnabled(!name.isEmpty());
        m_editor->focusFirstField();
    });
    connect(m_sites, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item->data(Qt::UserRole).toString().isEmpty())
            accept();
    });
    refreshSites(initial.siteName);
    if (m_sites->currentRow() == 0 && !initial.hostName.isEmpty())
        m_editor->setSession(initial);
    m_editor->focusFirstField();
}

void LoginDialog::refreshSites(const QString &selected)
{
    {
        const QSignalBlocker blocker(m_sites);
        m_sites->clear();
        auto *fresh = new QListWidgetItem(IconProvider::instance().icon(QStringLiteral("newfolder")),
                                         QStringLiteral("新建站点"), m_sites);
        fresh->setData(Qt::UserRole, QString());
        for (const auto &site : SiteStore::instance().sites) {
            auto *item = new QListWidgetItem(IconProvider::instance().icon(QStringLiteral("site")), site.siteName, m_sites);
            item->setData(Qt::UserRole, site.siteName);
            item->setToolTip(site.url(false));
        }
    }
    int row = 0;
    for (int index = 1; index < m_sites->count(); ++index)
        if (m_sites->item(index)->data(Qt::UserRole).toString() == selected)
            row = index;
    m_sites->setCurrentRow(row);
}

SessionData LoginDialog::session() const
{
    SessionData result = m_editor->session();
    if (result.siteName.isEmpty())
        result.siteName = result.endpoint();
    return result;
}

void LoginDialog::saveSite()
{
    const QString error = m_editor->validate();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法保存站点"), error);
        return;
    }
    SessionData site = session();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("SaveSiteDialog"));
    dialog.setWindowTitle(QStringLiteral("将会话保存为站点"));
    dialog.setMinimumWidth(400);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(site.siteName, &dialog);
    name->setObjectName(QStringLiteral("SaveSiteName"));
    form->addRow(QStringLiteral("站点名称(&N)"), name);
    layout->addLayout(form);
    auto *password = new QCheckBox(QStringLiteral("保存密码(&P)"), &dialog);
    password->setObjectName(QStringLiteral("SaveSitePassword"));
    password->setChecked(site.savePassword);
    layout->addWidget(password);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(name, &QLineEdit::textChanged, &dialog, [buttons](const QString &value) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!value.trimmed().isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    name->selectAll();
    name->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString chosen = name->text().trimmed();
    if (chosen.isEmpty())
        return;
    if (SiteStore::instance().indexOf(chosen) >= 0 && chosen != site.siteName &&
        QMessageBox::question(this, QStringLiteral("覆盖站点"), QStringLiteral("站点“%1”已存在，是否覆盖？").arg(chosen)) != QMessageBox::Yes)
        return;
    site.siteName = chosen;
    site.savePassword = password->isChecked();
    SiteStore::instance().addOrUpdate(site);
    SiteStore::instance().save();
    refreshSites(chosen);
}

void LoginDialog::deleteSite()
{
    const auto *item = m_sites->currentItem();
    const QString name = item ? item->data(Qt::UserRole).toString() : QString();
    if (name.isEmpty())
        return;
    if (QMessageBox::question(this, QStringLiteral("删除站点"), QStringLiteral("确定要删除站点“%1”吗？").arg(name)) != QMessageBox::Yes)
        return;
    SiteStore::instance().remove(name);
    SiteStore::instance().save();
    refreshSites();
}

void LoginDialog::accept()
{
    const QString error = m_editor->validate();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法连接"), error);
        return;
    }
    QDialog::accept();
}

}
