// SPDX-License-Identifier: MIT
// End-to-end transfer queue smoke test. Run against one of the local test
// servers started by tests/run_integration_tests.ps1.
#include "core/Backend.h"
#include "core/PathUtils.h"
#include "core/Settings.h"
#include "proto/LocalBackend.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

using namespace wsc;

namespace {

bool waitForQueue(TransferManager &manager, int timeoutMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&manager, &TransferManager::allTransfersFinished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    timeout.start(timeoutMs);
    if (manager.pendingCount() > 0 || manager.activeCount() > 0)
        loop.exec();
    return !timedOut;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void printQueue(const TransferManager &manager)
{
    for (const TransferRow &row : manager.model()->rows()) {
        std::printf("  queue[%d] %s status=%d done=%lld/%lld message=%s\n", row.id,
                    qPrintable(row.name), static_cast<int>(row.status),
                    static_cast<long long>(row.bytesDone),
                    static_cast<long long>(row.bytesTotal), qPrintable(row.message));
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    Settings &settings = Settings::instance();
    settings.load();
    settings.conflictPolicy = QStringLiteral("overwrite");
    settings.parallelTransfers = 2;
    settings.preserveTimestamps = false;
    settings.preservePermissions = false;

    const QStringList arguments = application.arguments();
    if (arguments.size() < 3) {
        std::fprintf(stderr,
                     "usage: transfer_smoke <session-url> <server-root>\n"
                     "example: transfer_smoke sftp://test:test@127.0.0.1:2222/ "
                     "C:\\code\\winscp\\qt\\build\\testroot\n");
        return 2;
    }
    SessionData session;
    QString error;
    if (!SessionData::parseUrl(arguments.at(1), &session, &error)) {
        std::fprintf(stderr, "url: %s\n", qPrintable(error));
        return 2;
    }
    const QString serverRoot = QDir::cleanPath(arguments.at(2));

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        std::fprintf(stderr, "cannot create temporary directory\n");
        return 2;
    }
    const QString localRoot = temporary.path();
    const QString sourceDir = QDir(localRoot).filePath(QStringLiteral("bundle"));
    const QString sourceFile = QDir(sourceDir).filePath(QStringLiteral("hello.txt"));
    const QString nestedFile = QDir(sourceDir).filePath(QStringLiteral("nested/data.bin"));
    QByteArray payload;
    for (int index = 0; index < 4096; ++index)
        payload.append(static_cast<char>('A' + index % 26));
    if (!writeFile(sourceFile, payload + "\n") ||
        !writeFile(nestedFile, QByteArray(8192, 'B'))) {
        std::fprintf(stderr, "cannot write local test files\n");
        return 2;
    }

    TransferManager manager;
    manager.setParallelLimit(2);
    QObject::connect(&manager, &TransferManager::statusMessage, &application,
                     [](const QString &message) {
                         std::fprintf(stderr, "status: %s\n", qPrintable(message));
                     });

    const FileInfo bundle = LocalBackend::infoForPath(sourceDir);
    const FileInfo single = LocalBackend::infoForPath(sourceFile);
    if (!bundle.isValid() || !single.isValid()) {
        std::fprintf(stderr, "invalid local file info\n");
        return 2;
    }

    std::printf("uploading to %s\n", qPrintable(session.endpoint()));
    QObject::connect(&manager, &TransferManager::allTransfersFinished, &application,
                     [] { std::printf("upload queue finished\n"); });
    manager.enqueueUpload(session, {bundle, single}, QStringLiteral("/smoke-upload"));
    if (!waitForQueue(manager, 60000)) {
        std::fprintf(stderr, "upload timed out\n");
        printQueue(manager);
        manager.shutdown();
        return 1;
    }
    bool uploadOk = true;
    for (const TransferRow &row : manager.model()->rows()) {
        if (row.status != TransferStatus::Completed) {
            uploadOk = false;
            std::fprintf(stderr, "upload row failed: %s (%s)\n", qPrintable(row.name),
                         qPrintable(row.message));
        }
    }
    const QString expectedNested =
        QDir(serverRoot).filePath(QStringLiteral("smoke-upload/bundle/nested/data.bin"));
    const QString expectedSingle =
        QDir(serverRoot).filePath(QStringLiteral("smoke-upload/hello.txt"));
    if (!QFileInfo::exists(expectedNested) || QFileInfo(expectedNested).size() != 8192)
        uploadOk = false;
    if (!QFileInfo::exists(expectedSingle) || QFileInfo(expectedSingle).size() != payload.size() + 1)
        uploadOk = false;
    if (!uploadOk) {
        printQueue(manager);
        manager.shutdown();
        return 1;
    }
    std::printf("upload verified: %lld + %lld bytes\n",
                static_cast<long long>(payload.size() + 1), 8192LL);

    FileInfo remoteFile;
    remoteFile.name = QStringLiteral("hello.txt");
    remoteFile.path = QStringLiteral("/hello.txt");
    remoteFile.size = QFileInfo(QDir(serverRoot).filePath(QStringLiteral("hello.txt"))).size();
    remoteFile.isDir = false;
    const QString downloadRoot = QDir(localRoot).filePath(QStringLiteral("downloads"));
    QDir().mkpath(downloadRoot);
    std::printf("downloading hello.txt\n");
    manager.enqueueDownload(session, {remoteFile}, downloadRoot);
    if (!waitForQueue(manager, 60000)) {
        std::fprintf(stderr, "download timed out\n");
        printQueue(manager);
        manager.shutdown();
        return 1;
    }
    bool downloadOk = true;
    for (const TransferRow &row : manager.model()->rows()) {
        if (row.status != TransferStatus::Completed)
            downloadOk = false;
    }
    const QByteArray downloaded = readFile(QDir(downloadRoot).filePath(QStringLiteral("hello.txt")));
    if (downloaded.isEmpty())
        downloadOk = false;
    if (!downloadOk) {
        printQueue(manager);
        manager.shutdown();
        return 1;
    }
    std::printf("download verified: %lld bytes\n", static_cast<long long>(downloaded.size()));

    manager.shutdown();
    std::printf("TRANSFER_SMOKE_OK\n");
    return 0;
}
