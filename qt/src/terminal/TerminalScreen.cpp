#include "terminal/TerminalScreen.h"
#include <QUrl>

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wsc {

namespace {

int codepointWidth(uint ucs4)
{
    if (ucs4 == 0)
        return 0;
    if (ucs4 < 32 || (ucs4 >= 0x7F && ucs4 < 0xA0))
        return 0;
    const auto in = [ucs4](uint first, uint last) { return ucs4 >= first && ucs4 <= last; };
    if (in(0x0300, 0x036F) || in(0x1AB0, 0x1AFF) || in(0x1DC0, 0x1DFF) ||
        in(0x20D0, 0x20FF) || in(0xFE20, 0xFE2F) || in(0xFE00, 0xFE0F) ||
        in(0xE0100, 0xE01EF) || ucs4 == 0x200D || in(0x1F3FB, 0x1F3FF))
        return 0;
    if (in(0x1100, 0x115F) || ucs4 == 0x2329 || ucs4 == 0x232A ||
        (in(0x2E80, 0xA4CF) && ucs4 != 0x303F) || in(0xAC00, 0xD7A3) ||
        in(0xF900, 0xFAFF) || in(0xFE10, 0xFE19) || in(0xFE30, 0xFE6F) ||
        in(0xFF00, 0xFF60) || in(0xFFE0, 0xFFE6) || in(0x1F300, 0x1FAFF) ||
        in(0x20000, 0x3FFFD))
        return 2;
    return 1;
}

QVector<int> parseParameters(const QString &raw)
{
    QVector<int> result;
    if (raw.isEmpty())
        return result;
    const QStringList parts = raw.split(QLatin1Char(';'));
    for (const QString &part : parts) {
        if (part.isEmpty()) {
            result.append(-1);
            continue;
        }
        bool ok = false;
        const int value = part.toInt(&ok);
        result.append(ok ? value : -1);
    }
    return result;
}

QString decodeUtf8(const QByteArray &data)
{
    return QString::fromUtf8(data);
}

} // namespace

TerminalScreen::TerminalScreen(int columns, int rows)
    : m_columns(std::max(2, columns))
    , m_rows(std::max(2, rows))
{
    m_palette = Theme::terminalPalette();
    reset();
}

void TerminalScreen::setPalette(const TerminalPalette &palette)
{
    m_palette = palette;
}

void TerminalScreen::reset()
{
    m_lines.clear();
    m_lines.reserve(m_rows);
    for (int row = 0; row < m_rows; ++row)
        m_lines.append(blankLine());
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_cursorRow = 0;
    m_cursorColumn = 0;
    m_pendingWrap = false;
    m_cursorVisible = true;
    m_applicationCursorKeys = false;
    m_bracketedPaste = false;
    m_mouseTracking = false;
    m_autoWrap = true;
    m_originMode = false;
    m_insertMode = false;
    m_alternateScreen = false;
    m_cursorStyle = 1;
    m_currentForeground = TermColor::defaultValue();
    m_currentBackground = TermColor::defaultValue();
    m_currentAttributes = 0;
    m_tabStops.clear();
    m_tabStops.resize(m_columns, false);
    for (int column = 8; column < m_columns; column += 8)
        m_tabStops[column] = true;
    resetParser();
    m_bell = false;
    m_titleChanged = false;
}

void TerminalScreen::clearScreen()
{
    for (TerminalLine &line : m_lines)
        line = blankLine();
    m_cursorRow = 0;
    m_cursorColumn = 0;
    m_pendingWrap = false;
}

void TerminalScreen::clearScrollback()
{
    m_scrollback.clear();
}

void TerminalScreen::setScrollbackLimit(int lines)
{
    m_scrollbackLimit = std::max(100, lines);
    trimScrollback();
}

void TerminalScreen::resetParser()
{
    m_state = State::Ground;
    m_csiParameters.clear();
    m_csiIntermediate.clear();
    m_csiPrivate = QChar();
    m_oscBuffer.clear();
    m_sawEscapeInOsc = false;
}

TerminalLine TerminalScreen::blankLine(bool useCurrentBackground) const
{
    TerminalLine line;
    line.cells.resize(m_columns);
    const TerminalCell cell = blankCell(useCurrentBackground);
    for (TerminalCell &target : line.cells)
        target = cell;
    return line;
}

TerminalCell TerminalScreen::blankCell(bool useCurrentBackground) const
{
    TerminalCell cell;
    if (useCurrentBackground)
        cell.background = m_currentBackground;
    return cell;
}

void TerminalScreen::ensureLineWidth(TerminalLine *line)
{
    if (!line)
        return;
    if (line->cells.size() < m_columns)
        line->cells.resize(m_columns);
    else if (line->cells.size() > m_columns)
        line->cells.resize(m_columns);
}

void TerminalScreen::setSize(int columns, int rows)
{
    columns = std::max(2, columns);
    rows = std::max(2, rows);
    if (columns == m_columns && rows == m_rows)
        return;

    const int oldRows = m_rows;
    m_columns = columns;
    m_rows = rows;

    if (!m_alternateScreen) {
        if (m_cursorRow < m_rows) {
            while (m_lines.size() > m_rows)
                m_lines.removeLast();
        } else {
            while (m_lines.size() > m_rows) {
                m_scrollback.append(m_lines.takeFirst());
                if (m_cursorRow > 0)
                    --m_cursorRow;
            }
        }
    } else {
        while (m_lines.size() > m_rows)
            m_lines.removeLast();
    }
    while (m_lines.size() < m_rows)
        m_lines.append(blankLine());
    for (TerminalLine &line : m_lines)
        ensureLineWidth(&line);

    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_cursorRow = qBound(0, m_cursorRow, m_rows - 1);
    m_cursorColumn = qBound(0, m_cursorColumn, m_columns - 1);
    m_pendingWrap = false;
    m_tabStops.resize(m_columns, false);
    for (int column = 8; column < m_columns; column += 8) {
        if (column >= m_tabStops.size())
            break;
        m_tabStops[column] = true;
    }
    trimScrollback();
    Q_UNUSED(oldRows);
}

void TerminalScreen::trimScrollback()
{
    while (m_scrollback.size() > m_scrollbackLimit)
        m_scrollback.removeFirst();
}

const TerminalLine &TerminalScreen::lineAt(int absoluteIndex) const
{
    if (absoluteIndex < 0)
        absoluteIndex = 0;
    if (absoluteIndex < m_scrollback.size())
        return m_scrollback.at(absoluteIndex);
    const int row = absoluteIndex - m_scrollback.size();
    if (row >= 0 && row < m_lines.size())
        return m_lines.at(row);
    m_blankLineCache = blankLine();
    return m_blankLineCache;
}

TerminalLine &TerminalScreen::mutableLineAt(int absoluteIndex)
{
    if (absoluteIndex < 0)
        absoluteIndex = 0;
    if (absoluteIndex < m_scrollback.size())
        return m_scrollback[absoluteIndex];
    int row = absoluteIndex - m_scrollback.size();
    if (row >= m_lines.size()) {
        row = m_lines.size() - 1;
    }
    return m_lines[row];
}

void TerminalScreen::feed(const QString &text)
{
    for (int index = 0; index < text.size(); ++index) {
        QChar character = text.at(index);
        uint codepoint = character.unicode();
        QString grapheme(character);
        if (character.isHighSurrogate() && index + 1 < text.size() &&
            text.at(index + 1).isLowSurrogate()) {
            const char16_t pair[] = {character.unicode(), text.at(index + 1).unicode(), 0};
            grapheme = QString::fromUtf16(pair, 2);
            codepoint = grapheme.toUcs4().value(0);
            ++index;
        }

        if (m_state == State::Ground) {
            if (codepoint == 0x1B) {
                m_state = State::Escape;
                m_csiParameters.clear();
                m_csiIntermediate.clear();
                m_csiPrivate = QChar();
                m_pendingWrap = false;
                continue;
            }
            if (codepoint < 0x20 || codepoint == 0x7F) {
                switch (codepoint) {
                case 0x07:
                    m_bell = true;
                    break;
                case 0x08:
                    backspace();
                    break;
                case 0x09:
                    tab();
                    break;
                case 0x0A:
                case 0x0B:
                case 0x0C:
                    this->index();
                    break;
                case 0x0D:
                    carriageReturn();
                    break;
                case 0x0E:
                case 0x0F:
                case 0x00:
                default:
                    break;
                }
                continue;
            }
            if (codepoint == 0x7F)
                continue;
            putCharacter(grapheme, codepointWidth(codepoint));
            continue;
        }

        switch (m_state) {
        case State::Escape:
            if (codepoint == 0x1B) {
                m_state = State::Escape;
            } else if (codepoint == '[') {
                m_state = State::Csi;
                m_csiParameters.clear();
                m_csiIntermediate.clear();
                m_csiPrivate = QChar();
            } else if (codepoint == ']') {
                m_state = State::Osc;
                m_oscBuffer.clear();
                m_sawEscapeInOsc = false;
            } else if (codepoint == 'P' || codepoint == 'X' || codepoint == '^' ||
                       codepoint == '_') {
                m_state = State::Dcs;
            } else if (codepoint == '(' || codepoint == ')' || codepoint == '*' ||
                       codepoint == '+' || codepoint == '-' || codepoint == '.' ||
                       codepoint == '/' || codepoint == '#' || codepoint == '%') {
                m_state = State::EscapeIntermediate;
            } else {
                handleEscapeSequence(static_cast<char>(codepoint));
                m_state = State::Ground;
            }
            break;
        case State::EscapeIntermediate:
            m_state = State::Ground;
            break;
        case State::Csi:
            if (codepoint == 0x1B) {
                m_state = State::Escape;
            } else if (codepoint == 0x18 || codepoint == 0x1A) {
                m_state = State::Ground;
            } else if (codepoint >= 0x20 && codepoint <= 0x2F) {
                if (m_csiPrivate.isNull())
                    m_csiIntermediate.append(character);
                else
                    m_csiIntermediate.append(character);
            } else if (codepoint >= 0x30 && codepoint <= 0x3F) {
                if (m_csiParameters.isEmpty() && m_csiIntermediate.isEmpty() &&
                    (codepoint == '?' || codepoint == '>' || codepoint == '!' ||
                     codepoint == '<' || codepoint == '=')) {
                    m_csiPrivate = character;
                } else {
                    m_csiParameters.append(character);
                }
            } else if (codepoint >= 0x40 && codepoint <= 0x7E) {
                handleCsi(static_cast<char>(codepoint), m_csiParameters);
                m_state = State::Ground;
            } else {
                m_state = State::CsiIgnore;
            }
            break;
        case State::CsiIgnore:
            if (codepoint >= 0x40 && codepoint <= 0x7E)
                m_state = State::Ground;
            else if (codepoint == 0x1B)
                m_state = State::Escape;
            break;
        case State::Osc:
            if (codepoint == 0x07) {
                handleOsc(m_oscBuffer);
                m_state = State::Ground;
            } else if (codepoint == 0x1B) {
                m_sawEscapeInOsc = true;
            } else if (m_sawEscapeInOsc) {
                if (codepoint == '\\') {
                    handleOsc(m_oscBuffer);
                    m_state = State::Ground;
                } else {
                    m_oscBuffer.append('\x1B');
                    m_oscBuffer.append(static_cast<char>(codepoint));
                    m_sawEscapeInOsc = false;
                }
            } else {
                m_oscBuffer.append(QString(character).toUtf8());
            }
            break;
        case State::Dcs:
            if (codepoint == 0x07) {
                m_state = State::Ground;
            } else if (codepoint == 0x1B) {
                m_sawEscapeInOsc = true;
            } else if (m_sawEscapeInOsc && codepoint == '\\') {
                m_state = State::Ground;
                m_sawEscapeInOsc = false;
            } else {
                m_sawEscapeInOsc = false;
            }
            break;
        case State::Ground:
            break;
        }
    }
}

void TerminalScreen::feed(const QChar &character)
{
    feed(QString(character));
}

void TerminalScreen::handleEscapeSequence(char final)
{
    switch (final) {
    case '7':
        saveCursor();
        break;
    case '8':
        restoreCursor();
        break;
    case 'D':
        index();
        break;
    case 'E':
        carriageReturn();
        index();
        break;
    case 'M':
        reverseIndex();
        break;
    case 'H':
        if (m_cursorColumn >= 0 && m_cursorColumn < m_tabStops.size())
            m_tabStops[m_cursorColumn] = true;
        break;
    case 'c':
        reset();
        break;
    case '\\':
    case '=':
    case '>':
    case 'N':
    case 'O':
    default:
        break;
    }
}

int TerminalScreen::parameter(const QVector<int> &values, int index, int defaultValue) const
{
    if (index < 0 || index >= values.size())
        return defaultValue;
    const int value = values.at(index);
    return value < 0 ? defaultValue : value;
}

void TerminalScreen::putCharacter(const QString &text, int width)
{
    if (width <= 0) {
        // Combining mark: append to the previous cell.
        TerminalLine &line = m_lines[m_cursorRow];
        int column = m_cursorColumn - 1;
        if (column < 0) {
            if (m_cursorRow == 0)
                return;
            TerminalLine &previous = m_lines[m_cursorRow - 1];
            if (!previous.cells.isEmpty())
                previous.cells.last().text.append(text);
            return;
        }
        if (column < line.cells.size())
            line.cells[column].text.append(text);
        return;
    }

    if (m_pendingWrap) {
        m_pendingWrap = false;
        carriageReturn();
        index();
    }
    if (width == 2 && m_cursorColumn >= m_columns - 1) {
        carriageReturn();
        index();
    }

    if (m_insertMode)
        insertCharacters(width);

    TerminalLine &line = m_lines[m_cursorRow];
    ensureLineWidth(&line);
    if (m_cursorColumn < line.cells.size()) {
        clearWideAt(&line, m_cursorColumn);
        if (width == 2)
            clearWideAt(&line, m_cursorColumn + 1);
    }

    TerminalCell cell;
    cell.text = text;
    cell.foreground = m_currentForeground;
    cell.background = m_currentBackground;
    cell.attributes = m_currentAttributes;
    if (width == 2)
        cell.attributes |= Wide;
    line.cells[m_cursorColumn] = cell;

    if (width == 2 && m_cursorColumn + 1 < line.cells.size()) {
        TerminalCell continuation;
        continuation.continuation = true;
        continuation.foreground = m_currentForeground;
        continuation.background = m_currentBackground;
        continuation.attributes = m_currentAttributes;
        line.cells[m_cursorColumn + 1] = continuation;
    }

    m_cursorColumn += width;
    if (m_cursorColumn >= m_columns) {
        m_cursorColumn = m_columns - 1;
        if (m_autoWrap)
            m_pendingWrap = true;
    }
}

void TerminalScreen::carriageReturn()
{
    m_cursorColumn = 0;
    m_pendingWrap = false;
}

void TerminalScreen::backspace()
{
    if (m_cursorColumn > 0)
        --m_cursorColumn;
    m_pendingWrap = false;
}

void TerminalScreen::tab()
{
    int column = m_cursorColumn + 1;
    while (column < m_columns && column < m_tabStops.size() && !m_tabStops.at(column))
        ++column;
    if (column >= m_columns)
        column = m_columns - 1;
    m_cursorColumn = column;
    m_pendingWrap = false;
}

void TerminalScreen::index()
{
    m_pendingWrap = false;
    if (m_cursorRow == m_scrollBottom)
        scrollUp();
    else if (m_cursorRow < m_rows - 1)
        ++m_cursorRow;
}

void TerminalScreen::lineFeed()
{
    index();
}

void TerminalScreen::reverseIndex()
{
    m_pendingWrap = false;
    if (m_cursorRow == m_scrollTop)
        scrollDown();
    else if (m_cursorRow > 0)
        --m_cursorRow;
}

void TerminalScreen::scrollUp(int count)
{
    count = std::max(1, count);
    for (int iteration = 0; iteration < count; ++iteration) {
        if (m_scrollTop < 0 || m_scrollBottom >= m_lines.size())
            return;
        TerminalLine removed = m_lines.takeAt(m_scrollTop);
        m_lines.insert(m_scrollBottom, blankLine());
        if (m_scrollTop == 0 && m_scrollBottom == m_rows - 1 && !m_alternateScreen) {
            m_scrollback.append(removed);
            trimScrollback();
        }
    }
    ensureLineWidth(&m_lines[m_scrollBottom]);
}

void TerminalScreen::scrollDown(int count)
{
    count = std::max(1, count);
    for (int iteration = 0; iteration < count; ++iteration) {
        if (m_scrollTop < 0 || m_scrollBottom >= m_lines.size())
            return;
        m_lines.removeAt(m_scrollBottom);
        m_lines.insert(m_scrollTop, blankLine());
    }
}

void TerminalScreen::setCursor(int row, int column, bool clampToRegion)
{
    if (m_originMode && clampToRegion) {
        row += m_scrollTop;
        row = qBound(m_scrollTop, row, m_scrollBottom);
    } else {
        row = qBound(0, row, m_rows - 1);
    }
    m_cursorRow = row;
    m_cursorColumn = qBound(0, column, m_columns - 1);
    m_pendingWrap = false;
}

void TerminalScreen::moveCursor(int deltaRow, int deltaColumn)
{
    m_cursorRow = qBound(0, m_cursorRow + deltaRow, m_rows - 1);
    m_cursorColumn = qBound(0, m_cursorColumn + deltaColumn, m_columns - 1);
    m_pendingWrap = false;
}

void TerminalScreen::insertCharacters(int count)
{
    count = qBound(1, count, m_columns);
    TerminalLine &line = m_lines[m_cursorRow];
    ensureLineWidth(&line);
    if (m_cursorColumn >= m_columns)
        return;
    for (int index = 0; index < count; ++index)
        line.cells.insert(m_cursorColumn, blankCell());
    while (line.cells.size() > m_columns)
        line.cells.removeLast();
}

void TerminalScreen::deleteCharacters(int count)
{
    count = qBound(1, count, m_columns);
    TerminalLine &line = m_lines[m_cursorRow];
    ensureLineWidth(&line);
    for (int index = 0; index < count; ++index) {
        if (m_cursorColumn < line.cells.size())
            line.cells.removeAt(m_cursorColumn);
        else
            break;
    }
    while (line.cells.size() < m_columns)
        line.cells.append(blankCell());
}

void TerminalScreen::insertLines(int count)
{
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom)
        return;
    count = std::max(1, count);
    for (int iteration = 0; iteration < count; ++iteration) {
        m_lines.removeAt(m_scrollBottom);
        m_lines.insert(m_cursorRow, blankLine());
    }
}

