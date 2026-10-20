// SPDX-License-Identifier: MIT
#pragma once

#include "ui/Theme.h"

#include <QByteArray>
#include <QColor>
#include <QPoint>
#include <QString>
#include <QVector>

namespace wsc {

/// Compact color representation used by terminal cells.
struct TermColor {
    quint32 value = 0;

    bool isDefault() const { return value == 0; }
    static TermColor defaultValue() { return TermColor{0}; }
    static TermColor indexed(int index) { return TermColor{0x01000000u | (index & 0xFFu)}; }
    static TermColor rgb(int red, int green, int blue)
    {
        return TermColor{0x02000000u | (static_cast<quint32>(red) << 16) |
                         (static_cast<quint32>(green) << 8) | static_cast<quint32>(blue)};
    }
    bool isIndexed() const { return (value & 0xFF000000u) == 0x01000000u; }
    bool isRgb() const { return (value & 0xFF000000u) == 0x02000000u; }
    int index() const { return static_cast<int>(value & 0xFFu); }
    int red() const { return static_cast<int>((value >> 16) & 0xFFu); }
    int green() const { return static_cast<int>((value >> 8) & 0xFFu); }
    int blue() const { return static_cast<int>(value & 0xFFu); }
};

struct TerminalCell {
    QString text;
    TermColor foreground;
    TermColor background;
    quint16 attributes = 0;
    bool continuation = false;
};

struct TerminalLine {
    QVector<TerminalCell> cells;
    bool wrapped = false;
};

/// VT100/xterm compatible screen buffer and escape-sequence parser.
class TerminalScreen {
public:
    enum Attribute : quint16 {
        Bold = 0x0001,
        Dim = 0x0002,
        Italic = 0x0004,
        Underline = 0x0008,
        Blink = 0x0010,
        Reverse = 0x0020,
        Strike = 0x0040,
        Wide = 0x0080,
    };

    TerminalScreen(int columns = 80, int rows = 24);

    void setPalette(const TerminalPalette &palette);
    const TerminalPalette &palette() const { return m_palette; }

    void setSize(int columns, int rows);
    int columns() const { return m_columns; }
    int rows() const { return m_rows; }

    void reset();
    void clearScreen();
    void clearScrollback();
    void setScrollbackLimit(int lines);

    void feed(const QString &text);
    void feed(const QChar &character);

    QByteArray takeResponses();
    bool takeBell();
    bool takeTitleChanged();
    QStringList takeWorkingDirectories();
    QString takeShellName();
    QString title() const { return m_title; }

    int scrollbackCount() const { return m_scrollback.size(); }
    int totalLines() const { return m_scrollback.size() + m_lines.size(); }
    const TerminalLine &lineAt(int absoluteIndex) const;
    TerminalLine &mutableLineAt(int absoluteIndex);

    int cursorRow() const { return m_cursorRow; }
    int cursorColumn() const { return m_cursorColumn; }
    int absoluteCursorRow() const { return m_scrollback.size() + m_cursorRow; }
    bool cursorVisible() const { return m_cursorVisible; }
    bool applicationCursorKeys() const { return m_applicationCursorKeys; }
    bool bracketedPaste() const { return m_bracketedPaste; }
    bool mouseTracking() const { return m_mouseTracking; }
    bool usingAlternateScreen() const { return m_alternateScreen; }
    bool autoWrap() const { return m_autoWrap; }
    int cursorStyle() const { return m_cursorStyle; }

    QString textRange(int startLine, int startColumn, int endLine, int endColumn) const;
    QString lineText(int absoluteIndex) const;

    QColor colorFor(TermColor color, bool bold, bool background) const;

private:
    enum class State {
        Ground,
        Escape,
        EscapeIntermediate,
        Csi,
        CsiIgnore,
        Osc,
        Dcs,
    };

    TerminalLine blankLine(bool useCurrentBackground = false) const;
    TerminalCell blankCell(bool useCurrentBackground = false) const;
    void trimScrollback();
    void ensureLineWidth(TerminalLine *line);
    void putCharacter(const QString &text, int width);
    void carriageReturn();
    void lineFeed();
    void reverseIndex();
    void index();
    void scrollUp(int count = 1);
    void scrollDown(int count = 1);
    void tab();
    void backspace();
    void saveCursor();
    void restoreCursor();
    void enterAlternateScreen(bool clear);
    void leaveAlternateScreen();
    void setCursor(int row, int column, bool clampToRegion = false);
    void moveCursor(int deltaRow, int deltaColumn);
    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    void insertCharacters(int count);
    void deleteCharacters(int count);
    void insertLines(int count);
    void deleteLines(int count);
    void eraseCharacters(int count);
    void repeatCharacter(int count);
    void setSgr(const QVector<int> &parameters, const QString &raw);
    void setMode(const QVector<int> &parameters, bool enabled, bool priv);
    void handleCsi(char final, const QString &parameters);
    void handleOsc(const QByteArray &payload);
    void handleEscapeSequence(char final);
    void resetParser();
    int parameter(const QVector<int> &values, int index, int defaultValue) const;
    void clampCursor();
    void clearWideAt(TerminalLine *line, int column);
    QColor blend(const QColor &color, const QColor &background, double factor) const;

    TerminalPalette m_palette;
    int m_columns = 80;
    int m_rows = 24;
    QVector<TerminalLine> m_lines;
    QVector<TerminalLine> m_scrollback;
    int m_scrollbackLimit = 5000;

    QVector<TerminalLine> m_mainLines;
    mutable TerminalLine m_blankLineCache;
    int m_mainCursorRow = 0;
    int m_mainCursorColumn = 0;
    TermColor m_mainForeground;
    TermColor m_mainBackground;
    quint16 m_mainAttributes = 0;
    bool m_mainPendingWrap = false;
    int m_mainScrollTop = 0;
    int m_mainScrollBottom = 23;

    int m_cursorRow = 0;
    int m_cursorColumn = 0;
    bool m_pendingWrap = false;
    bool m_cursorVisible = true;
    bool m_applicationCursorKeys = false;
    bool m_bracketedPaste = false;
    bool m_mouseTracking = false;
    bool m_autoWrap = true;
    bool m_originMode = false;
    bool m_insertMode = false;
    bool m_alternateScreen = false;
    int m_cursorStyle = 1;
    int m_scrollTop = 0;
    int m_scrollBottom = 23;
    QVector<bool> m_tabStops;

    TermColor m_currentForeground;
    TermColor m_currentBackground;
    quint16 m_currentAttributes = 0;

    // Saved cursor state (DECSC).
    int m_savedRow = 0;
    int m_savedColumn = 0;
    TermColor m_savedForeground;
    TermColor m_savedBackground;
    quint16 m_savedAttributes = 0;
    bool m_savedOriginMode = false;
    bool m_savedWrapMode = true;

    State m_state = State::Ground;
    QString m_csiParameters;
    QString m_csiIntermediate;
    QChar m_csiPrivate;
    QByteArray m_oscBuffer;
    QStringList m_workingDirectories;
    QString m_shellName;
    QByteArray m_responses;
    bool m_bell = false;
    bool m_titleChanged = false;
    QString m_title;
    bool m_sawEscapeInOsc = false;
};

} // namespace wsc
