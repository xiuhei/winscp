#include "ui/Theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

#include <array>

namespace wsc {

namespace {
QString g_currentTheme = QStringLiteral("dark");
bool g_isDark = true;

QColor blend(const QColor &first, const QColor &second, qreal factor)
{
    return QColor::fromRgbF(first.redF() * (1.0 - factor) + second.redF() * factor,
                            first.greenF() * (1.0 - factor) + second.greenF() * factor,
                            first.blueF() * (1.0 - factor) + second.blueF() * factor);
}
} // namespace

QColor TerminalPalette::ansiColor(int index, bool bold) const
{
    if (index < 0)
        return foreground;
    if (index < 8)
        return ansi[index];
    if (index < 16)
        return ansi[index];
    return xtermColor(index);
}

QColor TerminalPalette::xtermColor(int index) const
{
    if (index < 16)
        return ansi[qBound(0, index, 15)];
    if (index >= 232) {
        const int level = qBound(0, (index - 232) * 255 / 23, 255);
        return QColor(level, level, level);
    }
    const int value = index - 16;
    const int red = value / 36;
    const int green = (value % 36) / 6;
    const int blue = value % 6;
    const auto component = [](int level) {
        return level == 0 ? 0 : 55 + level * 40;
    };
    return QColor(component(red), component(green), component(blue));
}

QString Theme::styleSheet(const QString &name)
{
    QFile file(QStringLiteral(":/themes/%1.qss").arg(name));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

void Theme::apply(const QString &name)
{
    g_currentTheme = (name == QLatin1String("light")) ? QStringLiteral("light")
                                                       : QStringLiteral("dark");
    g_isDark = g_currentTheme == QLatin1String("dark");

    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        app->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

        QPalette palette;
        if (g_isDark) {
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#171b22")));
            palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#d7deea")));
            palette.setColor(QPalette::Base, QColor(QStringLiteral("#1b212b")));
            palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#1f2631")));
            palette.setColor(QPalette::Text, QColor(QStringLiteral("#d7deea")));
            palette.setColor(QPalette::Button, QColor(QStringLiteral("#232a35")));
            palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#d7deea")));
            palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#355777")));
            palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#2b3441")));
            palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#e7edf6")));
            palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#64748b")));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#5f6b7c")));
            palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                             QColor(QStringLiteral("#5f6b7c")));
            palette.setColor(QPalette::Disabled, QPalette::WindowText,
                             QColor(QStringLiteral("#5f6b7c")));
        } else {
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#f4f6f9")));
            palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#24303f")));
            palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f7f9fc")));
            palette.setColor(QPalette::Text, QColor(QStringLiteral("#24303f")));
            palette.setColor(QPalette::Button, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#24303f")));
            palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#cfe3fa")));
            palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#10243a")));
            palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#24303f")));
            palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#98a3b0")));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#a0aab6")));
        }
        palette.setColor(QPalette::Link, accent());
        app->setPalette(palette);
        app->setStyleSheet(styleSheet(g_currentTheme));

        QFont font = app->font();
        font.setFamily(QStringLiteral("Segoe UI"));
        font.setPointSize(10);
        font.setStyleHint(QFont::SansSerif);
        app->setFont(font);
    }
}

QString Theme::currentName()
{
    return g_currentTheme;
}

bool Theme::isDark()
{
    return g_isDark;
}

QColor Theme::accent()
{
    return g_isDark ? QColor(QStringLiteral("#e0821c")) : QColor(QStringLiteral("#d97913"));
}

QColor Theme::text()
{
    return g_isDark ? QColor(QStringLiteral("#d7deea")) : QColor(QStringLiteral("#24303f"));
}

QColor Theme::muted()
{
    return g_isDark ? QColor(QStringLiteral("#8492a6")) : QColor(QStringLiteral("#7b8a9d"));
}

QColor Theme::danger()
{
    return g_isDark ? QColor(QStringLiteral("#e06c75")) : QColor(QStringLiteral("#c0392b"));
}

QColor Theme::success()
{
    return g_isDark ? QColor(QStringLiteral("#56c596")) : QColor(QStringLiteral("#2e8b57"));
}

QColor Theme::iconColor()
{
    return g_isDark ? QColor(QStringLiteral("#c9d4e3")) : QColor(QStringLiteral("#465a71"));
}

QColor Theme::paneBackground()
{
    return g_isDark ? QColor(QStringLiteral("#1a1f27")) : QColor(QStringLiteral("#ffffff"));
}

QColor Theme::terminalBackground()
{
    return g_isDark ? QColor(QStringLiteral("#0e1116")) : QColor(QStringLiteral("#fbfcfe"));
}

QColor Theme::terminalForeground()
{
    return g_isDark ? QColor(QStringLiteral("#d7e0ea")) : QColor(QStringLiteral("#263238"));
}

TerminalPalette Theme::terminalPalette()
{
    TerminalPalette palette;
    palette.background = terminalBackground();
    palette.foreground = terminalForeground();
    palette.cursor = accent();
    if (g_isDark) {
        palette.selection = QColor(QStringLiteral("#2f4a68"));
        palette.selectionText = QColor(QStringLiteral("#ffffff"));
        const std::array<const char *, 16> colors = {
            "#1c2128", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd",
            "#56b6c2", "#c8d0dc", "#5c6370", "#ff7b86", "#b5e890", "#ffd38a",
            "#7cc4ff", "#dda0ff", "#7fe0ea", "#ffffff"};
        for (int index = 0; index < 16; ++index)
            palette.ansi[index] = QColor(QString::fromLatin1(colors[static_cast<size_t>(index)]));
    } else {
        palette.selection = QColor(QStringLiteral("#cfe3fa"));
        palette.selectionText = QColor(QStringLiteral("#10243a"));
        const std::array<const char *, 16> colors = {
            "#000000", "#c0392b", "#2e8b57", "#b8860b", "#2f6ea8", "#8e44ad",
            "#138d90", "#4a5a6b", "#5d6d7e", "#e74c3c", "#27ae60", "#d4a017",
            "#3f8fd4", "#a569bd", "#1abc9c", "#24303f"};
        for (int index = 0; index < 16; ++index)
            palette.ansi[index] = QColor(QString::fromLatin1(colors[static_cast<size_t>(index)]));
    }
    return palette;
}

} // namespace wsc
