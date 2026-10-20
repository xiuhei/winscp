// SPDX-License-Identifier: MIT
// Small console smoke test for the protocol backends. It is intentionally not
// part of the shipped application but is useful for CI and manual verification.
#include "core/Backend.h"
#include "core/PathUtils.h"
#include "core/Settings.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QUuid>

#include <cstdio>

using namespace wsc;

static bool verifyCancelledOverwrite(Backend *backend, const QString &home, QString *error)
{
    const QString directory = path::remoteJoin(home,
        QStringLiteral(".winscp-cancel-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!backend->mkdir(directory, error))
        return false;
    const auto cleanup = qScopeGuard([&] { backend->removeDir(directory, nullptr); });
    const QString target = path::remoteJoin(directory, QStringLiteral("original.txt"));
    if (!backend->writeFile(target, QByteArrayLiteral("original contents"), error))
        return false;
    QTemporaryFile input;
    if (!input.open() || input.write(QByteArray(512 * 1024, 'x')) != 512 * 1024 || !input.flush())
        return false;
    input.close();
    CancelToken cancel{false};
    const bool uploaded = backend->uploadFile(input.fileName(), target,
        [&](const TransferProgress &) { cancel.store(true); }, &cancel, error);
    if (uploaded || !cancel.load()) {
        *error = QStringLiteral("取消上传未生效。");
        return false;
    }
    QByteArray contents;
    if (!backend->readFile(target, &contents, error) || contents != QByteArrayLiteral("original contents")) {
        *error = QStringLiteral("取消覆盖上传破坏了原文件。");
        return false;
    }
    FileList files;
    if (!backend->list(directory, &files, error) || files.size() != 1) {
        *error = QStringLiteral("上传取消后残留临时文件。");
        return false;
    }
    return true;
}

static bool verifyTransferIntegrity(Backend *backend, const QString &home, QString *error)
{
    const QString directory = path::remoteJoin(home,
        QStringLiteral(".winscp-integrity-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!backend->mkdir(directory, error))
        return false;
    const auto cleanup = qScopeGuard([&] { backend->removeDir(directory, nullptr); });
    QTemporaryDir local;
    if (!local.isValid())
        return false;
    const QString remote = path::remoteJoin(directory, QStringLiteral("payload.bin"));
    QByteArray payload(1536 * 1024 + 37, Qt::Uninitialized);
    for (qsizetype i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>((i * 31 + i / 257) % 256);
    if (!backend->writeFile(remote, payload, error))
        return false;
    QByteArray contents;
    if (!backend->readFile(remote, &contents, error) || contents != payload) {
        *error = QStringLiteral("大文件读取内容或尾部不完整。");
        return false;
    }
    const QString destination = local.filePath(QStringLiteral("download.bin"));
    if (!backend->downloadFile(remote, destination, payload.size(), {}, nullptr, error))
        return false;
    QFile downloaded(destination);
    if (!downloaded.open(QIODevice::ReadOnly) || downloaded.readAll() != payload) {
        *error = QStringLiteral("下载内容或尾部不完整。");
        return false;
    }
    downloaded.close();
    if (backend->downloadFile(remote, destination, payload.size() + 100, {}, nullptr, error) ||
        !downloaded.open(QIODevice::ReadOnly) || downloaded.readAll() != payload) {
        *error = QStringLiteral("不完整下载替换了原本地文件。");
        return false;
    }
    downloaded.close();
    CancelToken cancel{false};
    const bool cancelledDownload = backend->downloadFile(remote, destination, payload.size(),
        [&](const TransferProgress &) { cancel.store(true); }, &cancel, error);
    if (cancelledDownload || !downloaded.open(QIODevice::ReadOnly) || downloaded.readAll() != payload) {
        *error = QStringLiteral("取消下载未保留原本地文件。");
        return false;
    }
    const QString named = path::remoteJoin(directory, QStringLiteral("report -> final "));
    if (!backend->writeFile(named, "named contents", error))
        return false;
    FileList files;
    if (!backend->list(directory, &files, error))
        return false;
    bool found = false;
    for (const auto &file : files)
        found |= file.path == named && file.name == QStringLiteral("report -> final ");
    if (!found || !backend->readFile(named, &contents, error) || contents != QByteArray("named contents")) {
        *error = QStringLiteral("文件名被列表解析或路径处理改变。");
        return false;
    }
    return true;
}

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
    if (session.protocol == Protocol::Ftp || session.protocol == Protocol::Ftps) {
        QFile log(Logger::instance().logFilePath());
        if (!session.password.isEmpty() && log.open(QIODevice::ReadOnly) &&
            log.readAll().contains("FTP > PASS " + session.password.toUtf8())) {
            std::fprintf(stderr, "FTP password was written to the log\n");
            return 1;
        }
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
    if (!verifyTransferIntegrity(backend.get(), home, &error)) {
        std::fprintf(stderr, "integrity regression failed: %s\n", qPrintable(error));
        return 1;
    }
    std::printf("large payload, download failure/cancel and exact filenames verified\n");
    if (session.protocol == Protocol::Sftp || session.protocol == Protocol::Scp ||
        session.protocol == Protocol::Ftp || session.protocol == Protocol::Ftps) {
        if (!verifyCancelledOverwrite(backend.get(), home, &error)) {
            std::fprintf(stderr, "cancel regression failed: %s\n", qPrintable(error));
            return 1;
        }
        std::printf("cancelled overwrite preserved original and removed temporary files\n");
    }
    backend->disconnect();
    return 0;
}
