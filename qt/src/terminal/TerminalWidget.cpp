#include "terminal/TerminalWidget.h"

#include "core/Settings.h"
#include "ui/IconProvider.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace wsc {

namespace {

QString normalizePasteText(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    return text;
}

} // namespace

TerminalWidget::TerminalWidget(int columns, int rows, QWidget *parent)
    : QWidget(parent)
    , m_screen(columns, rows)
    , m_palette(Theme::terminalPalette())
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setCursor(Qt::IBeamCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    const Settings &settings = Settings::instance();
    QFont font(settings.terminalFontFamily);
    font.setPointSize(settings.terminalFontSize);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    m_font = font;
    m_screen.setScrollbackLimit(settings.terminalScrollback);
    recomputeMetrics();

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(560);
    connect(m_blinkTimer, &QTimer::timeout, this, [this] {
        if (m_hasFocus && m_screen.cursorVisible()) {
            m_cursorOn = !m_cursorOn;
            const QRect cell = cursorRect();
            if (cell.isValid())
                update(cell.adjusted(-1, -1, 1, 1));
        }
    });
    m_blinkTimer->start();
}

void TerminalWidget::recomputeMetrics()
{
    QFontMetricsF metrics(m_font);
    m_cellWidth = std::max<qreal>(1.0, metrics.horizontalAdvance(QLatin1Char('M')));
    if (m_cellWidth < 2.0)
        m_cellWidth = std::max<qreal>(1.0, metrics.averageCharWidth());
    m_lineHeight = std::max<qreal>(1.0, metrics.height());
    m_ascent = metrics.ascent();

    m_boldFont = m_font;
    m_boldFont.setBold(true);
    m_italicFont = m_font;
    m_italicFont.setItalic(true);
    m_boldItalicFont = m_boldFont;
    m_boldItalicFont.setItalic(true);

    updateTerminalSize();
    update();
}

void TerminalWidget::updateTerminalSize()
{
    const int availableWidth = std::max(1, width() - m_padding * 2);
    const int availableHeight = std::max(1, height() - m_padding * 2);
    const int columns = std::max(2, static_cast<int>(std::floor(availableWidth / m_cellWidth)));
    const int rows = std::max(2, static_cast<int>(std::floor(availableHeight / m_lineHeight)));
    if (columns != m_screen.columns() || rows != m_screen.rows()) {
        m_screen.setSize(columns, rows);
        m_screen.setScrollbackLimit(Settings::instance().terminalScrollback);
        clampScroll();
        emit sizeChanged(columns, rows);
    }
    update();
}

void TerminalWidget::setTerminalFont(const QFont &font)
{
    m_font = font;
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    recomputeMetrics();
}

void TerminalWidget::applySettings()
{
    const Settings &settings = Settings::instance();
    QFont font(settings.terminalFontFamily);
    font.setPointSize(settings.terminalFontSize);
    m_screen.setScrollbackLimit(settings.terminalScrollback);
    setTerminalFont(font);
    setPalette(Theme::terminalPalette());
}

void TerminalWidget::setPalette(const TerminalPalette &palette)
{
    m_palette = palette;
    m_screen.setPalette(palette);
    update();
}

void TerminalWidget::setScrollbackLimit(int lines)
{
    m_screen.setScrollbackLimit(lines);
    clampScroll();
    update();
}

void TerminalWidget::setSessionRunning(bool running)
{
    m_sessionRunning = running;
    if (!running)
        m_cursorOn = true;
    update();
}

void TerminalWidget::feed(const QByteArray &utf8)
{
    const int before = m_screen.scrollbackCount();
    const QString text = m_decoder.decode(utf8);
    m_screen.feed(text);
    const QByteArray responses = m_screen.takeResponses();
    if (!responses.isEmpty())
        emit inputReady(responses);
    if (m_screen.takeBell())
        QApplication::beep();
    if (m_screen.takeTitleChanged())
        emit titleChanged(m_screen.title());
    const QString shell = m_screen.takeShellName();
    if (!shell.isEmpty())
        emit shellDetected(shell);
    for (const QString &directory : m_screen.takeWorkingDirectories())
        emit workingDirectoryChanged(directory);
    const int after = m_screen.scrollbackCount();
    if (m_scrollOffset > 0)
        m_scrollOffset += after - before;
    clampScroll();
    update();
}

