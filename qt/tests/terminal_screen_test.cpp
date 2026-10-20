// SPDX-License-Identifier: MIT
#include "terminal/TerminalScreen.h"

#include <QtTest>

using namespace wsc;

class TerminalScreenTest : public QObject {
    Q_OBJECT
private slots:
    void basicText();
    void newline();
    void cursorAddressing();
    void eraseDisplay();
    void sgrAttributes();
    void wideCharacters();
    void scrollback();
    void resizePreservesContent();
    void alternateScreen();
    void titleAndResponses();
    void wrapping();
    void oversizedEscapeSequences();
};

void TerminalScreenTest::oversizedEscapeSequences()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("\x1b]0;") + QString(1024 * 1024, QLatin1Char('x')));
    QVERIFY(screen.title().isEmpty());
    screen.feed(QStringLiteral("\x07OK"));
    QCOMPARE(screen.lineText(0), QStringLiteral("OK"));
    screen.feed(QStringLiteral("\x1b[") + QString(1024 * 1024, QLatin1Char('1')) + QStringLiteral("m!"));
    QCOMPARE(screen.lineText(0), QStringLiteral("OK!"));
    screen.feed(QStringLiteral("\x1b]0;valid\x07"));
    QCOMPARE(screen.title(), QStringLiteral("valid"));
}

void TerminalScreenTest::basicText()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("hello"));
    QCOMPARE(screen.lineText(0), QStringLiteral("hello"));
    QCOMPARE(screen.cursorRow(), 0);
    QCOMPARE(screen.cursorColumn(), 5);
}

void TerminalScreenTest::newline()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("one\r\ntwo"));
    QCOMPARE(screen.lineText(0), QStringLiteral("one"));
    QCOMPARE(screen.lineText(1), QStringLiteral("two"));
    QCOMPARE(screen.cursorRow(), 1);
    QCOMPARE(screen.cursorColumn(), 3);
}

void TerminalScreenTest::cursorAddressing()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("\x1b[5;10Habc"));
    QCOMPARE(screen.cursorRow(), 4);
    QCOMPARE(screen.cursorColumn(), 12);
    QCOMPARE(screen.lineText(4).mid(9, 3), QStringLiteral("abc"));
}

void TerminalScreenTest::eraseDisplay()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("abc\x1b[2J"));
    QCOMPARE(screen.lineText(0), QString());
}

void TerminalScreenTest::sgrAttributes()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("\x1b[1;31mred\x1b[0m normal"));
    QCOMPARE(screen.lineText(0), QStringLiteral("red normal"));
    const TerminalCell &red = screen.lineAt(0).cells.at(0);
    QVERIFY(red.attributes & TerminalScreen::Bold);
    QVERIFY(red.foreground.isIndexed());
    QCOMPARE(red.foreground.index(), 1);
    QVERIFY(screen.lineAt(0).cells.at(3).attributes == 0);
}

void TerminalScreenTest::wideCharacters()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("你好"));
    const TerminalLine &line = screen.lineAt(0);
    QCOMPARE(line.cells.at(0).text, QStringLiteral("你"));
    QVERIFY(line.cells.at(1).continuation);
    QCOMPARE(line.cells.at(2).text, QStringLiteral("好"));
    QCOMPARE(screen.cursorColumn(), 4);
    QCOMPARE(screen.lineText(0), QStringLiteral("你好"));
}

void TerminalScreenTest::scrollback()
{
    TerminalScreen screen(80, 5);
    QString input;
    for (int index = 0; index < 10; ++index) {
        if (index > 0)
            input += QStringLiteral("\r\n");
        input += QString::number(index);
    }
    screen.feed(input);
    QVERIFY(screen.scrollbackCount() >= 4);
    QCOMPARE(screen.lineText(screen.totalLines() - 1), QStringLiteral("9"));
}

void TerminalScreenTest::resizePreservesContent()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("hello"));
    screen.setSize(80, 10);
    QCOMPARE(screen.rows(), 10);
    QCOMPARE(screen.lineText(0), QStringLiteral("hello"));
}

void TerminalScreenTest::alternateScreen()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("main"));
    screen.feed(QStringLiteral("\x1b[?1049h"));
    QVERIFY(screen.usingAlternateScreen());
    screen.feed(QStringLiteral("alt"));
    QCOMPARE(screen.lineText(0), QStringLiteral("alt"));
    screen.feed(QStringLiteral("\x1b[?1049l"));
    QVERIFY(!screen.usingAlternateScreen());
    QCOMPARE(screen.lineText(0), QStringLiteral("main"));
}

void TerminalScreenTest::titleAndResponses()
{
    TerminalScreen screen(80, 24);
    screen.feed(QStringLiteral("\x1b]2;my title\x07"));
    QVERIFY(screen.takeTitleChanged());
    QCOMPARE(screen.title(), QStringLiteral("my title"));
    screen.feed(QStringLiteral("\x1b[6n"));
    QVERIFY(screen.takeResponses().contains("\x1b[1;1R"));
}

void TerminalScreenTest::wrapping()
{
    TerminalScreen screen(10, 3);
    screen.feed(QStringLiteral("1234567890X"));
    QCOMPARE(screen.lineText(0), QStringLiteral("1234567890"));
    QCOMPARE(screen.lineText(1), QStringLiteral("X"));
    QCOMPARE(screen.cursorRow(), 1);
    QCOMPARE(screen.cursorColumn(), 1);
}

QTEST_APPLESS_MAIN(TerminalScreenTest)

#include "terminal_screen_test.moc"