void TerminalScreen::deleteLines(int count)
{
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom)
        return;
    count = std::max(1, count);
    for (int iteration = 0; iteration < count; ++iteration) {
        m_lines.removeAt(m_cursorRow);
        m_lines.insert(m_scrollBottom, blankLine());
    }
}

void TerminalScreen::eraseCharacters(int count)
{
    count = std::max(1, count);
    TerminalLine &line = m_lines[m_cursorRow];
    ensureLineWidth(&line);
    for (int column = m_cursorColumn; column < std::min(m_columns, m_cursorColumn + count); ++column) {
        clearWideAt(&line, column);
        line.cells[column] = blankCell(true);
    }
}

void TerminalScreen::repeatCharacter(int count)
{
    if (count <= 0 || m_cursorColumn == 0)
        return;
    const int column = m_cursorColumn == 0 ? 0 : m_cursorColumn - 1;
    if (column >= m_columns)
        return;
    const TerminalCell cell = m_lines[m_cursorRow].cells.value(column);
    const QString text = cell.text;
    if (text.isEmpty())
        return;
    for (int index = 0; index < count; ++index)
        putCharacter(text, cell.attributes & Wide ? 2 : 1);
}

void TerminalScreen::eraseInDisplay(int mode)
{
    if (mode == 3) {
        m_scrollback.clear();
        return;
    }
    if (mode == 0) {
        eraseInLine(0);
        for (int row = m_cursorRow + 1; row < m_rows; ++row)
            m_lines[row] = blankLine();
    } else if (mode == 1) {
        for (int row = 0; row < m_cursorRow; ++row)
            m_lines[row] = blankLine();
        eraseInLine(1);
    } else if (mode == 2) {
        for (TerminalLine &line : m_lines)
            line = blankLine();
    }
}

