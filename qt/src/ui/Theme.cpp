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

struct ColorToken {
    const char *name;
    const char *light;
    const char *dark;
};

constexpr ColorToken colors[] = {
    {"window", "#f3f5f7", "#292929"},
    {"surface", "#f9fafb", "#242424"},
    {"base", "#ffffff", "#1f1f1f"},
    {"border", "#d5dbe3", "#454545"},
    {"text", "#24303f", "#dedede"},
    {"muted", "#657386", "#b6b6b6"},
    {"hover", "#e9edf1", "#303030"},
    {"pressed", "#dfe7ef", "#3a3a3a"},
    {"focus", "#8190a0", "#878787"},
    {"disabled", "#9ba5b0", "#858585"},
    {"selection", "#dceafa", "#414141"},
    {"selection-text", "#173d64", "#f0f0f0"},
    {"inactive", "#e9edf2", "#343434"},
    {"active-header", "#eef2f6", "#2a2a2a"},
    {"primary", "#2468a4", "#dedede"},
    {"primary-hover", "#1c5890", "#f0f0f0"},
    {"primary-pressed", "#174c7b", "#b6b6b6"},
    {"primary-text", "#ffffff", "#1f1f1f"},
    {"terminal", "#181818", "#181818"},
    {"accent", "#0066b8", "#dedede"},
    {"icon", "#465a71", "#f0f0f0"},
    {"danger", "#c0392b", "#e06c75"},
    {"success", "#2e8b57", "#56c596"},
};

QColor color(const char *name)
{
    for (const auto &token : colors) {
        if (QLatin1String(token.name) == QLatin1String(name))
            return QColor(QLatin1String(g_isDark ? token.dark : token.light));
    }
    Q_UNREACHABLE();
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
    const bool dark = name == QLatin1String("dark");
    QFile file(QStringLiteral(":/themes/common.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QString sheet = QString::fromUtf8(file.readAll());
    for (const auto &token : colors)
        sheet.replace(QLatin1Char('@') + QLatin1String(token.name) + QLatin1Char('@'),
                      QLatin1String(dark ? token.dark : token.light));
    sheet.replace(QStringLiteral("@theme@"), dark ? QStringLiteral("dark") : QStringLiteral("light"));
    sheet.replace(QStringLiteral("@strong-weight@"), dark ? QStringLiteral("500") : QStringLiteral("600"));
    return sheet;
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
        palette.setColor(QPalette::Window, color("window"));
        palette.setColor(QPalette::WindowText, text());
        palette.setColor(QPalette::Base, paneBackground());
        palette.setColor(QPalette::AlternateBase, color("surface"));
        palette.setColor(QPalette::Text, text());
        palette.setColor(QPalette::Button, color("surface"));
        palette.setColor(QPalette::ButtonText, text());
        palette.setColor(QPalette::BrightText, color("primary-text"));
        palette.setColor(QPalette::Highlight, color("selection"));
        palette.setColor(QPalette::HighlightedText, color("selection-text"));
        palette.setColor(QPalette::ToolTipBase, color("surface"));
        palette.setColor(QPalette::ToolTipText, text());
        palette.setColor(QPalette::PlaceholderText, muted());
        for (const auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
            palette.setColor(QPalette::Disabled, role, color("disabled"));
        palette.setColor(QPalette::Link, accent());
        app->setPalette(palette);
        app->setStyleSheet(styleSheet(g_currentTheme));

        QFont font = app->font();
        font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                          QStringLiteral("Microsoft YaHei")});
        font.setPointSize(9);
        font.setWeight(static_cast<QFont::Weight>(g_isDark ? 350 : 400));
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        font.setFeature(QFont::Tag("tnum"), 1);
#endif
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
    return color("accent");
}

QColor Theme::text()
{
    return color("text");
}

QColor Theme::muted()
{
    return color("muted");
}

QColor Theme::danger()
{
    return color("danger");
}

QColor Theme::success()
{
    return color("success");
}

QColor Theme::iconColor()
{
    return color("icon");
}

QColor Theme::paneBackground()
{
    return color("base");
}

QColor Theme::terminalBackground()
{
    return color("terminal");
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
