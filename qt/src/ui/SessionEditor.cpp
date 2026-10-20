#include "ui/SessionEditor.h"
#include "ui/IconProvider.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace wsc {

SessionEditor::SessionEditor(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    setSession(SessionData{});
}

void SessionEditor::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto *connection = new QGroupBox(QStringLiteral("会话"), this);
    auto *form = new QGridLayout(connection);
    form->setContentsMargins(12, 16, 12, 12);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(6);
    form->setColumnStretch(0, 1);
    form->setColumnStretch(1, 1);
    const auto label = [connection, form](const QString &text, QWidget *field, int row, int column, int span = 1) {
        auto *caption = new QLabel(text, connection);
        caption->setBuddy(field);
        form->addWidget(caption, row, column, 1, span);
        form->addWidget(field, row + 1, column, 1, span);
    };
    m_protocolCombo = new QComboBox(connection);
    m_protocolCombo->setObjectName(QStringLiteral("SessionProtocol"));
    for (Protocol protocol : {Protocol::Sftp, Protocol::Scp, Protocol::Ftp, Protocol::Ftps, Protocol::WebDav})
        m_protocolCombo->addItem(protocolDisplayName(protocol), static_cast<int>(protocol));
    label(QStringLiteral("文件协议(&F)"), m_protocolCombo, 0, 0);
    m_hostEdit = new QLineEdit(connection);
    m_hostEdit->setObjectName(QStringLiteral("SessionHost"));
    label(QStringLiteral("主机名(&H)"), m_hostEdit, 2, 0, 2);
    m_portSpin = new QSpinBox(connection);
    m_portSpin->setObjectName(QStringLiteral("SessionPort"));
    m_portSpin->setRange(1, 65535);
    m_portSpin->setFixedWidth(110);
    label(QStringLiteral("端口号(&R)"), m_portSpin, 2, 2);
    m_userEdit = new QLineEdit(connection);
    m_userEdit->setObjectName(QStringLiteral("SessionUser"));
    label(QStringLiteral("用户名(&U)"), m_userEdit, 4, 0);
    auto *passwordRow = new QWidget(connection);
    auto *passwordLayout = new QHBoxLayout(passwordRow);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->setSpacing(2);
    m_passwordEdit = new QLineEdit(passwordRow);
    m_passwordEdit->setObjectName(QStringLiteral("SessionPassword"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordToggle = new QToolButton(passwordRow);
    m_passwordToggle->setCheckable(true);
    m_passwordToggle->setIcon(IconProvider::instance().icon(QStringLiteral("eye")));
    m_passwordToggle->setToolTip(QStringLiteral("显示密码"));
    passwordLayout->addWidget(m_passwordEdit, 1);
    passwordLayout->addWidget(m_passwordToggle);
    label(QStringLiteral("密码(&P)"), passwordRow, 4, 1, 2);
    passwordRow->setFocusProxy(m_passwordEdit);
    auto *save = new QPushButton(QStringLiteral("保存(&S)…"), connection);
    save->setObjectName(QStringLiteral("SaveSiteButton"));
    save->setAutoDefault(false);
    auto *advanced = new QPushButton(QStringLiteral("高级(&A)…"), connection);
    advanced->setObjectName(QStringLiteral("SessionAdvancedButton"));
    advanced->setCheckable(true);
    advanced->setAutoDefault(false);
    form->addWidget(save, 6, 0, Qt::AlignLeft);
    form->addWidget(advanced, 6, 1, 1, 2, Qt::AlignRight);
    form->setRowMinimumHeight(6, 36);
    layout->addWidget(connection);
    auto *options = new QGroupBox(QStringLiteral("高级设置"), this);
    options->setObjectName(QStringLiteral("SessionAdvanced"));
    m_optionsForm = new QFormLayout(options);
    m_optionsForm->setContentsMargins(12, 16, 12, 12);
    m_optionsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_keyRow = new QWidget(options);
    auto *keyLayout = new QHBoxLayout(m_keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    m_keyEdit = new QLineEdit(m_keyRow);
    m_keyEdit->setObjectName(QStringLiteral("SessionPrivateKey"));
    auto *browse = new QToolButton(m_keyRow);
    browse->setText(QStringLiteral("…"));
    browse->setToolTip(QStringLiteral("选择私钥文件"));
    keyLayout->addWidget(m_keyEdit, 1);
    keyLayout->addWidget(browse);
    m_optionsForm->addRow(QStringLiteral("私钥文件"), m_keyRow);
    m_passphraseEdit = new QLineEdit(options);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    m_optionsForm->addRow(QStringLiteral("私钥口令"), m_passphraseEdit);
    m_implicitTlsCheck = new QCheckBox(QStringLiteral("隐式 TLS（端口 990）"), options);
    m_optionsForm->addRow(m_implicitTlsCheck);
    m_verifyTlsCheck = new QCheckBox(QStringLiteral("验证服务器 TLS 证书"), options);
    m_optionsForm->addRow(m_verifyTlsCheck);
    m_webDavHttpsCheck = new QCheckBox(QStringLiteral("使用 HTTPS"), options);
    m_optionsForm->addRow(m_webDavHttpsCheck);
    m_timeoutSpin = new QSpinBox(options);
    m_timeoutSpin->setRange(5, 300);
    m_timeoutSpin->setSuffix(QStringLiteral(" 秒"));
    m_optionsForm->addRow(QStringLiteral("连接超时"), m_timeoutSpin);
    m_keepAliveSpin = new QSpinBox(options);
    m_keepAliveSpin->setRange(0, 600);
    m_keepAliveSpin->setSuffix(QStringLiteral(" 秒"));
    m_optionsForm->addRow(QStringLiteral("保持连接间隔"), m_keepAliveSpin);
    layout->addWidget(options);
    layout->addStretch(1);
    options->hide();
    connect(advanced, &QPushButton::toggled, options, &QWidget::setVisible);
    connect(save, &QPushButton::clicked, this, &SessionEditor::saveRequested);
    connect(m_passwordToggle, &QToolButton::toggled, this, [this](bool visible) {
        m_passwordEdit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        m_passwordToggle->setIcon(IconProvider::instance().icon(visible ? QStringLiteral("eye-off") : QStringLiteral("eye")));
        m_passwordToggle->setToolTip(visible ? QStringLiteral("隐藏密码") : QStringLiteral("显示密码"));
    });
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择私钥文件"), m_keyEdit->text());
        if (!file.isEmpty())
            m_keyEdit->setText(file);
    });
    connect(m_protocolCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (m_portSpin->value() == defaultPort(m_previousProtocol) || m_portSpin->value() == 990 || m_portSpin->value() == 443)
            updatePortForProtocol();
        m_previousProtocol = currentProtocol();
        updateVisibility();
    });
    connect(m_implicitTlsCheck, &QCheckBox::toggled, this, &SessionEditor::updatePortForProtocol);
    connect(m_webDavHttpsCheck, &QCheckBox::toggled, this, &SessionEditor::updatePortForProtocol);
    setTabOrder(m_protocolCombo, m_hostEdit);
    setTabOrder(m_hostEdit, m_portSpin);
    setTabOrder(m_portSpin, m_userEdit);
    setTabOrder(m_userEdit, m_passwordEdit);
    setTabOrder(m_passwordEdit, m_passwordToggle);
    setTabOrder(m_passwordToggle, save);
    setTabOrder(save, advanced);
}