void TerminalScreen::eraseInLine(int mode)
{
    TerminalLine &line = m_lines[m_cursorRow];
    ensureLineWidth(&line);
    if (mode == 0) {
        for (int column = m_cursorColumn; column < m_columns; ++column) {
            clearWideAt(&line, column);
            line.cells[column] = blankCell(true);
        }
    } else if (mode == 1) {
        for (int column = 0; column <= m_cursorColumn && column < m_columns; ++column) {
            clearWideAt(&line, column);
            line.cells[column] = blankCell(true);
        }
    } else if (mode == 2) {
        line = blankLine(true);
    }
}

void TerminalScreen::clearWideAt(TerminalLine *line, int column)
{
    if (!line || column < 0 || column >= line->cells.size())
        return;
    TerminalCell &cell = line->cells[column];
    if (cell.continuation) {
        if (column > 0)
            line->cells[column - 1] = blankCell();
        cell = blankCell();
        return;
    }
    if ((cell.attributes & Wide) && column + 1 < line->cells.size())
        line->cells[column + 1] = blankCell();
}

void TerminalScreen::saveCursor()
{
    m_savedRow = m_cursorRow;
    m_savedColumn = m_cursorColumn;
    m_savedForeground = m_currentForeground;
    m_savedBackground = m_currentBackground;
    m_savedAttributes = m_currentAttributes;
    m_savedOriginMode = m_originMode;
    m_savedWrapMode = m_autoWrap;
}

