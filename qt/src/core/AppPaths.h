// SPDX-License-Identifier: MIT
#pragma once

#include <QString>

namespace wsc {

/// Portable, testable application locations.
///
/// Priority:
///   1. WINSCPQT_HOME environment variable
///   2. <application dir>/data when a "portable.ini" marker sits next to the exe
///   3. QStandardPaths::AppConfigLocation / AppDataLocation
class AppPaths {
public:
    static QString configDir();
    static QString dataDir();
    static QString logsDir();
    static QString sessionsFile();
    static QString sitesFile();
    static QString knownHostsFile();
    static QString settingsFile();
    static QString cacheDir();
    static QString tempDir();
    static QString themeFile(const QString &name);
    static bool isPortable();
    static void ensureDirectories();
    static void setApplicationRoot(const QString &root);
};

} // namespace wsc
