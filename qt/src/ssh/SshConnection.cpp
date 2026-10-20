#include "ssh/SshConnection.h"

#include "core/AppPaths.h"
#include "core/KnownHosts.h"
#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>

#include <libssh2.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

#ifdef max
#  undef max
#endif
#ifdef min
#  undef min
#endif

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_type = SOCKET;
#  define WSC_INVALID_SOCKET INVALID_SOCKET
#  define WSC_SOCKET_ERROR SOCKET_ERROR
#  define wscCloseSocket closesocket
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_type = int;
#  define WSC_INVALID_SOCKET (-1)
#  define WSC_SOCKET_ERROR (-1)
#  define wscCloseSocket close
#endif

namespace wsc {

namespace {

std::once_flag g_sshInitFlag;

void initializeSsh()
{
    std::call_once(g_sshInitFlag, [] {
#ifdef Q_OS_WIN
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        libssh2_init(0);
    });
}

QString sshErrorText(LIBSSH2_SESSION *session, const QString &fallback)
{
    if (!session)
        return fallback;
    char *message = nullptr;
    int length = 0;
    const int code = libssh2_session_last_error(session, &message, &length, 0);
    QString detail;
    if (message && length > 0)
        detail = QString::fromUtf8(message, length).trimmed();
    QString translated;
    switch (code) {
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
        translated = QStringLiteral("连接超时。");
        break;
    case LIBSSH2_ERROR_AUTHENTICATION_FAILED:
        translated = QStringLiteral("认证失败：用户名、密码或私钥不正确。");
        break;
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
        translated = QStringLiteral("服务器断开了连接。");
        break;
    case LIBSSH2_ERROR_KEX_FAILURE:
        translated = QStringLiteral("SSH 密钥交换失败。");
        break;
    case LIBSSH2_ERROR_BANNER_RECV:
    case LIBSSH2_ERROR_BANNER_SEND:
        translated = QStringLiteral("无法读取 SSH 服务器标识，端口可能不是 SSH。");
        break;
    case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED:
        translated = QStringLiteral("私钥格式不受支持或无法验证。");
        break;
    case LIBSSH2_ERROR_METHOD_NOT_SUPPORTED:
        translated = QStringLiteral("服务器不支持所选的认证方式。");
        break;
    case LIBSSH2_ERROR_ALLOC:
        translated = QStringLiteral("内存分配失败。");
        break;
    case LIBSSH2_ERROR_CHANNEL_FAILURE:
        translated = QStringLiteral("SSH 通道打开失败。");
        break;
    default:
        break;
    }
    if (translated.isEmpty())
        translated = fallback;
    if (!detail.isEmpty() && detail != translated)
        translated += QStringLiteral("（%1）").arg(detail);
    return translated;
}

QString hostKeyAlgorithm(int type)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        return QStringLiteral("ssh-rsa");
    case LIBSSH2_HOSTKEY_TYPE_DSS:
        return QStringLiteral("ssh-dss");
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        return QStringLiteral("ecdsa-sha2-nistp256");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        return QStringLiteral("ecdsa-sha2-nistp384");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        return QStringLiteral("ecdsa-sha2-nistp521");
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        return QStringLiteral("ssh-ed25519");
#endif
    default:
        return QStringLiteral("unknown");
    }
}

QString fingerprintFromSession(LIBSSH2_SESSION *session)
{
    const char *hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash)
        return {};
    QByteArray digest(hash, 32);
    QString base64 = QString::fromLatin1(digest.toBase64());
    base64.remove(QLatin1Char('='));
    return QStringLiteral("SHA256:") + base64;
}

HostKeyVerifier defaultVerifier()
{
    return [](const HostKeyInfo &info, QString *error) {
        KnownHostKey existing;
        const HostKeyStatus status =
            KnownHosts::instance().verify(info.host, info.port, info.algorithm, info.fingerprint,
                                          &existing, error);
        if (status == HostKeyStatus::Match)
            return true;
        if (status == HostKeyStatus::Unknown) {
            Logger::instance().warning(
                QStringLiteral("首次连接 %1:%2，主机密钥为 %3。已自动信任并保存。")
                    .arg(info.host)
                    .arg(info.port)
                    .arg(info.fingerprint));
            return KnownHosts::instance().addOrUpdate(info.host, info.port, info.algorithm,
                                                      info.fingerprint, error);
        }
        if (status == HostKeyStatus::Error) {
            if (error)
                *error = QStringLiteral("无法校验主机密钥。");
        }
        return false;
    };
}