void TerminalScreen::restoreCursor()
{
    m_cursorRow = qBound(0, m_savedRow, m_rows - 1);
    m_cursorColumn = qBound(0, m_savedColumn, m_columns - 1);
    m_currentForeground = m_savedForeground;
    m_currentBackground = m_savedBackground;
    m_currentAttributes = m_savedAttributes;
    m_originMode = m_savedOriginMode;
    m_autoWrap = m_savedWrapMode;
    m_pendingWrap = false;
}

void TerminalScreen::enterAlternateScreen(bool clear)
{
    if (m_alternateScreen)
        return;
    m_mainLines = m_lines;
    m_mainCursorRow = m_cursorRow;
    m_mainCursorColumn = m_cursorColumn;
    m_mainForeground = m_currentForeground;
    m_mainBackground = m_currentBackground;
    m_mainAttributes = m_currentAttributes;
    m_mainPendingWrap = m_pendingWrap;
    m_mainScrollTop = m_scrollTop;
    m_mainScrollBottom = m_scrollBottom;
    m_alternateScreen = true;
    if (clear) {
        for (TerminalLine &line : m_lines)
            line = blankLine();
        m_cursorRow = 0;
        m_cursorColumn = 0;
        m_pendingWrap = false;
    }
}

void TerminalScreen::leaveAlternateScreen()
{
    if (!m_alternateScreen)
        return;
    m_alternateScreen = false;
    m_lines = m_mainLines;
    m_cursorRow = m_mainCursorRow;
    m_cursorColumn = m_mainCursorColumn;
    m_currentForeground = m_mainForeground;
    m_currentBackground = m_mainBackground;
    m_currentAttributes = m_mainAttributes;
    m_pendingWrap = m_mainPendingWrap;
    m_scrollTop = m_mainScrollTop;
    m_scrollBottom = m_mainScrollBottom;
    while (m_lines.size() < m_rows)
        m_lines.append(blankLine());
}

