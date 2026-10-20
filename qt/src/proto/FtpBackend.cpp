#include "proto/FtpBackend.h"

#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/PathUtils.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QSslCipher>
#include <QSslError>
#include <QSslSocket>
#include <QTemporaryFile>
#include <QThread>
#include <QTimeZone>

#include <algorithm>

namespace wsc {

namespace {

QString parsePasvHost(const QString &reply, int *port)
{
    static const QRegularExpression re(QStringLiteral("\\((\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+)\\)"));
    const QRegularExpressionMatch match = re.match(reply);
    if (!match.hasMatch()) {
        // Some servers omit parentheses.
        static const QRegularExpression loose(
            QStringLiteral("(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+)"));
        const QRegularExpressionMatch looseMatch = loose.match(reply);
        if (!looseMatch.hasMatch())
            return {};
        const int p1 = looseMatch.captured(5).toInt();
        const int p2 = looseMatch.captured(6).toInt();
        if (port)
            *port = p1 * 256 + p2;
        return QStringLiteral("%1.%2.%3.%4").arg(looseMatch.captured(1), looseMatch.captured(2),
                                                  looseMatch.captured(3), looseMatch.captured(4));
    }
    const int p1 = match.captured(5).toInt();
    const int p2 = match.captured(6).toInt();
    if (port)
        *port = p1 * 256 + p2;
    return QStringLiteral("%1.%2.%3.%4")
        .arg(match.captured(1), match.captured(2), match.captured(3), match.captured(4));
}

int parseEpsvPort(const QString &reply)
{
    static const QRegularExpression re(QStringLiteral("\\(\\|\\|\\|(\\d+)\\|"));
    const QRegularExpressionMatch match = re.match(reply);
    if (!match.hasMatch())
        return 0;
    return match.captured(1).toInt();
}

QDateTime parseFtpTimestamp(const QString &value)
{
    QString text = value.trimmed();
    if (text.size() < 14)
        return {};
    text = text.left(14);
    QDateTime utc = QDateTime::fromString(text, QStringLiteral("yyyyMMddHHmmss"));
    if (!utc.isValid())
        return {};
    utc.setTimeZone(QTimeZone::UTC);
    return utc.toTimeZone(QTimeZone::systemTimeZone());
}

quint32 parseModeString(const QString &permissions)
{
    if (permissions.size() < 10)
        return 0;
    quint32 mode = 0;
    switch (permissions.at(0).toLatin1()) {
    case 'd':
        mode |= 0040000;
        break;
    case 'l':
        mode |= 0120000;
        break;
    case 'b':
        mode |= 0060000;
        break;
    case 'c':
        mode |= 0020000;
        break;
    case 'p':
        mode |= 0010000;
        break;
    case 's':
        mode |= 0140000;
        break;
    default:
        mode |= 0100000;
        break;
    }
    const int shifts[9] = {8, 7, 6, 5, 4, 3, 2, 1, 0};
    const char characters[9] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
    for (int index = 0; index < 9; ++index) {
        const QChar character = permissions.at(index + 1);
        if (character == QLatin1Char(characters[index]) || character == QLatin1Char('s') ||
            character == QLatin1Char('t'))
            mode |= (1u << shifts[index]);
    }
    if (permissions.at(3) == QLatin1Char('s') || permissions.at(3) == QLatin1Char('S'))
        mode |= 04000;
    if (permissions.at(6) == QLatin1Char('s') || permissions.at(6) == QLatin1Char('S'))
        mode |= 02000;
    if (permissions.at(9) == QLatin1Char('t') || permissions.at(9) == QLatin1Char('T'))
        mode |= 01000;
    return mode;
}

QString modeToSiteChmod(quint32 mode)
{
    return QString::number(mode & 07777, 8).rightJustified(4, QLatin1Char('0'));
}

} // namespace

FtpBackend::FtpBackend(Protocol protocol)
    : m_protocol(protocol)
{
}

FtpBackend::~FtpBackend()
{
    disconnect();
}

bool FtpBackend::isConnected() const
{
    return m_connected && m_control && m_control->state() == QAbstractSocket::ConnectedState;
}

QString FtpBackend::quote(const QString &path) const
{
    QString result = path;
    result.remove(QLatin1Char('\r'));
    result.remove(QLatin1Char('\n'));
    return result;
}

bool FtpBackend::connect(const SessionData &session, QString *error)
{
    disconnect();
    m_protocol = session.protocol;
    m_host = session.hostName;
    m_port = session.port > 0 ? session.port : defaultPort(session.protocol);
    m_user = session.userName;
    m_verifyTls = session.tlsVerifyCertificate;
    m_useMlsd = false;
    m_useMlst = false;
    m_useEpsv = false;
    m_useUtf8 = false;
    m_mlsdFailed = false;
    m_timeoutMs = std::max(5, session.timeoutSeconds) * 1000;
    m_dataTimeoutMs = std::max(30, session.timeoutSeconds) * 1000;
    m_tlsErrors.clear();

    if (m_protocol == Protocol::Ftps && !QSslSocket::supportsSsl()) {
        if (error)
            *error = QStringLiteral("当前 Qt 构建不支持 TLS/SSL，无法使用 FTPS。");
        return false;
    }

    m_control = new QSslSocket();
    m_control->setPeerVerifyName(m_host);
    QSslConfiguration controlConfiguration = QSslConfiguration::defaultConfiguration();
    controlConfiguration.setSslOption(QSsl::SslOptionDisableSessionTickets, false);
    controlConfiguration.setSslOption(QSsl::SslOptionDisableSessionSharing, false);
    controlConfiguration.setSslOption(QSsl::SslOptionDisableSessionPersistence, false);
    if (!m_verifyTls)
        controlConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);
    m_control->setSslConfiguration(controlConfiguration);
    QObject::connect(m_control, &QSslSocket::sslErrors, m_control,
                     [this](const QList<QSslError> &errors) {
        for (const QSslError &sslError : errors)
            m_tlsErrors.append(sslError.errorString());
        if (!m_verifyTls)
            m_control->ignoreSslErrors();
    });
    QObject::connect(m_control, &QSslSocket::newSessionTicketReceived, m_control, [this] {
        m_sessionTicket = m_control->sslConfiguration().sessionTicket();
    });
    m_control->connectToHost(m_host, static_cast<quint16>(m_port));
    if (!m_control->waitForConnected(m_timeoutMs)) {
        if (error)
            *error = QStringLiteral("无法连接到 %1:%2：%3")
                         .arg(m_host)
                         .arg(m_port)
                         .arg(m_control->errorString());
        disconnect();
        return false;
    }