QString derivePublicKey(const QString &privateKeyPath, const QString &passphrase, QString *error)
{
    QString executable = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
    if (executable.isEmpty()) {
#ifdef Q_OS_WIN
        const QString systemPath =
            QDir(QStringLiteral("C:/Windows/System32/OpenSSH")).filePath(QStringLiteral("ssh-keygen.exe"));
        if (QFileInfo::exists(systemPath))
            executable = systemPath;
#endif
    }
    if (executable.isEmpty())
        return {};

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(executable, {QStringLiteral("-y"), QStringLiteral("-f"), privateKeyPath});
    if (!process.waitForStarted(5000))
        return {};
    if (!passphrase.isEmpty())
        process.write(passphrase.toUtf8() + '\n');
    process.closeWriteChannel();
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return {};
    }
    const QByteArray output = process.readAllStandardOutput().trimmed();
    if (output.isEmpty() || !output.startsWith("ssh-") && !output.startsWith("ecdsa-") &&
                                !output.startsWith("sk-"))
        return {};
    AppPaths::ensureDirectories();
    const QString path = QDir(AppPaths::tempDir())
                             .filePath(QStringLiteral("key-%1.pub")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    file.write(output + '\n');
    file.close();
    return path;
}

} // namespace

SshConnection::SshConnection()
{
    initializeSsh();
}

SshConnection::~SshConnection()
{
    disconnect();
}

bool SshConnection::fail(const QString &message, QString *error)
{
    if (error)
        *error = message;
    return false;
}