void TerminalScreen::setSgr(const QVector<int> &parameters, const QString &raw)
{
    QVector<int> values = parameters;
    if (values.isEmpty())
        values.append(0);
    const bool colonForm = raw.contains(QLatin1Char(':'));
    if (colonForm) {
        values.clear();
        const QStringList parts = raw.split(QLatin1Char(';'));
        for (const QString &part : parts) {
            const QStringList subParts = part.split(QLatin1Char(':'));
            for (const QString &sub : subParts) {
                bool ok = false;
                const int value = sub.toInt(&ok);
                values.append(ok ? value : 0);
            }
        }
    }

    for (int index = 0; index < values.size(); ++index) {
        const int value = values.at(index);
        switch (value) {
        case 0:
            m_currentAttributes = 0;
            m_currentForeground = TermColor::defaultValue();
            m_currentBackground = TermColor::defaultValue();
            break;
        case 1:
            m_currentAttributes |= Bold;
            break;
        case 2:
            m_currentAttributes |= Dim;
            break;
        case 3:
            m_currentAttributes |= Italic;
            break;
        case 4:
            m_currentAttributes |= Underline;
            break;
        case 5:
        case 6:
            m_currentAttributes |= Blink;
            break;
        case 7:
            m_currentAttributes |= Reverse;
            break;
        case 9:
            m_currentAttributes |= Strike;
            break;
        case 22:
            m_currentAttributes &= ~(Bold | Dim);
            break;
        case 23:
            m_currentAttributes &= ~Italic;
            break;
        case 24:
            m_currentAttributes &= ~Underline;
            break;
        case 25:
            m_currentAttributes &= ~Blink;
            break;
        case 27:
            m_currentAttributes &= ~Reverse;
            break;
        case 29:
            m_currentAttributes &= ~Strike;
            break;
        case 38:
        case 48: {
            const bool foreground = value == 38;
            if (index + 2 < values.size() && values.at(index + 1) == 5) {
                const int colorIndex = values.at(index + 2);
                if (foreground)
                    m_currentForeground = TermColor::indexed(colorIndex);
                else
                    m_currentBackground = TermColor::indexed(colorIndex);
                index += 2;
            } else if (index + 4 < values.size() && values.at(index + 1) == 2) {
                const TermColor color = TermColor::rgb(values.at(index + 2), values.at(index + 3),
                                                       values.at(index + 4));
                if (foreground)
                    m_currentForeground = color;
                else
                    m_currentBackground = color;
                index += 4;
            }
            break;
        }
        case 39:
            m_currentForeground = TermColor::defaultValue();
            break;
        case 49:
            m_currentBackground = TermColor::defaultValue();
            break;
        default:
            if (value >= 30 && value <= 37)
                m_currentForeground = TermColor::indexed(value - 30);
            else if (value >= 40 && value <= 47)
                m_currentBackground = TermColor::indexed(value - 40);
            else if (value >= 90 && value <= 97)
                m_currentForeground = TermColor::indexed(value - 90 + 8);
            else if (value >= 100 && value <= 107)
                m_currentBackground = TermColor::indexed(value - 100 + 8);
            break;
        }
    }
}