QSize TerminalWidget::sizeHint() const
{
    return QSize(static_cast<int>(m_cellWidth * 100 + m_padding * 2),
                 static_cast<int>(m_lineHeight * 28 + m_padding * 2));
}

QSize TerminalWidget::minimumSizeHint() const
{
    return QSize(static_cast<int>(m_cellWidth * 30 + m_padding * 2),
                 static_cast<int>(m_lineHeight * 8 + m_padding * 2));
}

int TerminalWidget::visibleStartLine() const
{
    const int total = m_screen.totalLines();
    const int rows = m_screen.rows();
    return std::max(0, total - rows - m_scrollOffset);
}

int TerminalWidget::absoluteLineAt(int viewportRow) const
{
    return visibleStartLine() + viewportRow;
}

void TerminalWidget::clampScroll()
{
    const int maxOffset = std::max(0, m_screen.scrollbackCount());
    m_scrollOffset = qBound(0, m_scrollOffset, maxOffset);
}

void TerminalWidget::scrollTo(int offset)
{
    const int previous = m_scrollOffset;
    m_scrollOffset = offset;
    clampScroll();
    if (previous != m_scrollOffset)
        update();
}

void TerminalWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(rect(), m_palette.background);

    const int rows = m_screen.rows();
    const int columns = m_screen.columns();
    const int startLine = visibleStartLine();
    const bool cursorInView = m_screen.absoluteCursorRow() >= startLine &&
        m_screen.absoluteCursorRow() < startLine + rows;

    for (int row = 0; row < rows; ++row) {
        const int absoluteLine = startLine + row;
        if (absoluteLine >= m_screen.totalLines())
            break;
        const TerminalLine &line = m_screen.lineAt(absoluteLine);
        const qreal y = m_padding + row * m_lineHeight;
        const qreal baseline = y + m_ascent;

        // Backgrounds (including selection and reverse video).
        for (int column = 0; column < columns; ++column) {
            if (column >= line.cells.size())
                break;
            const TerminalCell &cell = line.cells.at(column);
            if (cell.continuation && !cellSelected(absoluteLine, column))
                continue;
            QColor background = m_screen.colorFor(cell.background, false, true);
            if (cell.attributes & TerminalScreen::Reverse)
                background = m_screen.colorFor(cell.foreground, cell.attributes & TerminalScreen::Bold, false);
            if (cellSelected(absoluteLine, column))
                background = m_palette.selection;
            if (background != m_palette.background) {
                painter.fillRect(QRectF(m_padding + column * m_cellWidth, y, m_cellWidth,
                                        m_lineHeight),
                                 background);
            }
        }

        // Text runs grouped by style.
        QString run;
        QString runKey;
        QColor runColor;
        QFont runFont;
        int runStartColumn = 0;
        auto flush = [&] {
            if (run.isEmpty())
                return;
            painter.setFont(runFont);
            painter.setPen(runColor);
            painter.drawText(QPointF(m_padding + runStartColumn * m_cellWidth, baseline), run);
            run.clear();
        };

        for (int column = 0; column < columns; ++column) {
            if (column >= line.cells.size())
                break;
            const TerminalCell &cell = line.cells.at(column);
            if (cell.continuation)
                continue;
            if (cell.text.isEmpty()) {
                flush();
                runKey.clear();
                continue;
            }
            const bool bold = (cell.attributes & TerminalScreen::Bold) != 0;
            const bool italic = (cell.attributes & TerminalScreen::Italic) != 0;
            QColor foreground = m_screen.colorFor(cell.foreground, bold, false);
            QColor background = m_screen.colorFor(cell.background, false, true);
            if (cell.attributes & TerminalScreen::Reverse) {
                std::swap(foreground, background);
            }
            if (cellSelected(absoluteLine, column))
                foreground = m_palette.selectionText;
            if (cell.attributes & TerminalScreen::Dim)
                foreground = QColor::fromRgbF(
                    foreground.redF() * 0.6 + background.redF() * 0.4,
                    foreground.greenF() * 0.6 + background.greenF() * 0.4,
                    foreground.blueF() * 0.6 + background.blueF() * 0.4);
            const QString key = QStringLiteral("%1|%2|%3|%4")
                                    .arg(foreground.name())
                                    .arg(bold ? 1 : 0)
                                    .arg(italic ? 1 : 0)
                                    .arg(static_cast<int>(cell.attributes));
            if (key != runKey) {
                flush();
                runKey = key;
                runColor = foreground;
                runFont = bold ? (italic ? m_boldItalicFont : m_boldFont)
                               : (italic ? m_italicFont : m_font);
                runStartColumn = column;
            }
            run.append(cell.text);
            if (cell.attributes & TerminalScreen::Underline) {
                const qreal underlineY = y + m_lineHeight - 2;
                painter.setPen(foreground);
                painter.drawLine(QPointF(m_padding + column * m_cellWidth, underlineY),
                                 QPointF(m_padding + (column + 1) * m_cellWidth, underlineY));
            }
            if (cell.attributes & TerminalScreen::Strike) {
                const qreal strikeY = y + m_lineHeight * 0.55;
                painter.setPen(foreground);
                painter.drawLine(QPointF(m_padding + column * m_cellWidth, strikeY),
                                 QPointF(m_padding + (column + 1) * m_cellWidth, strikeY));
            }
        }
        flush();
    }

    // Cursor.
    if (m_screen.cursorVisible() && cursorInView) {
        const int row = m_screen.absoluteCursorRow() - startLine;
        const int column = m_screen.cursorColumn();
        const qreal x = m_padding + column * m_cellWidth;
        const qreal y = m_padding + row * m_lineHeight;
        QColor cursorColor = m_palette.cursor;
        if (!m_hasFocus)
            cursorColor.setAlpha(120);
        painter.setPen(QPen(cursorColor, 1.5));
        painter.setBrush(Qt::NoBrush);
        if (m_screen.cursorStyle() == 3 || m_screen.cursorStyle() == 4) {
            painter.drawLine(QPointF(x, y + m_lineHeight - 1.5),
                             QPointF(x + m_cellWidth, y + m_lineHeight - 1.5));
        } else if (m_screen.cursorStyle() == 5 || m_screen.cursorStyle() == 6) {
            painter.drawLine(QPointF(x + 1, y), QPointF(x + 1, y + m_lineHeight));
        } else {
            QColor fill = cursorColor;
            fill.setAlpha(m_hasFocus ? 90 : 45);
            painter.fillRect(QRectF(x, y, m_cellWidth, m_lineHeight), fill);
            painter.setPen(QPen(cursorColor, 1.0));
            painter.drawRect(QRectF(x + 0.5, y + 0.5, m_cellWidth - 1, m_lineHeight - 1));
        }
    }

    if (m_scrollOffset > 0) {
        const QString indicator = QStringLiteral("回滚 %1 行").arg(m_scrollOffset);
        const QFontMetricsF metrics(m_font);
        const QRectF box(width() - metrics.horizontalAdvance(indicator) - 26, 4,
                         metrics.horizontalAdvance(indicator) + 18, m_lineHeight + 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, m_palette.background.lightness() < 128 ? 140 : 45));
        painter.drawRoundedRect(box, 4, 4);
        painter.setPen(m_palette.foreground);
        painter.setFont(m_font);
        painter.drawText(box, Qt::AlignCenter, indicator);
    }
}

