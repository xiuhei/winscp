#include "ui/TerminalPanel.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "core/FileSystemController.h"
#include "terminal/SshShellSession.h"
#include "terminal/TerminalWidget.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace wsc {

namespace {

QString shellQuote(const QString &path)
{
    QString result = path;
    result.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(result);
}

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

} // namespace

TerminalPanel::TerminalPanel(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("TerminalPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumHeight(110);
    buildUi();
    updateHint();
    updateWelcomeText();
}

TerminalPanel::~TerminalPanel()
{
    const QSignalBlocker blocker(this);
    closeAll();
}

void TerminalPanel::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *toolbar = new QFrame(this);
    toolbar->setObjectName(QStringLiteral("TerminalToolbar"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(6, 3, 6, 3);
    toolbarLayout->setSpacing(6);
    auto *icon = new QLabel(toolbar);
    icon->setObjectName(QStringLiteral("TerminalHeaderIcon"));
    icon->setPixmap(IconProvider::instance().icon(QStringLiteral("terminal"))
                        .pixmap(16, 16));
    auto *title = new QLabel(QStringLiteral("终端"), toolbar);
    title->setObjectName(QStringLiteral("TerminalTitle"));
    toolbarLayout->addWidget(icon);
    toolbarLayout->addWidget(title);
    m_hintLabel = new QLabel(toolbar);
    m_hintLabel->setObjectName(QStringLiteral("TerminalHint"));
    m_hintLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toolbarLayout->addWidget(m_hintLabel, 1);
    toolbarLayout->addStretch(0);

    m_copyButton = makeToolButton(toolbar, QStringLiteral("copy"), QStringLiteral("复制选中内容 (Ctrl+Shift+C)"));
    m_pasteButton = makeToolButton(toolbar, QStringLiteral("paste"), QStringLiteral("粘贴 (Ctrl+Shift+V)"));
    m_clearButton = makeToolButton(toolbar, QStringLiteral("clear"), QStringLiteral("清屏"));
    m_reconnectButton = makeToolButton(toolbar, QStringLiteral("refresh"), QStringLiteral("重新连接"));
    m_newButton = makeToolButton(toolbar, QStringLiteral("terminal-plus"), QStringLiteral("新建终端"));
    m_closeButton = makeToolButton(toolbar, QStringLiteral("close"), QStringLiteral("关闭当前终端"));
    toolbarLayout->addWidget(m_copyButton);
    toolbarLayout->addWidget(m_pasteButton);
    toolbarLayout->addWidget(m_clearButton);
    toolbarLayout->addWidget(m_reconnectButton);
    toolbarLayout->addWidget(m_newButton);
    toolbarLayout->addWidget(m_closeButton);
    outer->addWidget(toolbar);

    m_stack = new QStackedWidget(this);
    m_tabs = new QTabWidget(m_stack);
    m_tabs->setObjectName(QStringLiteral("TerminalTabs"));
    m_tabs->setTabsClosable(false);
    m_tabs->setMovable(false);
    m_tabs->setDocumentMode(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setUsesScrollButtons(true);
    m_tabs->setCornerWidget(nullptr);
    m_stack->addWidget(m_tabs);

    buildWelcomePage();
    m_stack->addWidget(m_welcomePage);
    m_stack->setCurrentWidget(m_welcomePage);
    outer->addWidget(m_stack, 1);

    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { updateHint(); });
    connect(m_tabs, &QTabWidget::tabBarDoubleClicked, this, [this](int index) {
        if (index >= 0)
            setTabTitle(index, m_tabsData.at(index).baseTitle);
    });
    connect(m_newButton, &QToolButton::clicked, this, &TerminalPanel::openTerminal);
    connect(m_copyButton, &QToolButton::clicked, this, [this] {
        if (TerminalWidget *terminal = currentTerminal())
            terminal->copySelection();
    });
    connect(m_pasteButton, &QToolButton::clicked, this, [this] {
        if (TerminalWidget *terminal = currentTerminal()) {
            if (terminal->isSessionRunning())
                terminal->paste();
        }
    });
    connect(m_clearButton, &QToolButton::clicked, this, [this] {
        if (TerminalWidget *terminal = currentTerminal())
            terminal->clearScreen();
    });
    connect(m_reconnectButton, &QToolButton::clicked, this, [this] {
        const int index = currentTabIndex();
        if (index < 0)
            return;
        const SessionData session = m_tabsData.at(index).sessionData;
        const QString directory = m_tabsData.at(index).initialDirectory;
        closeTab(index);
        if (!session.hostName.isEmpty())
            openForSession(session, directory);
    });
    connect(m_closeButton, &QToolButton::clicked, this, [this] {
        const int index = currentTabIndex();
        if (index >= 0)
            closeTab(index);
    });
}

void TerminalPanel::buildWelcomePage()
{
    m_welcomePage = new QWidget(m_stack);
    m_welcomePage->setObjectName(QStringLiteral("WelcomePage"));
    auto *layout = new QVBoxLayout(m_welcomePage);
    layout->setContentsMargins(24, 18, 24, 18);
    layout->setSpacing(8);
    layout->addStretch(1);
    auto *icon = new QLabel(m_welcomePage);
    icon->setObjectName(QStringLiteral("TerminalWelcomeIcon"));
    icon->setPixmap(IconProvider::instance().icon(QStringLiteral("terminal"), Theme::accent())
                        .pixmap(24, 24));
    icon->setAlignment(Qt::AlignCenter);
    m_welcomeTitle = new QLabel(QStringLiteral("SSH 终端"), m_welcomePage);
    m_welcomeTitle->setObjectName(QStringLiteral("WelcomeTitle"));
    m_welcomeTitle->setAlignment(Qt::AlignCenter);
    m_welcomeHint = new QLabel(m_welcomePage);
    m_welcomeHint->setObjectName(QStringLiteral("WelcomeText"));
    m_welcomeHint->setAlignment(Qt::AlignCenter);
    m_welcomeHint->setWordWrap(true);
    m_welcomeButton = new QPushButton(QStringLiteral("打开终端"), m_welcomePage);
    m_welcomeButton->setProperty("accent", true);
    m_welcomeButton->setIcon(IconProvider::instance().icon(QStringLiteral("terminal")));
    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_welcomeButton);
    buttonRow->addStretch(1);
    layout->addWidget(icon);
    layout->addWidget(m_welcomeTitle);
    layout->addWidget(m_welcomeHint);
    layout->addLayout(buttonRow);
    layout->addStretch(1);

    connect(m_welcomeButton, &QPushButton::clicked, this, &TerminalPanel::openTerminal);
}

void TerminalPanel::setFileSystemController(FileSystemController *controller)
{
    m_controller = controller;
}

void TerminalPanel::setSession(const SessionData &session)
{
    m_session = session;
    updateHint();
    updateWelcomeText();
}

void TerminalPanel::updateHint()
{
    if (!m_hintLabel)
        return;
    const int index = currentTabIndex();
    if (index >= 0) {
        const TabData &data = m_tabsData.at(index);
        QString hint = data.sessionData.hostName.isEmpty() ? data.baseTitle
                                                           : data.sessionData.endpoint();
        if (!data.connected)
            hint += QStringLiteral(" · 已断开");
        m_hintLabel->setText(hint);
        m_hintLabel->setToolTip(data.sessionData.hostName.isEmpty()
                                    ? QString()
                                    : data.sessionData.url());
        return;
    }
    if (!m_session.hostName.isEmpty()) {
        m_hintLabel->setText(m_session.endpoint() + QStringLiteral(" · 就绪"));
        m_hintLabel->setToolTip(m_session.url());
    } else {
        m_hintLabel->setText(QStringLiteral("未连接 · 连接 SFTP/SCP 会话后可打开 SSH 终端"));
        m_hintLabel->setToolTip(QString());
    }
}

void TerminalPanel::updateWelcomeText()
{
    if (!m_welcomeHint || !m_welcomeButton)
        return;
    if (!m_session.hostName.isEmpty() && protocolUsesSsh(m_session.protocol)) {
        m_welcomeTitle->setText(QStringLiteral("SSH 终端"));
        m_welcomeHint->setText(
            QStringLiteral("点击下方按钮，为 %1 打开一个 SSH 终端。\n终端固定显示在界面下方。")
                .arg(m_session.endpoint()));
        m_welcomeButton->setText(QStringLiteral("打开终端"));
    } else if (!m_session.hostName.isEmpty()) {
        m_welcomeTitle->setText(QStringLiteral("当前协议不支持终端"));
        m_welcomeHint->setText(
            QStringLiteral("SSH 终端仅支持 SFTP 和 SCP 会话。当前协议：%1。")
                .arg(protocolDisplayName(m_session.protocol)));
        m_welcomeButton->setText(QStringLiteral("连接新会话"));
    } else {
        m_welcomeTitle->setText(QStringLiteral("SSH 终端"));
        m_welcomeHint->setText(
            QStringLiteral("先连接一个 SFTP 或 SCP 会话，然后在此打开终端。\n"
                           "终端显示在文件管理器下方，可拖动分隔条调整高度。"));
        m_welcomeButton->setText(QStringLiteral("连接服务器"));
    }
}

void TerminalPanel::openTerminal()
{
    if (m_session.hostName.isEmpty() || !protocolUsesSsh(m_session.protocol)) {
        emit connectRequested();
        return;
    }
    openForSession(m_session);
}

void TerminalPanel::openForSession(const SessionData &session, const QString &initialDirectory)
{
    if (!protocolUsesSsh(session.protocol)) {
        QMessageBox::information(this, QStringLiteral("无法打开终端"),
                                 QStringLiteral("SSH 终端仅支持 SFTP 和 SCP 会话。"));
        return;
    }
    if (session.hostName.isEmpty()) {
        emit connectRequested();
        return;
    }

    if (!m_controller || !m_controller->isConnected()) {
        emit connectRequested();
        return;
    }

    auto *terminal = new TerminalWidget(100, 28, this);
    terminal->applySettings();
    terminal->setSessionRunning(false);

    const int columns = terminal->screen()->columns();
    const int rows = terminal->screen()->rows();
    auto *ssh = m_controller->createShell(columns, rows, Settings::instance().terminalShell.trimmed());

    TabData data;
    data.widget = terminal;
    data.session = ssh;
    data.sessionData = session;
    data.baseTitle = session.rootLabel();
    data.initialDirectory = initialDirectory;
    data.connected = false;

    connect(terminal, &TerminalWidget::inputReady, ssh, &SshShellSession::writeData);
    connect(terminal, &TerminalWidget::sizeChanged, ssh, &SshShellSession::resizePty);
    connect(terminal, &TerminalWidget::titleChanged, this, [this, terminal](const QString &title) {
        if (title.isEmpty())
            return;
        for (int index = 0; index < m_tabsData.size(); ++index) {
            if (m_tabsData.at(index).widget == terminal) {
                setTabTitle(index, title);
                break;
            }
        }
    });
    connect(terminal, &TerminalWidget::fontZoomRequested, this, &TerminalPanel::zoomFont);
    ssh->setOutputBackpressure(true);
    connect(ssh, &SshShellSession::outputReady, terminal,
            [terminal, ssh = QPointer<SshShellSession>(ssh)](const QByteArray &data) {
                terminal->feed(data);
                if (ssh)
                    ssh->acknowledgeOutput(data.size());
            });
    connect(ssh, &SshShellSession::connected, this, [this, terminal = QPointer<TerminalWidget>(terminal), ssh, initialDirectory](
                                                   const QString &banner) {
        if (!terminal)
            return;
        for (TabData &item : m_tabsData) {
            if (item.widget == terminal) {
                item.connected = true;
                terminal->setSessionRunning(true);
                if (!initialDirectory.isEmpty()) {
                    const QString command = QStringLiteral("cd -- %1\r")
                                                .arg(shellQuote(initialDirectory));
                    ssh->writeData(command.toUtf8());
                }
                break;
            }
        }
        Q_UNUSED(banner);
        updateHint();
        terminal->setFocus();
    });
    connect(ssh, &SshShellSession::sessionError, this,
            [this, terminal = QPointer<TerminalWidget>(terminal)](const QString &message) {
                if (!terminal)
                    return;
                terminal->feed(QByteArrayLiteral("\r\n\x1b[1;31m[错误] ") + message.toUtf8() +
                               QByteArrayLiteral("\x1b[0m\r\n"));
                emit statusMessage(QStringLiteral("终端错误：%1").arg(message));
            });
    connect(ssh, &SshShellSession::sessionClosed, this,
            [this, terminal = QPointer<TerminalWidget>(terminal)](int exitStatus, const QString &reason) {
                if (!terminal)
                    return;
                terminal->setSessionRunning(false);
                const QByteArray line = QByteArrayLiteral("\r\n\x1b[33m[终端已断开")
                    + (exitStatus >= 0 ? QByteArrayLiteral("，退出码 ") + QByteArray::number(exitStatus)
                                       : QByteArray())
                    + QByteArrayLiteral("] ") + reason.toUtf8() + QByteArrayLiteral("\x1b[0m\r\n");
                terminal->feed(line);
                for (TabData &item : m_tabsData) {
                    if (item.widget == terminal) {
                        item.connected = false;
                        break;
                    }
                }
                updateHint();
            });

    appendTab(data, data.baseTitle);
    updateWelcomeText();
    updateHint();
    m_controller->startShell(ssh);
    terminal->setFocus();
}

void TerminalPanel::appendTab(const TabData &data, const QString &title)
{
    m_tabsData.append(data);
    const int index = m_tabs->addTab(data.widget, title);
    m_tabs->setTabToolTip(index, data.sessionData.url());

    auto *closeButton = new QToolButton(m_tabs);
    closeButton->setIcon(IconProvider::instance().icon(QStringLiteral("close"), Theme::muted()));
    closeButton->setIconSize(QSize(11, 11));
    closeButton->setFixedSize(17, 17);
    closeButton->setAutoRaise(true);
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setToolTip(QStringLiteral("关闭终端"));
    closeButton->setStyleSheet(
        QStringLiteral("QToolButton { background: transparent; border: none; padding: 0; }"
                       "QToolButton:hover { background: palette(midlight); }"));
    m_tabs->tabBar()->setTabButton(index, QTabBar::RightSide, closeButton);
    TerminalWidget *widget = data.widget;
    connect(closeButton, &QToolButton::clicked, this, [this, widget] {
        for (int tab = 0; tab < m_tabsData.size(); ++tab) {
            if (m_tabsData.at(tab).widget == widget) {
                closeTab(tab);
                break;
            }
        }
    });

    m_stack->setCurrentWidget(m_tabs);
    m_tabs->setCurrentIndex(index);
    m_closeButton->setEnabled(true);
    m_reconnectButton->setEnabled(true);
    emit terminalCountChanged(m_tabsData.size());
}

void TerminalPanel::closeTab(int index)
{
    if (index < 0 || index >= m_tabsData.size())
        return;
    TabData data = m_tabsData.takeAt(index);
    if (data.session) {
        data.session->stop();
        data.session->deleteLater();
    }
    m_tabs->removeTab(index);
    data.widget->deleteLater();
    if (m_tabsData.isEmpty())
        m_stack->setCurrentWidget(m_welcomePage);
    m_closeButton->setEnabled(!m_tabsData.isEmpty());
    m_reconnectButton->setEnabled(!m_tabsData.isEmpty());
    updateHint();
    updateWelcomeText();
    emit terminalCountChanged(m_tabsData.size());
}

void TerminalPanel::setTabTitle(int index, const QString &title)
{
    if (index < 0 || index >= m_tabsData.size())
        return;
    QString display = title.trimmed();
    if (display.isEmpty())
        display = m_tabsData.at(index).baseTitle;
    if (display.size() > 42)
        display = display.left(39) + QStringLiteral("…");
    m_tabs->setTabText(index, display);
    m_tabs->setTabToolTip(index, m_tabsData.at(index).sessionData.url());
}

TerminalWidget *TerminalPanel::currentTerminal() const
{
    const int index = currentTabIndex();
    return index >= 0 ? m_tabsData.at(index).widget : nullptr;
}

int TerminalPanel::currentTabIndex() const
{
    if (!m_tabs || m_tabsData.isEmpty())
        return -1;
    const int index = m_tabs->currentIndex();
    return (index >= 0 && index < m_tabsData.size()) ? index : -1;
}

void TerminalPanel::zoomFont(int delta)
{
    Settings &settings = Settings::instance();
    const int newSize = qBound(6, settings.terminalFontSize + delta, 32);
    if (newSize == settings.terminalFontSize)
        return;
    settings.terminalFontSize = newSize;
    settings.save();
    applySettings();
}

void TerminalPanel::applySettings()
{
    findChild<QLabel *>(QStringLiteral("TerminalHeaderIcon"))->setPixmap(
        IconProvider::instance().icon(QStringLiteral("terminal")).pixmap(QSize(16, 16), devicePixelRatioF()));
    findChild<QLabel *>(QStringLiteral("TerminalWelcomeIcon"))->setPixmap(
        IconProvider::instance().icon(QStringLiteral("terminal"), Theme::accent())
            .pixmap(QSize(24, 24), devicePixelRatioF()));
    for (TabData &data : m_tabsData) {
        if (data.widget)
            data.widget->applySettings();
    }
    for (int index = 0; index < m_tabsData.size(); ++index) {
        if (auto *button = qobject_cast<QToolButton *>(
                m_tabs->tabBar()->tabButton(index, QTabBar::RightSide))) {
            button->setIcon(IconProvider::instance().icon(QStringLiteral("close"), Theme::muted()));
        }
    }
    updateHint();
}

void TerminalPanel::focusTerminal()
{
    if (TerminalWidget *terminal = currentTerminal())
        terminal->setFocus();
    else
        m_welcomeButton->setFocus();
}

void TerminalPanel::closeAll()
{
    for (int index = m_tabsData.size() - 1; index >= 0; --index) {
        TabData data = m_tabsData.takeAt(index);
        if (data.session) {
            data.session->stop();
            data.session->deleteLater();
        }
        delete data.widget;
    }
    if (m_tabs)
        m_tabs->clear();
    m_stack->setCurrentWidget(m_welcomePage);
    m_closeButton->setEnabled(false);
    m_reconnectButton->setEnabled(false);
    updateHint();
    emit terminalCountChanged(0);
}

void TerminalPanel::openDemoTerminal()
{
    auto *terminal = new TerminalWidget(100, 28, this);
    terminal->applySettings();
    terminal->setSessionRunning(true);
    const QByteArray demo =
        "\x1b[1;38;5;214m  WinSCP Qt Terminal\x1b[0m  \x1b[90m— zsh 5.9 · xterm-256color\x1b[0m\r\n"
        "\x1b[90mLast login: Sun Oct 19 00:42:11 2026 from 192.168.1.10\x1b[0m\r\n\r\n"
        "\x1b[1;32mdemo@winscp\x1b[0m:\x1b[1;34m~\x1b[0m$ ls -lh\r\n"
        "total 48K\r\n"
        "drwxr-xr-x  8 demo demo 4.0K Oct 19 00:40 \x1b[1;34mprojects\x1b[0m\r\n"
        "-rw-r--r--  1 demo demo  12K Oct 19 00:38 README.md\r\n"
        "-rwxr-xr-x  1 demo demo 2.1K Oct 19 00:37 \x1b[1;32mdeploy.sh\x1b[0m\r\n"
        "drwxr-xr-x  3 demo demo 4.0K Oct 18 23:55 .config\r\n\r\n"
        "\x1b[1;32mdemo@winscp\x1b[0m:\x1b[1;34m~\x1b[0m$ printf '\\e[38;5;39mSSH 终端已就绪\\e[0m\\n'\r\n"
        "\x1b[38;5;39mSSH 终端已就绪\x1b[0m\r\n\r\n"
        "\x1b[1;32mdemo@winscp\x1b[0m:\x1b[1;34m~\x1b[0m$ ";
    terminal->feed(demo);

    TabData data;
    data.widget = terminal;
    data.baseTitle = QStringLiteral("demo@winscp: ~");
    data.connected = true;
    data.sessionData = m_session;
    data.sessionData.userName = QStringLiteral("demo");
    data.sessionData.hostName = QStringLiteral("winscp.local");
    data.sessionData.protocol = Protocol::Sftp;
    appendTab(data, data.baseTitle);
    updateHint();
    QTimer::singleShot(250, terminal, [terminal, demo] {
        terminal->clearScreen();
        terminal->feed(demo);
    });
}

} // namespace wsc
