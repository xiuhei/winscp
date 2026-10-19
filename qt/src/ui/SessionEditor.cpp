#include "ui/SessionEditor.h"

#include "core/PathUtils.h"
#include "ui/IconProvider.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace wsc {

SessionEditor::SessionEditor(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    updateVisibility();
}

void SessionEditor::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *siteGroup = new QGroupBox(QStringLiteral("站点"), this);
    m_siteGroup = siteGroup;
    auto *siteForm = new QFormLayout(siteGroup);
    siteForm->setContentsMargins(12, 18, 12, 10);
    siteForm->setLabelAlignment(Qt::AlignRight);
    m_siteNameEdit = new QLineEdit(siteGroup);
    m_siteNameEdit->setPlaceholderText(QStringLiteral("例如：生产服务器"));
    m_noteEdit = new QLineEdit(siteGroup);
    m_noteEdit->setPlaceholderText(QStringLiteral("可选备注"));
    siteForm->addRow(QStringLiteral("站点名称："), m_siteNameEdit);
    siteForm->addRow(QStringLiteral("备注："), m_noteEdit);
    layout->addWidget(siteGroup);

    auto *connectionGroup = new QGroupBox(QStringLiteral("连接"), this);
    m_connectionForm = new QFormLayout(connectionGroup);
    m_connectionForm->setContentsMargins(12, 18, 12, 10);
    m_connectionForm->setLabelAlignment(Qt::AlignRight);
    m_connectionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_protocolCombo = new QComboBox(connectionGroup);
    m_protocolCombo->addItem(QStringLiteral("SFTP（SSH 文件传输）"), static_cast<int>(Protocol::Sftp));
    m_protocolCombo->addItem(QStringLiteral("SCP（SSH 复制）"), static_cast<int>(Protocol::Scp));
    m_protocolCombo->addItem(QStringLiteral("FTP"), static_cast<int>(Protocol::Ftp));
    m_protocolCombo->addItem(QStringLiteral("FTPS（TLS/SSL）"), static_cast<int>(Protocol::Ftps));
    m_protocolCombo->addItem(QStringLiteral("WebDAV"), static_cast<int>(Protocol::WebDav));
    m_connectionForm->addRow(QStringLiteral("协议："), m_protocolCombo);

    auto *hostRow = new QWidget(connectionGroup);
    auto *hostLayout = new QHBoxLayout(hostRow);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(8);
    m_hostEdit = new QLineEdit(hostRow);
    m_hostEdit->setPlaceholderText(QStringLiteral("主机名或 IP 地址"));
    m_portSpin = new QSpinBox(hostRow);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_portSpin->setFixedWidth(84);
    hostLayout->addWidget(m_hostEdit, 1);
    hostLayout->addWidget(m_portSpin);
    m_connectionForm->addRow(QStringLiteral("主机："), hostRow);

    m_userEdit = new QLineEdit(connectionGroup);
    m_userEdit->setPlaceholderText(QStringLiteral("用户名"));
    m_connectionForm->addRow(QStringLiteral("用户名："), m_userEdit);

    auto *passwordRow = new QWidget(connectionGroup);
    auto *passwordLayout = new QHBoxLayout(passwordRow);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->setSpacing(6);
    m_passwordEdit = new QLineEdit(passwordRow);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(QStringLiteral("留空则使用私钥或 SSH 代理"));
    m_passwordToggle = new QToolButton(passwordRow);
    m_passwordToggle->setCheckable(true);
    m_passwordToggle->setIcon(IconProvider::instance().icon(QStringLiteral("eye")));
    m_passwordToggle->setIconSize(QSize(18, 18));
    m_passwordToggle->setFixedSize(28, 28);
    m_passwordToggle->setToolTip(QStringLiteral("显示密码"));
    passwordLayout->addWidget(m_passwordEdit, 1);
    passwordLayout->addWidget(m_passwordToggle);
    m_connectionForm->addRow(QStringLiteral("密码："), passwordRow);

    m_keyRow = new QWidget(connectionGroup);
    auto *keyLayout = new QHBoxLayout(m_keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(6);
    m_keyEdit = new QLineEdit(m_keyRow);
    m_keyEdit->setPlaceholderText(QStringLiteral("OpenSSH 私钥文件（可选）"));
    auto *keyBrowse = new QToolButton(m_keyRow);
    keyBrowse->setText(QStringLiteral("…"));
    keyBrowse->setFixedWidth(30);
    keyBrowse->setToolTip(QStringLiteral("选择私钥文件"));
    keyLayout->addWidget(m_keyEdit, 1);
    keyLayout->addWidget(keyBrowse);
    m_connectionForm->addRow(QStringLiteral("私钥："), m_keyRow);

    m_passphraseRow = new QWidget(connectionGroup);
    auto *passphraseLayout = new QHBoxLayout(m_passphraseRow);
    passphraseLayout->setContentsMargins(0, 0, 0, 0);
    m_passphraseEdit = new QLineEdit(m_passphraseRow);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setPlaceholderText(QStringLiteral("私钥口令（可选）"));
    passphraseLayout->addWidget(m_passphraseEdit);
    m_connectionForm->addRow(QStringLiteral("私钥口令："), m_passphraseRow);

    m_remoteDirectoryEdit = new QLineEdit(connectionGroup);
    m_remoteDirectoryEdit->setPlaceholderText(QStringLiteral("留空表示主目录"));
    m_connectionForm->addRow(QStringLiteral("远程目录："), m_remoteDirectoryEdit);

    auto *localRow = new QWidget(connectionGroup);
    auto *localLayout = new QHBoxLayout(localRow);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(6);
    m_localDirectoryEdit = new QLineEdit(localRow);
    m_localDirectoryEdit->setPlaceholderText(QStringLiteral("留空表示使用上次的本地目录"));
    auto *localBrowse = new QToolButton(localRow);
    localBrowse->setText(QStringLiteral("…"));
    localBrowse->setFixedWidth(30);
    localBrowse->setToolTip(QStringLiteral("选择本地目录"));
    localLayout->addWidget(m_localDirectoryEdit, 1);
    localLayout->addWidget(localBrowse);
    m_connectionForm->addRow(QStringLiteral("本地目录："), localRow);

    layout->addWidget(connectionGroup);

    auto *optionsGroup = new QGroupBox(QStringLiteral("选项"), this);
    m_optionsForm = new QFormLayout(optionsGroup);
    m_optionsForm->setContentsMargins(12, 18, 12, 10);
    m_optionsForm->setLabelAlignment(Qt::AlignRight);
    m_optionsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_savePasswordCheck = new QCheckBox(QStringLiteral("保存密码（使用 Windows DPAPI 加密）"), optionsGroup);
    m_savePasswordCheck->setChecked(true);
    m_optionsForm->addRow(QString(), m_savePasswordCheck);
    m_passiveCheck = new QCheckBox(QStringLiteral("使用被动模式（PASV）"), optionsGroup);
    m_passiveCheck->setChecked(true);
    m_optionsForm->addRow(QString(), m_passiveCheck);
    m_implicitTlsCheck = new QCheckBox(QStringLiteral("隐式 TLS（端口 990）"), optionsGroup);
    m_optionsForm->addRow(QString(), m_implicitTlsCheck);
    m_verifyTlsCheck = new QCheckBox(QStringLiteral("验证服务器 TLS 证书"), optionsGroup);
    m_verifyTlsCheck->setChecked(true);
    m_optionsForm->addRow(QString(), m_verifyTlsCheck);
    m_webDavHttpsCheck = new QCheckBox(QStringLiteral("使用 HTTPS"), optionsGroup);
    m_optionsForm->addRow(QString(), m_webDavHttpsCheck);
    m_compressionCheck = new QCheckBox(QStringLiteral("启用 SSH 压缩"), optionsGroup);
    m_compressionCheck->setChecked(true);
    m_optionsForm->addRow(QString(), m_compressionCheck);

    auto *timeoutRow = new QWidget(optionsGroup);
    auto *timeoutLayout = new QHBoxLayout(timeoutRow);
    timeoutLayout->setContentsMargins(0, 0, 0, 0);
    m_timeoutSpin = new QSpinBox(timeoutRow);
    m_timeoutSpin->setRange(5, 300);
    m_timeoutSpin->setValue(20);
    m_timeoutSpin->setSuffix(QStringLiteral(" 秒"));
    m_keepAliveSpin = new QSpinBox(timeoutRow);
    m_keepAliveSpin->setRange(0, 600);
    m_keepAliveSpin->setValue(30);
    m_keepAliveSpin->setSuffix(QStringLiteral(" 秒"));
    timeoutLayout->addWidget(new QLabel(QStringLiteral("超时"), timeoutRow));
    timeoutLayout->addWidget(m_timeoutSpin);
    timeoutLayout->addSpacing(12);
    timeoutLayout->addWidget(new QLabel(QStringLiteral("保活"), timeoutRow));
    timeoutLayout->addWidget(m_keepAliveSpin);
    timeoutLayout->addStretch(1);
    m_optionsForm->addRow(QStringLiteral("网络："), timeoutRow);

    layout->addWidget(optionsGroup);
    layout->addStretch(1);

    connect(m_passwordToggle, &QToolButton::toggled, this, [this](bool checked) {
        m_passwordEdit->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
        m_passwordToggle->setIcon(IconProvider::instance().icon(
            checked ? QStringLiteral("eye-off") : QStringLiteral("eye")));
        m_passwordToggle->setToolTip(checked ? QStringLiteral("隐藏密码")
                                             : QStringLiteral("显示密码"));
    });
    connect(keyBrowse, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择私钥文件"), m_keyEdit->text(), QStringLiteral("所有文件 (*)"));
        if (!path.isEmpty())
            m_keyEdit->setText(path);
    });
    connect(localBrowse, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择本地目录"),
            m_localDirectoryEdit->text().isEmpty() ? QDir::homePath() : m_localDirectoryEdit->text());
        if (!path.isEmpty())
            m_localDirectoryEdit->setText(path);
    });
    connect(m_protocolCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        const Protocol protocol = currentProtocol();
        const int currentPort = m_portSpin->value();
        const int oldDefault = defaultPort(m_previousProtocol);
        if (currentPort == oldDefault || currentPort == 0 || currentPort == 990 ||
            currentPort == 21 || currentPort == 22 || currentPort == 80 || currentPort == 443)
            updatePortForProtocol();
        m_previousProtocol = protocol;
        updateVisibility();
    });
    connect(m_implicitTlsCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (currentProtocol() == Protocol::Ftps) {
            const QSignalBlocker blocker(m_portSpin);
            m_portSpin->setValue(checked ? 990 : 21);
        }
    });
    connect(m_webDavHttpsCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (currentProtocol() == Protocol::WebDav) {
            const QSignalBlocker blocker(m_portSpin);
            m_portSpin->setValue(checked ? 443 : 80);
        }
    });
}