bool TerminalWidget::cellSelected(int absoluteLine, int column) const
{
    if (!m_hasSelection)
        return false;
    int startLine = m_selectionStartLine;
    int startColumn = m_selectionStartColumn;
    int endLine = m_selectionEndLine;
    int endColumn = m_selectionEndColumn;
    if (startLine > endLine || (startLine == endLine && startColumn > endColumn)) {
        std::swap(startLine, endLine);
        std::swap(startColumn, endColumn);
    }
    if (absoluteLine < startLine || absoluteLine > endLine)
        return false;
    if (absoluteLine == startLine && column < startColumn)
        return false;
    if (absoluteLine == endLine && column > endColumn)
        return false;
    return true;
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateTerminalSize();
    clampScroll();
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    m_hasFocus = true;
    m_cursorOn = true;
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    m_hasFocus = false;
    update();
}

QPoint TerminalWidget::cellAt(const QPoint &position) const
{
    const int column = qBound(0, static_cast<int>((position.x() - m_padding) / m_cellWidth),
                              m_screen.columns() - 1);
    const int row = qBound(0, static_cast<int>((position.y() - m_padding) / m_lineHeight),
                           m_screen.rows() - 1);
    return QPoint(column, absoluteLineAt(row));
}

void TerminalWidget::startSelection(const QPoint &position)
{
    const QPoint cell = cellAt(position);
    m_selecting = true;
    m_hasSelection = false;
    m_selectionStartLine = cell.y();
    m_selectionStartColumn = cell.x();
    m_selectionEndLine = cell.y();
    m_selectionEndColumn = cell.x();
    emit copyAvailableChanged(false);
    update();
}

