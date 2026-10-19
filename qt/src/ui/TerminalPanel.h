// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"
#include "core/SessionData.h"

#include <QFrame>
#include <QPointer>
#include <QVector>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QToolButton;

namespace wsc {

class SshShellSession;
class FileSystemController;
class TerminalWidget;

class TerminalPanel : public QFrame {
    Q_OBJECT
public:
    explicit TerminalPanel(QWidget *parent = nullptr);
    ~TerminalPanel() override;

    void setFileSystemController(FileSystemController *controller);
    void setSession(const SessionData &session);
    void openForSession(const SessionData &session, const QString &initialDirectory = {});
    void openTerminal();
    void openDemoTerminal();
    void closeAll();
    void applySettings();

    bool hasTerminals() const { return !m_tabsData.isEmpty(); }
    int terminalCount() const { return m_tabsData.size(); }
    void focusTerminal();

signals:
    void connectRequested();
    void statusMessage(const QString &message);
    void terminalCountChanged(int count);

private:
    struct TabData {
        TerminalWidget *widget = nullptr;
        QPointer<SshShellSession> session;
        SessionData sessionData;
        QString baseTitle;
        QString initialDirectory;
        bool connected = false;
    };

    void buildUi();
    void buildWelcomePage();
    void updateHint();
    void updateWelcomeText();
    void closeTab(int index);
    TerminalWidget *currentTerminal() const;
    int currentTabIndex() const;
    void appendTab(const TabData &data, const QString &title);
    void setTabTitle(int index, const QString &title);
    void zoomFont(int delta);

    QStackedWidget *m_stack = nullptr;
    QTabWidget *m_tabs = nullptr;
    QWidget *m_welcomePage = nullptr;
    QLabel *m_hintLabel = nullptr;
    QLabel *m_welcomeTitle = nullptr;
    QLabel *m_welcomeHint = nullptr;
    QPushButton *m_welcomeButton = nullptr;
    QToolButton *m_copyButton = nullptr;
    QToolButton *m_pasteButton = nullptr;
    QToolButton *m_clearButton = nullptr;
    QToolButton *m_reconnectButton = nullptr;
    QToolButton *m_newButton = nullptr;
    QToolButton *m_closeButton = nullptr;
    QVector<TabData> m_tabsData;
    SessionData m_session;
    FileSystemController *m_controller = nullptr;
    bool m_closing = false;
};

} // namespace wsc
