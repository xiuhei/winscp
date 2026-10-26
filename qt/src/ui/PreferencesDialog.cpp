#include "ui/PreferencesDialog.h"

#include "core/Settings.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace wsc {

PreferencesDialog::PreferencesDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("首选项 - WinSCP Qt"));
    setWindowIcon(IconProvider::instance().appIcon());
    resize(620, 580);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("首选项"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(10);
    title->setObjectName(QStringLiteral("DialogHeading"));
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *tabs = new QTabWidget(this);
    const Settings &settings = Settings::instance();

    auto *generalPage = new QWidget(tabs);
    auto *generalForm = new QFormLayout(generalPage);
    generalForm->setContentsMargins(14, 14, 14, 14);
    generalForm->setSpacing(10);
    m_themeCombo = new QComboBox(generalPage);
    m_themeCombo->addItem(QStringLiteral("深色"), QStringLiteral("dark"));
    m_themeCombo->addItem(QStringLiteral("浅色"), QStringLiteral("light"));
    m_themeCombo->setCurrentIndex(settings.theme == QLatin1String("light") ? 1 : 0);
    generalForm->addRow(QStringLiteral("界面主题："), m_themeCombo);
    m_showHiddenCheck = new QCheckBox(QStringLiteral("显示隐藏文件和文件夹"), generalPage);
    m_showHiddenCheck->setChecked(settings.showHiddenFiles);
    generalForm->addRow(QString(), m_showHiddenCheck);
    m_confirmDeleteCheck = new QCheckBox(QStringLiteral("删除文件前确认"), generalPage);
    m_confirmDeleteCheck->setChecked(settings.confirmDelete);
    generalForm->addRow(QString(), m_confirmDeleteCheck);
    m_confirmExitCheck = new QCheckBox(QStringLiteral("退出程序前确认"), generalPage);
    m_confirmExitCheck->setChecked(settings.confirmExit);
    generalForm->addRow(QString(), m_confirmExitCheck);
    m_savePasswordsCheck = new QCheckBox(QStringLiteral("保存密码（Windows DPAPI 加密）"), generalPage);
    m_savePasswordsCheck->setChecked(settings.savePasswords);
    generalForm->addRow(QString(), m_savePasswordsCheck);
    m_askHostKeyCheck = new QCheckBox(QStringLiteral("首次连接时确认 SSH 主机密钥"), generalPage);
    m_askHostKeyCheck->setChecked(settings.askUnknownHostKey);
    generalForm->addRow(QString(), m_askHostKeyCheck);
    tabs->addTab(generalPage, QStringLiteral("常规"));

    auto *transferPage = new QWidget(tabs);
    auto *transferForm = new QFormLayout(transferPage);
    transferForm->setContentsMargins(14, 14, 14, 14);
    transferForm->setSpacing(10);
    m_parallelSpin = new QSpinBox(transferPage);
    m_parallelSpin->setRange(1, 8);
    m_parallelSpin->setValue(settings.parallelTransfers);
    m_parallelSpin->setSuffix(QStringLiteral(" 个并发任务"));
    transferForm->addRow(QStringLiteral("并行传输："), m_parallelSpin);
    m_conflictCombo = new QComboBox(transferPage);
    m_conflictCombo->addItem(QStringLiteral("询问"), QStringLiteral("ask"));
    m_conflictCombo->addItem(QStringLiteral("覆盖目标文件"), QStringLiteral("overwrite"));
    m_conflictCombo->addItem(QStringLiteral("跳过已存在文件"), QStringLiteral("skip"));
    const int conflictIndex = m_conflictCombo->findData(settings.conflictPolicy);
    m_conflictCombo->setCurrentIndex(conflictIndex >= 0 ? conflictIndex : 0);
    transferForm->addRow(QStringLiteral("文件冲突："), m_conflictCombo);
    m_preserveTimesCheck = new QCheckBox(QStringLiteral("保留文件修改时间"), transferPage);
    m_preserveTimesCheck->setChecked(settings.preserveTimestamps);
    transferForm->addRow(QString(), m_preserveTimesCheck);
    m_preservePermissionsCheck = new QCheckBox(QStringLiteral("上传时保留 Unix 权限"), transferPage);
    m_preservePermissionsCheck->setChecked(settings.preservePermissions);
    transferForm->addRow(QString(), m_preservePermissionsCheck);
    m_cacheDaysSpin = new QSpinBox(transferPage);
    m_cacheDaysSpin->setObjectName(QStringLiteral("CacheRetentionDays"));
    m_cacheDaysSpin->setRange(1, 3650);
    m_cacheDaysSpin->setValue(settings.cacheRetentionDays);
    m_cacheDaysSpin->setSuffix(QStringLiteral(" 天"));
    transferForm->addRow(QStringLiteral("远程打开缓存保留："), m_cacheDaysSpin);
    tabs->addTab(transferPage, QStringLiteral("传输"));

    auto *terminalPage = new QWidget(tabs);
    auto *terminalForm = new QFormLayout(terminalPage);
    terminalForm->setContentsMargins(14, 14, 14, 14);
    terminalForm->setSpacing(10);
    m_terminalFontCombo = new QFontComboBox(terminalPage);
    m_terminalFontCombo->setCurrentFont(QFont(settings.terminalFontFamily));
    terminalForm->addRow(QStringLiteral("字体："), m_terminalFontCombo);
    m_terminalSizeSpin = new QSpinBox(terminalPage);
    m_terminalSizeSpin->setRange(6, 32);
    m_terminalSizeSpin->setValue(settings.terminalFontSize);
    m_terminalSizeSpin->setSuffix(QStringLiteral(" pt"));
    terminalForm->addRow(QStringLiteral("字号："), m_terminalSizeSpin);
    m_scrollbackSpin = new QSpinBox(terminalPage);
    m_scrollbackSpin->setRange(500, 100000);
    m_scrollbackSpin->setSingleStep(500);
    m_scrollbackSpin->setValue(settings.terminalScrollback);
    m_scrollbackSpin->setSuffix(QStringLiteral(" 行"));
    terminalForm->addRow(QStringLiteral("回滚缓冲区："), m_scrollbackSpin);
    m_shellEdit = new QLineEdit(terminalPage);
    m_shellEdit->setPlaceholderText(QStringLiteral("留空使用服务器默认 shell"));
    m_shellEdit->setText(settings.terminalShell);
    terminalForm->addRow(QStringLiteral("Shell："), m_shellEdit);
    tabs->addTab(terminalPage, QStringLiteral("终端"));

    auto *toolsPage = new QWidget(tabs);
    auto *toolsForm = new QFormLayout(toolsPage);
    toolsForm->setContentsMargins(14, 14, 14, 14);
    toolsForm->setSpacing(10);
    auto *editorRow = new QWidget(toolsPage);
    auto *editorLayout = new QHBoxLayout(editorRow);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(6);
    m_editorEdit = new QLineEdit(editorRow);
    m_editorEdit->setText(settings.editorCommand);
    m_editorEdit->setPlaceholderText(QStringLiteral("例如：notepad.exe"));
    auto *browseButton = new QToolButton(editorRow);
    browseButton->setText(QStringLiteral("…"));
    browseButton->setFixedWidth(30);
    editorLayout->addWidget(m_editorEdit, 1);
    editorLayout->addWidget(browseButton);
    toolsForm->addRow(QStringLiteral("外部编辑器："), editorRow);
    m_timeoutSpin = new QSpinBox(toolsPage);
    m_timeoutSpin->setRange(5, 300);
    m_timeoutSpin->setValue(settings.connectionTimeout);
    m_timeoutSpin->setSuffix(QStringLiteral(" 秒"));
    toolsForm->addRow(QStringLiteral("连接超时："), m_timeoutSpin);
    m_keepAliveSpin = new QSpinBox(toolsPage);
    m_keepAliveSpin->setRange(0, 600);
    m_keepAliveSpin->setValue(settings.keepAlive);
    m_keepAliveSpin->setSuffix(QStringLiteral(" 秒"));
    toolsForm->addRow(QStringLiteral("保活间隔："), m_keepAliveSpin);
    tabs->addTab(toolsPage, QStringLiteral("工具"));
    layout->addWidget(tabs, 1);

    layout->addWidget(new QLabel(QStringLiteral("提示：终端固定显示在界面下方，可通过 Ctrl+T 快速打开。"), this));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("accent", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseButton, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择编辑器程序"), QStringLiteral("C:/Windows/notepad.exe"),
            QStringLiteral("程序 (*.exe);;所有文件 (*)"));
        if (!path.isEmpty())
            m_editorEdit->setText(path);
    });
}

