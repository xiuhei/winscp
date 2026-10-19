// SPDX-License-Identifier: MIT
// Small console smoke test for the protocol backends. It is intentionally not
// part of the shipped application but is useful for CI and manual verification.
#include "core/Backend.h"
#include "core/PathUtils.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <cstdio>

using namespace wsc;

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    Settings::instance().load();

    const QStringList arguments = application.arguments();
    if (arguments.size() < 2) {
        std::fprintf(stderr, "usage: backend_smoke <session-url>\n");
        return 2;
    }
    SessionData session;
    QString error;
    if (!SessionData::parseUrl(arguments.at(1), &session, &error)) {
        std::fprintf(stderr, "url: %s\n", qPrintable(error));
        return 2;
    }
    session.timeoutSeconds = 30;
    BackendPtr backend = createBackend(session.protocol);
    if (!backend) {
        std::fprintf(stderr, "unsupported protocol\n");
        return 2;
    }
    std::printf("connecting to %s (%s)\n", qPrintable(session.endpoint()),
                qPrintable(protocolDisplayName(session.protocol)));
    if (!backend->connect(session, &error)) {
        std::fprintf(stderr, "connect failed: %s\n", qPrintable(error));
        return 1;
    }
    const QString home = backend->homePath();
    std::printf("home: %s\n", qPrintable(home));
    FileList entries;
    if (!backend->list(home, &entries, &error)) {
        std::fprintf(stderr, "list failed: %s\n", qPrintable(error));
        return 1;
    }
    std::printf("entries: %lld\n", static_cast<long long>(entries.size()));
    for (int index = 0; index < qMin<int>(entries.size(), 30); ++index) {
        const FileInfo &info = entries.at(index);
        std::printf("  %c %10lld  %s  %s%s\n", info.isDir ? 'd' : '-',
                    static_cast<long long>(info.size), qPrintable(info.permissionsString()),
                    qPrintable(info.name), info.isSymlink ? " (link)" : "");
    }

    // Exercise a real download when a small file is available.
    const FileInfo *file = nullptr;
    for (const FileInfo &info : entries) {
        if (!info.isDir && !info.isSymlink && info.size > 0 && info.size < 256 * 1024) {
            file = &info;
            break;
        }
    }
    if (file) {
        const QString target = QDir(QDir::tempPath())
                                   .filePath(QStringLiteral("winscpqt-smoke-%1").arg(file->name));
        std::printf("downloading %s -> %s\n", qPrintable(file->name), qPrintable(target));
        if (!backend->downloadFile(file->path, target, file->size, nullptr, nullptr, &error)) {
            std::fprintf(stderr, "download failed: %s\n", qPrintable(error));
            return 1;
        }
        std::printf("downloaded %lld bytes\n", static_cast<long long>(QFileInfo(target).size()));
        QFile::remove(target);
    }

    if (arguments.size() >= 4) {
        const QString localPath = arguments.at(2);
        const QString remotePath = arguments.at(3);
        std::printf("uploading %s -> %s\n", qPrintable(localPath), qPrintable(remotePath));
        if (!backend->uploadFile(localPath, remotePath, nullptr, nullptr, &error)) {
            std::fprintf(stderr, "upload failed: %s\n", qPrintable(error));
            return 1;
        }
        FileInfo uploaded;
        if (!backend->stat(remotePath, &uploaded, &error)) {
            std::fprintf(stderr, "stat after upload failed: %s\n", qPrintable(error));
            return 1;
        }
        std::printf("uploaded %lld bytes mode %s\n", static_cast<long long>(uploaded.size),
                    qPrintable(uploaded.permissionsString()));
        if (qEnvironmentVariableIsSet("WINSCPQT_SMOKE_DELETE")) {
            if (!backend->removeFile(remotePath, &error)) {
                std::fprintf(stderr, "remove failed: %s\n", qPrintable(error));
                return 1;
            }
            std::printf("removed %s\n", qPrintable(remotePath));
        }
    }
    backend->disconnect();
    return 0;
}