void TerminalWidget::updateSelection(const QPoint &position)
{
    if (!m_selecting)
        return;
    QPoint point = position;
    if (point.y() < m_padding && m_scrollOffset < m_screen.scrollbackCount()) {
        m_scrollOffset = std::min(m_scrollOffset + 1, m_screen.scrollbackCount());
        point.setY(m_padding);
    } else if (point.y() > height() - m_padding && m_scrollOffset > 0) {
        m_scrollOffset = std::max(0, m_scrollOffset - 1);
        point.setY(height() - m_padding - 1);
    }
    const QPoint cell = cellAt(point);
    m_selectionEndLine = cell.y();
    m_selectionEndColumn = cell.x();
    m_hasSelection = m_selectionStartLine != m_selectionEndLine ||
        m_selectionStartColumn != m_selectionEndColumn;
    emit copyAvailableChanged(m_hasSelection);
    update();
}

void TerminalWidget::finishSelection()
{
    if (!m_selecting)
        return;
    m_selecting = false;
    if (m_selectionStartLine == m_selectionEndLine &&
        m_selectionStartColumn == m_selectionEndColumn) {
        m_hasSelection = false;
        emit copyAvailableChanged(false);
    }
    update();
}

void TerminalWidget::clearSelection()
{
    m_selecting = false;
    if (!m_hasSelection)
        return;
    m_hasSelection = false;
    emit copyAvailableChanged(false);
    update();
}

void TerminalWidget::selectWordAt(const QPoint &position)
{
    const QPoint cell = cellAt(position);
    const QString text = m_screen.lineText(cell.y());
    if (text.isEmpty())
        return;
    int column = std::min(cell.x(), static_cast<int>(text.size()) - 1);
    const auto isWord = [](QChar character) {
        return character.isLetterOrNumber() || character == QLatin1Char('_') ||
               character == QLatin1Char('-') || character == QLatin1Char('.') ||
               character == QLatin1Char('/') || character == QLatin1Char('~') ||
               character == QLatin1Char(':');
    };
    int start = column;
    while (start > 0 && isWord(text.at(start - 1)))
        --start;
    int end = column;
    while (end + 1 < text.size() && isWord(text.at(end + 1)))
        ++end;
    if (!isWord(text.at(column))) {
        start = column;
        end = column;
    }
    m_hasSelection = true;
    m_selectionStartLine = cell.y();
    m_selectionEndLine = cell.y();
    m_selectionStartColumn = start;
    m_selectionEndColumn = end;
    emit copyAvailableChanged(true);
    update();
}