void PreferencesDialog::accept()
{
    Settings &settings = Settings::instance();
    settings.theme = m_themeCombo->currentData().toString();
    settings.showHiddenFiles = m_showHiddenCheck->isChecked();
    settings.confirmDelete = m_confirmDeleteCheck->isChecked();
    settings.confirmExit = m_confirmExitCheck->isChecked();
    settings.savePasswords = m_savePasswordsCheck->isChecked();
    settings.askUnknownHostKey = m_askHostKeyCheck->isChecked();
    settings.cacheRetentionDays = m_cacheDaysSpin->value();
    settings.parallelTransfers = m_parallelSpin->value();
    settings.conflictPolicy = m_conflictCombo->currentData().toString();
    settings.preserveTimestamps = m_preserveTimesCheck->isChecked();
    settings.preservePermissions = m_preservePermissionsCheck->isChecked();
    settings.terminalFontFamily = m_terminalFontCombo->currentFont().family();
    settings.terminalFontSize = m_terminalSizeSpin->value();
    settings.terminalScrollback = m_scrollbackSpin->value();
    settings.terminalShell = m_shellEdit->text().trimmed();
    settings.editorCommand = m_editorEdit->text().trimmed();
    settings.connectionTimeout = m_timeoutSpin->value();
    settings.keepAlive = m_keepAliveSpin->value();
    settings.save();
    QDialog::accept();
}

} // namespace wsc