    Reply greeting;
    if (m_protocol == Protocol::Ftps && session.implicitTls) {
        if (!upgradeToTls(true, error)) {
            disconnect();
            return false;
        }
    }
    if (!readReply(&greeting, error)) {
        disconnect();
        return false;
    }
    if (greeting.code != 220 && greeting.code != 120) {
        if (error)
            *error = errorForReply(greeting, QStringLiteral("服务器拒绝连接"));
        disconnect();
        return false;
    }
    if (m_protocol == Protocol::Ftps && !session.implicitTls) {
        if (!upgradeToTls(false, error)) {
            disconnect();
            return false;
        }
    }

    if (!login(session, error)) {
        disconnect();
        return false;
    }
    m_connected = true;
    Logger::instance().success(QStringLiteral("%1 会话已建立：%2（主目录 %3）")
                                   .arg(backendName(), session.endpoint(), m_home));
    return true;
}

bool FtpBackend::openControl(QString *error)
{
    Q_UNUSED(error);
    return m_control != nullptr;
}

bool FtpBackend::upgradeToTls(bool implicit, QString *error)
{
    if (!m_control) {
        if (error)
            *error = QStringLiteral("FTP 控制连接未建立。");
        return false;
    }
    m_tlsErrors.clear();
    if (!implicit) {
        Reply reply;
        if (!expect(QStringLiteral("AUTH TLS"), 234, &reply, error)) {
            if (error)
                *error = QStringLiteral("服务器不支持显式 FTPS：%1").arg(errorForReply(reply, QString()));
            return false;
        }
    }
    m_control->startClientEncryption();
    if (!m_control->waitForEncrypted(m_timeoutMs)) {
        if (error) {
            *error = QStringLiteral("TLS 握手失败：%1").arg(m_control->errorString());
            if (!m_tlsErrors.isEmpty())
                *error += QStringLiteral("\n%1").arg(m_tlsErrors.join(QLatin1Char('\n')));
        }
        return false;
    }
    Reply reply;
    if (!expect(QStringLiteral("PBSZ 0"), 200, &reply, error))
        return false;
    if (!expect(QStringLiteral("PROT P"), 200, &reply, error))
        return false;
    m_secure = true;
    m_sessionTicket = m_control->sslConfiguration().sessionTicket();
    if (m_sessionTicket.isEmpty()) {
        // TLS 1.3 session tickets are delivered just after the handshake.
        QElapsedTimer timer;
        timer.start();
        while (m_sessionTicket.isEmpty() && timer.elapsed() < 1200)
            m_control->waitForReadyRead(100);
        m_sessionTicket = m_control->sslConfiguration().sessionTicket();
    }
    Logger::instance().info(QStringLiteral("FTPS 控制连接已加密：%1（会话票据 %2 字节，%3，协议 %4）")
                                .arg(m_host)
                                .arg(m_sessionTicket.size())
                                .arg(QSslSocket::sslLibraryVersionString())
                                .arg(static_cast<int>(m_control->sessionProtocol())));
    return true;
}

bool FtpBackend::login(const SessionData &session, QString *error)
{
    QString user = session.userName;
    QString password = session.password;
    if (user.isEmpty()) {
        user = QStringLiteral("anonymous");
        if (password.isEmpty())
            password = QStringLiteral("anonymous@");
    }
    Reply reply;
    if (!sendCommand(QStringLiteral("USER %1").arg(quote(user)), &reply, error))
        return false;
    if (reply.code == 230) {
        // Logged in without a password (anonymous).
    } else if (reply.code == 331) {
        if (!sendCommand(QStringLiteral("PASS %1").arg(quote(password)), &reply, error))
            return false;
        if (reply.code != 230 && reply.code != 202) {
            if (error)
                *error = errorForReply(reply, QStringLiteral("登录失败"));
            return false;
        }
    } else if (reply.code == 332) {
        if (error)
            *error = QStringLiteral("服务器要求 ACCT 账户信息，当前客户端不支持。");
        return false;
    } else {
        if (error)
            *error = errorForReply(reply, QStringLiteral("登录失败"));
        return false;
    }

    if (!expect(QStringLiteral("TYPE I"), 200, &reply, error))
        return false;

    if (!sendCommand(QStringLiteral("FEAT"), &reply, error)) {
        if (error)
            error->clear();
    } else if (reply.code == 211) {
        for (const QString &line : reply.lines) {
            const QString upper = line.toUpper();
            if (upper.contains(QStringLiteral("MLSD")))
                m_useMlsd = true;
            if (upper.contains(QStringLiteral("MLST")))
                m_useMlst = true;
            if (upper.contains(QStringLiteral("EPSV")))
                m_useEpsv = true;
            if (upper.contains(QStringLiteral("UTF8")))
                m_useUtf8 = true;
        }
    }
    if (m_useUtf8) {
        Reply utf8Reply;
        if (sendCommand(QStringLiteral("OPTS UTF8 ON"), &utf8Reply, error)) {
            // Best effort only.
        }
    }

    if (!sendCommand(QStringLiteral("PWD"), &reply, error))
        return false;
    if (reply.code == 257) {
        const QString text = reply.lines.value(0);
        const int firstQuote = text.indexOf(QLatin1Char('"'));
        const int lastQuote = text.lastIndexOf(QLatin1Char('"'));
        if (firstQuote >= 0 && lastQuote > firstQuote)
            m_home = path::normalizeRemote(text.mid(firstQuote + 1, lastQuote - firstQuote - 1));
        else {
            QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() >= 2)
                m_home = path::normalizeRemote(parts.at(1));
        }
    }
    if (m_home.isEmpty())
        m_home = QStringLiteral("/");
    m_cwd = m_home;

    Logger::instance().info(QStringLiteral("FTP 登录成功：%1，特性：MLSD=%2 MLST=%3 EPSV=%4 UTF8=%5")
                               .arg(m_user)
                               .arg(m_useMlsd)
                               .arg(m_useMlst)
                               .arg(m_useEpsv)
                               .arg(m_useUtf8));
    return true;
}

