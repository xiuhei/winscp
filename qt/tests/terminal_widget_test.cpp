#include "terminal/TerminalWidget.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QVBoxLayout>
#include <QtTest>

using namespace wsc;

class TerminalWidgetTest : public QObject {
    Q_OBJECT
private slots:
    void keyboardStaysInTerminal()
    {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *terminal = new TerminalWidget(80, 24, &window);
        layout->addWidget(terminal);
        QAction upload(&window);
        upload.setShortcut(QKeySequence("Ctrl+U"));
        window.addAction(&upload);
        QSignalSpy action(&upload, &QAction::triggered);
        QSignalSpy input(terminal, &TerminalWidget::inputReady);
        terminal->setSessionRunning(true);
        window.show();
        window.activateWindow();
        terminal->setFocus();
        QTest::qWait(50);
        QTest::keyClick(terminal, Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(action.size(), 0);
        QCOMPARE(input.takeFirst().first().toByteArray(), QByteArray(1, '\x15'));
        QTest::keyClick(terminal, Qt::Key_Tab);
        QCOMPARE(input.takeFirst().first().toByteArray(), QByteArray("\t"));
        QTest::keyClick(terminal, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(input.takeFirst().first().toByteArray(), QByteArray(1, '\x03'));
        QTest::keyClick(terminal, Qt::Key_Up);
        QCOMPARE(input.takeFirst().first().toByteArray(), QByteArray("\x1b[A"));
        terminal->feed("\x1b[?1h");
        QTest::keyClick(terminal, Qt::Key_Up);
        QCOMPARE(input.takeFirst().first().toByteArray(), QByteArray("\x1bOA"));
    }

    void utf8AndBracketedPaste()
    {
        TerminalWidget terminal;
        terminal.setSessionRunning(true);
        QSignalSpy input(&terminal, &TerminalWidget::inputReady);
        const QByteArray text = QStringLiteral("中文").toUtf8();
        terminal.feed(text.left(2));
        terminal.feed(text.mid(2));
        QCOMPARE(terminal.screen()->lineText(0), QStringLiteral("中文"));
        terminal.feed("\x1b[?2004h");
        QApplication::clipboard()->setText(QStringLiteral("一行\n二行"));
        terminal.paste();
        const QByteArray paste = input.takeFirst().first().toByteArray();
        QVERIFY(paste.startsWith("\x1b[200~"));
        QVERIFY(paste.endsWith("\x1b[201~"));
        QVERIFY(paste.contains(QStringLiteral("一行").toUtf8()));
        QInputMethodEvent ime;
        ime.setCommitString(QStringLiteral("输入法"));
        QApplication::sendEvent(&terminal, &ime);
        QCOMPARE(input.takeFirst().first().toByteArray(), QStringLiteral("输入法").toUtf8());
    }
};

QTEST_MAIN(TerminalWidgetTest)
#include "terminal_widget_test.moc"
