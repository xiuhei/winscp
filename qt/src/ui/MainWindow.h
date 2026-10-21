#pragma once

#include "core/SessionData.h"
#include "transfer/TransferTypes.h"

#include <QList>
#include <QMainWindow>
#include <QPointer>

class QDialog;
class QStackedWidget;
class QTabBar;

namespace wsc {

class SessionWorkspace;
class TransferManager;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void setInitialSession(const SessionData &session, bool connectNow);
    void enableDemoMode();
    void newSession();
    SessionWorkspace *activeWorkspace() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    SessionWorkspace *createWorkspace();
    SessionWorkspace *openSession(const SessionData &session, bool connectNow,
                                  SessionWorkspace *workspace = nullptr);
    void activate(SessionWorkspace *workspace);
    void updateTab(SessionWorkspace *workspace);
    void closeSession(int index);
    void requestLogin();
    void editCredentials(SessionWorkspace *workspace);
    void showTransferQueue(bool visible);
    void shutdown();
    ConflictAction askConflict(const QString &targetPath, TransferDirection direction,
                               qint64 sourceSize, qint64 targetSize);

    QTabBar *m_tabs = nullptr;
    QStackedWidget *m_stack = nullptr;
    QList<SessionWorkspace *> m_sessions;
    SessionWorkspace *m_emptyWorkspace = nullptr;
    QPointer<SessionWorkspace> m_active;
    TransferManager *m_transfers = nullptr;
    QDialog *m_queueDialog = nullptr;
    bool m_closing = false;
    bool m_loginOpen = false;
    bool m_loginPending = false;
    bool m_transferBatchHasErrors = false;
};

}