bool FtpBackend::readReply(Reply *reply, QString *error)
{
    if (!reply)
        return false;
    reply->code = 0;
    reply->lines.clear();
    if (!m_control || m_control->state() != QAbstractSocket::ConnectedState) {
        if (error)
            *error = QStringLiteral("FTP 控制连接已断开。");
        return false;
    }

    auto readLine = [this](QByteArray *line, QString *readError) -> bool {
        while (!m_control->canReadLine()) {
            if (m_control->state() != QAbstractSocket::ConnectedState) {
                if (readError)
                    *readError = QStringLiteral("FTP 服务器关闭了连接。");
                return false;
            }
            if (!m_control->waitForReadyRead(m_timeoutMs)) {
                if (m_control->state() != QAbstractSocket::ConnectedState) {
                    if (readError)
                        *readError = QStringLiteral("FTP 服务器关闭了连接。");
                } else if (readError) {
                    *readError = QStringLiteral("等待 FTP 响应超时。");
                }
                return false;
            }
        }
        *line = m_control->readLine();
        while (line->endsWith('\n') || line->endsWith('\r'))
            line->chop(1);
        return true;
    };

    QByteArray line;
    if (!readLine(&line, error))
        return false;
    if (line.size() < 3 || !line.left(3).toInt()) {
        if (error)
            *error = QStringLiteral("无效的 FTP 响应：%1").arg(QString::fromUtf8(line));
        return false;
    }
    reply->code = line.left(3).toInt();
    reply->lines.append(QString::fromUtf8(line.mid(4)));
    const bool multiline = line.size() > 3 && line.at(3) == '-';
    if (multiline) {
        const QByteArray terminator = line.left(3) + ' ';
        while (true) {
            if (!readLine(&line, error))
                return false;
            reply->lines.append(QString::fromUtf8(line.mid(4)));
            if (line.startsWith(terminator))
                break;
        }
    }
    if (error)
        error->clear();
    return true;
}

bool FtpBackend::sendCommand(const QString &command, Reply *reply, QString *error)
{
    if (!m_control || m_control->state() != QAbstractSocket::ConnectedState) {
        if (error)
            *error = QStringLiteral("FTP 控制连接已断开。");
        return false;
    }
    QByteArray encoded = command.toUtf8();
    encoded.replace("\r", QByteArray());
    encoded.replace("\n", QByteArray());
    encoded.append("\r\n");
    if (m_control->write(encoded) < 0) {
        if (error)
            *error = QStringLiteral("发送 FTP 命令失败：%1").arg(m_control->errorString());
        return false;
    }
    m_control->waitForBytesWritten(m_timeoutMs);
    Logger::instance().debug(QStringLiteral("FTP > %1").arg(command));
    return readReply(reply, error);
}

bool FtpBackend::expect(const QString &command, int expectedCode, Reply *reply, QString *error)
{
    Reply localReply;
    Reply *target = reply ? reply : &localReply;
    if (!sendCommand(command, target, error))
        return false;
    if (target->code != expectedCode) {
        if (error)
            *error = errorForReply(*target, QStringLiteral("命令 %1 失败").arg(command));
        return false;
    }
    return true;
}

QString FtpBackend::errorForReply(const Reply &reply, const QString &context) const
{
    QString meaning;
    switch (reply.code) {
    case 421:
        meaning = QStringLiteral("服务不可用，连接即将关闭。");
        break;
    case 425:
        meaning = QStringLiteral("无法打开数据连接。");
        break;
    case 426:
        meaning = QStringLiteral("连接已关闭，传输中止。");
        break;
    case 450:
        meaning = QStringLiteral("请求的文件操作不可用。");
        break;
    case 451:
        meaning = QStringLiteral("请求的操作被中止。");
        break;
    case 452:
        meaning = QStringLiteral("存储空间不足。");
        break;
    case 500:
    case 502:
    case 504:
        meaning = QStringLiteral("服务器不支持该命令。");
        break;
    case 530:
        meaning = QStringLiteral("登录失败或权限不足。");
        break;
    case 532:
        meaning = QStringLiteral("需要账户信息才能存储文件。");
        break;
    case 550:
        meaning = QStringLiteral("文件不可用、不存在或没有权限。");
        break;
    case 552:
        meaning = QStringLiteral("超出存储配额。");
        break;
    case 553:
        meaning = QStringLiteral("文件名不允许。");
        break;
    default:
        if (reply.code >= 400)
            meaning = QStringLiteral("FTP 错误 %1。").arg(reply.code);
        break;
    }
    QString message = context;
    if (!meaning.isEmpty()) {
        if (!message.isEmpty())
            message += QStringLiteral("：");
        message += meaning;
    }
    if (!reply.lines.isEmpty()) {
        const QString detail = reply.lines.join(QStringLiteral(" / ")).trimmed();
        if (!detail.isEmpty())
            message += QStringLiteral("\n服务器：%1").arg(detail);
    }
    return message;
}

bool FtpBackend::cwd(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    if (m_cwd == normalized)
        return true;
    Reply reply;
    if (!sendCommand(QStringLiteral("CWD %1").arg(quote(normalized)), &reply, error))
        return false;
    if (reply.code != 250) {
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法进入目录 %1").arg(normalized));
        return false;
    }
    m_cwd = normalized;
    return true;
}

