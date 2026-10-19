#include "core/FileSystemController.h"
#include "core/Settings.h"
#include "terminal/TerminalWidget.h"
#include "ui/MainWindow.h"
#include "ui/TerminalPanel.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QSplitter>
#include <QtTest>
#include <algorithm>

using namespace wsc;

class WindowTest : public QObject {
    Q_OBJECT
public:
    SessionData ssh;
    SessionData ftp;

private slots:
    void automaticTerminal()
    {
        Settings::instance().load();
        Settings::instance().askUnknownHostKey = false;
        m_window = std::make_unique<MainWindow>();
        m_window->show();
        m_window->setInitialSession(ssh, true);
        m_fs = m_window->findChild<FileSystemController *>();
        m_panel = m_window->findChild<TerminalPanel *>();
        QVERIFY(m_fs);
        QVERIFY(m_panel);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs->isConnected(), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(m_panel->terminalCount(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(m_panel->findChild<TerminalWidget *>()->isSessionRunning(), 10000);
        auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("MainSplitter"));
        QVERIFY(splitter);
        QCOMPARE(splitter->orientation(), Qt::Vertical);
        QCOMPARE(splitter->indexOf(m_panel), 1);
    }

    void multipleShells()
    {
        m_panel->openTerminal();
        QTRY_COMPARE_WITH_TIMEOUT(m_panel->terminalCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(allRunning(), 10000);
    }

    void disconnectAndReconnect()
    {
        m_fs->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!m_fs->isConnected(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_panel->terminalCount(), 0, 10000);
        m_window->setInitialSession(ssh, true);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs->isConnected(), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(m_panel->terminalCount(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(allRunning(), 10000);
    }

    void switchToNonSshSession()
    {
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (box && box->isVisible()) {
                    if (auto *yes = box->button(QMessageBox::Yes))
                        yes->click();
                }
            }
        });
        answer.start(20);
        m_window->setInitialSession(ftp, true);
        QTRY_VERIFY_WITH_TIMEOUT(m_fs->isConnected() && m_fs->protocol() == Protocol::Ftp, 20000);
        QCOMPARE(m_panel->terminalCount(), 0);
        for (auto *action : m_window->findChildren<QAction *>())
            QVERIFY(!action->text().contains(QStringLiteral("关于")));
    }

    void cleanupTestCase() { m_window.reset(); }

private:
    bool allRunning() const
    {
        const auto terminals = m_panel->findChildren<TerminalWidget *>();
        return !terminals.isEmpty() && std::all_of(terminals.begin(), terminals.end(),
                                                  [](auto *terminal) { return terminal->isSessionRunning(); });
    }
    std::unique_ptr<MainWindow> m_window;
    FileSystemController *m_fs = nullptr;
    TerminalPanel *m_panel = nullptr;
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    Q_INIT_RESOURCE(resources);
    if (argc < 3)
        return 2;
    WindowTest test;
    if (!SessionData::parseUrl(QString::fromLocal8Bit(argv[1]), &test.ssh) ||
        !SessionData::parseUrl(QString::fromLocal8Bit(argv[2]), &test.ftp))
        return 2;
    return QTest::qExec(&test, QStringList{QString::fromLocal8Bit(argv[0]), QStringLiteral("-o"),
                                          qEnvironmentVariable("WINSCPQT_TEST_LOG", QStringLiteral("-")) + QStringLiteral(",txt")});
}

#include "window_test.moc"