Protocol SessionEditor::currentProtocol() const
{
    return static_cast<Protocol>(m_protocolCombo->currentData().toInt());
}

void SessionEditor::updateVisibility()
{
    const Protocol protocol = currentProtocol();
    const bool ssh = protocolUsesSsh(protocol);
    m_optionsForm->setRowVisible(m_keyRow, ssh);
    m_optionsForm->setRowVisible(m_passphraseEdit, ssh);
    m_optionsForm->setRowVisible(m_implicitTlsCheck, protocol == Protocol::Ftps);
    m_optionsForm->setRowVisible(m_verifyTlsCheck, protocol == Protocol::Ftps || protocol == Protocol::WebDav);
    m_optionsForm->setRowVisible(m_webDavHttpsCheck, protocol == Protocol::WebDav);
}

void SessionEditor::updatePortForProtocol()
{
    const Protocol protocol = currentProtocol();
    int port = defaultPort(protocol);
    if (protocol == Protocol::Ftps && m_implicitTlsCheck->isChecked())
        port = 990;
    if (protocol == Protocol::WebDav && m_webDavHttpsCheck->isChecked())
        port = 443;
    m_portSpin->setValue(port);
}

void SessionEditor::setSession(const SessionData &session)
{
    m_session = session;
    const QSignalBlocker protocolBlocker(m_protocolCombo);
    const QSignalBlocker implicitBlocker(m_implicitTlsCheck);
    const QSignalBlocker httpsBlocker(m_webDavHttpsCheck);
    const int index = m_protocolCombo->findData(static_cast<int>(session.protocol));
    m_protocolCombo->setCurrentIndex(index >= 0 ? index : 0);
    m_previousProtocol = currentProtocol();
    m_hostEdit->setText(session.hostName);
    m_portSpin->setValue(session.port > 0 ? session.port : defaultPort(session.protocol));
    m_userEdit->setText(session.userName);
    m_passwordToggle->setChecked(false);
    m_passwordEdit->setText(session.password);
    m_keyEdit->setText(session.privateKeyFile);
    m_passphraseEdit->setText(session.passphrase);
    m_implicitTlsCheck->setChecked(session.implicitTls);
    m_webDavHttpsCheck->setChecked(session.webDavHttps);
    m_verifyTlsCheck->setChecked(session.tlsVerifyCertificate);
    m_timeoutSpin->setValue(session.timeoutSeconds);
    m_keepAliveSpin->setValue(session.keepAliveSeconds);
    updateVisibility();
}