QString FtpBackend::homePath()
{
    return m_home;
}

bool FtpBackend::openDataConnection(QSslSocket **dataSocket, QString *error, bool preferTls12)
{
    if (!dataSocket)
        return false;
    *dataSocket = nullptr;
    int port = 0;
    QString host;

    const bool tryEpsv = m_useEpsv || m_control->peerAddress().protocol() == QAbstractSocket::IPv6Protocol;
    if (tryEpsv) {
        Reply reply;
        if (sendCommand(QStringLiteral("EPSV"), &reply, error) && reply.code == 229) {
            port = parseEpsvPort(reply.lines.value(0));
            if (port > 0)
                host = m_control->peerAddress().toString();
        } else if (error) {
            error->clear();
        }
    }

    if (port <= 0) {
        Reply reply;
        if (!sendCommand(QStringLiteral("PASV"), &reply, error))
            return false;
        if (reply.code != 227) {
            if (error)
                *error = errorForReply(reply, QStringLiteral("无法进入被动模式"));
            return false;
        }
        int parsedPort = 0;
        host = parsePasvHost(reply.lines.join(QLatin1Char(' ')), &parsedPort);
        port = parsedPort;
        if (port <= 0) {
            if (error)
                *error = QStringLiteral("无法解析 PASV 响应：%1").arg(reply.text());
            return false;
        }
        if (host.isEmpty() || host == QLatin1String("0.0.0.0"))
            host = m_control->peerAddress().toString();
    }

    const auto connectData = [this, preferTls12](const QString &targetHost, int targetPort,
                                                 QSslSocket **socketOut,
                                                 QString *connectError) -> bool {
        auto *socket = new QSslSocket();
        socket->setPeerVerifyName(m_host);
        if (m_secure) {
            QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
            configuration.setSslOption(QSsl::SslOptionDisableSessionTickets, false);
            configuration.setSslOption(QSsl::SslOptionDisableSessionSharing, false);
            configuration.setSslOption(QSsl::SslOptionDisableSessionPersistence, false);
            // TLS 1.3 tickets provide session reuse (required by some servers)
            // for listings/downloads. Some servers' FTPS upload handlers close
            // TLS 1.3 data connections prematurely, so uploads use TLS 1.2.
            configuration.setProtocol(preferTls12 ? QSsl::TlsV1_2 : QSsl::TlsV1_2OrLater);
            if (!m_verifyTls)
                configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
            if (!m_sessionTicket.isEmpty())
                configuration.setSessionTicket(m_sessionTicket);
            socket->setSslConfiguration(configuration);
        }
        QObject::connect(socket, &QSslSocket::sslErrors, socket,
                         [this, socket](const QList<QSslError> &errors) {
                             for (const QSslError &sslError : errors)
                                 m_tlsErrors.append(sslError.errorString());
                             if (!m_verifyTls)
                                 socket->ignoreSslErrors();
                         });
        socket->connectToHost(targetHost, static_cast<quint16>(targetPort));
        if (!socket->waitForConnected(m_timeoutMs)) {
            if (connectError) {
                *connectError = QStringLiteral("无法建立 FTP 数据连接：%1").arg(socket->errorString());
            }
            delete socket;
            return false;
        }
        if (m_secure) {
            socket->startClientEncryption();
            if (!socket->waitForEncrypted(m_timeoutMs)) {
                if (connectError) {
                    *connectError = QStringLiteral("FTP 数据连接 TLS 握手失败：%1")
                                        .arg(socket->errorString());
                }
                delete socket;
                return false;
            }
            Logger::instance().debug(QStringLiteral("FTPS 数据连接已加密：%1:%2（%3）")
                                         .arg(targetHost)
                                         .arg(targetPort)
                                         .arg(socket->sessionCipher().name()));
        }
        *socketOut = socket;
        return true;
    };

    QString connectError;
    if (!connectData(host, port, dataSocket, &connectError)) {
        // NATed servers sometimes report an unusable address; retry through the
        // control connection's peer address.
        const QString peer = m_control->peerAddress().toString();
        if (!peer.isEmpty() && peer != host &&
            connectData(peer, port, dataSocket, &connectError)) {
            return true;
        }
        if (error)
            *error = connectError;
        return false;
    }
    return true;
}
void FtpBackend::closeDataConnection(QSslSocket *socket)
{
    if (!socket)
        return;
    // Drain any trailing plaintext/close-notify records so that deleting the
    // socket does not turn the TLS shutdown into a TCP reset.
    socket->readAll();
    socket->waitForReadyRead(100);
    socket->readAll();
    socket->flush();
    socket->disconnectFromHost();
    if (socket->state() != QAbstractSocket::UnconnectedState) {
        socket->waitForBytesWritten(1000);
        socket->waitForDisconnected(5000);
    }
    socket->readAll();
    delete socket;
}

bool FtpBackend::readDataAll(QSslSocket *dataSocket, QByteArray *out, QString *error)
{
    if (!dataSocket || !out)
        return false;
    out->clear();
    while (true) {
        const QByteArray chunk = dataSocket->readAll();
        if (!chunk.isEmpty()) {
            out->append(chunk);
            continue;
        }
        if (dataSocket->state() == QAbstractSocket::UnconnectedState)
            break;
        if (m_secure && !out->isEmpty()) {
            if (!dataSocket->waitForReadyRead(800))
                break;
            continue;
        }
        if (!dataSocket->waitForReadyRead(m_dataTimeoutMs)) {
            if (dataSocket->state() == QAbstractSocket::UnconnectedState)
                break;
            if (dataSocket->bytesAvailable() == 0) {
                if (dataSocket->waitForDisconnected(500))
                    break;
                if (error)
                    *error = QStringLiteral("读取 FTP 数据超时：%1").arg(dataSocket->errorString());
                return false;
            }
        }
    }
    return true;
}