void TerminalWidget::selectLineAt(const QPoint &position)
{
    const QPoint cell = cellAt(position);
    m_hasSelection = true;
    m_selectionStartLine = cell.y();
    m_selectionEndLine = cell.y();
    m_selectionStartColumn = 0;
    m_selectionEndColumn = m_screen.columns() - 1;
    emit copyAvailableChanged(true);
    update();
}

QString TerminalWidget::selectedText() const
{
    if (!m_hasSelection)
        return {};
    int startLine = m_selectionStartLine;
    int startColumn = m_selectionStartColumn;
    int endLine = m_selectionEndLine;
    int endColumn = m_selectionEndColumn;
    if (startLine > endLine || (startLine == endLine && startColumn > endColumn)) {
        std::swap(startLine, endLine);
        std::swap(startColumn, endColumn);
    }
    return m_screen.textRange(startLine, startColumn, endLine, endColumn);
}

bool TerminalWidget::hasSelection() const
{
    return m_hasSelection;
}

void TerminalWidget::copySelection()
{
    const QString text = selectedText();
    if (text.isEmpty())
        return;
    QApplication::clipboard()->setText(text);
}

void TerminalWidget::selectAll()
{
    if (m_screen.totalLines() <= 0)
        return;
    m_hasSelection = true;
    m_selectionStartLine = 0;
    m_selectionStartColumn = 0;
    m_selectionEndLine = m_screen.totalLines() - 1;
    m_selectionEndColumn = m_screen.columns() - 1;
    emit copyAvailableChanged(true);
    update();
}

void TerminalWidget::paste()
{
    const QString text = normalizePasteText(QApplication::clipboard()->text());
    if (text.isEmpty())
        return;
    sendText(text, true);
}

void TerminalWidget::sendText(const QString &text, bool allowBracketedPaste)
{
    if (text.isEmpty() || !m_sessionRunning)
        return;
    QByteArray data = text.toUtf8();
    if (allowBracketedPaste && m_screen.bracketedPaste())
        data = QByteArrayLiteral("\x1b[200~") + data + QByteArrayLiteral("\x1b[201~");
    emit inputReady(data);
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        paste();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    if (event->modifiers() & Qt::ShiftModifier) {
        startSelection(event->pos());
        return;
    }
    startSelection(event->pos());
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_selecting && (event->buttons() & Qt::LeftButton))
        updateSelection(event->pos());
    QWidget::mouseMoveEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_selecting)
        finishSelection();
    QWidget::mouseReleaseEvent(event);
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        selectWordAt(event->pos());
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        emit fontZoomRequested(event->angleDelta().y() > 0 ? 1 : -1);
        event->accept();
        return;
    }
    if (m_screen.usingAlternateScreen() && m_scrollOffset == 0) {
        const char *sequence = event->angleDelta().y() > 0 ? "\x1b[A" : "\x1b[B";
        if (m_sessionRunning)
            emit inputReady(QByteArray(sequence));
        event->accept();
        return;
    }
    const int steps = std::max(1, std::abs(event->angleDelta().y()) / 120);
    scrollTo(m_scrollOffset + (event->angleDelta().y() > 0 ? steps * 3 : -steps * 3));
    event->accept();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *copyAction = menu.addAction(IconProvider::instance().icon(QStringLiteral("copy")),
                                         QStringLiteral("复制"));
    copyAction->setEnabled(m_hasSelection);
    QAction *pasteAction = menu.addAction(IconProvider::instance().icon(QStringLiteral("paste")),
                                          QStringLiteral("粘贴"));
    pasteAction->setEnabled(m_sessionRunning);
    QAction *selectAllAction = menu.addAction(QStringLiteral("全选"));
    menu.addSeparator();
    QAction *clearAction = menu.addAction(IconProvider::instance().icon(QStringLiteral("clear")),
                                          QStringLiteral("清屏"));
    QAction *clearScrollbackAction = menu.addAction(QStringLiteral("清除回滚缓冲区"));
    QAction *resetAction = menu.addAction(QStringLiteral("重置终端"));
    menu.addSeparator();
    QAction *fontUpAction = menu.addAction(QStringLiteral("增大字体"));
    QAction *fontDownAction = menu.addAction(QStringLiteral("减小字体"));
    QAction *chosen = menu.exec(event->globalPos());
    if (!chosen)
        return;
    if (chosen == copyAction)
        copySelection();
    else if (chosen == pasteAction)
        paste();
    else if (chosen == selectAllAction)
        selectAll();
    else if (chosen == clearAction) {
        clearScreen();
    } else if (chosen == clearScrollbackAction) {
        clearScrollback();
    } else if (chosen == resetAction) {
        resetTerminal();
    } else if (chosen == fontUpAction) {
        emit fontZoomRequested(1);
    } else if (chosen == fontDownAction) {
        emit fontZoomRequested(-1);
    }
}