void TerminalScreen::setMode(const QVector<int> &parameters, bool enabled, bool priv)
{
    for (int value : parameters) {
        if (value < 0)
            continue;
        if (priv) {
            switch (value) {
            case 1:
                m_applicationCursorKeys = enabled;
                break;
            case 7:
                m_autoWrap = enabled;
                if (!enabled)
                    m_pendingWrap = false;
                break;
            case 12:
                break;
            case 25:
                m_cursorVisible = enabled;
                break;
            case 47:
            case 1047:
            case 1049:
                if (enabled) {
                    if (value == 1049)
                        saveCursor();
                    enterAlternateScreen(true);
                } else {
                    leaveAlternateScreen();
                    if (value == 1049)
                        restoreCursor();
                }
                break;
            case 1000:
            case 1002:
            case 1003:
                m_mouseTracking = enabled;
                break;
            case 1006:
                break;
            case 2004:
                m_bracketedPaste = enabled;
                break;
            default:
                break;
            }
        } else {
            if (value == 4) {
                m_insertMode = enabled;
            }
        }
    }
}

void TerminalScreen::handleCsi(char final, const QString &parameters)
{
    const QVector<int> values = parseParameters(parameters);
    const int first = parameter(values, 0, -1);
    const int count = first < 1 ? 1 : first;
    const bool priv = !m_csiPrivate.isNull();

    switch (final) {
    case '@':
        insertCharacters(count);
        break;
    case 'A':
        moveCursor(-count, 0);
        break;
    case 'B':
    case 'e':
        moveCursor(count, 0);
        break;
    case 'C':
    case 'a':
        moveCursor(0, count);
        break;
    case 'D':
        moveCursor(0, -count);
        break;
    case 'E':
        m_cursorRow = qBound(0, m_cursorRow + count, m_rows - 1);
        m_cursorColumn = 0;
        m_pendingWrap = false;
        break;
    case 'F':
        m_cursorRow = qBound(0, m_cursorRow - count, m_rows - 1);
        m_cursorColumn = 0;
        m_pendingWrap = false;
        break;
    case 'G':
    case '`':
        m_cursorColumn = qBound(0, count - 1, m_columns - 1);
        m_pendingWrap = false;
        break;
    case 'H':
    case 'f': {
        const int row = parameter(values, 0, 1);
        const int column = parameter(values, 1, 1);
        setCursor(row - 1, column - 1, true);
        break;
    }
    case 'I':
        for (int index = 0; index < count; ++index)
            tab();
        break;
    case 'J':
        eraseInDisplay(first < 0 ? 0 : first);
        break;
    case 'K':
        eraseInLine(first < 0 ? 0 : first);
        break;
    case 'L':
        insertLines(count);
        break;
    case 'M':
        deleteLines(count);
        break;
    case 'P':
        deleteCharacters(count);
        break;
    case 'S':
        scrollUp(count);
        break;
    case 'T':
        scrollDown(count);
        break;
    case 'X':
        eraseCharacters(count);
        break;
    case 'Z':
        for (int index = 0; index < count; ++index) {
            int column = m_cursorColumn - 1;
            while (column > 0 && column < m_tabStops.size() && !m_tabStops.at(column))
                --column;
            m_cursorColumn = std::max(0, column);
        }
        break;
    case 'b':
        repeatCharacter(count);
        break;
    case 'c':
        if (priv && m_csiPrivate == QLatin1Char('>'))
            m_responses.append("\x1b[>0;100;0c");
        else
            m_responses.append("\x1b[?1;2c");
        break;
    case 'd':
        m_cursorRow = qBound(0, count - 1, m_rows - 1);
        m_pendingWrap = false;
        break;
    case 'g':
        if (first == 3) {
            std::fill(m_tabStops.begin(), m_tabStops.end(), false);
        } else if (m_cursorColumn < m_tabStops.size()) {
            m_tabStops[m_cursorColumn] = false;
        }
        break;
    case 'h':
        setMode(values, true, priv);
        break;
    case 'l':
        setMode(values, false, priv);
        break;
    case 'm':
        setSgr(values, parameters);
        break;
    case 'n':
        if (first == 5)
            m_responses.append("\x1b[0n");
        else if (first == 6)
            m_responses.append(QStringLiteral("\x1b[%1;%2R")
                                   .arg(m_cursorRow + 1)
                                   .arg(m_cursorColumn + 1)
                                   .toUtf8());
        break;
    case 'p':
        if (m_csiIntermediate == QLatin1String("!")) {
            m_cursorVisible = true;
            m_originMode = false;
            m_autoWrap = true;
            m_insertMode = false;
            m_applicationCursorKeys = false;
            m_scrollTop = 0;
            m_scrollBottom = m_rows - 1;
            m_currentAttributes = 0;
            m_currentForeground = TermColor::defaultValue();
            m_currentBackground = TermColor::defaultValue();
            m_savedRow = 0;
            m_savedColumn = 0;
        }
        break;
    case 'q':
        if (priv) {
            // DECSCUSR cursor style.
            if (first >= 0 && first <= 6)
                m_cursorStyle = first;
        }
        break;
    case 'r': {
        const int top = parameter(values, 0, 1);
        const int bottom = parameter(values, 1, m_rows);
        if (top < bottom && top >= 1 && bottom <= m_rows) {
            m_scrollTop = top - 1;
            m_scrollBottom = bottom - 1;
            setCursor(0, 0, true);
        }
        break;
    }
    case 's':
        saveCursor();
        break;
    case 'u':
        restoreCursor();
        break;
    case 't':
        // Window manipulation requests are ignored.
        break;
    default:
        break;
    }
}