Protocol SessionEditor::currentProtocol() const
{
    return static_cast<Protocol>(m_protocolCombo->currentData().toInt());
}

void SessionEditor::setFormRowVisible(QFormLayout *form, QWidget *field, bool visible)
{
    if (!form || !field)
        return;
    field->setVisible(visible);
    if (QWidget *label = form->labelForField(field))
        label->setVisible(visible);
}

void SessionEditor::updateVisibility()
{
    const Protocol protocol = currentProtocol();
    const bool ssh = protocol == Protocol::Sftp || protocol == Protocol::Scp;
    const bool ftp = protocol == Protocol::Ftp;
    const bool ftps = protocol == Protocol::Ftps;
    const bool dav = protocol == Protocol::WebDav;

    setFormRowVisible(m_connectionForm, m_keyRow, ssh);
    setFormRowVisible(m_connectionForm, m_passphraseRow, ssh);
    setFormRowVisible(m_optionsForm, m_passiveCheck, ftp || ftps);
    setFormRowVisible(m_optionsForm, m_implicitTlsCheck, ftps);
    setFormRowVisible(m_optionsForm, m_verifyTlsCheck, ftps || dav);
    setFormRowVisible(m_optionsForm, m_webDavHttpsCheck, dav);
    setFormRowVisible(m_optionsForm, m_compressionCheck, ssh);

    if (protocol == Protocol::WebDav && m_remoteDirectoryEdit->text().isEmpty())
        m_remoteDirectoryEdit->setPlaceholderText(QStringLiteral("WebDAV 根路径，例如 /dav/"));
    else
        m_remoteDirectoryEdit->setPlaceholderText(QStringLiteral("留空表示主目录"));
}

