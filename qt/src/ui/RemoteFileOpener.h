#pragma once

#include "core/SessionData.h"
#include <QHash>
#include <QObject>
#include <memory>

class QWidget;

namespace wsc {

class FileSystemController;
class TransferManager;

class RemoteFileOpener : public QObject {
    Q_OBJECT
public:
    RemoteFileOpener(FileSystemController *files, TransferManager *transfers, QWidget *parent);
    void open(const SessionData &session, const FileInfo &file);
    void cleanup();

signals:
    void statusMessage(const QString &message);

private:
    struct Request;
    void stated(quint64 requestId, const SessionData &session, const QString &path, const FileInfo &info,
                const QString &error);
    void update(int id);
    void fail(const std::shared_ptr<Request> &request, const QString &message);
    void openLocal(const QString &path);
    quint64 m_nextRequestId = 1;
    FileSystemController *m_files;
    TransferManager *m_transfers;
    QWidget *m_parent;
    QHash<QString, std::shared_ptr<Request>> m_requests;
};

}
