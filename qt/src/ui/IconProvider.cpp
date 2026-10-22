#include "ui/IconProvider.h"

#include "ui/Theme.h"

#include <QFileIconProvider>
#include <QFileInfo>
#include <QIconEngine>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

#include <cmath>

namespace wsc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kCanvas = 64;

QPainterPath pagePath()
{
    QPainterPath path;
    path.moveTo(16, 8);
    path.lineTo(38, 8);
    path.lineTo(50, 20);
    path.lineTo(50, 56);
    path.lineTo(16, 56);
    path.closeSubpath();
    return path;
}

void drawFold(QPainter &painter, const QColor &color)
{
    QPainterPath fold;
    fold.moveTo(38, 8);
    fold.lineTo(38, 20);
    fold.lineTo(50, 20);
    painter.drawPath(fold);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(color.red(), color.green(), color.blue(), 40));
    QPainterPath triangle;
    triangle.moveTo(38, 8);
    triangle.lineTo(50, 20);
    triangle.lineTo(38, 20);
    triangle.closeSubpath();
    painter.drawPath(triangle);
}

void drawFolder(QPainter &painter, const QColor &color)
{
    QPainterPath folder;
    folder.moveTo(8, 18);
    folder.lineTo(26, 18);
    folder.lineTo(32, 25);
    folder.lineTo(56, 25);
    folder.lineTo(56, 51);
    folder.lineTo(8, 51);
    folder.closeSubpath();
    painter.setBrush(QColor(color.red(), color.green(), color.blue(), 32));
    painter.drawPath(folder);
    painter.setBrush(Qt::NoBrush);
    QPainterPath tab;
    tab.moveTo(8, 22);
    tab.lineTo(8, 18);
    tab.lineTo(26, 18);
    tab.lineTo(32, 25);
    painter.drawPath(tab);
}

void drawArrow(QPainter &painter, bool up)
{
    const qreal direction = up ? -1.0 : 1.0;
    QPainterPath shaft;
    shaft.moveTo(32, 16);
    shaft.lineTo(32, 48);
    painter.drawPath(shaft);
    QPainterPath head;
    head.moveTo(32 - 11, 16 + (up ? 11 : 37));
    head.lineTo(32, up ? 16 : 48);
    head.lineTo(32 + 11, 16 + (up ? 11 : 37));
    painter.drawPath(head);
    Q_UNUSED(direction);
}

QColor withAlpha(const QColor &color, int alpha)
{
    QColor result = color;
    result.setAlpha(alpha);
    return result;
}

class ThemeIconEngine : public QIconEngine {
public:
    ThemeIconEngine(const QString &name, const QColor &color) : m_name(name), m_color(color) {}
    QIconEngine *clone() const override { return new ThemeIconEngine(m_name, m_color); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State) override
    {
        return scaledPixmap(size, mode, QIcon::Off, 1.0);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        QColor color = m_color.isValid() ? m_color : Theme::iconColor();
        if (mode == QIcon::Disabled)
            color = Theme::isDark() ? QColor(QStringLiteral("#a0a0a0")) : QColor(QStringLiteral("#8c939b"));
        return IconProvider::instance().pixmap(m_name, qMin(size.width(), size.height()), color, scale);
    }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        const auto image = scaledPixmap(rect.size(), mode, state, painter->device()->devicePixelRatioF());
        const QSizeF logicalSize = image.deviceIndependentSize();
        painter->drawPixmap(QPointF(rect.center()) - QPointF(logicalSize.width() / 2,
                                                           logicalSize.height() / 2), image);
    }
private:
    QString m_name;
    QColor m_color;
};

} // namespace

IconProvider &IconProvider::instance()
{
    static IconProvider provider;
    return provider;
}

QIcon IconProvider::icon(const QString &name, const QColor &color, int size)
{
    const QString key = QStringLiteral("%1|%2|%3").arg(name, color.isValid() ? color.name() : QStringLiteral("theme"), QString::number(size));
    const auto found = m_cache.constFind(key);
    if (found != m_cache.constEnd())
        return found.value();

    QIcon result(new ThemeIconEngine(name, color));
    m_cache.insert(key, result);
    return result;
}