void SessionEditor::updatePortForProtocol()
{
    const Protocol protocol = currentProtocol();
    int port = defaultPort(protocol);
    if (protocol == Protocol::Ftps && m_implicitTlsCheck->isChecked())
        port = 990;
    if (protocol == Protocol::WebDav && m_webDavHttpsCheck->isChecked())
        port = 443;
    const QSignalBlocker blocker(m_portSpin);
    m_portSpin->setValue(port);
}

void SessionEditor::setSiteNameVisible(bool visible)
{
    if (m_siteGroup)
        m_siteGroup->setVisible(visible);
}

void SessionEditor::setSession(const SessionData &session)
{
    const QSignalBlocker blocker(m_protocolCombo);
    const int index = m_protocolCombo->findData(static_cast<int>(session.protocol));
    m_protocolCombo->setCurrentIndex(index >= 0 ? index : 0);
    m_previousProtocol = session.protocol;
    m_siteNameEdit->setText(session.siteName);
    m_noteEdit->setText(session.note);
    m_hostEdit->setText(session.hostName);
    m_portSpin->setValue(session.port > 0 ? session.port : defaultPort(session.protocol));
    m_userEdit->setText(session.userName);
    m_passwordEdit->setText(session.password);
    m_keyEdit->setText(session.privateKeyFile);
    m_passphraseEdit->setText(session.passphrase);
    m_remoteDirectoryEdit->setText(session.remoteDirectory);
    m_localDirectoryEdit->setText(session.localDirectory);
    m_savePasswordCheck->setChecked(session.savePassword);
    m_passiveCheck->setChecked(session.passiveMode);
    {
        const QSignalBlocker implicitBlocker(m_implicitTlsCheck);
        const QSignalBlocker httpsBlocker(m_webDavHttpsCheck);
        m_implicitTlsCheck->setChecked(session.implicitTls);
        m_webDavHttpsCheck->setChecked(session.webDavHttps);
    }
    m_verifyTlsCheck->setChecked(session.tlsVerifyCertificate);
    m_compressionCheck->setChecked(session.compression);
    m_timeoutSpin->setValue(session.timeoutSeconds > 0 ? session.timeoutSeconds : 20);
    m_keepAliveSpin->setValue(session.keepAliveSeconds);
    m_portSpin->setValue(session.port > 0 ? session.port : defaultPort(session.protocol));
    updateVisibility();
}