bool SshConnection::connectToHost(const QString &host, int port, QString *error)
{
    initializeSsh();
    disconnect();
    m_timeoutSeconds = std::max(1, m_timeoutSeconds);

    const QByteArray hostUtf8 = host.trimmed().toUtf8();
    const QByteArray portText = QByteArray::number(port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *result = nullptr;
    const int gai = getaddrinfo(hostUtf8.constData(), portText.constData(), &hints, &result);
    if (gai != 0) {
#ifdef Q_OS_WIN
        return fail(QStringLiteral("无法解析主机名 %1（WSA 错误 %2）。").arg(host).arg(WSAGetLastError()),
                    error);
#else
        return fail(QStringLiteral("无法解析主机名 %1：%2").arg(host, QString::fromUtf8(gai_strerror(gai))),
                    error);
#endif
    }

    socket_type socketHandle = WSC_INVALID_SOCKET;
    QString lastConnectError;
    for (addrinfo *address = result; address; address = address->ai_next) {
        socketHandle = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketHandle == WSC_INVALID_SOCKET)
            continue;

#ifdef Q_OS_WIN
        u_long nonBlocking = 1;
        ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
#else
        const int flags = fcntl(socketHandle, F_GETFL, 0);
        fcntl(socketHandle, F_SETFL, flags | O_NONBLOCK);
#endif
        const int connectResult =
            ::connect(socketHandle, address->ai_addr, static_cast<int>(address->ai_addrlen));
        bool connected = connectResult == 0;
        if (!connected) {
#ifdef Q_OS_WIN
            const int wsaError = WSAGetLastError();
            const bool inProgress = wsaError == WSAEWOULDBLOCK || wsaError == WSAEINPROGRESS;
#else
            const bool inProgress = errno == EINPROGRESS;
#endif
            if (inProgress) {
                fd_set writeSet;
                FD_ZERO(&writeSet);
                FD_SET(socketHandle, &writeSet);
                timeval timeout{m_timeoutSeconds, 0};
#ifdef Q_OS_WIN
                const int selected = ::select(0, nullptr, &writeSet, nullptr, &timeout);
#else
                const int selected = ::select(socketHandle + 1, nullptr, &writeSet, nullptr, &timeout);
#endif
                if (selected > 0) {
                    int socketError = 0;
#ifdef Q_OS_WIN
                    int length = sizeof(socketError);
                    getsockopt(socketHandle, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char *>(&socketError), &length);
#else
                    socklen_t length = sizeof(socketError);
                    getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, &socketError, &length);
#endif
                    connected = socketError == 0;
                    if (!connected)
                        lastConnectError = QStringLiteral("连接被拒绝（错误 %1）。").arg(socketError);
                } else if (selected == 0) {
                    lastConnectError = QStringLiteral("连接超时（%1 秒）。").arg(m_timeoutSeconds);
                } else {
                    lastConnectError = QStringLiteral("连接失败。");
                }
            } else {
                lastConnectError = QStringLiteral("无法连接到 %1:%2。").arg(host).arg(port);
            }
        }
        if (connected) {
            int one = 1;
            setsockopt(socketHandle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one),
                       sizeof(one));
            setsockopt(socketHandle, SOL_SOCKET, SO_KEEPALIVE,
                       reinterpret_cast<const char *>(&one), sizeof(one));
            break;
        }
        wscCloseSocket(socketHandle);
        socketHandle = WSC_INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if (socketHandle == WSC_INVALID_SOCKET) {
        if (lastConnectError.isEmpty())
            lastConnectError = QStringLiteral("无法连接到 %1:%2。").arg(host).arg(port);
        return fail(lastConnectError, error);
    }

    m_socket = static_cast<qintptr>(socketHandle);
    m_session = libssh2_session_init();
    if (!m_session) {
        wscCloseSocket(socketHandle);
        m_socket = -1;
        return fail(QStringLiteral("无法初始化 SSH 会话。"), error);
    }
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, m_timeoutSeconds * 1000);
    libssh2_session_banner_set(m_session, "SSH-2.0-WinSCPQt_1.0");

    const int handshake =
        libssh2_session_handshake(m_session, static_cast<libssh2_socket_t>(socketHandle));
    if (handshake != 0) {
        const QString message = sshErrorText(
            m_session, QStringLiteral("SSH 握手失败（错误 %1）。").arg(handshake));
        disconnect();
        return fail(message, error);
    }

    if (const char *banner = libssh2_session_banner_get(m_session))
        m_banner = QString::fromUtf8(banner).trimmed();

    const char *hostKeyData = nullptr;
    size_t hostKeyLength = 0;
    int hostKeyType = 0;
    hostKeyData = libssh2_session_hostkey(m_session, &hostKeyLength, &hostKeyType);
    if (!hostKeyData) {
        const QString message = sshErrorText(m_session, QStringLiteral("无法获取服务器主机密钥。"));
        disconnect();
        return fail(message, error);
    }
    m_hostKey.host = host;
    m_hostKey.port = port;
    m_hostKey.algorithm = hostKeyAlgorithm(hostKeyType);
    m_hostKey.fingerprint = fingerprintFromSession(m_session);
    if (m_hostKey.fingerprint.isEmpty()) {
        disconnect();
        return fail(QStringLiteral("无法计算服务器主机密钥指纹。"), error);
    }

    const HostKeyVerifier verifier = m_verifier ? m_verifier : defaultVerifier();
    QString verifyError;
    if (!verifier(m_hostKey, &verifyError)) {
        if (verifyError.isEmpty())
            verifyError = QStringLiteral("主机密钥验证失败，连接已取消。");
        disconnect();
        return fail(verifyError, error);
    }

    Logger::instance().info(QStringLiteral("已连接到 %1:%2（%3，%4）")
                                .arg(host)
                                .arg(port)
                                .arg(m_hostKey.algorithm, m_hostKey.fingerprint));
    m_connected = true;
    if (error)
        error->clear();
    return true;
}