SessionData SessionEditor::session() const
{
    SessionData data = m_session;
    data.protocol = currentProtocol();
    data.hostName = m_hostEdit->text().trimmed();
    data.port = m_portSpin->value();
    data.userName = m_userEdit->text().trimmed();
    data.password = m_passwordEdit->text();
    data.privateKeyFile = protocolUsesSsh(data.protocol) ? m_keyEdit->text().trimmed() : QString();
    data.passphrase = protocolUsesSsh(data.protocol) ? m_passphraseEdit->text() : QString();
    data.implicitTls = m_implicitTlsCheck->isChecked();
    data.tlsVerifyCertificate = m_verifyTlsCheck->isChecked();
    data.webDavHttps = m_webDavHttpsCheck->isChecked();
    data.timeoutSeconds = m_timeoutSpin->value();
    data.keepAliveSeconds = m_keepAliveSpin->value();
    return data;
}

QString SessionEditor::validate() const
{
    if (m_hostEdit->text().trimmed().isEmpty())
        return QStringLiteral("请输入主机名或 IP 地址。");
    if (protocolUsesSsh(currentProtocol())) {
        if (m_userEdit->text().trimmed().isEmpty())
            return QStringLiteral("SSH 会话需要用户名。");
        const QString key = m_keyEdit->text().trimmed();
        if (!key.isEmpty() && !QFileInfo(key).isFile())
            return QStringLiteral("私钥文件不存在：%1").arg(key);
    }
    return {};
}

void SessionEditor::focusFirstField()
{
    if (m_hostEdit->text().isEmpty())
        m_hostEdit->setFocus();
    else if (m_userEdit->text().isEmpty())
        m_userEdit->setFocus();
    else
        focusPassword();
}

void SessionEditor::focusPassword()
{
    m_passwordEdit->setFocus();
    m_passwordEdit->selectAll();
}

}