void TerminalScreen::handleOsc(const QByteArray &payload)
{
    const int separator = payload.indexOf(';');
    if (separator < 0)
        return;
    bool ok = false;
    const int command = payload.left(separator).toInt(&ok);
    if (!ok)
        return;
    QString directory;
    const QByteArray value = payload.mid(separator + 1);
    if (command == 7) {
        const QUrl url = QUrl::fromEncoded(value);
        if (url.scheme() == QLatin1String("file"))
            directory = url.path();
    } else if (command == 777 && value.startsWith("winscp-shell;")) {
        m_shellName = QString::fromUtf8(value.mid(13));
    } else if (command == 777 && value.startsWith("winscp-cwd;")) {
        directory = QString::fromUtf8(value.mid(11));
    }
    if (directory.startsWith(QLatin1Char('/')) && !directory.contains(QChar::Null))
        m_workingDirectories.append(directory);
    if (command == 0 || command == 2) {
        const QString title = decodeUtf8(payload.mid(separator + 1)).trimmed();
        if (!title.isEmpty() && title != m_title) {
            m_title = title;
            m_titleChanged = true;
        }
    }
}

QString TerminalScreen::takeShellName()
{
    QString result;
    result.swap(m_shellName);
    return result;
}

QStringList TerminalScreen::takeWorkingDirectories()
{
    QStringList result;
    result.swap(m_workingDirectories);
    return result;
}