QPixmap IconProvider::pixmap(const QString &name, int size, const QColor &color, qreal dpr)
{
    const QColor effective = color.isValid() ? color : Theme::iconColor();
    return render(name, size, effective, dpr);
}

QPixmap IconProvider::render(const QString &name, int size, const QColor &color, qreal dpr)
{
    QPixmap pixmap(static_cast<int>(size * dpr), static_cast<int>(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(size / static_cast<qreal>(kCanvas), size / static_cast<qreal>(kCanvas));

    QPen pen(color, 3.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (name == QLatin1String("app")) {
        QLinearGradient gradient(0, 0, 64, 64);
        gradient.setColorAt(0.0, QColor(QStringLiteral("#f2a23d")));
        gradient.setColorAt(1.0, QColor(QStringLiteral("#d06d0b")));
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        painter.drawRoundedRect(QRectF(2, 2, 60, 60), 14, 14);
        painter.setBrush(QColor(255, 255, 255, 235));
        QPainterPath folder;
        folder.moveTo(11, 22);
        folder.lineTo(27, 22);
        folder.lineTo(32, 28);
        folder.lineTo(53, 28);
        folder.lineTo(53, 48);
        folder.lineTo(11, 48);
        folder.closeSubpath();
        painter.drawPath(folder);
        painter.setPen(QPen(QColor(QStringLiteral("#c46c0e")), 5, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        painter.drawLine(QPointF(24, 41), QPointF(24, 33));
        painter.drawLine(QPointF(20, 37), QPointF(24, 33));
        painter.drawLine(QPointF(28, 37), QPointF(24, 33));
        painter.setPen(QPen(QColor(QStringLiteral("#2f6ea8")), 5, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        painter.drawLine(QPointF(42, 35), QPointF(42, 44));
        painter.drawLine(QPointF(38, 40), QPointF(42, 44));
        painter.drawLine(QPointF(46, 40), QPointF(42, 44));
    } else if (name == QLatin1String("folder")) {
        drawFolder(painter, color);
    } else if (name == QLatin1String("file") || name == QLatin1String("text")) {
        painter.setBrush(withAlpha(color, 24));
        painter.drawPath(pagePath());
        drawFold(painter, color);
        if (name == QLatin1String("text")) {
            painter.drawLine(QPointF(24, 31), QPointF(42, 31));
            painter.drawLine(QPointF(24, 38), QPointF(42, 38));
            painter.drawLine(QPointF(24, 45), QPointF(36, 45));
        }
    } else if (name == QLatin1String("link")) {
        painter.setBrush(withAlpha(color, 24));
        painter.drawPath(pagePath());
        drawFold(painter, color);
        painter.setPen(QPen(color, 3.0));
        painter.drawEllipse(QPointF(27, 39), 5, 5);
        painter.drawEllipse(QPointF(39, 39), 5, 5);
        painter.drawLine(QPointF(31, 39), QPointF(35, 39));
    } else if (name == QLatin1String("image")) {
        painter.setBrush(withAlpha(color, 24));
        painter.drawPath(pagePath());
        drawFold(painter, color);
        painter.drawRect(QRectF(22, 31, 22, 16));
        painter.drawEllipse(QPointF(28, 36), 2.2, 2.2);
        QPainterPath mountain;
        mountain.moveTo(23, 45);
        mountain.lineTo(30, 38);
        mountain.lineTo(35, 43);
        mountain.lineTo(39, 39);
        mountain.lineTo(43, 45);
        painter.drawPath(mountain);
    } else if (name == QLatin1String("archive")) {
        painter.setBrush(withAlpha(color, 24));
        painter.drawPath(pagePath());
        drawFold(painter, color);
        painter.drawLine(QPointF(32, 28), QPointF(32, 31));
        painter.drawLine(QPointF(30, 31), QPointF(34, 31));
        painter.drawLine(QPointF(30, 35), QPointF(34, 35));
        painter.drawLine(QPointF(30, 39), QPointF(34, 39));
        painter.drawRect(QRectF(29, 43, 6, 6));
    } else if (name == QLatin1String("media")) {
        painter.setBrush(withAlpha(color, 24));
        painter.drawPath(pagePath());
        drawFold(painter, color);
        QPainterPath triangle;
        triangle.moveTo(27, 32);
        triangle.lineTo(41, 39);
        triangle.lineTo(27, 46);
        triangle.closeSubpath();
        painter.setBrush(withAlpha(color, 80));
        painter.drawPath(triangle);
    } else if (name == QLatin1String("code")) {
        painter.setBrush(withAlpha(color, 24));
        painter.drawPath(pagePath());
        drawFold(painter, color);
        painter.drawLine(QPointF(27, 33), QPointF(22, 39));
        painter.drawLine(QPointF(22, 39), QPointF(27, 45));
        painter.drawLine(QPointF(37, 33), QPointF(42, 39));
        painter.drawLine(QPointF(42, 39), QPointF(37, 45));
        painter.drawLine(QPointF(34, 31), QPointF(30, 47));
    } else if (name == QLatin1String("executable")) {
        painter.setBrush(withAlpha(color, 24));
        painter.drawPath(pagePath());
        drawFold(painter, color);
        QPainterPath gear;
        gear.addEllipse(QPointF(32, 39), 6, 6);
        painter.drawPath(gear);
        for (int index = 0; index < 8; ++index) {
            const qreal angle = index * kPi / 4.0;
            const QPointF start(32 + std::cos(angle) * 8, 39 + std::sin(angle) * 8);
            const QPointF end(32 + std::cos(angle) * 12, 39 + std::sin(angle) * 12);
            painter.drawLine(start, end);
        }
    } else if (name == QLatin1String("up")) {
        drawArrow(painter, true);
    } else if (name == QLatin1String("down")) {
        drawArrow(painter, false);
    } else if (name == QLatin1String("upload")) {
        drawArrow(painter, true);
        painter.drawLine(QPointF(16, 52), QPointF(48, 52));
    } else if (name == QLatin1String("download")) {
        drawArrow(painter, false);
        painter.drawLine(QPointF(16, 16), QPointF(48, 16));
    } else if (name == QLatin1String("refresh")) {
        QPainterPath arc;
        arc.arcMoveTo(QRectF(13, 13, 38, 38), 45);
        arc.arcTo(QRectF(13, 13, 38, 38), 45, 285);
        painter.drawPath(arc);
        QPainterPath head;
        head.moveTo(43, 11);
        head.lineTo(50, 20);
        head.lineTo(38, 21);
        painter.drawPath(head);
    } else if (name == QLatin1String("home")) {
        QPainterPath house;
        house.moveTo(10, 32);
        house.lineTo(32, 12);
        house.lineTo(54, 32);
        painter.drawPath(house);
        painter.drawRect(QRectF(18, 32, 28, 21));
        painter.drawRect(QRectF(28, 41, 8, 12));
    } else if (name == QLatin1String("connect") || name == QLatin1String("link-chain")) {
        painter.setPen(QPen(color, 3.2));
        painter.drawArc(QRectF(12, 22, 22, 20), 40 * 16, 220 * 16);
        painter.drawArc(QRectF(30, 22, 22, 20), 220 * 16, 220 * 16);
        painter.drawLine(QPointF(26, 32), QPointF(38, 32));
    } else if (name == QLatin1String("disconnect")) {
        painter.setPen(QPen(color, 3.2));
        painter.drawArc(QRectF(11, 22, 20, 20), 40 * 16, 190 * 16);
        painter.drawArc(QRectF(33, 22, 20, 20), 310 * 16, 190 * 16);
        painter.drawLine(QPointF(28, 26), QPointF(34, 38));
        painter.drawLine(QPointF(34, 26), QPointF(28, 38));
    } else if (name == QLatin1String("newfolder")) {
        drawFolder(painter, color);
        painter.setPen(QPen(Theme::accent(), 4.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(46, 34), QPointF(46, 46));
        painter.drawLine(QPointF(40, 40), QPointF(52, 40));
    } else if (name == QLatin1String("delete")) {
        painter.drawLine(QPointF(14, 20), QPointF(50, 20));
        painter.drawLine(QPointF(25, 20), QPointF(27, 14));
        painter.drawLine(QPointF(27, 14), QPointF(37, 14));
        painter.drawLine(QPointF(37, 14), QPointF(39, 20));
        QPainterPath bin;
        bin.moveTo(20, 20);
        bin.lineTo(23, 52);
        bin.lineTo(41, 52);
        bin.lineTo(44, 20);
        painter.drawPath(bin);
        painter.drawLine(QPointF(29, 28), QPointF(30, 45));
        painter.drawLine(QPointF(35, 28), QPointF(34, 45));
    } else if (name == QLatin1String("rename") || name == QLatin1String("edit")) {
        painter.drawRect(QRectF(12, 14, 24, 34));
        QPainterPath pencil;
        pencil.moveTo(24, 44);
        pencil.lineTo(27, 34);
        pencil.lineTo(49, 12);
        pencil.lineTo(56, 19);
        pencil.lineTo(34, 41);
        pencil.closeSubpath();
        painter.drawPath(pencil);
        painter.drawLine(QPointF(27, 34), QPointF(34, 41));
    } else if (name == QLatin1String("terminal") || name == QLatin1String("terminal-plus")) {
        painter.drawRoundedRect(QRectF(8, 12, 48, 40), 5, 5);
        painter.drawLine(QPointF(9, 21), QPointF(55, 21));
        painter.drawLine(QPointF(18, 29), QPointF(24, 35));
        painter.drawLine(QPointF(24, 35), QPointF(18, 41));
        painter.drawLine(QPointF(29, 42), QPointF(40, 42));
        if (name == QLatin1String("terminal-plus")) {
            painter.setPen(QPen(Theme::accent(), 4.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(48, 30), QPointF(48, 42));
            painter.drawLine(QPointF(42, 36), QPointF(54, 36));
        }
    } else if (name == QLatin1String("queue")) {
        painter.drawLine(QPointF(12, 18), QPointF(52, 18));
        painter.drawLine(QPointF(12, 30), QPointF(52, 30));
        painter.drawLine(QPointF(12, 42), QPointF(40, 42));
        painter.setBrush(color);
        painter.drawEllipse(QPointF(15, 18), 2.3, 2.3);
        painter.drawEllipse(QPointF(15, 30), 2.3, 2.3);
        painter.drawEllipse(QPointF(15, 42), 2.3, 2.3);
    } else if (name == QLatin1String("sync")) {
        painter.drawArc(QRectF(10, 14, 34, 30), 30 * 16, 150 * 16);
        painter.drawArc(QRectF(20, 20, 34, 30), 210 * 16, 150 * 16);
        QPainterPath head1;
        head1.moveTo(43, 13);
        head1.lineTo(50, 20);
        head1.lineTo(41, 23);
        painter.drawPath(head1);
        QPainterPath head2;
        head2.moveTo(21, 51);
        head2.lineTo(14, 44);
        head2.lineTo(23, 41);
        painter.drawPath(head2);
    } else if (name == QLatin1String("settings")) {
        painter.drawEllipse(QPointF(32, 32), 7, 7);
        for (int index = 0; index < 8; ++index) {
            const qreal angle = index * kPi / 4.0;
            const QPointF start(32 + std::cos(angle) * 13, 32 + std::sin(angle) * 13);
            const QPointF end(32 + std::cos(angle) * 20, 32 + std::sin(angle) * 20);
            painter.drawLine(start, end);
        }
    } else if (name == QLatin1String("site") || name == QLatin1String("server")) {
        painter.drawRoundedRect(QRectF(12, 10, 40, 14), 3, 3);
        painter.drawRoundedRect(QRectF(12, 28, 40, 14), 3, 3);
        painter.drawRoundedRect(QRectF(12, 46, 40, 8), 3, 3);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(19, 17), 2, 2);
        painter.drawEllipse(QPointF(19, 35), 2, 2);
    } else if (name == QLatin1String("open")) {
        painter.drawRect(QRectF(12, 24, 34, 28));
        painter.drawLine(QPointF(28, 16), QPointF(52, 16));
        painter.drawLine(QPointF(52, 16), QPointF(52, 40));
        painter.drawLine(QPointF(50, 18), QPointF(28, 40));
    } else if (name == QLatin1String("properties")) {
        painter.drawEllipse(QPointF(32, 32), 22, 22);
        painter.drawLine(QPointF(32, 28), QPointF(32, 44));
        painter.setBrush(color);
        painter.drawEllipse(QPointF(32, 21), 2.5, 2.5);
    } else if (name == QLatin1String("search")) {
        painter.drawEllipse(QPointF(27, 27), 14, 14);
        painter.drawLine(QPointF(38, 38), QPointF(52, 52));
    } else if (name == QLatin1String("close")) {
        painter.drawLine(QPointF(18, 18), QPointF(46, 46));
        painter.drawLine(QPointF(46, 18), QPointF(18, 46));
    } else if (name == QLatin1String("copy")) {
        painter.drawRoundedRect(QRectF(10, 14, 28, 34), 3, 3);
        painter.drawRoundedRect(QRectF(22, 22, 28, 34), 3, 3);
    } else if (name == QLatin1String("paste")) {
        painter.drawRoundedRect(QRectF(14, 14, 36, 42), 4, 4);
        painter.setBrush(withAlpha(color, 100));
        painter.drawRoundedRect(QRectF(24, 8, 16, 10), 3, 3);
        painter.drawLine(QPointF(22, 30), QPointF(42, 30));
        painter.drawLine(QPointF(22, 38), QPointF(42, 38));
        painter.drawLine(QPointF(22, 46), QPointF(36, 46));
    } else if (name == QLatin1String("clear")) {
        painter.drawLine(QPointF(16, 48), QPointF(48, 16));
        painter.drawLine(QPointF(22, 12), QPointF(52, 42));
        painter.drawLine(QPointF(10, 54), QPointF(54, 54));
    } else if (name == QLatin1String("play")) {
        QPainterPath triangle;
        triangle.moveTo(20, 14);
        triangle.lineTo(50, 32);
        triangle.lineTo(20, 50);
        triangle.closeSubpath();
        painter.setBrush(withAlpha(color, 70));
        painter.drawPath(triangle);
    } else if (name == QLatin1String("stop")) {
        painter.drawRoundedRect(QRectF(16, 16, 32, 32), 4, 4);
    } else if (name == QLatin1String("eye")) {
        QPainterPath eye;
        eye.moveTo(8, 32);
        eye.cubicTo(20, 12, 44, 12, 56, 32);
        eye.cubicTo(44, 52, 20, 52, 8, 32);
        painter.drawPath(eye);
        painter.drawEllipse(QPointF(32, 32), 7, 7);
    } else if (name == QLatin1String("eye-off")) {
        QPainterPath eye;
        eye.moveTo(8, 32);
        eye.cubicTo(20, 12, 44, 12, 56, 32);
        eye.cubicTo(44, 52, 20, 52, 8, 32);
        painter.drawPath(eye);
        painter.drawEllipse(QPointF(32, 32), 7, 7);
        painter.drawLine(QPointF(10, 10), QPointF(54, 54));
    } else if (name == QLatin1String("disk") || name == QLatin1String("monitor")) {
        painter.drawRoundedRect(QRectF(8, 12, 48, 32), 4, 4);
        painter.drawLine(QPointF(24, 52), QPointF(40, 52));
        painter.drawLine(QPointF(32, 44), QPointF(32, 52));
    } else if (name == QLatin1String("arrow-left")) {
        painter.drawLine(QPointF(52, 32), QPointF(14, 32));
        painter.drawLine(QPointF(14, 32), QPointF(27, 19));
        painter.drawLine(QPointF(14, 32), QPointF(27, 45));
    } else if (name == QLatin1String("arrow-right")) {
        painter.drawLine(QPointF(12, 32), QPointF(50, 32));
        painter.drawLine(QPointF(50, 32), QPointF(37, 19));
        painter.drawLine(QPointF(50, 32), QPointF(37, 45));
    } else if (name == QLatin1String("filter")) {
        QPainterPath funnel;
        funnel.moveTo(12, 14);
        funnel.lineTo(52, 14);
        funnel.lineTo(36, 34);
        funnel.lineTo(36, 50);
        funnel.lineTo(28, 44);
        funnel.lineTo(28, 34);
        funnel.closeSubpath();
        painter.drawPath(funnel);
    } else if (name == QLatin1String("lock")) {
        painter.drawRoundedRect(QRectF(16, 28, 32, 26), 4, 4);
        QPainterPath shackle;
        shackle.moveTo(22, 28);
        shackle.lineTo(22, 21);
        shackle.cubicTo(22, 8, 42, 8, 42, 21);
        shackle.lineTo(42, 28);
        painter.drawPath(shackle);
    } else if (name == QLatin1String("key")) {
        painter.drawEllipse(QPointF(22, 40), 9, 9);
        painter.drawLine(QPointF(29, 33), QPointF(52, 10));
        painter.drawLine(QPointF(44, 18), QPointF(50, 24));
        painter.drawLine(QPointF(38, 24), QPointF(44, 30));
    } else if (name == QLatin1String("user")) {
        painter.drawEllipse(QPointF(32, 22), 10, 10);
        QPainterPath shoulders;
        shoulders.moveTo(12, 54);
        shoulders.cubicTo(14, 38, 50, 38, 52, 54);
        painter.drawPath(shoulders);
    } else if (name == QLatin1String("plus")) {
        painter.drawLine(QPointF(14, 32), QPointF(50, 32));
        painter.drawLine(QPointF(32, 14), QPointF(32, 50));
    } else if (name == QLatin1String("menu")) {
        painter.drawLine(QPointF(12, 18), QPointF(52, 18));
        painter.drawLine(QPointF(12, 32), QPointF(52, 32));
        painter.drawLine(QPointF(12, 46), QPointF(52, 46));
    } else {
        painter.setBrush(withAlpha(color, 20));
        painter.drawPath(pagePath());
        drawFold(painter, color);
        painter.drawLine(QPointF(24, 35), QPointF(42, 35));
        painter.drawLine(QPointF(24, 43), QPointF(38, 43));
    }

    return pixmap;
}

QIcon IconProvider::fileIcon(const FileInfo &info, bool local, bool large)
{
    static QFileIconProvider provider;
    if (local && !Theme::isDark()) {
        QFileInfo fileInfo(info.path);
        if (fileInfo.exists() || fileInfo.isSymLink()) {
            QIcon systemIcon = provider.icon(fileInfo);
            if (!systemIcon.isNull())
                return systemIcon;
        }
    }
    if (info.isSymlink)
        return icon(QStringLiteral("link"));
    if (info.isDir)
        return provider.icon(QFileIconProvider::Folder);
    const QString key = info.iconKey();
    if (key == QLatin1String("folder"))
        return provider.icon(QFileIconProvider::Folder);
    if (key == QLatin1String("file"))
        return icon(QStringLiteral("file"));
    return icon(key);
}

QIcon IconProvider::appIcon()
{
    QIcon result;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        result.addPixmap(render(QStringLiteral("app"), size, Theme::accent(), 1.0));
    return result;
}

} // namespace wsc