bool SshConnection::authenticatePassword(const QString &user, const QString &password,
                                         QString *error)
{
    if (!m_session)
        return fail(QStringLiteral("尚未建立 SSH 连接。"), error);
    if (user.isEmpty())
        return fail(QStringLiteral("用户名不能为空。"), error);
    if (password.isEmpty())
        return fail(QStringLiteral("密码不能为空。"), error);
    const QByteArray userUtf8 = user.toUtf8();
    const QByteArray passwordUtf8 = password.toUtf8();
    const int result = libssh2_userauth_password_ex(
        m_session, userUtf8.constData(), static_cast<unsigned int>(userUtf8.size()),
        passwordUtf8.constData(), static_cast<unsigned int>(passwordUtf8.size()), nullptr);
    if (result != 0)
        return fail(sshErrorText(m_session, QStringLiteral("密码认证失败。")), error);
    if (!isAuthenticated())
        return fail(QStringLiteral("服务器未确认认证结果。"), error);
    if (error)
        error->clear();
    return true;
}

bool SshConnection::authenticateKeyFile(const QString &user, const QString &privateKeyPath,
                                        const QString &passphrase, QString *error)
{
    if (!m_session)
        return fail(QStringLiteral("尚未建立 SSH 连接。"), error);
    if (user.isEmpty())
        return fail(QStringLiteral("用户名不能为空。"), error);
    if (!QFileInfo::exists(privateKeyPath))
        return fail(QStringLiteral("私钥文件不存在：%1").arg(privateKeyPath), error);

    QString publicKeyPath = privateKeyPath + QStringLiteral(".pub");
    QString derivedPath;
    if (!QFileInfo::exists(publicKeyPath)) {
        QString deriveError;
        derivedPath = derivePublicKey(privateKeyPath, passphrase, &deriveError);
        if (!derivedPath.isEmpty())
            publicKeyPath = derivedPath;
        else
            publicKeyPath.clear();
    }

    const QByteArray userUtf8 = user.toUtf8();
    const QByteArray privateUtf8 = QFile::encodeName(privateKeyPath);
    const QByteArray publicUtf8 = publicKeyPath.isEmpty() ? QByteArray() : QFile::encodeName(publicKeyPath);
    const QByteArray passphraseUtf8 = passphrase.toUtf8();
    const int result = libssh2_userauth_publickey_fromfile_ex(
        m_session, userUtf8.constData(), static_cast<unsigned int>(userUtf8.size()),
        publicKeyPath.isEmpty() ? nullptr : publicUtf8.constData(), privateUtf8.constData(),
        passphraseUtf8.isEmpty() ? nullptr : passphraseUtf8.constData());
    if (!derivedPath.isEmpty())
        QFile::remove(derivedPath);
    if (result != 0)
        return fail(sshErrorText(m_session, QStringLiteral("私钥认证失败。")), error);
    if (!isAuthenticated())
        return fail(QStringLiteral("服务器未确认认证结果。"), error);
    if (error)
        error->clear();
    return true;
}

namespace {
struct KeyboardInteractiveContext {
    QByteArray password;
};

void keyboardInteractiveCallback(const char *, int, const char *, int, int promptCount,
                                 const LIBSSH2_USERAUTH_KBDINT_PROMPT *, LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                                 void **abstract)
{
    auto *context = abstract ? static_cast<KeyboardInteractiveContext *>(*abstract) : nullptr;
    if (!context)
        return;
    for (int index = 0; index < promptCount; ++index) {
        responses[index].text = static_cast<char *>(malloc(context->password.size() + 1));
        if (responses[index].text) {
            memcpy(responses[index].text, context->password.constData(), context->password.size());
            responses[index].text[context->password.size()] = '\0';
            responses[index].length = static_cast<unsigned int>(context->password.size());
        }
    }
}
} // namespace