bool TerminalWidget::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(key);
            return true;
        }
    }
    return QWidget::event(event);
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    const bool alt = modifiers.testFlag(Qt::AltModifier);

    if (control && shift) {
        if (event->key() == Qt::Key_C) {
            copySelection();
            return;
        }
        if (event->key() == Qt::Key_V) {
            paste();
            return;
        }
        if (event->key() == Qt::Key_A) {
            selectAll();
            return;
        }
        if (event->key() == Qt::Key_Home) {
            scrollTo(m_screen.scrollbackCount());
            return;
        }
        if (event->key() == Qt::Key_End) {
            scrollTo(0);
            return;
        }
        if (event->key() == Qt::Key_Up) {
            scrollTo(m_scrollOffset + 1);
            return;
        }
        if (event->key() == Qt::Key_Down) {
            scrollTo(m_scrollOffset - 1);
            return;
        }
    }
    if (shift && !control) {
        if (event->key() == Qt::Key_PageUp) {
            scrollTo(m_scrollOffset + m_screen.rows() - 1);
            return;
        }
        if (event->key() == Qt::Key_PageDown) {
            scrollTo(m_scrollOffset - (m_screen.rows() - 1));
            return;
        }
    }

    m_hasSelection = false;
    emit copyAvailableChanged(false);

    if (event->key() == Qt::Key_Escape && !control && !alt && event->text().isEmpty()) {
        if (m_sessionRunning)
            emit inputReady(QByteArrayLiteral("\x1b"));
        return;
    }

    if (!m_sessionRunning)
        return;

    const Qt::KeyboardModifiers relevant = modifiers &
        (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);

    const bool appCursor = m_screen.applicationCursorKeys();
    QByteArray data;
    const auto key = event->key();

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        data = QByteArrayLiteral("\r");
        break;
    case Qt::Key_Backspace:
        data = QByteArrayLiteral("\x7f");
        break;
    case Qt::Key_Tab:
        data = shift ? QByteArrayLiteral("\x1b[Z") : QByteArrayLiteral("\t");
        break;
    case Qt::Key_Backtab:
        data = QByteArrayLiteral("\x1b[Z");
        break;
    case Qt::Key_Up:
        data = appCursor ? QByteArrayLiteral("\x1bOA") : QByteArrayLiteral("\x1b[A");
        break;
    case Qt::Key_Down:
        data = appCursor ? QByteArrayLiteral("\x1bOB") : QByteArrayLiteral("\x1b[B");
        break;
    case Qt::Key_Right:
        data = appCursor ? QByteArrayLiteral("\x1bOC") : QByteArrayLiteral("\x1b[C");
        break;
    case Qt::Key_Left:
        data = appCursor ? QByteArrayLiteral("\x1bOD") : QByteArrayLiteral("\x1b[D");
        break;
    case Qt::Key_Home:
        data = appCursor ? QByteArrayLiteral("\x1bOH") : QByteArrayLiteral("\x1b[H");
        break;
    case Qt::Key_End:
        data = appCursor ? QByteArrayLiteral("\x1bOF") : QByteArrayLiteral("\x1b[F");
        break;
    case Qt::Key_Insert:
        data = QByteArrayLiteral("\x1b[2~");
        break;
    case Qt::Key_Delete:
        data = QByteArrayLiteral("\x1b[3~");
        break;
    case Qt::Key_PageUp:
        data = QByteArrayLiteral("\x1b[5~");
        break;
    case Qt::Key_PageDown:
        data = QByteArrayLiteral("\x1b[6~");
        break;
    case Qt::Key_F1:
        data = QByteArrayLiteral("\x1bOP");
        break;
    case Qt::Key_F2:
        data = QByteArrayLiteral("\x1bOQ");
        break;
    case Qt::Key_F3:
        data = QByteArrayLiteral("\x1bOR");
        break;
    case Qt::Key_F4:
        data = QByteArrayLiteral("\x1bOS");
        break;
    case Qt::Key_F5:
        data = QByteArrayLiteral("\x1b[15~");
        break;
    case Qt::Key_F6:
        data = QByteArrayLiteral("\x1b[17~");
        break;
    case Qt::Key_F7:
        data = QByteArrayLiteral("\x1b[18~");
        break;
    case Qt::Key_F8:
        data = QByteArrayLiteral("\x1b[19~");
        break;
    case Qt::Key_F9:
        data = QByteArrayLiteral("\x1b[20~");
        break;
    case Qt::Key_F10:
        data = QByteArrayLiteral("\x1b[21~");
        break;
    case Qt::Key_F11:
        data = QByteArrayLiteral("\x1b[23~");
        break;
    case Qt::Key_F12:
        data = QByteArrayLiteral("\x1b[24~");
        break;
    case Qt::Key_Space:
        if (control)
            data = QByteArrayLiteral("\x00");
        break;
    default:
        break;
    }

    if (data.isEmpty() && control) {
        int code = -1;
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            code = key - Qt::Key_A + 1;
        else if (key == Qt::Key_BracketLeft)
            code = 0x1B;
        else if (key == Qt::Key_Backslash)
            code = 0x1C;
        else if (key == Qt::Key_BracketRight)
            code = 0x1D;
        else if (key == Qt::Key_AsciiCircum)
            code = 0x1E;
        else if (key == Qt::Key_Underscore)
            code = 0x1F;
        else if (key == Qt::Key_At)
            code = 0x00;
        else if (key == Qt::Key_Question)
            code = 0x7F;
        if (code >= 0)
            data = QByteArray(1, static_cast<char>(code));
    }

    if (data.isEmpty()) {
        QString text = event->text();
        if (!text.isEmpty() && !control) {
            data = text.toUtf8();
        }
    }

    if (!data.isEmpty() && alt) {
        if (data.startsWith('\x1b'))
            data = data; // Meta is already an escape sequence.
        else
            data.prepend('\x1b');
    }
    if (!data.isEmpty())
        emit inputReady(data);
    Q_UNUSED(relevant);
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty())
        sendText(event->commitString());
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImCursorRectangle)
        return cursorRect();
    if (query == Qt::ImEnabled)
        return true;
    if (query == Qt::ImCursorPosition)
        return 0;
    return {};
}

QRect TerminalWidget::cursorRect() const
{
    const int startLine = visibleStartLine();
    const int absolute = m_screen.absoluteCursorRow();
    if (absolute < startLine || absolute >= startLine + m_screen.rows())
        return {};
    const int row = absolute - startLine;
    return QRect(static_cast<int>(m_padding + m_screen.cursorColumn() * m_cellWidth),
                 static_cast<int>(m_padding + row * m_lineHeight),
                 static_cast<int>(m_cellWidth), static_cast<int>(m_lineHeight));
}

void TerminalWidget::clearScreen()
{
    m_screen.clearScreen();
    m_scrollOffset = 0;
    clearSelection();
    update();
}

void TerminalWidget::clearScrollback()
{
    m_screen.clearScrollback();
    m_scrollOffset = 0;
    update();
}

void TerminalWidget::resetTerminal()
{
    m_screen.reset();
    m_scrollOffset = 0;
    clearSelection();
    update();
}

} // namespace wsc