SessionData SessionEditor::session() const
{
    SessionData data;
    data.siteName = m_siteNameEdit->text().trimmed();
    data.note = m_noteEdit->text().trimmed();
    data.protocol = currentProtocol();
    data.hostName = m_hostEdit->text().trimmed();
    data.port = m_portSpin->value();
    data.userName = m_userEdit->text().trimmed();
    data.password = m_passwordEdit->text();
    if (data.protocol == Protocol::Sftp || data.protocol == Protocol::Scp) {
        data.privateKeyFile = m_keyEdit->text().trimmed();
        data.passphrase = m_passphraseEdit->text();
    }
    data.remoteDirectory = m_remoteDirectoryEdit->text().trimmed();
    if ((data.protocol == Protocol::Ftp || data.protocol == Protocol::Ftps ||
         data.protocol == Protocol::WebDav) &&
        data.remoteDirectory.isEmpty())
        data.remoteDirectory = QStringLiteral("/");
    data.localDirectory = m_localDirectoryEdit->text().trimmed();
    data.savePassword = m_savePasswordCheck->isChecked();
    data.passiveMode = m_passiveCheck->isChecked();
    data.implicitTls = m_implicitTlsCheck->isChecked();
    data.tlsVerifyCertificate = m_verifyTlsCheck->isChecked();
    data.webDavHttps = m_webDavHttpsCheck->isChecked();
    if (data.protocol == Protocol::WebDav && data.port == 0)
        data.port = data.webDavHttps ? 443 : 80;
    data.compression = m_compressionCheck->isChecked();
    data.timeoutSeconds = m_timeoutSpin->value();
    data.keepAliveSeconds = m_keepAliveSpin->value();
    if (data.siteName.isEmpty())
        data.siteName = data.endpoint();
    return data;
}

QString SessionEditor::validate() const
{
    const Protocol protocol = currentProtocol();
    if (m_hostEdit->text().trimmed().isEmpty())
        return QStringLiteral("请输入主机名或 IP 地址。");
    if (m_portSpin->value() <= 0 || m_portSpin->value() > 65535)
        return QStringLiteral("端口号无效。");
    if (protocol == Protocol::Sftp || protocol == Protocol::Scp) {
        if (m_userEdit->text().trimmed().isEmpty())
            return QStringLiteral("SSH 会话需要用户名。");
        const QString keyPath = m_keyEdit->text().trimmed();
        if (!keyPath.isEmpty() && !QFileInfo::exists(keyPath))
            return QStringLiteral("私钥文件不存在：%1").arg(keyPath);
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
        m_passwordEdit->setFocus();
}

void SessionEditor::focusPassword()
{
    m_passwordEdit->setFocus();
    m_passwordEdit->selectAll();
}

} // namespace wsc