bool SshConnection::authenticateKeyboardInteractive(const QString &user, const QString &password,
                                                    QString *error)
{
    if (!m_session)
        return fail(QStringLiteral("尚未建立 SSH 连接。"), error);
    KeyboardInteractiveContext context;
    context.password = password.toUtf8();
    const QByteArray userUtf8 = user.toUtf8();
    void **abstract = libssh2_session_abstract(m_session);
    void *previous = abstract ? *abstract : nullptr;
    if (abstract)
        *abstract = &context;
    const int result = libssh2_userauth_keyboard_interactive_ex(
        m_session, userUtf8.constData(), static_cast<unsigned int>(userUtf8.size()),
        keyboardInteractiveCallback);
    if (abstract)
        *abstract = previous;
    if (result != 0)
        return fail(sshErrorText(m_session, QStringLiteral("交互式认证失败。")), error);
    if (!isAuthenticated())
        return fail(QStringLiteral("服务器未确认认证结果。"), error);
    if (error)
        error->clear();
    return true;
}

bool SshConnection::authenticateAgent(const QString &user, QString *error)
{
    if (!m_session)
        return fail(QStringLiteral("尚未建立 SSH 连接。"), error);
    LIBSSH2_AGENT *agent = libssh2_agent_init(m_session);
    if (!agent)
        return fail(sshErrorText(m_session, QStringLiteral("无法初始化 SSH 代理。")), error);
    bool success = false;
    if (libssh2_agent_connect(agent) == 0 &&
        libssh2_agent_list_identities(agent) == 0) {
        struct libssh2_agent_publickey *identity = nullptr;
        struct libssh2_agent_publickey *previous = nullptr;
        while (true) {
            const int next = libssh2_agent_get_identity(agent, &identity, previous);
            if (next != 0)
                break;
            const QByteArray userUtf8 = user.toUtf8();
            if (libssh2_agent_userauth(agent, userUtf8.constData(), identity) == 0) {
                success = true;
                break;
            }
            previous = identity;
        }
    }
    const QString agentError = sshErrorText(m_session, QStringLiteral("SSH 代理认证失败。"));
    libssh2_agent_disconnect(agent);
    libssh2_agent_free(agent);
    if (!success)
        return fail(agentError, error);
    if (!isAuthenticated())
        return fail(QStringLiteral("服务器未确认认证结果。"), error);
    if (error)
        error->clear();
    return true;
}

bool SshConnection::isAuthenticated() const
{
    return m_session && libssh2_userauth_authenticated(m_session) == 1;
}

QString SshConnection::lastError() const
{
    return sshErrorText(m_session, QStringLiteral("未知 SSH 错误。"));
}

bool SshConnection::waitForSocket(int timeoutMs) const
{
    if (!m_session || m_socket < 0)
        return false;
    fd_set readSet;
    fd_set writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    const socket_type socketHandle = static_cast<socket_type>(m_socket);
    FD_SET(socketHandle, &readSet);
    FD_SET(socketHandle, &writeSet);
    const int directions = libssh2_session_block_directions(m_session);
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return ::select(
#ifdef Q_OS_WIN
        0,
#else
        socketHandle + 1,
#endif
        directions & LIBSSH2_SESSION_BLOCK_INBOUND ? &readSet : nullptr,
        directions & LIBSSH2_SESSION_BLOCK_OUTBOUND ? &writeSet : nullptr,
        nullptr, &timeout) > 0;
}

void SshConnection::enableKeepAlive(int seconds)
{
    if (!m_session)
        return;
    libssh2_keepalive_config(m_session, 1, std::max(5, seconds));
}

void SshConnection::sendKeepAlive()
{
    if (!m_session)
        return;
    int secondsToNext = 0;
    const int result = libssh2_keepalive_send(m_session, &secondsToNext);
    if (result != 0 && result != LIBSSH2_ERROR_EAGAIN)
        m_connected = false;
}

void SshConnection::disconnect()
{
    m_connected = false;
    if (m_session) {
        libssh2_session_set_blocking(m_session, 0);
        libssh2_session_disconnect(m_session, "WinSCPQt closing session");
    }
    if (m_socket >= 0) {
        wscCloseSocket(static_cast<socket_type>(m_socket));
        m_socket = -1;
    }
    if (m_session) {
        // Closing the socket lets libssh2 release remaining channels without waiting for replies.
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
}

bool SshConnection::verifyHostKey(QString *error)
{
    const HostKeyVerifier verifier = m_verifier ? m_verifier : defaultVerifier();
    return verifier(m_hostKey, error);
}

} // namespace wsc
