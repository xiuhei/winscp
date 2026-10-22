// SPDX-License-Identifier: MIT
#pragma once

#include "core/Types.h"

#include <QColor>
#include <QHash>
#include <QIcon>

namespace wsc {

/// Runtime-painted icon set. Keeps the application self contained (no image
/// resources) and lets the palette follow the active theme.
class IconProvider {
public:
    static IconProvider &instance();

    QIcon icon(const QString &name, const QColor &color = QColor(), int size = 64);
    QIcon fileIcon(const FileInfo &info, bool local, bool large = false);
    QIcon appIcon();

    QPixmap pixmap(const QString &name, int size, const QColor &color = QColor(), qreal dpr = 1.0);

private:
    IconProvider() = default;
    QPixmap render(const QString &name, int size, const QColor &color, qreal dpr = 1.0);
    QHash<QString, QIcon> m_cache;
};

} // namespace wsc
