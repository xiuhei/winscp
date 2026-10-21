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
    void compactScrollback();
    void styledScrollback();
    void sharedCharacterStorage();
    void resizeMovesLinesToScrollback();
};

void TerminalScreenTest::compactScrollback()
{
    TerminalScreen screen(160, 2);
    screen.setScrollbackLimit(100);
    for (int index = 0; index < 110; ++index)
        screen.feed(QString::number(index) + QStringLiteral("\r\n"));
    QCOMPARE(screen.scrollbackCount(), 100);
    QCOMPARE(screen.lineText(0), QStringLiteral("9"));
    QCOMPARE(screen.lineText(99), QStringLiteral("108"));
    QCOMPARE(screen.textRange(98, 0, 99, 159), QStringLiteral("107\n108"));
    for (int row = 0; row < screen.scrollbackCount(); ++row) {
        const auto &cells = screen.lineAt(row).cells;
        QVERIFY(cells.capacity() <= 3);
    }
    screen.setSize(80, 2);
    QCOMPARE(screen.lineText(0), QStringLiteral("9"));
    auto &line = screen.mutableLineAt(0);
    QCOMPARE(line.cells.size(), 80);
    line.cells[79].text = QStringLiteral("!");
    QCOMPARE(screen.lineText(0).size(), 80);
    screen.clearScrollback();
    QCOMPARE(screen.scrollbackCount(), 0);
}

void TerminalScreenTest::styledScrollback()
{
    TerminalScreen screen(10, 2);
    screen.feed(QStringLiteral("\x1b[44m\x1b[2K\x1b[0m\r\n\r\n"));
    QCOMPARE(screen.lineAt(0).cells.size(), 10);
    QCOMPARE(screen.lineAt(0).cells.at(9).background.index(), 4);
    screen.feed(QStringLiteral("\r\n"));
    QVERIFY(screen.lineAt(1).cells.isEmpty());
    QCOMPARE(screen.textRange(0, 0, 1, 9), QStringLiteral("\n"));

    screen.feed(QStringLiteral("你好\r\n\r\n"));
    const int row = screen.scrollbackCount() - 1;
    QCOMPARE(screen.lineText(row), QStringLiteral("你好"));
    QCOMPARE(screen.lineAt(row).cells.size(), 4);
    QVERIFY(screen.lineAt(row).cells.at(3).continuation);
}

void TerminalScreenTest::sharedCharacterStorage()
{
    TerminalScreen screen(20, 2);
    screen.feed(QStringLiteral("ee\u0301\U0001F642\u4f60"));
    QCOMPARE(screen.lineText(0), QStringLiteral("ee\u0301\U0001F642\u4f60"));
    QCOMPARE(screen.lineAt(0).cells.at(0).text, QStringLiteral("e"));
    QCOMPARE(screen.lineAt(0).cells.at(1).text, QStringLiteral("e\u0301"));
    QCOMPARE(screen.cursorColumn(), 6);
    screen.feed(QStringLiteral("\r\nee"));
    const auto &cells = screen.lineAt(1).cells;
    QCOMPARE(cells.at(0).text.constData(), cells.at(1).text.constData());
    QCOMPARE(cells.at(0).text.constData(), screen.lineAt(0).cells.at(0).text.constData());
    screen.feed(QStringLiteral("\x1b[?1049hX\x1b[?1049lY"));
    QCOMPARE(screen.lineText(1), QStringLiteral("eeY"));
    QCOMPARE(screen.lineAt(0).cells.at(0).text, QStringLiteral("e"));
}

void TerminalScreenTest::resizeMovesLinesToScrollback()
{
    TerminalScreen screen(160, 4);
    screen.feed(QStringLiteral("one\r\ntwo\r\nthree\r\nfour"));
    screen.setSize(160, 2);
    QCOMPARE(screen.scrollbackCount(), 2);
    QCOMPARE(screen.lineText(0), QStringLiteral("one"));
    QCOMPARE(screen.lineText(1), QStringLiteral("two"));
    QCOMPARE(screen.lineAt(0).cells.capacity(), 3);
    QCOMPARE(screen.lineAt(1).cells.capacity(), 3);
    QCOMPARE(screen.lineText(2), QStringLiteral("three"));
    QCOMPARE(screen.lineText(3), QStringLiteral("four"));
}

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
