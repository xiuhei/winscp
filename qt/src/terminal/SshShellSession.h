#pragma once

#include <QByteArray>
#include <QObject>

class QTimer;
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

namespace wsc {

class SshConnection;

// All channels of a libssh2 session run serially on the file-system worker.
class SshShellSession : public QObject {
    Q_OBJECT
public:
    SshShellSession(int columns, int rows, const QString &shell = {});
    ~SshShellSession() override;

    void start(SshConnection *connection);
    void writeData(const QByteArray &data);
    void resizePty(int columns, int rows);
    void stop();
    void setOutputBackpressure(bool enabled);
    void acknowledgeOutput(qsizetype bytes);

signals:
    void outputReady(const QByteArray &data);
    void connected(const QString &serverBanner);
    void sessionError(const QString &message);
    void sessionClosed(int exitStatus, const QString &reason);

private:
    void poll();
    void close(int exitStatus, const QString &reason);
    void fail(const QString &message);

    SshConnection *m_connection = nullptr;
    LIBSSH2_CHANNEL *m_channel = nullptr;
    QTimer *m_timer = nullptr;
    QByteArray m_pendingInput;
    qsizetype m_outputInFlight = 0;
    bool m_outputBackpressure = false;
    int m_columns;
    int m_rows;
    QString m_shell;
    bool m_resizePending = false;
    bool m_closed = false;
};

} // namespace wsc
