#include "terminal/SshShellSession.h"

#include "ssh/SshConnection.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <libssh2.h>

#include <algorithm>

namespace wsc {

namespace {

class NonBlockingSession {
public:
    explicit NonBlockingSession(LIBSSH2_SESSION *session) : m_session(session)
    {
        libssh2_session_set_blocking(m_session, 0);
    }
    ~NonBlockingSession() { libssh2_session_set_blocking(m_session, 1); }
private:
    LIBSSH2_SESSION *m_session;
};

} // namespace

SshShellSession::SshShellSession(int columns, int rows, const QString &shell)
    : m_columns(std::max(2, columns)), m_rows(std::max(2, rows)), m_shell(shell)
{
}

SshShellSession::~SshShellSession()
{
    close(-1, QStringLiteral("终端已关闭。"));
}

void SshShellSession::start(SshConnection *connection)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_closed || m_channel)
        return;
    if (!connection || !connection->isAuthenticated()) {
        fail(QStringLiteral("当前没有可复用的 SSH 文件会话，请先连接服务器。"));
        return;
    }
    m_connection = connection;
    m_channel = libssh2_channel_open_session(connection->session());
    if (!m_channel) {
        fail(QStringLiteral("无法打开 SSH 终端通道：%1").arg(connection->lastError()));
        return;
    }
    if (libssh2_channel_request_pty_ex(m_channel, "xterm-256color", 14, nullptr, 0,
                                      m_columns, m_rows, 0, 0) != 0) {
        fail(QStringLiteral("服务器拒绝分配 PTY：%1").arg(connection->lastError()));
        return;
    }
    const QByteArray command = m_shell.toUtf8();
    const int result = command.isEmpty() ? libssh2_channel_shell(m_channel)
                                         : libssh2_channel_exec(m_channel, command.constData());
    if (result != 0) {
        fail(QStringLiteral("无法启动远程 Shell：%1").arg(connection->lastError()));
        return;
    }
    m_timer = new QTimer(this);
    m_timer->setInterval(8);
    connect(m_timer, &QTimer::timeout, this, &SshShellSession::poll);
    m_timer->start();
    emit connected(connection->serverBanner());
}

void SshShellSession::writeData(const QByteArray &data)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, data] { writeData(data); });
        return;
    }
    if (!m_closed)
        m_pendingInput.append(data);
}

void SshShellSession::resizePty(int columns, int rows)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, columns, rows] { resizePty(columns, rows); });
        return;
    }
    m_columns = std::max(2, columns);
    m_rows = std::max(2, rows);
    m_resizePending = true;
}

void SshShellSession::stop()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { stop(); }, Qt::QueuedConnection);
        return;
    }
    close(-1, QStringLiteral("终端已关闭。"));
}

void SshShellSession::setOutputBackpressure(bool enabled)
{
    QMetaObject::invokeMethod(this, [this, enabled] { m_outputBackpressure = enabled; });
}

void SshShellSession::acknowledgeOutput(qsizetype bytes)
{
    QMetaObject::invokeMethod(this, [this, bytes] {
        m_outputInFlight = std::max<qsizetype>(0, m_outputInFlight - bytes);
    });
}

void SshShellSession::poll()
{
    if (!m_channel || m_closed)
        return;
    QString error;
    bool eof = false;
    int exitStatus = -1;
    {
        NonBlockingSession nonBlocking(m_connection->session());
        char buffer[32768];
        // Bound each tick so file operations and other terminal tabs can run.
        QByteArray output;
        for (int stream = 0; stream < 2 && error.isEmpty(); ++stream) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                if (m_outputBackpressure && m_outputInFlight + output.size() >= 256 * 1024)
                    break;
                const auto read = libssh2_channel_read_ex(m_channel, stream, buffer, sizeof(buffer));
                if (read > 0)
                    output.append(buffer, static_cast<int>(read));
                else {
                    if (read < 0 && read != LIBSSH2_ERROR_EAGAIN)
                        error = QStringLiteral("终端读取失败：%1").arg(m_connection->lastError());
                    break;
                }
            }
        }
        if (!output.isEmpty()) {
            if (m_outputBackpressure)
                m_outputInFlight += output.size();
            emit outputReady(output);
        }
        eof = libssh2_channel_eof(m_channel) != 0;
        if (eof)
            exitStatus = libssh2_channel_get_exit_status(m_channel);
    }
    // Complete sends before another channel uses the session's transport buffer.
    if (error.isEmpty() && !eof && !m_pendingInput.isEmpty()) {
        const auto written = libssh2_channel_write(m_channel, m_pendingInput.constData(),
                                                    std::min<qsizetype>(16384, m_pendingInput.size()));
        if (written > 0)
            m_pendingInput.remove(0, written);
        else if (written < 0)
            error = QStringLiteral("终端写入失败：%1").arg(m_connection->lastError());
    }
    if (error.isEmpty() && !eof && m_resizePending) {
        if (libssh2_channel_request_pty_size(m_channel, m_columns, m_rows) == 0)
            m_resizePending = false;
        else
            error = QStringLiteral("无法调整 PTY 尺寸：%1").arg(m_connection->lastError());
    }
    if (!error.isEmpty())
        fail(error);
    else if (eof)
        close(exitStatus, QStringLiteral("远程 Shell 已退出。"));
}

void SshShellSession::fail(const QString &message)
{
    emit sessionError(message);
    close(-1, message);
}

void SshShellSession::close(int exitStatus, const QString &reason)
{
    if (m_closed)
        return;
    m_closed = true;
    if (m_timer)
        m_timer->stop();
    if (m_channel) {
        const long timeout = libssh2_session_get_timeout(m_connection->session());
        libssh2_session_set_timeout(m_connection->session(), 1000);
        libssh2_channel_free(m_channel);
        libssh2_session_set_timeout(m_connection->session(), timeout);
        m_channel = nullptr;
    }
    m_pendingInput.clear();
    m_connection = nullptr;
    emit sessionClosed(exitStatus, reason);
}

} // namespace wsc
