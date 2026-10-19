// SPDX-License-Identifier: MIT
#include "core/Backend.h"
#include "core/PathUtils.h"
#include "core/SessionData.h"
#include "sync/SyncTypes.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

using namespace wsc;

namespace {

bool writeFile(const QString &path, const QByteArray &data, const QDateTime &modified = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(data) != data.size())
        return false;
    file.close();
    if (modified.isValid()) {
        QFile update(path);
        if (update.open(QIODevice::ReadWrite))
            update.setFileTime(modified, QFileDevice::FileModificationTime);
    }
    return true;
}

int countAction(const SyncPlan &plan, SyncAction action)
{
    int count = 0;
    for (const SyncItem &item : plan.items) {
        if (item.action == action)
            ++count;
    }
    return count;
}

const SyncItem *findItem(const SyncPlan &plan, const QString &relative, SyncAction action)
{
    for (const SyncItem &item : plan.items) {
        if (item.relativePath == relative && item.action == action)
            return &item;
    }
    return nullptr;
}

void dumpPlan(const SyncPlan &plan)
{
    for (const SyncItem &item : plan.items) {
        std::printf("  %s %s -> %s (%s)\n", qPrintable(syncActionName(item.action)),
                    qPrintable(item.relativePath), qPrintable(item.targetDirectory),
                    qPrintable(item.reason));
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() < 3) {
        std::fprintf(stderr, "usage: sync_smoke <session-url> <server-root>\n");
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
    if (!temporary.isValid())
        return 2;
    const QString localRoot = QDir(temporary.path()).filePath(QStringLiteral("sync-local"));
    const QString remoteRoot = QDir(serverRoot).filePath(QStringLiteral("sync-remote"));
    const QString remoteRootPath = QStringLiteral("/sync-remote");
    QDir(localRoot).removeRecursively();
    QDir(remoteRoot).removeRecursively();
    QDir().mkpath(localRoot);
    QDir().mkpath(remoteRoot);

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime older = now.addSecs(-3600);
    const QDateTime newest = now.addSecs(120);

    bool ok = true;
    ok = ok && writeFile(QDir(localRoot).filePath(QStringLiteral("same.txt")), "same", older);
    ok = ok && writeFile(QDir(remoteRoot).filePath(QStringLiteral("same.txt")), "same", older);
    ok = ok && writeFile(QDir(localRoot).filePath(QStringLiteral("local-newer.txt")), "local", newest);
    ok = ok && writeFile(QDir(remoteRoot).filePath(QStringLiteral("local-newer.txt")), "local", older);
    ok = ok && writeFile(QDir(localRoot).filePath(QStringLiteral("local-only.txt")), "only-local", older);
    ok = ok && writeFile(QDir(remoteRoot).filePath(QStringLiteral("remote-only.txt")), "only-remote", older);
    ok = ok && writeFile(QDir(localRoot).filePath(QStringLiteral("remote-newer.txt")), "remote", older);
    ok = ok && writeFile(QDir(remoteRoot).filePath(QStringLiteral("remote-newer.txt")), "remote", newest);
    ok = ok && writeFile(QDir(localRoot).filePath(QStringLiteral("nested/local.txt")), "nested-local", older);
    ok = ok && writeFile(QDir(remoteRoot).filePath(QStringLiteral("nested/remote.txt")), "nested-remote", older);
    if (!ok) {
        std::fprintf(stderr, "cannot prepare sync test files\n");
        return 1;
    }

    SyncOptions options;
    options.direction = SyncDirection::LocalToRemote;
    options.recursive = true;
    options.compareTime = true;
    options.deleteExtras = false;
    SyncPlan plan = buildSyncPlan(session, localRoot, remoteRootPath, options, HostKeyPrompt());
    if (!plan.error.isEmpty()) {
        std::fprintf(stderr, "plan error: %s\n", qPrintable(plan.error));
        return 1;
    }
    std::printf("local->remote plan:\n");
    dumpPlan(plan);
    if (countAction(plan, SyncAction::Upload) != 3 ||
        countAction(plan, SyncAction::Download) != 0 ||
        countAction(plan, SyncAction::DeleteRemote) != 0) {
        std::fprintf(stderr, "unexpected L2R counts\n");
        return 1;
    }
    if (!findItem(plan, QStringLiteral("local-newer.txt"), SyncAction::Upload) ||
        !findItem(plan, QStringLiteral("local-only.txt"), SyncAction::Upload) ||
        !findItem(plan, QStringLiteral("nested/local.txt"), SyncAction::Upload) ||
        findItem(plan, QStringLiteral("same.txt"), SyncAction::Upload)) {
        std::fprintf(stderr, "unexpected L2R actions\n");
        return 1;
    }
    const SyncItem *nested = findItem(plan, QStringLiteral("nested/local.txt"), SyncAction::Upload);
    if (!nested || !nested->targetDirectory.endsWith(QStringLiteral("/sync-remote/nested"))) {
        std::fprintf(stderr, "unexpected nested target: %s\n",
                     nested ? qPrintable(nested->targetDirectory) : "<missing>");
        return 1;
    }

    options.deleteExtras = true;
    plan = buildSyncPlan(session, localRoot, remoteRootPath, options, HostKeyPrompt());
    std::printf("local->remote with deletes:\n");
    dumpPlan(plan);
    if (countAction(plan, SyncAction::DeleteRemote) != 2 ||
        !findItem(plan, QStringLiteral("remote-only.txt"), SyncAction::DeleteRemote) ||
        !findItem(plan, QStringLiteral("nested/remote.txt"), SyncAction::DeleteRemote)) {
        std::fprintf(stderr, "unexpected delete actions\n");
        return 1;
    }

    options = SyncOptions();
    options.direction = SyncDirection::RemoteToLocal;
    options.recursive = true;
    options.compareTime = true;
    plan = buildSyncPlan(session, localRoot, remoteRootPath, options, HostKeyPrompt());
    std::printf("remote->local plan:\n");
    dumpPlan(plan);
    if (countAction(plan, SyncAction::Download) != 3 ||
        !findItem(plan, QStringLiteral("remote-newer.txt"), SyncAction::Download) ||
        !findItem(plan, QStringLiteral("remote-only.txt"), SyncAction::Download) ||
        !findItem(plan, QStringLiteral("nested/remote.txt"), SyncAction::Download)) {
        std::fprintf(stderr, "unexpected R2L actions\n");
        return 1;
    }

    options = SyncOptions();
    options.direction = SyncDirection::Both;
    options.recursive = true;
    options.compareTime = true;
    plan = buildSyncPlan(session, localRoot, remoteRootPath, options, HostKeyPrompt());
    std::printf("bidirectional plan:\n");
    dumpPlan(plan);
    if (countAction(plan, SyncAction::Upload) != 3 ||
        countAction(plan, SyncAction::Download) != 3) {
        std::fprintf(stderr, "unexpected bidirectional counts\n");
        return 1;
    }

    QDir(localRoot).removeRecursively();
    QDir(remoteRoot).removeRecursively();
    std::printf("SYNC_SMOKE_OK\n");
    return 0;
}