QByteArray TerminalScreen::takeResponses()
{
    QByteArray result;
    result.swap(m_responses);
    return result;
}

bool TerminalScreen::takeBell()
{
    const bool value = m_bell;
    m_bell = false;
    return value;
}

bool TerminalScreen::takeTitleChanged()
{
    const bool value = m_titleChanged;
    m_titleChanged = false;
    return value;
}

QString TerminalScreen::lineText(int absoluteIndex) const
{
    const TerminalLine &line = lineAt(absoluteIndex);
    QString text;
    text.reserve(line.cells.size());
    for (const TerminalCell &cell : line.cells) {
        if (cell.continuation)
            continue;
        if (cell.text.isEmpty())
            text.append(QLatin1Char(' '));
        else
            text.append(cell.text);
    }
    while (text.endsWith(QLatin1Char(' ')))
        text.chop(1);
    return text;
}

QString TerminalScreen::textRange(int startLine, int startColumn, int endLine, int endColumn) const
{
    if (startLine > endLine || (startLine == endLine && startColumn > endColumn))
        std::swap(startLine, endLine), std::swap(startColumn, endColumn);
    startLine = qBound(0, startLine, totalLines() - 1);
    endLine = qBound(0, endLine, totalLines() - 1);
    QString result;
    for (int lineIndex = startLine; lineIndex <= endLine; ++lineIndex) {
        const TerminalLine &line = lineAt(lineIndex);
        const int from = lineIndex == startLine ? qBound(0, startColumn, m_columns - 1) : 0;
        const int to = lineIndex == endLine ? qBound(0, endColumn, m_columns - 1) : m_columns - 1;
        QString lineText;
        for (int column = from; column <= to; ++column) {
            if (column < 0 || column >= line.cells.size())
                continue;
            const TerminalCell &cell = line.cells.at(column);
            if (cell.continuation)
                continue;
            if (cell.text.isEmpty())
                lineText.append(QLatin1Char(' '));
            else
                lineText.append(cell.text);
        }
        while (lineText.endsWith(QLatin1Char(' ')))
            lineText.chop(1);
        if (lineIndex > startLine)
            result.append(QLatin1Char('\n'));
        result.append(lineText);
    }
    return result;
}

QColor TerminalScreen::colorFor(TermColor color, bool bold, bool background) const
{
    if (color.isDefault())
        return background ? m_palette.background : m_palette.foreground;
    if (color.isRgb())
        return QColor(color.red(), color.green(), color.blue());
    if (color.isIndexed()) {
        const int index = color.index();
        if (index < 8)
            return m_palette.ansi[bold && !background ? index + 8 : index];
        if (index < 16)
            return m_palette.ansi[index];
        return m_palette.xtermColor(index);
    }
    return m_palette.foreground;
}

QColor TerminalScreen::blend(const QColor &color, const QColor &background, double factor) const
{
    return QColor::fromRgbF(color.redF() * (1.0 - factor) + background.redF() * factor,
                            color.greenF() * (1.0 - factor) + background.greenF() * factor,
                            color.blueF() * (1.0 - factor) + background.blueF() * factor);
}

} // namespace wsc