bool FtpBackend::runListCommand(const QString &command, QByteArray *data, int *replyCode,
                                QString *error)
{
    if (replyCode)
        *replyCode = 0;
    QSslSocket *dataSocket = nullptr;
    if (!openDataConnection(&dataSocket, error))
        return false;
    Reply reply;
    if (!sendCommand(command, &reply, error)) {
        closeDataConnection(dataSocket);
        return false;
    }
    if (replyCode)
        *replyCode = reply.code;
    if (reply.code != 125 && reply.code != 150) {
        closeDataConnection(dataSocket);
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法列出目录"));
        return false;
    }
    if (!readDataAll(dataSocket, data, error)) {
        closeDataConnection(dataSocket);
        return false;
    }
    closeDataConnection(dataSocket);
    if (!readReply(&reply, error))
        return false;
    if (reply.code != 226 && reply.code != 250) {
        if (error)
            *error = errorForReply(reply, QStringLiteral("目录列表传输未完成"));
        return false;
    }
    return true;
}

bool FtpBackend::parseMlsdLine(const QString &line, const QString &directory, FileInfo *info)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return false;
    const int space = trimmed.indexOf(QLatin1Char(' '));
    if (space <= 0)
        return false;
    const QString factPart = trimmed.left(space);
    if (!factPart.contains(QLatin1Char('=')))
        return false;
    const QString name = trimmed.mid(space + 1);
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return false;

    FileInfo result;
    result.name = name;
    result.path = path::remoteJoin(directory, name);
    result.isHidden = name.startsWith(QLatin1Char('.'));
    QString type;
    quint32 mode = 0;
    bool haveMode = false;

    for (const QString &fact : factPart.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const int equals = fact.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = fact.left(equals).toLower();
        const QString value = fact.mid(equals + 1);
        if (key == QLatin1String("type")) {
            type = value.toLower();
        } else if (key == QLatin1String("size")) {
            result.size = value.toLongLong();
        } else if (key == QLatin1String("modify")) {
            result.modified = parseFtpTimestamp(value);
        } else if (key == QLatin1String("create")) {
            result.accessed = parseFtpTimestamp(value);
        } else if (key == QLatin1String("unix.mode")) {
            bool ok = false;
            mode = value.toUInt(&ok, 8);
            haveMode = ok;
        } else if (key == QLatin1String("unix.owner")) {
            result.owner = value;
        } else if (key == QLatin1String("unix.group")) {
            result.group = value;
        }
    }

    if (type == QLatin1String("cdir") || type == QLatin1String("pdir"))
        return false;
    result.isDir = type == QLatin1String("dir");
    result.isSymlink = type.contains(QStringLiteral("symlink")) || type == QLatin1String("link");
    if (result.isSymlink)
        result.isDir = false;
    if (haveMode) {
        mode &= 07777;
        mode |= result.isSymlink ? 0120000u : (result.isDir ? 0040000u : 0100000u);
        result.mode = mode;
        result.isExecutable = (mode & 0111) != 0;
    } else {
        result.mode = result.isSymlink ? 0120777u : (result.isDir ? 040755u : 0100644u);
        result.isExecutable = !result.isDir && !result.isSymlink && (result.mode & 0111) != 0;
    }
    if (info)
        *info = result;
    return true;
}

bool FtpBackend::parseUnixLine(const QString &line, const QString &directory, FileInfo *info)
{
    static const QRegularExpression re(
        QStringLiteral("^([bcdlps-][rwxstST-]{9}[+@.]?)\\s+\\d+\\s+(\\S+)\\s+(\\S+)\\s+(\\d+)\\s+"
                       "([A-Za-z]{3}\\s+\\d{1,2}\\s+(?:\\d{4}|\\d{1,2}:\\d{2}))\\s+(.*)$"));
    const QRegularExpressionMatch match = re.match(line);
    if (!match.hasMatch())
        return false;
    QString name = match.captured(6);
    QString linkTarget;
    const int arrow = name.indexOf(QStringLiteral(" -> "));
    if (arrow >= 0) {
        linkTarget = name.mid(arrow + 4);
        name = name.left(arrow);
    }
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return false;

    FileInfo result;
    result.name = name;
    result.path = path::remoteJoin(directory, name);
    result.mode = parseModeString(match.captured(1));
    result.isDir = match.captured(1).startsWith(QLatin1Char('d'));
    result.isSymlink = match.captured(1).startsWith(QLatin1Char('l'));
    result.isExecutable = (result.mode & 0111) != 0;
    result.isHidden = name.startsWith(QLatin1Char('.'));
    result.owner = match.captured(2);
    result.group = match.captured(3);
    result.size = match.captured(4).toLongLong();
    result.linkTarget = linkTarget;

    const QString dateText = match.captured(5).simplified();
    QDateTime parsed;
    if (dateText.contains(QLatin1Char(':'))) {
        parsed = QLocale::c().toDateTime(dateText, QStringLiteral("MMM d HH:mm"));
        if (parsed.isValid()) {
            QDate date = parsed.date();
            date.setDate(QDate::currentDate().year(), date.month(), date.day());
            if (date > QDate::currentDate().addDays(1))
                date = date.addYears(-1);
            parsed.setDate(date);
        }
    } else {
        parsed = QLocale::c().toDateTime(dateText, QStringLiteral("MMM d yyyy"));
    }
    result.modified = parsed.isValid() ? parsed : QDateTime();

    if (info)
        *info = result;
    return true;
}

