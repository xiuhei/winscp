#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/SessionData.h"
#include "core/Settings.h"
#include "ui/IconProvider.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QCursor>
#include <QFileInfo>
#include <QToolTip>
#include <QMessageBox>
#include <QTimer>

using namespace wsc;

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("WinSCPQt"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("winscpqt.local"));
    QCoreApplication::setApplicationName(QStringLiteral("WinSCPQt"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    QApplication application(argc, argv);
    application.setApplicationDisplayName(QStringLiteral("WinSCP Qt"));
    application.setQuitOnLastWindowClosed(true);

    Q_INIT_RESOURCE(resources);

    AppPaths::ensureDirectories();
    Settings::instance().load();
    SiteStore::instance().load();

    Theme::apply(Settings::instance().theme);
    application.setWindowIcon(IconProvider::instance().appIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("WinSCP Qt — 使用 C++ / Qt Widgets 重写的 WinSCP，"
                       "内置位于界面下方的 SSH 终端。"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption demoOption(QStringLiteral("demo"),
                                  QStringLiteral("使用内置演示数据启动，便于截图和界面预览。"));
    QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"),
        QStringLiteral("启动后将主窗口保存为 PNG 文件并退出。"), QStringLiteral("file"));
    QCommandLineOption urlOption(QStringLiteral("url"),
                                 QStringLiteral("要连接的会话地址（也可作为位置参数）。"),
                                 QStringLiteral("url"));
    QCommandLineOption exportIconOption(
        QStringLiteral("export-icon"),
        QStringLiteral("导出应用图标为 PNG（用于生成 Windows 图标资源）。"),
        QStringLiteral("file"));
    parser.addOption(demoOption);
    parser.addOption(screenshotOption);
    parser.addOption(urlOption);
    parser.addOption(exportIconOption);
    parser.addPositionalArgument(QStringLiteral("session"),
                                 QStringLiteral("会话地址，例如 sftp://user:pass@host:22/path"));
    parser.process(application);

    if (parser.isSet(exportIconOption)) {
        const QString iconPath = QFileInfo(parser.value(exportIconOption)).absoluteFilePath();
        QDir().mkpath(QFileInfo(iconPath).absolutePath());
        return IconProvider::instance().appIcon().pixmap(256, 256).save(iconPath, "PNG") ? 0 : 1;
    }

    Logger::instance().info(QStringLiteral("WinSCP Qt %1 启动")
                                .arg(QCoreApplication::applicationVersion()));

    MainWindow window;
    window.show();

    QString sessionError;
    SessionData initialSession;
    QString sessionArgument;
    if (parser.isSet(urlOption))
        sessionArgument = parser.value(urlOption);
    if (sessionArgument.isEmpty() && !parser.positionalArguments().isEmpty())
        sessionArgument = parser.positionalArguments().first();

    if (parser.isSet(demoOption)) {
        window.enableDemoMode();
    } else if (!sessionArgument.isEmpty()) {
        if (SessionData::parseUrl(sessionArgument, &initialSession, &sessionError)) {
            window.setInitialSession(initialSession, true);
        } else {
            QTimer::singleShot(350, &window, [&window, sessionError] {
                QMessageBox::warning(&window, QStringLiteral("无法解析会话地址"), sessionError);
            });
        }
    }

    if (parser.isSet(screenshotOption)) {
        const QString path = QFileInfo(parser.value(screenshotOption)).absoluteFilePath();
        const int delay = parser.isSet(demoOption) ? 1600 : (sessionArgument.isEmpty() ? 1600 : 7000);
        QTimer::singleShot(delay, &application, [&window, path] {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QCursor::setPos(2, 2);
            QToolTip::hideText();
            QApplication::processEvents();
            const bool saved = window.grab().save(path, "PNG");
            if (!saved)
                qWarning("Failed to save screenshot to %s", qPrintable(path));
            QCoreApplication::quit();
        });
    }

    return application.exec();
}

