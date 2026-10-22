#include "ui/Theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>
#include <QStringList>

#include <array>

namespace wsc {

namespace {
QString g_currentTheme = QStringLiteral("light");
bool g_isDark = false;
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
    g_currentTheme = (name == QLatin1String("dark")) ? QStringLiteral("dark")
                                                      : QStringLiteral("light");
    g_isDark = g_currentTheme == QLatin1String("dark");

    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        auto *style = QStyleFactory::create(g_isDark ? QStringLiteral("Fusion")
                                                     : QStringLiteral("windowsvista"));
        app->setStyle(style ? style : QStyleFactory::create(QStringLiteral("Fusion")));

        QPalette palette;
        if (g_isDark) {
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#2b2b2b")));
            palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#dedede")));
            palette.setColor(QPalette::Base, QColor(QStringLiteral("#202020")));
            palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#262626")));
            palette.setColor(QPalette::Text, QColor(QStringLiteral("#dedede")));
            palette.setColor(QPalette::Button, QColor(QStringLiteral("#333333")));
            palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#dedede")));
            palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#4a4a4a")));
            palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#333333")));
            palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#eeeeee")));
            palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#a0a0a0")));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#a0a0a0")));
            palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                             QColor(QStringLiteral("#a0a0a0")));
            palette.setColor(QPalette::Disabled, QPalette::WindowText,
                             QColor(QStringLiteral("#a0a0a0")));
        } else {
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#f3f5f7")));
            palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#24303f")));
            palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::Text, QColor(QStringLiteral("#24303f")));
            palette.setColor(QPalette::Button, QColor(QStringLiteral("#f3f5f7")));
            palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#24303f")));
            palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2468a4")));
            palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#ffffff")));
            palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#24303f")));
            palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#98a3b0")));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#a0aab6")));
        }
        palette.setColor(QPalette::Link, accent());
        app->setPalette(palette);
        app->setStyleSheet(styleSheet(QStringLiteral("common")) + QLatin1Char('\n') +
                          styleSheet(g_currentTheme));

        QFont font = app->font();
        font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                          QStringLiteral("Microsoft YaHei")});
        font.setPointSize(9);
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
    return g_isDark ? QColor(QStringLiteral("#dedede")) : QColor(QStringLiteral("#0066b8"));
}

QColor Theme::text()
{
    return g_isDark ? QColor(QStringLiteral("#dedede")) : QColor(QStringLiteral("#24303f"));
}

QColor Theme::muted()
{
    return g_isDark ? QColor(QStringLiteral("#b6b6b6")) : QColor(QStringLiteral("#657386"));
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
    return g_isDark ? QColor(QStringLiteral("#f0f0f0")) : QColor(QStringLiteral("#465a71"));
}

QColor Theme::paneBackground()
{
    return g_isDark ? QColor(QStringLiteral("#202020")) : QColor(QStringLiteral("#ffffff"));
}

QColor Theme::terminalBackground()
{
    return QColor(QStringLiteral("#000000"));
}

QColor Theme::terminalForeground()
{
    return QColor(QStringLiteral("#dedede"));
}

TerminalPalette Theme::terminalPalette()
{
    TerminalPalette palette;
    palette.background = terminalBackground();
    palette.foreground = terminalForeground();
    palette.cursor = palette.foreground;
    palette.selection = QColor(QStringLiteral("#454545"));
    palette.selectionText = QColor(QStringLiteral("#ffffff"));
    const std::array<const char *, 16> colors = {
        "#1c1c1c", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd",
        "#56b6c2", "#c8c8c8", "#5c5c5c", "#ff7b86", "#b5e890", "#ffd38a",
        "#7cc4ff", "#dda0ff", "#7fe0ea", "#ffffff"};
    for (int index = 0; index < 16; ++index)
        palette.ansi[index] = QColor(QString::fromLatin1(colors[static_cast<size_t>(index)]));
    return palette;
}

} // namespace wsc
