#include "ui/LoginDialog.h"

#include "ui/IconProvider.h"
#include "ui/SessionEditor.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace wsc {

LoginDialog::LoginDialog(const SessionData &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("登录 - WinSCP Qt"));
    setWindowIcon(IconProvider::instance().appIcon());
    resize(620, 760);
    setMinimumSize(560, 560);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(14);

    auto *header = new QHBoxLayout();
    header->setSpacing(12);
    auto *icon = new QLabel(this);
    icon->setPixmap(IconProvider::instance().appIcon().pixmap(52, 52));
    icon->setFixedSize(52, 52);
    auto *titleColumn = new QVBoxLayout();
    titleColumn->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("登录到远程服务器"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(15);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto *subtitle = new QLabel(
        QStringLiteral("选择协议并输入连接信息。连接成功后会在界面下方打开终端。"), this);
    subtitle->setStyleSheet(QStringLiteral("color: #8492a6;"));
    subtitle->setWordWrap(true);
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitle);
    header->addWidget(icon);
    header->addLayout(titleColumn, 1);
    layout->addLayout(header);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor = new SessionEditor(scroll);
    m_editor->setSiteNameVisible(true);
    scroll->setWidget(m_editor);
    layout->addWidget(scroll, 1);

    auto *footer = new QHBoxLayout();
    m_saveSiteCheck = new QCheckBox(QStringLiteral("保存为站点"), this);
    footer->addWidget(m_saveSiteCheck);
    footer->addStretch(1);

    auto *cancelButton = new QPushButton(QStringLiteral("取消"), this);
    auto *connectButton = new QPushButton(QStringLiteral("连接"), this);
    connectButton->setProperty("accent", true);
    connectButton->setIcon(IconProvider::instance().icon(QStringLiteral("connect"), Qt::white));
    connectButton->setDefault(true);
    footer->addWidget(cancelButton);
    footer->addWidget(connectButton);
    layout->addLayout(footer);

    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(connectButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_saveSiteCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_editor->setSiteNameVisible(checked);
        if (checked)
            m_editor->focusFirstField();
    });

    m_editor->setSession(initial);
    m_editor->setSiteNameVisible(false);
    if (initial.siteName.isEmpty() || initial.siteName == initial.endpoint()) {
        m_editor->setSiteNameVisible(false);
    } else {
        m_saveSiteCheck->setChecked(true);
        m_editor->setSiteNameVisible(true);
    }
    m_editor->focusFirstField();
}

SessionData LoginDialog::session() const
{
    SessionData result = m_editor->session();
    if (!m_saveSiteCheck->isChecked() && result.siteName == result.endpoint())
        result.siteName.clear();
    return result;
}

bool LoginDialog::saveAsSite() const
{
    return m_saveSiteCheck->isChecked();
}

void LoginDialog::accept()
{
    const QString error = m_editor->validate();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法连接"), error);
        return;
    }
    if (m_saveSiteCheck->isChecked()) {
        SessionData current = m_editor->session();
        if (current.siteName.trimmed().isEmpty() || current.siteName == current.endpoint()) {
            QMessageBox::warning(this, QStringLiteral("需要站点名称"),
                                 QStringLiteral("勾选“保存为站点”后，请填写站点名称。"));
            return;
        }
    }
    QDialog::accept();
}

} // namespace wsc