bool FtpBackend::parseDosLine(const QString &line, const QString &directory, FileInfo *info)
{
    static const QRegularExpression re(
        QStringLiteral("^(\\d{2})-(\\d{2})-(\\d{2,4})\\s+(\\d{2}):(\\d{2})(AM|PM)\\s+"
                       "(<DIR>|\\d+)\\s+(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(line);
    if (!match.hasMatch())
        return false;
    const QString name = match.captured(8);
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    FileInfo result;
    result.name = name;
    result.path = path::remoteJoin(directory, name);
    result.isDir = match.captured(7).compare(QLatin1String("<DIR>"), Qt::CaseInsensitive) == 0;
    result.mode = result.isDir ? 040755u : 0100644u;
    result.isHidden = name.startsWith(QLatin1Char('.'));
    if (!result.isDir)
        result.size = match.captured(7).toLongLong();
    int hour = match.captured(4).toInt();
    const QString ampm = match.captured(6).toUpper();
    if (ampm == QLatin1String("PM") && hour < 12)
        hour += 12;
    if (ampm == QLatin1String("AM") && hour == 12)
        hour = 0;
    QString yearText = match.captured(3);
    int year = yearText.toInt();
    if (year < 100)
        year += year < 70 ? 2000 : 1900;
    result.modified = QDateTime(QDate(year, match.captured(1).toInt(), match.captured(2).toInt()),
                                QTime(hour, match.captured(5).toInt()));
    if (info)
        *info = result;
    return true;
}

bool FtpBackend::parseList(const QByteArray &data, const QString &directory, FileList *out,
                           QString *error)
{
    if (!out)
        return false;
    out->clear();
    const QString text = QString::fromUtf8(data);
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r?\n")),
                                         Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        if (error)
            error->clear();
        return true;
    }
    bool looksMlsd = false;
    for (const QString &line : lines) {
        if (line.contains(QLatin1Char('=')) &&
            line.left(line.indexOf(QLatin1Char(' '))).contains(QLatin1Char(';'))) {
            looksMlsd = true;
            break;
        }
    }
    int parsed = 0;
    for (const QString &line : lines) {
        FileInfo info;
        bool ok = false;
        if (looksMlsd)
            ok = parseMlsdLine(line, directory, &info);
        if (!ok)
            ok = parseUnixLine(line, directory, &info);
        if (!ok)
            ok = parseDosLine(line, directory, &info);
        if (ok) {
            if (info.name != QLatin1String(".") && info.name != QLatin1String(".."))
                out->append(info);
            ++parsed;
        } else if (!line.trimmed().isEmpty()) {
            Logger::instance().debug(QStringLiteral("无法解析 FTP 列表行：%1").arg(line));
        }
    }
    if (parsed == 0) {
        if (error)
            *error = QStringLiteral("无法解析服务器返回的目录列表。");
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool FtpBackend::list(const QString &pathValue, FileList *out, QString *error)
{
    if (!out)
        return false;
    const QString directory = path::normalizeRemote(pathValue);
    if (!cwd(directory, error))
        return false;

    QByteArray data;
    int replyCode = 0;
    if (m_useMlsd && !m_mlsdFailed) {
        QString mlsdError;
        if (runListCommand(QStringLiteral("MLSD"), &data, &replyCode, &mlsdError)) {
            if (parseList(data, directory, out, error))
                return true;
            Logger::instance().warning(QStringLiteral("MLSD 解析失败，改用 LIST。"));
        } else {
            if (replyCode == 500 || replyCode == 502 || replyCode == 504 || replyCode == 550 ||
                replyCode == 0) {
                Logger::instance().warning(QStringLiteral("服务器不支持 MLSD，改用 LIST：%1")
                                               .arg(mlsdError));
                m_mlsdFailed = true;
            } else {
                if (error)
                    *error = mlsdError;
                return false;
            }
        }
        data.clear();
    }

    QString listError;
    if (!runListCommand(QStringLiteral("LIST"), &data, &replyCode, &listError)) {
        if (error)
            *error = listError;
        return false;
    }
    if (!parseList(data, directory, out, error)) {
        if (error) {
            *error += QStringLiteral("\n服务器返回了 %1 行无法识别的数据。").arg(
                data.count('\n') + 1);
        }
        return false;
    }
    if (m_mlsdFailed)
        m_useMlsd = false;
    return true;
}

bool FtpBackend::stat(const QString &pathValue, FileInfo *out, QString *error)
{
    if (!out)
        return false;
    const QString normalized = path::normalizeRemote(pathValue);
    const QString parent = path::remoteParent(normalized);
    const QString name = path::remoteName(normalized);

    if (m_useMlst) {
        Reply reply;
        if (sendCommand(QStringLiteral("MLST %1").arg(quote(normalized)), &reply, error) &&
            reply.code == 250) {
            for (const QString &line : reply.lines) {
                FileInfo info;
                if (line.contains(QLatin1Char('=')) && parseMlsdLine(line.trimmed(), parent, &info)) {
                    info.path = normalized;
                    *out = info;
                    if (error)
                        error->clear();
                    return true;
                }
            }
        }
    }

    FileInfo directoryInfo;
    directoryInfo.name = name;
    directoryInfo.path = normalized;
    directoryInfo.isDir = true;
    directoryInfo.mode = 040755u;

    const QString previous = m_cwd;
    QString cwdError;
    if (cwd(normalized, &cwdError)) {
        *out = directoryInfo;
        if (!previous.isEmpty() && previous != m_cwd) {
            QString ignored;
            cwd(previous, &ignored);
        }
        if (error)
            error->clear();
        return true;
    }

    Reply sizeReply;
    if (sendCommand(QStringLiteral("SIZE %1").arg(quote(normalized)), &sizeReply, error) &&
        sizeReply.code == 213) {
        FileInfo info;
        info.name = name;
        info.path = normalized;
        info.size = sizeReply.lines.value(0).trimmed().toLongLong();
        info.mode = 0100644u;
        Reply mdtmReply;
        if (sendCommand(QStringLiteral("MDTM %1").arg(quote(normalized)), &mdtmReply, error) &&
            mdtmReply.code == 213) {
            info.modified = parseFtpTimestamp(mdtmReply.lines.value(0));
        }
        *out = info;
        if (error)
            error->clear();
        return true;
    }

    if (error) {
        *error = cwdError.isEmpty() ? QStringLiteral("无法获取 %1 的信息。").arg(normalized)
                                    : cwdError;
    }
    return false;
}

bool FtpBackend::mkdir(const QString &pathValue, QString *error)
{
    Reply reply;
    if (!sendCommand(QStringLiteral("MKD %1").arg(quote(path::normalizeRemote(pathValue))), &reply,
                     error))
        return false;
    if (reply.code != 257) {
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法创建目录"));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool FtpBackend::removeFile(const QString &pathValue, QString *error)
{
    Reply reply;
    if (!sendCommand(QStringLiteral("DELE %1").arg(quote(path::normalizeRemote(pathValue))), &reply,
                     error))
        return false;
    if (reply.code != 250) {
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法删除文件"));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool FtpBackend::removeDir(const QString &pathValue, QString *error)
{
    return removeDirRecursive(path::normalizeRemote(pathValue), 0, error);
}

bool FtpBackend::removeDirRecursive(const QString &pathValue, int depth, QString *error)
{
    if (depth > 128) {
        if (error)
            *error = QStringLiteral("目录层级过深，已中止删除。");
        return false;
    }
    FileList entries;
    if (!list(pathValue, &entries, error))
        return false;
    for (const FileInfo &entry : entries) {
        bool ok = false;
        if (entry.isDir && !entry.isSymlink)
            ok = removeDirRecursive(entry.path, depth + 1, error);
        else
            ok = removeFile(entry.path, error);
        if (!ok)
            return false;
    }
    Reply reply;
    if (!sendCommand(QStringLiteral("RMD %1").arg(quote(pathValue)), &reply, error))
        return false;
    if (reply.code != 250) {
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法删除目录"));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool FtpBackend::rename(const QString &from, const QString &to, QString *error)
{
    Reply reply;
    if (!sendCommand(QStringLiteral("RNFR %1").arg(quote(path::normalizeRemote(from))), &reply,
                     error))
        return false;
    if (reply.code != 350) {
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法重命名"));
        return false;
    }
    if (!sendCommand(QStringLiteral("RNTO %1").arg(quote(path::normalizeRemote(to))), &reply,
                     error))
        return false;
    if (reply.code != 250) {
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法重命名"));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool FtpBackend::chmod(const QString &pathValue, quint32 mode, QString *error)
{
    Reply reply;
    if (!sendCommand(QStringLiteral("SITE CHMOD %1 %2")
                         .arg(modeToSiteChmod(mode), quote(path::normalizeRemote(pathValue))),
                     &reply, error))
        return false;
    if (reply.code != 200 && reply.code != 250) {
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法修改权限"));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool FtpBackend::readFile(const QString &pathValue, QByteArray *data, QString *error)
{
    if (!data)
        return false;
    AppPaths::ensureDirectories();
    const QString tempPath = QDir(AppPaths::tempDir())
                                 .filePath(QStringLiteral("ftp-read-%1.tmp")
                                               .arg(QDateTime::currentMSecsSinceEpoch()));
    if (!downloadFile(pathValue, tempPath, -1, nullptr, nullptr, error))
        return false;
    QFile file(tempPath);
    const bool ok = file.open(QIODevice::ReadOnly);
    if (!ok) {
        if (error)
            *error = QStringLiteral("无法读取临时文件：%1").arg(file.errorString());
    } else {
        *data = file.readAll();
        file.close();
    }
    QFile::remove(tempPath);
    return ok;
}

bool FtpBackend::writeFile(const QString &pathValue, const QByteArray &data, QString *error)
{
    AppPaths::ensureDirectories();
    const QString tempPath = QDir(AppPaths::tempDir())
                                 .filePath(QStringLiteral("ftp-write-%1.tmp")
                                               .arg(QDateTime::currentMSecsSinceEpoch()));
    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入临时文件：%1").arg(file.errorString());
        return false;
    }
    if (file.write(data) != data.size()) {
        if (error)
            *error = QStringLiteral("无法写入临时文件：%1").arg(file.errorString());
        file.close();
        QFile::remove(tempPath);
        return false;
    }
    file.close();
    const bool ok = uploadFile(tempPath, pathValue, nullptr, nullptr, error);
    QFile::remove(tempPath);
    return ok;
}

bool FtpBackend::retrToSink(const QString &remotePath, const std::function<bool(const char *, qint64)> &sink,
                            qint64 totalHint, const ProgressCallback &progress, CancelToken *cancel,
                            QString *error)
{
    QSslSocket *dataSocket = nullptr;
    if (!openDataConnection(&dataSocket, error))
        return false;
    const QString remote = path::normalizeRemote(remotePath);
    Reply reply;
    if (!sendCommand(QStringLiteral("RETR %1").arg(quote(remote)), &reply, error)) {
        closeDataConnection(dataSocket);
        return false;
    }
    if (reply.code != 125 && reply.code != 150) {
        closeDataConnection(dataSocket);
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法下载 %1").arg(remote));
        return false;
    }

    qint64 done = 0;
    bool cancelled = false;
    bool writeFailed = false;
    char buffer[128 * 1024];
    while (true) {
        if (cancel && cancel->load()) {
            cancelled = true;
            break;
        }
        if (m_secure && totalHint > 0 && done >= totalHint)
            break;
        const qint64 available = dataSocket->bytesAvailable();
        if (available > 0) {
            const qint64 read = dataSocket->read(buffer, std::min<qint64>(available, static_cast<qint64>(sizeof(buffer))));
            if (read > 0) {
                if (!sink(buffer, read)) {
                    writeFailed = true;
                    break;
                }
                done += read;
                if (progress)
                    progress(TransferProgress{done, totalHint, path::remoteName(remote)});
            }
            continue;
        }
        if (dataSocket->state() == QAbstractSocket::UnconnectedState)
            break;
        if (m_secure && done > 0) {
            if (!dataSocket->waitForReadyRead(800))
                break;
            continue;
        }
        if (!dataSocket->waitForReadyRead(m_dataTimeoutMs)) {
            if (dataSocket->state() == QAbstractSocket::UnconnectedState)
                break;
            if (error)
                *error = QStringLiteral("下载超时：%1").arg(dataSocket->errorString());
            closeDataConnection(dataSocket);
            return false;
        }
    }
    closeDataConnection(dataSocket);

    Reply finalReply;
    if (!readReply(&finalReply, error))
        return false;
    if (cancelled) {
        if (error)
            *error = QStringLiteral("传输已取消。");
        return false;
    }
    if (writeFailed) {
        if (error)
            *error = QStringLiteral("写入下载数据失败。");
        return false;
    }
    if (finalReply.code != 226 && finalReply.code != 250) {
        if (error)
            *error = errorForReply(finalReply, QStringLiteral("下载未完成"));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool FtpBackend::downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                              const ProgressCallback &progress, CancelToken *cancel,
                              QString *error)
{
    QFileInfo localInfo(localPath);
    QDir().mkpath(localInfo.absolutePath());
    QSaveFile output(localPath);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入本地文件 %1：%2")
                         .arg(QDir::toNativeSeparators(localPath), output.errorString());
        return false;
    }
    const bool ok = retrToSink(
        remotePath,
        [&output](const char *data, qint64 size) {
            return output.write(data, size) == size;
        },
        totalHint, progress, cancel, error);
    if (!ok) {
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        if (error)
            *error = QStringLiteral("无法保存下载文件：%1").arg(output.errorString());
        return false;
    }
    return true;
}

bool FtpBackend::uploadFile(const QString &localPath, const QString &remotePath,
                            const ProgressCallback &progress, CancelToken *cancel, QString *error)
{
    QFile input(localPath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取本地文件 %1：%2")
                         .arg(QDir::toNativeSeparators(localPath), input.errorString());
        return false;
    }
    QSslSocket *dataSocket = nullptr;
    if (!openDataConnection(&dataSocket, error, true))
        return false;
    const QString remote = path::normalizeRemote(remotePath);
    Reply reply;
    if (!sendCommand(QStringLiteral("STOR %1").arg(quote(remote)), &reply, error)) {
        closeDataConnection(dataSocket);
        return false;
    }
    if (reply.code != 125 && reply.code != 150) {
        closeDataConnection(dataSocket);
        if (error)
            *error = errorForReply(reply, QStringLiteral("无法上传 %1").arg(remote));
        return false;
    }

    const qint64 total = input.size();
    qint64 done = 0;
    bool cancelled = false;
    bool writeFailed = false;
    QByteArray buffer;
    buffer.resize(128 * 1024);
    while (!input.atEnd()) {
        if (cancel && cancel->load()) {
            cancelled = true;
            break;
        }
        const qint64 read = input.read(buffer.data(), buffer.size());
        if (read <= 0)
            break;
        qint64 offset = 0;
        while (offset < read) {
            const qint64 written = dataSocket->write(buffer.constData() + offset, read - offset);
            if (written <= 0) {
                Logger::instance().warning(
                    QStringLiteral("FTP 数据写入失败：write=%1 state=%2 error=%3")
                        .arg(written)
                        .arg(static_cast<int>(dataSocket->state()))
                        .arg(dataSocket->errorString()));
                writeFailed = true;
                break;
            }
            offset += written;
            if (!dataSocket->waitForBytesWritten(m_dataTimeoutMs)) {
                Logger::instance().warning(
                    QStringLiteral("FTP 数据写入未完成：state=%1 error=%2")
                        .arg(static_cast<int>(dataSocket->state()))
                        .arg(dataSocket->errorString()));
            }
            dataSocket->flush();
        }
        if (writeFailed)
            break;
        done += read;
        if (progress)
            progress(TransferProgress{done, total, QFileInfo(localPath).fileName()});
    }

    if (cancelled || writeFailed) {
        dataSocket->abort();
        delete dataSocket;
        Reply abortReply;
        sendCommand(QStringLiteral("ABOR"), &abortReply, nullptr);
        if (error) {
            *error = cancelled ? QStringLiteral("传输已取消。")
                               : QStringLiteral("写入 FTP 数据连接失败。");
        }
        return false;
    }

    if (m_secure) {
        dataSocket->waitForBytesWritten(m_dataTimeoutMs);
        Logger::instance().debug(QStringLiteral("FTP 上传刷新：待发送 %1 字节，状态 %2")
                                     .arg(dataSocket->bytesToWrite())
                                     .arg(static_cast<int>(dataSocket->state())));
    }
    dataSocket->disconnectFromHost();
    if (dataSocket->state() != QAbstractSocket::UnconnectedState)
        dataSocket->waitForDisconnected(m_dataTimeoutMs);
    Logger::instance().debug(QStringLiteral("FTP 上传数据连接关闭：已发送 %1 字节，状态 %2，错误 %3")
                                 .arg(done)
                                 .arg(static_cast<int>(dataSocket->state()))
                                 .arg(dataSocket->errorString()));
    delete dataSocket;

    Reply finalReply;
    if (!readReply(&finalReply, error))
        return false;
    Logger::instance().debug(QStringLiteral("FTP 上传完成响应：%1").arg(finalReply.code));
    if (finalReply.code != 226 && finalReply.code != 250) {
        if (error)
            *error = errorForReply(finalReply, QStringLiteral("上传未完成"));
        return false;
    }
    if (error)
        error->clear();
    return true;
}

void FtpBackend::keepAlive()
{
    if (!isConnected())
        return;
    Reply reply;
    QString error;
    sendCommand(QStringLiteral("NOOP"), &reply, &error);
}

void FtpBackend::disconnect()
{
    if (m_control) {
        if (m_control->state() == QAbstractSocket::ConnectedState) {
            QByteArray quit = QByteArrayLiteral("QUIT\r\n");
            m_control->write(quit);
            m_control->waitForBytesWritten(1000);
        }
        m_control->disconnectFromHost();
        if (m_control->state() != QAbstractSocket::UnconnectedState)
            m_control->waitForDisconnected(2000);
        delete m_control;
        m_control = nullptr;
    }
    m_connected = false;
    m_secure = false;
    m_cwd = QStringLiteral("/");
}

} // namespace wsc


