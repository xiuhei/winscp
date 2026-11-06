#include "ui/MainWindow.h"

#include "core/PathUtils.h"
#include "core/Settings.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"
#include "ui/IconProvider.h"
#include "ui/LoginDialog.h"
#include "ui/SessionWorkspace.h"
#include "ui/TransferQueueWidget.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

namespace wsc {
namespace {

class SessionTabBar : public QTabBar {
public:
    using QTabBar::QTabBar;

    QSize tabSizeHint(int index) const override
    {
        QSize size = QTabBar::tabSizeHint(index);
        size.setWidth(index == count() - 1 ? 32 : qBound(100, size.width(), 240));
        size.setHeight(qMax(26, size.height()));
        return size;
    }
};

bool sameEndpoint(const SessionData &left, const SessionData &right)
{
    return left.protocol == right.protocol && left.hostName.compare(right.hostName, Qt::CaseInsensitive) == 0 &&
           left.port == right.port && left.userName == right.userName && left.webDavHttps == right.webDavHttps;
}

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_transfers(new TransferManager(this))
{
    setMinimumSize(900, 600);
    setWindowIcon(IconProvider::instance().appIcon());
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_tabs = new SessionTabBar(central);
    m_tabs->setObjectName(QStringLiteral("SessionTabs"));
    m_tabs->setAccessibleName(QStringLiteral("会话标签页"));
    m_tabs->setExpanding(false);
    m_tabs->setTabsClosable(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->addTab(QString());
    m_tabs->setTabIcon(0, IconProvider::instance().icon(QStringLiteral("plus")));
    m_tabs->setTabToolTip(0, QStringLiteral("新建会话 / 登录"));
    for (auto side : {QTabBar::LeftSide, QTabBar::RightSide}) {
        QWidget *button = m_tabs->tabButton(0, side);
        m_tabs->setTabButton(0, side, nullptr);
        delete button;
    }
    m_stack = new QStackedWidget(central);
    m_stack->setObjectName(QStringLiteral("SessionWorkspaces"));
    layout->addWidget(m_tabs);
    layout->addWidget(m_stack, 1);
    setCentralWidget(central);

    m_queueDialog = new QDialog(this, Qt::Window);
    m_queueDialog->setObjectName(QStringLiteral("TransferQueueWindow"));
    m_queueDialog->setWindowTitle(QStringLiteral("传输进度"));
    m_queueDialog->setMinimumWidth(560);
    m_queueDialog->resize(760, 320);
    auto *queueLayout = new QVBoxLayout(m_queueDialog);
    queueLayout->setContentsMargins(0, 0, 0, 0);
    queueLayout->addWidget(new TransferQueueWidget(m_transfers, m_queueDialog));
    connect(m_queueDialog, &QDialog::finished, this, [this] { showTransferQueue(false); });
    m_transfers->setParallelLimit(Settings::instance().parallelTransfers);
    m_transfers->setConflictResolver([this](const QString &target, TransferDirection direction,
                                            qint64 sourceSize, qint64 targetSize) {
        return askConflict(target, direction, sourceSize, targetSize);
    });
    connect(m_transfers, &TransferManager::taskAdded, this, [this] { showTransferQueue(true); });
    connect(m_transfers, &TransferManager::taskStateChanged, this, [this](int id) {
        const auto row = m_transfers->model()->rowAt(m_transfers->model()->rowForId(id));
        if (row.status == TransferStatus::Failed || row.status == TransferStatus::Cancelled)
            m_transferBatchHasErrors = true;
    });
    connect(m_transfers, &TransferManager::allTransfersFinished, this, [this] {
        if (!m_transferBatchHasErrors)
            showTransferQueue(false);
        m_transferBatchHasErrors = false;
    });

    m_emptyWorkspace = createWorkspace();
    activate(m_emptyWorkspace);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_sessions.size()) {
            activate(m_sessions.at(index));
        } else if (index == m_sessions.size()) {
            const QSignalBlocker blocker(m_tabs);
            m_tabs->setCurrentIndex(qMax(0, static_cast<int>(m_sessions.indexOf(m_active))));
            requestLogin();
        }
    });
    connect(m_tabs, &QTabBar::tabBarClicked, this, [this](int index) {
        if (index == m_sessions.size())
            requestLogin();
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, &MainWindow::closeSession);

    const auto &settings = Settings::instance();
    if (!restoreGeometry(settings.windowGeometry)) {
        const QRect available = QGuiApplication::primaryScreen()
            ? QGuiApplication::primaryScreen()->availableGeometry() : QRect(0, 0, 1440, 900);
        resize(std::min(1100, available.width() - 80), std::min(760, available.height() - 80));
        move(available.center() - rect().center());
    }
    if (!settings.windowState.isEmpty())
        restoreState(settings.windowState, 1);
}

MainWindow::~MainWindow()
{
    shutdown();
}

SessionWorkspace *MainWindow::activeWorkspace() const
{
    return m_active;
}

SessionWorkspace *MainWindow::createWorkspace()
{
    auto *workspace = new SessionWorkspace(m_transfers, m_stack);
    workspace->menuBar()->hide();
    workspace->removeToolBar(workspace->toolBar());
    workspace->toolBar()->hide();
    m_stack->addWidget(workspace);
    connect(workspace, &SessionWorkspace::loginRequested, this, &MainWindow::requestLogin);
    connect(workspace, &SessionWorkspace::sessionRequested, this,
            [this](const SessionData &session) { openSession(session, true); });
    connect(workspace, &SessionWorkspace::sessionChanged, this, [this, workspace] { updateTab(workspace); });
    connect(workspace, &SessionWorkspace::exitRequested, this, &QWidget::close);
    connect(workspace, &SessionWorkspace::transferQueueVisibilityRequested, this, &MainWindow::showTransferQueue);
    connect(workspace, &SessionWorkspace::preferencesChanged, this, [this, workspace] {
        for (auto *view : m_stack->findChildren<SessionWorkspace *>(QString(), Qt::FindDirectChildrenOnly))
            if (view != workspace)
                view->applyPreferences();
    });
    workspace->setTransferQueueVisible(m_queueDialog->isVisible());
    return workspace;
}

void MainWindow::activate(SessionWorkspace *workspace)
{
    if (!workspace || m_closing)
        return;
    if (m_active && m_active != workspace) {
        auto *previous = m_active->toolBar();
        removeToolBar(previous);
        previous->setParent(m_active);
        previous->hide();
    }
    m_active = workspace;
    menuBar()->clear();
    for (auto *action : workspace->menuBar()->actions())
        menuBar()->addAction(action);
    addToolBar(workspace->toolBar());
    workspace->toolBar()->show();
    m_stack->setCurrentWidget(workspace);
    const int index = m_sessions.indexOf(workspace);
    const QSignalBlocker blocker(m_tabs);
    m_tabs->setCurrentIndex(index < 0 ? 0 : index);
    workspace->activate();
    setWindowTitle(workspace->windowTitle());
}

SessionWorkspace *MainWindow::openSession(const SessionData &session, bool connectNow,
                                         SessionWorkspace *workspace)
{
    if (!workspace)
        workspace = m_emptyWorkspace ? m_emptyWorkspace : createWorkspace();
    if (workspace == m_emptyWorkspace)
        m_emptyWorkspace = nullptr;
    if (!m_sessions.contains(workspace)) {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->insertTab(m_sessions.size(), session.displayName());
        m_sessions.append(workspace);
    }
    activate(workspace);
    workspace->setInitialSession(session, connectNow);
    updateTab(workspace);
    return workspace;
}

void MainWindow::setInitialSession(const SessionData &session, bool connectNow)
{
    SessionWorkspace *reuse = m_active && !m_active->hasConnection() && sameEndpoint(m_active->session(), session)
        ? m_active.data() : nullptr;
    const bool needsCredentials = connectNow && session.password.isEmpty() && session.privateKeyFile.isEmpty() &&
                                  !session.hostName.isEmpty();
    auto *workspace = openSession(session, connectNow && !needsCredentials, reuse);
    if (needsCredentials)
        QTimer::singleShot(0, this, [this, view = QPointer<SessionWorkspace>(workspace)] {
            if (view && !m_closing)
                editCredentials(view);
        });
}

void MainWindow::updateTab(SessionWorkspace *workspace)
{
    const int index = m_sessions.indexOf(workspace);
    if (index >= 0) {
        m_tabs->setTabText(index, workspace->session().displayName());
        m_tabs->setTabToolTip(index, workspace->windowTitle());
    }
    if (workspace == m_active)
        setWindowTitle(workspace->windowTitle());
}

void MainWindow::requestLogin()
{
    if (m_closing || m_loginOpen || m_loginPending)
        return;
    m_loginPending = true;
    QTimer::singleShot(0, this, [this] {
        newSession();
        m_loginPending = false;
    });
}

void MainWindow::newSession()
{
    if (m_closing || m_loginOpen)
        return;
    m_loginOpen = true;
    const auto reset = qScopeGuard([this] { m_loginOpen = false; });
    SessionData initial = m_active ? m_active->session() : SessionData();
    if (!initial.isValid() && !Settings::instance().recentSessions.isEmpty())
        initial = Settings::instance().recentSessions.first();
    if (!initial.isValid()) {
        initial.timeoutSeconds = Settings::instance().connectionTimeout;
        initial.keepAliveSeconds = Settings::instance().keepAlive;
    }
    initial.password.clear();
    initial.savePassword = false;
    LoginDialog dialog(initial, this);
    if (dialog.exec() == QDialog::Accepted && !m_closing && dialog.session().protocol != Protocol::Local)
        openSession(dialog.session(), true);
}

void MainWindow::editCredentials(SessionWorkspace *workspace)
{
    if (m_loginOpen || m_closing)
        return;
    m_loginOpen = true;
    const auto reset = qScopeGuard([this] { m_loginOpen = false; });
    LoginDialog dialog(workspace->session(), this);
    if (dialog.exec() == QDialog::Accepted && !m_closing)
        openSession(dialog.session(), true, workspace);
}

void MainWindow::closeSession(int index)
{
    if (m_closing || index < 0 || index >= m_sessions.size())
        return;
    auto *workspace = m_sessions.takeAt(index);
    const QSignalBlocker blocker(m_tabs);
    m_tabs->removeTab(index);
    if (m_sessions.isEmpty()) {
        m_emptyWorkspace = createWorkspace();
        activate(m_emptyWorkspace);
    } else if (workspace == m_active) {
        activate(m_sessions.at(qMin(index, static_cast<int>(m_sessions.size() - 1))));
    } else {
        m_tabs->setCurrentIndex(m_sessions.indexOf(m_active));
    }
    workspace->shutdown();
    m_stack->removeWidget(workspace);
    workspace->deleteLater();
    if (m_sessions.isEmpty() && Settings::instance().showLoginDialog)
        requestLogin();
}

void MainWindow::enableDemoMode()
{
    auto *workspace = m_emptyWorkspace ? m_emptyWorkspace : createWorkspace();
    workspace->enableDemoMode();
    openSession(workspace->session(), false, workspace);
}

void MainWindow::showTransferQueue(bool visible)
{
    if (m_closing)
        return;
    for (auto *workspace : m_stack->findChildren<SessionWorkspace *>(QString(), Qt::FindDirectChildrenOnly))
        workspace->setTransferQueueVisible(visible);
    m_queueDialog->setVisible(visible);
    if (visible)
        m_queueDialog->raise();
}

ConflictAction MainWindow::askConflict(const QString &targetPath, TransferDirection direction,
                                       qint64 sourceSize, qint64 targetSize)
{
    if (m_closing)
        return ConflictAction::Cancel;
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("目标文件已存在"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("目标已存在："));
    box.setInformativeText(QStringLiteral("%1\n\n方向：%2\n源大小：%3\n目标大小：%4")
        .arg(targetPath, transferDirectionName(direction), path::formatSize(sourceSize), path::formatSize(targetSize)));
    auto *overwrite = box.addButton(QStringLiteral("覆盖"), QMessageBox::AcceptRole);
    auto *skip = box.addButton(QStringLiteral("跳过"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("取消传输"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == overwrite)
        return ConflictAction::Overwrite;
    return box.clickedButton() == skip ? ConflictAction::Skip : ConflictAction::Cancel;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_closing && Settings::instance().confirmExit &&
        QMessageBox::question(this, QStringLiteral("退出"), QStringLiteral("确定要退出 WinSCP Qt 吗？")) != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    const int active = m_transfers->model()->activeCount();
    if (!m_closing && active > 0 && QMessageBox::question(this, QStringLiteral("传输仍在进行"),
        QStringLiteral("仍有 %1 个传输任务。退出将取消它们，是否继续？").arg(active)) != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    auto &settings = Settings::instance();
    settings.windowGeometry = saveGeometry();
    settings.windowState = saveState(1);
    if (m_active)
        m_active->saveLayoutSettings();
    settings.save();
    shutdown();
    event->accept();
}

void MainWindow::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    for (auto *workspace : m_stack->findChildren<SessionWorkspace *>(QString(), Qt::FindDirectChildrenOnly))
        workspace->shutdown();
    m_transfers->shutdown();
}

}
