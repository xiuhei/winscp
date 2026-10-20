#include "ui/RemoteFileOpener.h"

#include "core/AppPaths.h"
#include "core/FileSystemController.h"
#include "core/PathUtils.h"
#include "core/Settings.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QTimer>
#include <QUrl>

namespace wsc {
namespace {

QString identity(const SessionData &session, const QString &path)
{
    const QJsonArray parts{protocolId(session.protocol), session.hostName.toLower(), session.port,
                           session.userName, session.webDavHttps, path};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(parts).toJson(QJsonDocument::Compact),
                                                       QCryptographicHash::Sha256).toHex());
}

QJsonObject readRecord(const QString &directory)
{
    QFile file(QDir(directory).filePath(QStringLiteral("metadata.json")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeRecord(const QString &directory, const QJsonObject &record)
{
    QSaveFile file(QDir(directory).filePath(QStringLiteral("metadata.json")));
    return file.open(QIODevice::WriteOnly) &&
           file.write(QJsonDocument(record).toJson()) >= 0 && file.commit();
}

bool sameSource(const QJsonObject &record, const FileInfo &info)
{
    return info.modified.isValid() && info.size >= 0 &&
           record.value(QStringLiteral("size")).toVariant().toLongLong() == info.size &&
           record.value(QStringLiteral("modified")).toString() == info.modified.toUTC().toString(Qt::ISODateWithMs);
}

bool isText(const QByteArray &sample)
{
    if (sample.isEmpty())
        return true;
    if (sample.contains('\0'))
        return false;
    const auto mime = QMimeDatabase().mimeTypeForData(sample);
    if (!mime.inherits(QStringLiteral("text/plain")) && !mime.isDefault())
        return false;
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(sample);
    if (decoder.hasError())
        return false;
    for (const QChar ch : decoded) {
        if (ch.category() == QChar::Other_Control && ch != QLatin1Char('\n') &&
            ch != QLatin1Char('\r') && ch != QLatin1Char('\t') && ch != QLatin1Char('\f') &&
            ch != QLatin1Char('\b') && ch.unicode() != 27)
            return false;
    }
    return true;
}

}

struct RemoteFileOpener::Request {
    QString key;
    FileInfo file;
    QString directory;
    QString localPath;
    QJsonObject record;
    QPointer<QProgressDialog> dialog;
    quint64 requestId = 0;
    int taskId = -1;
    bool verifying = false;
    bool cancelled = false;
};

RemoteFileOpener::RemoteFileOpener(FileSystemController *files, TransferManager *transfers, QWidget *parent)
    : QObject(parent), m_files(files), m_transfers(transfers), m_parent(parent)
{
    connect(files, &FileSystemController::fileStated, this, &RemoteFileOpener::stated);
    connect(transfers->model(), &TransferQueueModel::rowsChanged, this, [this] {
        const auto requests = m_requests.values();
        for (const auto &request : requests)
            if (request->taskId >= 0)
                update(request->taskId);
    });
    connect(files, &FileSystemController::connecting, this, [this] {
        const auto requests = m_requests.values();
        for (const auto &request : requests) {
            if (request->taskId >= 0)
                m_transfers->cancelTask(request->taskId);
            fail(request, QStringLiteral("会话已切换，请重新打开文件。"));
        }
    });
    connect(files, &FileSystemController::disconnected, this, [this] {
        const auto requests = m_requests.values();
        for (const auto &request : requests)
            if (request->taskId < 0 || request->verifying)
                fail(request, QStringLiteral("连接已断开，请重新打开文件。"));
    });
    auto *timer = new QTimer(this);
    timer->setInterval(60 * 60 * 1000);
    connect(timer, &QTimer::timeout, this, &RemoteFileOpener::cleanup);
    timer->start();
    cleanup();
}

void RemoteFileOpener::open(const SessionData &session, const FileInfo &file)
{
    const QString key = identity(session, file.path);
    if (auto existing = m_requests.value(key)) {
        if (existing->dialog) {
            existing->dialog->show();
            existing->dialog->raise();
            existing->dialog->activateWindow();
        }
        return;
    }
    cleanup();
    auto request = std::make_shared<Request>();
    request->requestId = m_nextRequestId++;
    request->key = key;
    request->file = file;
    auto *dialog = new QProgressDialog(QStringLiteral("正在检查远程文件：%1").arg(file.name),
                                        QStringLiteral("取消"), 0, 0, m_parent, Qt::Window);
    dialog->setObjectName(QStringLiteral("RemoteOpenProgress"));
    dialog->setWindowTitle(QStringLiteral("下载并打开 — %1").arg(file.name));
    dialog->setWindowModality(Qt::NonModal);
    dialog->setMinimumDuration(0);
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    request->dialog = dialog;
    m_requests.insert(key, request);
    connect(dialog, &QProgressDialog::canceled, this, [this, key] {
        const auto request = m_requests.value(key);
        if (!request)
            return;
        request->cancelled = true;
        if (request->taskId >= 0 && !request->verifying)
            m_transfers->cancelTask(request->taskId);
        else
            m_requests.remove(key);
    });
    connect(dialog, &QProgressDialog::canceled, dialog, &QObject::deleteLater);
    dialog->show();
    m_files->statFile(file.path, request->requestId);
}

void RemoteFileOpener::stated(quint64 requestId, const SessionData &session, const QString &path,
                              const FileInfo &info, const QString &error)
{
    const auto request = m_requests.value(identity(session, path));
    if (!request || request->requestId != requestId || request->cancelled)
        return;
    if (!error.isEmpty() || !info.isValid() || info.isDir) {
        fail(request, error.isEmpty() ? QStringLiteral("远程文件不存在或不是普通文件。") : error);
        return;
    }
    if (request->verifying) {
        const QFileInfo local(request->localPath);
        const bool stable = info.size == request->file.size && info.modified == request->file.modified &&
                            local.size() == info.size;
        QFile downloaded(request->localPath);
        const bool snapshot = !stable && info.size > request->file.size &&
                              local.size() >= request->file.size && local.size() <= info.size &&
                              downloaded.open(QIODevice::ReadOnly) && isText(downloaded.peek(65536));
        if (!stable && !snapshot) {
            fail(request, QStringLiteral("下载期间远程文件已改变，请重新打开。"));
            return;
        }
        request->record.insert(QStringLiteral("complete"), stable);
        request->record.insert(QStringLiteral("localModified"), QString::number(local.lastModified().toMSecsSinceEpoch()));
        writeRecord(request->directory, request->record);
        m_requests.remove(request->key);
        if (request->dialog)
            request->dialog->close();
        openLocal(request->localPath, request->file.path, snapshot);
        return;
    }
    request->file = info;
    request->file.path = path;
    request->file.name = path::remoteName(path);
    // stat follows a file link; the transfer must download its contents.
    request->file.isSymlink = false;
    const QByteArray revision = request->key.toLatin1() + QByteArray::number(info.size) +
                                info.modified.toUTC().toString(Qt::ISODateWithMs).toUtf8();
    const QString folder = QString::fromLatin1(QCryptographicHash::hash(revision, QCryptographicHash::Sha256).toHex());
    request->directory = QDir(AppPaths::downloadCacheDir()).filePath(folder);
    request->localPath = QDir(request->directory).filePath(QStringLiteral("content/") + path::safeRemoteFileName(request->file.name));
    QJsonObject record = readRecord(request->directory);
    const QFileInfo local(request->localPath);
    const bool cached = record.value(QStringLiteral("complete")).toBool() && sameSource(record, info) &&
        local.isFile() && !local.isSymLink() && local.size() == info.size &&
        record.value(QStringLiteral("localModified")).toString() == QString::number(local.lastModified().toMSecsSinceEpoch());
    if (!QDir().mkpath(QFileInfo(request->localPath).absolutePath()) || QFileInfo(request->directory).isSymLink() ||
        QFileInfo(QFileInfo(request->localPath).absolutePath()).isSymLink()) {
        fail(request, QStringLiteral("无法创建程序目录下的下载缓存：%1").arg(request->directory));
        return;
    }
    record = {{QStringLiteral("cache"), QStringLiteral("winscp-remote-open")},
              {QStringLiteral("size"), QString::number(info.size)},
              {QStringLiteral("modified"), info.modified.toUTC().toString(Qt::ISODateWithMs)},
              {QStringLiteral("lastUsed"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
              {QStringLiteral("complete"), cached},
              {QStringLiteral("localModified"), QString::number(local.lastModified().toMSecsSinceEpoch())}};
    request->record = record;
    if (!writeRecord(request->directory, record)) {
        fail(request, QStringLiteral("无法写入下载缓存记录。"));
        return;
    }
    if (cached) {
        m_requests.remove(request->key);
        if (request->dialog)
            request->dialog->close();
        openLocal(request->localPath, request->file.path);
        return;
    }
    if (local.exists() && !QFile::remove(request->localPath)) {
        fail(request, QStringLiteral("缓存文件正在使用，无法更新：%1").arg(request->localPath));
        return;
    }
    request->taskId = m_transfers->enqueueDownload(session, {request->file}, QFileInfo(request->localPath).absolutePath(), true);
}

void RemoteFileOpener::update(int id)
{
    std::shared_ptr<Request> request;
    for (const auto &item : m_requests)
        if (item->taskId == id) {
            request = item;
            break;
        }
    if (!request || request->verifying)
        return;
    const auto row = m_transfers->model()->rowAt(m_transfers->model()->rowForId(id));
    if (request->cancelled) {
        if (row.status == TransferStatus::Completed || row.status == TransferStatus::Cancelled || row.status == TransferStatus::Failed)
            m_requests.remove(request->key);
        return;
    }
    if (row.status == TransferStatus::Failed || row.status == TransferStatus::Cancelled) {
        fail(request, row.message.isEmpty() ? QStringLiteral("传输已取消。") : row.message);
        return;
    }
    if (row.status == TransferStatus::Completed) {
        if (request->cancelled || !QFileInfo(request->localPath).isFile() ||
            QFileInfo(request->localPath).size() < request->file.size) {
            fail(request, QStringLiteral("下载文件不完整。"));
            return;
        }
        request->verifying = true;
        request->requestId = m_nextRequestId++;
        m_files->statFile(request->file.path, request->requestId);
        return;
    }
    if (request->dialog) {
        request->dialog->setLabelText(QStringLiteral("%1\n%2 / %3\n%4").arg(request->file.name,
            path::formatSize(row.bytesDone), path::formatSize(row.bytesTotal), row.message));
        request->dialog->setRange(0, row.bytesTotal > 0 ? 1000 : 0);
        if (row.bytesTotal > 0)
            request->dialog->setValue(static_cast<int>(row.progress() * 1000));
    }
}

void RemoteFileOpener::fail(const std::shared_ptr<Request> &request, const QString &message)
{
    m_requests.remove(request->key);
    if (request->dialog) {
        disconnect(request->dialog, nullptr, this, nullptr);
        request->dialog->setRange(0, 1);
        request->dialog->setValue(0);
        request->dialog->setLabelText(message);
        request->dialog->setCancelButtonText(QStringLiteral("关闭"));
    }
    emit statusMessage(message);
}

void RemoteFileOpener::openLocal(const QString &localPath, const QString &remotePath, bool readOnly)
{
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit statusMessage(QStringLiteral("无法读取下载文件：%1").arg(file.errorString()));
        return;
    }
    const QByteArray sample = file.peek(65536);
    if (isText(sample)) {
        if (file.size() > 32 * 1024 * 1024) {
            QMessageBox::information(m_parent, QStringLiteral("文件过大"),
                QStringLiteral("内置编辑器支持不超过 32 MB 的文本。文件已下载到：\n%1").arg(localPath));
            return;
        }
        const QByteArray data = file.readAll();
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder.decode(data);
        if (decoder.hasError() || decoded.contains(QChar::Null)) {
            QMessageBox::information(m_parent, QStringLiteral("无法编辑"),
                QStringLiteral("该文件不是有效的 UTF-8 文本。文件已下载到：\n%1").arg(localPath));
            return;
        }
        emit textFileReady(remotePath, data, readOnly);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(localPath))) {
        const QString message = QStringLiteral("没有可用的程序打开此文件。文件已下载到：\n%1").arg(localPath);
        QMessageBox::warning(m_parent, QStringLiteral("无法打开"), message);
        emit statusMessage(message);
    }
}

void RemoteFileOpener::cleanup()
{
    const QDir root(AppPaths::downloadCacheDir());
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-Settings::instance().cacheRetentionDays);
    const QRegularExpression hash(QStringLiteral("^[a-f0-9]{64}$"));
    for (const QFileInfo &entry : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.isSymLink() || !hash.match(entry.fileName()).hasMatch())
            continue;
        bool active = false;
        for (const auto &request : m_requests)
            active |= request->directory == entry.absoluteFilePath();
        if (active)
            continue;
        const auto record = readRecord(entry.absoluteFilePath());
        const auto used = QDateTime::fromString(record.value(QStringLiteral("lastUsed")).toString(), Qt::ISODateWithMs);
        if (record.value(QStringLiteral("cache")).toString() == QLatin1String("winscp-remote-open") &&
            used.isValid() && used < cutoff) {
            const QDir directory(entry.absoluteFilePath());
            if (QFileInfo(directory.filePath(QStringLiteral("content"))).isSymLink())
                continue;
            // Keep the record if a file is in use so cleanup can retry later.
            if (QDir(directory.filePath(QStringLiteral("content"))).removeRecursively()) {
                QFile::remove(directory.filePath(QStringLiteral("metadata.json")));
                root.rmdir(entry.fileName());
            }
        }
    }
}

}
