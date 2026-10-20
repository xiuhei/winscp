// SPDX-License-Identifier: MIT
#pragma once

#include "terminal/TerminalScreen.h"
#include "ui/Theme.h"

#include <QByteArray>
#include <QFont>
#include <QPoint>
#include <QStringDecoder>
#include <QWidget>

class QTimer;

namespace wsc {

/// Renders a TerminalScreen with a custom character grid painter and turns
/// keyboard/IME input into terminal byte sequences.
class TerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit TerminalWidget(int columns = 100, int rows = 28, QWidget *parent = nullptr);

    void feed(const QByteArray &utf8);
    void setTerminalFont(const QFont &font);
    void applySettings();
    void setSessionRunning(bool running);
    bool isSessionRunning() const { return m_sessionRunning; }
    void setPalette(const TerminalPalette &palette);
    void setScrollbackLimit(int lines);
    void clearScreen();
    void clearScrollback();
    void resetTerminal();

    TerminalScreen *screen() { return &m_screen; }
    const TerminalScreen *screen() const { return &m_screen; }
    QString title() const { return m_screen.title(); }
    QString selectedText() const;
    bool hasSelection() const;
    void copySelection();
    void paste();
    void selectAll();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void inputReady(const QByteArray &data);
    void sizeChanged(int columns, int rows);
    void shellDetected(const QString &shell);
    void workingDirectoryChanged(const QString &directory);
    void titleChanged(const QString &title);
    void fontZoomRequested(int delta);
    void copyAvailableChanged(bool available);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    void recomputeMetrics();
    void updateTerminalSize();
    void sendText(const QString &text, bool allowBracketedPaste = false);
    void startSelection(const QPoint &position);
    void updateSelection(const QPoint &position);
    void finishSelection();
    void clearSelection();
    void selectWordAt(const QPoint &position);
    void selectLineAt(const QPoint &position);
    QPoint cellAt(const QPoint &position) const;
    int absoluteLineAt(int viewportRow) const;
    int visibleStartLine() const;
    int rowHeight() const;
    int columnWidth() const;
    void clampScroll();
    void ensureCursorVisible();
    void scrollTo(int offset);
    void sendKey(QKeyEvent *event);
    QRect cursorRect() const;
    bool cellSelected(int absoluteLine, int column) const;

    TerminalScreen m_screen;
    TerminalPalette m_palette;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    QFont m_font;
    QFont m_boldFont;
    QFont m_italicFont;
    QFont m_boldItalicFont;
    qreal m_cellWidth = 8.0;
    qreal m_lineHeight = 16.0;
    qreal m_ascent = 12.0;
    int m_padding = 8;
    bool m_sessionRunning = false;
    int m_scrollOffset = 0;
    int m_lastScrollback = 0;

    bool m_selecting = false;
    bool m_hasSelection = false;
    int m_selectionStartLine = 0;
    int m_selectionStartColumn = 0;
    int m_selectionEndLine = 0;
    int m_selectionEndColumn = 0;
    int m_clickCount = 0;
    QTimer *m_blinkTimer = nullptr;
    bool m_cursorOn = true;
    bool m_hasFocus = false;
};

} // namespace wsc
