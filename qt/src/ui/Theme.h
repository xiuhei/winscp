// SPDX-License-Identifier: MIT
#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace wsc {

struct TerminalPalette {
    QColor background;
    QColor foreground;
    QColor cursor;
    QColor selection;
    QColor selectionText;
    QColor ansi[16];

    QColor ansiColor(int index, bool bold) const;
    QColor xtermColor(int index) const;
};

class Theme {
public:
    static void apply(const QString &name);
    static QString currentName();
    static bool isDark();

    static QColor accent();
    static QColor text();
    static QColor muted();
    static QColor danger();
    static QColor success();
    static QColor iconColor();
    static QColor paneBackground();
    static QColor terminalBackground();
    static QColor terminalForeground();

    static TerminalPalette terminalPalette();
    static QString styleSheet(const QString &name);
};

} // namespace wsc
