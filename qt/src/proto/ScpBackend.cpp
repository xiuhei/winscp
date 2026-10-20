#include "proto/ScpBackend.h"

#include "core/AppPaths.h"
#include "core/KnownHosts.h"
#include "core/Logger.h"
#include "core/PathUtils.h"
#include "ssh/SshConnection.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTimeZone>
#include <QUuid>

#include <libssh2.h>

#include <algorithm>

namespace wsc {

namespace {

quint32 parseMode(const QString &permissions)
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
    if (permissions.at(3).toLower() == QLatin1Char('s'))
        mode |= 04000;
    if (permissions.at(6).toLower() == QLatin1Char('s'))
        mode |= 02000;
    if (permissions.at(9).toLower() == QLatin1Char('t'))
        mode |= 01000;
    return mode;
}

QDateTime parseDateText(const QString &text)
{
    const QString value = text.simplified();
    if (value.contains(QLatin1Char('-'))) {
        QDateTime parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd HH:mm"));
        if (parsed.isValid())
            return parsed;
        parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd"));
        return parsed;
    }
    if (value.contains(QLatin1Char(':'))) {
        QDateTime parsed = QLocale::c().toDateTime(value, QStringLiteral("MMM d HH:mm"));
        if (parsed.isValid()) {
            QDate date = parsed.date();
            date.setDate(QDate::currentDate().year(), date.month(), date.day());
            if (date > QDate::currentDate().addDays(1))
                date = date.addYears(-1);
            parsed.setDate(date);
            return parsed;
        }
    }
    return QLocale::c().toDateTime(value, QStringLiteral("MMM d yyyy"));
}

} // namespace

ScpBackend::ScpBackend() = default;

ScpBackend::~ScpBackend()
{
    disconnect();
}

bool ScpBackend::isConnected() const
{
    return m_connected && m_ssh && m_ssh->isConnected();
}

bool ScpBackend::connect(const SessionData &session, QString *error)
{
    disconnect();
    m_ssh = std::make_unique<SshConnection>();
    m_ssh->setTimeoutSeconds(session.timeoutSeconds);
    if (m_hostKeyPrompt) {
        m_ssh->setHostKeyVerifier(
            [prompt = m_hostKeyPrompt](const HostKeyInfo &info, QString *error) {
                KnownHostKey existing;
                const HostKeyStatus status = KnownHosts::instance().verify(
                    info.host, info.port, info.algorithm, info.fingerprint, &existing, error);
                if (status == HostKeyStatus::Match)
                    return true;
                if (status == HostKeyStatus::Mismatch)
                    return false;
                QString promptError;
                if (!prompt(info.host, info.port, info.algorithm, info.fingerprint, &promptError)) {
                    if (error)
                        *error = promptError;
                    return false;
                }
                return KnownHosts::instance().addOrUpdate(info.host, info.port, info.algorithm,
                                                          info.fingerprint, error);
            });
    }
    if (!m_ssh->connectToHost(session.hostName, session.port, error))
        return false;
    if (!authenticate(session, error)) {
        m_ssh->disconnect();
        return false;
    }
    m_ssh->enableKeepAlive(session.keepAliveSeconds);
    m_connected = true;
    QByteArray output;
    int exitCode = -1;
    QString pwdError;
    if (exec(QStringLiteral("pwd"), &output, nullptr, &exitCode, &pwdError) && exitCode == 0) {
        const QString home = QString::fromUtf8(output).trimmed().section(QLatin1Char('\n'), 0, 0);
        if (!home.isEmpty())
            m_home = path::normalizeRemote(home);
    }
    m_connected = true;
    Logger::instance().success(QStringLiteral("SCP 会话已建立：%1（主目录 %2）")
                                   .arg(session.endpoint(), m_home));
    if (error)
        error->clear();
    return true;
}

bool ScpBackend::authenticate(const SessionData &session, QString *error)
{
    if (!session.privateKeyFile.trimmed().isEmpty()) {
        if (m_ssh->authenticateKeyFile(session.userName, session.privateKeyFile,
                                       session.passphrase, error))
            return true;
        if (session.password.isEmpty())
            return false;
    }
    if (!session.password.isEmpty()) {
        QString passwordError;
        if (m_ssh->authenticatePassword(session.userName, session.password, &passwordError))
            return true;
        QString interactiveError;
        if (m_ssh->authenticateKeyboardInteractive(session.userName, session.password,
                                                   &interactiveError))
            return true;
        if (error)
            *error = passwordError.isEmpty() ? interactiveError : passwordError;
        return false;
    }
    if (m_ssh->authenticateAgent(session.userName, error))
        return true;
    if (error && error->isEmpty())
        *error = QStringLiteral("未提供密码或私钥，SSH 代理也没有可用的身份。");
    return false;
}

void ScpBackend::disconnect()
{
    m_connected = false;
    if (m_ssh) {
        m_ssh->disconnect();
        m_ssh.reset();
    }
}

QString ScpBackend::shellQuote(const QString &value) const
{
    QString cleaned = value;
    cleaned.remove(QLatin1Char('\r'));
    cleaned.remove(QLatin1Char('\n'));
    cleaned.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(cleaned);
}

QString ScpBackend::commandError(const QString &command, const QByteArray &stderrData,
                                 int exitCode) const
{
    QString detail = QString::fromUtf8(stderrData).trimmed();
    if (detail.isEmpty())
        detail = QStringLiteral("命令返回退出码 %1。").arg(exitCode);
    return QStringLiteral("远程命令失败：%1\n%2").arg(command, detail);
}

bool ScpBackend::readChannel(LIBSSH2_CHANNEL *channel,
                              const std::function<bool(const char *, qint64)> &sink,
                              QByteArray *stderrData, CancelToken *cancel, QString *error)
{
    libssh2_session_set_blocking(m_ssh->session(), 0);
    const auto restore = qScopeGuard([&] { libssh2_session_set_blocking(m_ssh->session(), 1); });
    QElapsedTimer inactivity;
    inactivity.start();
    char buffer[32768];
    while (true) {
        if (cancel && cancel->load()) {
            if (error)
                *error = QStringLiteral("传输已取消。");
            return false;
        }
        bool received = false;
        for (int stream = 0; stream < 2; ++stream) {
            for (int attempt = 0; attempt < 16; ++attempt) {
                const auto read = libssh2_channel_read_ex(channel, stream, buffer, sizeof(buffer));
                if (read > 0) {
                    received = true;
                    if (stream == 0) {
                        if (!sink(buffer, read))
                            return false;
                    } else if (stderrData && stderrData->size() < 65536) {
                        stderrData->append(buffer, std::min<qint64>(read, 65536 - stderrData->size()));
                    }
                } else {
                    if (read < 0 && read != LIBSSH2_ERROR_EAGAIN) {
                        if (error)
                            *error = QStringLiteral("读取 SSH 通道失败：%1").arg(m_ssh->lastError());
                        return false;
                    }
                    break;
                }
            }
        }
        if (libssh2_channel_eof(channel))
            return true;
        if (received)
            inactivity.restart();
        else if (inactivity.elapsed() >= m_ssh->timeoutSeconds() * 1000) {
            if (error)
                *error = QStringLiteral("读取 SSH 通道超时。");
            return false;
        } else {
            m_ssh->waitForSocket(100);
        }
    }
}

bool ScpBackend::exec(const QString &command, QByteArray *output, QByteArray *errorOutput,
                      int *exitCode, QString *error, qint64 outputLimit)
{
    if (!isConnected()) {
        if (error)
            *error = QStringLiteral("SCP 会话未连接。");
        return false;
    }
    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(m_ssh->session());
    if (!channel) {
        if (error)
            *error = QStringLiteral("无法打开 SSH 命令通道：%1").arg(m_ssh->lastError());
        return false;
    }
    const QByteArray encoded = command.toUtf8();
    if (libssh2_channel_exec(channel, encoded.constData()) != 0) {
        if (error)
            *error = QStringLiteral("无法执行远程命令：%1").arg(m_ssh->lastError());
        libssh2_channel_free(channel);
        return false;
    }

    QByteArray stdoutData;
    QByteArray stderrData;
    const bool failed = !readChannel(channel, [&](const char *chunk, qint64 size) {
        if (stdoutData.size() + size > outputLimit) {
            if (error)
                *error = QStringLiteral("远程命令输出过大。");
            return false;
        }
        stdoutData.append(chunk, size);
        return true;
    }, &stderrData, nullptr, error);
    int status = -1;
    if (!failed) {
        libssh2_channel_close(channel);
        status = libssh2_channel_get_exit_status(channel);
    }
    libssh2_channel_free(channel);
    if (failed)
        return false;
    if (output)
        *output = stdoutData;
    if (errorOutput)
        *errorOutput = stderrData;
    if (exitCode)
        *exitCode = status;
    return true;
}

bool ScpBackend::execOk(const QString &command, QByteArray *output, QString *error, qint64 outputLimit)
{
    QByteArray errorOutput;
    int exitCode = -1;
    if (!exec(command, output, &errorOutput, &exitCode, error, outputLimit))
        return false;
    if (exitCode != 0) {
        if (error)
            *error = commandError(command, errorOutput, exitCode);
        return false;
    }
    return true;
}

bool ScpBackend::parseListLine(const QString &line, const QString &directory, FileInfo *info,
                               QString *error)
{
    static const QRegularExpression re(
        QStringLiteral("^([bcdlps-][rwxstST-]{9}[+@.]?)\\s+\\d+\\s+(\\S+)\\s+(\\S+)\\s+(\\d+)\\s+"
                       "(\\d{4}-\\d{2}-\\d{2}\\s+\\d{2}:\\d{2}|[A-Za-z]{3}\\s+\\d{1,2}\\s+"
                       "(?:\\d{4}|\\d{1,2}:\\d{2}))\\s+(.*)$"));
    const QRegularExpressionMatch match = re.match(line);
    if (!match.hasMatch()) {
        if (error)
            *error = QStringLiteral("无法解析远程目录行：%1").arg(line);
        return false;
    }
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
    result.mode = parseMode(match.captured(1));
    result.isDir = match.captured(1).startsWith(QLatin1Char('d'));
    result.isSymlink = match.captured(1).startsWith(QLatin1Char('l'));
    result.isExecutable = (result.mode & 0111) != 0;
    result.isHidden = name.startsWith(QLatin1Char('.'));
    result.owner = match.captured(2);
    result.group = match.captured(3);
    result.size = match.captured(4).toLongLong();
    result.modified = parseDateText(match.captured(5));
    result.linkTarget = linkTarget;
    if (info)
        *info = result;
    return true;
}

bool ScpBackend::list(const QString &pathValue, FileList *out, QString *error)
{
    if (!out)
        return false;
    out->clear();
    const QString directory = path::normalizeRemote(pathValue);
    const QString quoted = shellQuote(directory);
    static const QStringList commands = {
        QStringLiteral("env LC_ALL=C ls -la --time-style=long-iso -- %1"),
        QStringLiteral("env LC_ALL=C ls -la -- %1"),
        QStringLiteral("LC_ALL=C ls -la %1"),
    };
    QByteArray output;
    QByteArray errorOutput;
    int exitCode = -1;
    QString lastError;
    bool succeeded = false;
    for (const QString &format : commands) {
        const QString command = format.arg(quoted);
        if (!exec(command, &output, &errorOutput, &exitCode, &lastError))
            continue;
        if (exitCode == 0) {
            succeeded = true;
            break;
        }
        lastError = commandError(command, errorOutput, exitCode);
    }
    if (!succeeded) {
        if (error)
            *error = lastError.isEmpty() ? QStringLiteral("无法列出远程目录 %1。").arg(directory)
                                         : lastError;
        return false;
    }

    const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'),
                                                              Qt::SkipEmptyParts);
    int parsed = 0;
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty() || line.startsWith(QLatin1String("total ")))
            continue;
        FileInfo info;
        if (parseListLine(line, directory, &info, nullptr)) {
            ++parsed;
            out->append(info);
        } else {
            Logger::instance().debug(QStringLiteral("无法解析 SCP 列表行：%1").arg(line));
        }
    }
    if (parsed == 0 && !lines.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法解析远程目录列表：%1").arg(directory);
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool ScpBackend::stat(const QString &pathValue, FileInfo *out, QString *error)
{
    if (!out)
        return false;
    const QString normalized = path::normalizeRemote(pathValue);
    const QString quoted = shellQuote(normalized);
    QByteArray output;
    int exitCode = -1;
    QString lastError;
    bool ok = false;
    for (const QString &format : {QStringLiteral("env LC_ALL=C ls -lad --time-style=long-iso -- %1"),
                                  QStringLiteral("env LC_ALL=C ls -lad -- %1")}) {
        const QString command = format.arg(quoted);
        QByteArray errorOutput;
        if (exec(command, &output, &errorOutput, &exitCode, &lastError) && exitCode == 0) {
            ok = true;
            break;
        }
        lastError = commandError(command, errorOutput, exitCode);
    }
    if (!ok) {
        if (error)
            *error = lastError;
        return false;
    }
    const QString line = QString::fromUtf8(output).section(QLatin1Char('\n'), 0, 0);
    const QString parent = path::remoteParent(normalized);
    FileInfo info;
    if (!parseListLine(line, parent, &info, error))
        return false;
    info.path = normalized;
    *out = info;
    return true;
}

bool ScpBackend::mkdir(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    return execOk(QStringLiteral("mkdir -p -- %1").arg(shellQuote(normalized)), nullptr, error);
}

bool ScpBackend::removeFile(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    return execOk(QStringLiteral("rm -f -- %1").arg(shellQuote(normalized)), nullptr, error);
}

bool ScpBackend::removeDir(const QString &pathValue, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    if (path::remoteIsRoot(normalized)) {
        if (error)
            *error = QStringLiteral("拒绝删除远程根目录。");
        return false;
    }
    return execOk(QStringLiteral("rm -rf -- %1").arg(shellQuote(normalized)), nullptr, error);
}

bool ScpBackend::rename(const QString &from, const QString &to, QString *error)
{
    const QString source = path::normalizeRemote(from);
    const QString target = path::normalizeRemote(to);
    return execOk(QStringLiteral("mv -f -- %1 %2").arg(shellQuote(source), shellQuote(target)),
                  nullptr, error);
}

bool ScpBackend::chmod(const QString &pathValue, quint32 mode, QString *error)
{
    const QString normalized = path::normalizeRemote(pathValue);
    return execOk(QStringLiteral("chmod %1 -- %2")
                      .arg(QString::number(mode & 07777, 8).rightJustified(4, QLatin1Char('0')),
                           shellQuote(normalized)),
                  nullptr, error);
}

bool ScpBackend::createSymlink(const QString &target, const QString &linkPath, QString *error)
{
    const QString link = path::normalizeRemote(linkPath);
    return execOk(QStringLiteral("ln -sfn -- %1 %2").arg(shellQuote(target), shellQuote(link)),
                  nullptr, error);
}

bool ScpBackend::readFile(const QString &pathValue, QByteArray *data, QString *error)
{
    if (!data)
        return false;
    const QString normalized = path::normalizeRemote(pathValue);
    QByteArray output;
    if (!execOk(QStringLiteral("cat -- %1").arg(shellQuote(normalized)), &output, error, MaxEditorBytes))
        return false;
    if (output.size() > 32 * 1024 * 1024) {
        if (error)
            *error = QStringLiteral("文件过大（%1），无法在编辑器中打开。")
                         .arg(path::formatSize(output.size()));
        return false;
    }
    *data = output;
    return true;
}

bool ScpBackend::writeFile(const QString &pathValue, const QByteArray &data, QString *error)
{
    AppPaths::ensureDirectories();
    const QString tempPath =
        QDir(AppPaths::tempDir())
            .filePath(QStringLiteral("scp-write-%1.tmp")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法创建临时文件：%1").arg(file.errorString());
        return false;
    }
    file.write(data);
    file.close();
    const bool ok = uploadFile(tempPath, pathValue, nullptr, nullptr, error);
    QFile::remove(tempPath);
    return ok;
}

bool ScpBackend::downloadStream(const QString &remotePath, const QString &localPath, qint64 totalHint,
                                const ProgressCallback &progress, CancelToken *cancel,
                                QString *error)
{
    const QString remote = path::normalizeRemote(remotePath);
    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(m_ssh->session());
    if (!channel) {
        if (error)
            *error = QStringLiteral("无法打开 SSH 下载通道：%1").arg(m_ssh->lastError());
        return false;
    }
    const QByteArray command = QStringLiteral("cat -- %1").arg(shellQuote(remote)).toUtf8();
    if (libssh2_channel_exec(channel, command.constData()) != 0) {
        if (error)
            *error = QStringLiteral("无法启动 cat：%1").arg(m_ssh->lastError());
        libssh2_channel_free(channel);
        return false;
    }

    QFileInfo localInfo(localPath);
    QDir().mkpath(localInfo.absolutePath());
    QSaveFile output(localPath);
    if (!output.open(QIODevice::WriteOnly)) {
        libssh2_channel_free(channel);
        if (error)
            *error = QStringLiteral("无法写入本地文件 %1：%2")
                         .arg(QDir::toNativeSeparators(localPath), output.errorString());
        return false;
    }

    qint64 done = 0;
    QByteArray stderrData;
    const bool failed = !readChannel(channel, [&](const char *chunk, qint64 size) {
        if (output.write(chunk, size) != size) {
            if (error)
                *error = QStringLiteral("写入本地文件失败：%1").arg(output.errorString());
            return false;
        }
        done += size;
        if (progress)
            progress(TransferProgress{done, totalHint, path::remoteName(remote)});
        return true;
    }, &stderrData, cancel, error);
    const bool cancelled = cancel && cancel->load();
    int exitCode = -1;
    libssh2_channel_close(channel);
    exitCode = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);

    if (cancelled) {
        output.cancelWriting();
        if (error)
            *error = QStringLiteral("传输已取消。");
        return false;
    }
    if (failed) {
        output.cancelWriting();
        return false;
    }
    if (exitCode != 0) {
        output.cancelWriting();
        if (error)
            *error = commandError(QStringLiteral("cat -- %1").arg(shellQuote(remote)), stderrData,
                                  exitCode);
        return false;
    }
    if (!output.commit()) {
        if (error)
            *error = QStringLiteral("无法保存下载文件：%1").arg(output.errorString());
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool ScpBackend::uploadStream(const QString &localPath, const QString &remotePath,
                              const ProgressCallback &progress, CancelToken *cancel, QString *error)
{
    QFile input(localPath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取本地文件 %1：%2")
                         .arg(QDir::toNativeSeparators(localPath), input.errorString());
        return false;
    }
    const QString remote = path::normalizeRemote(remotePath);
    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(m_ssh->session());
    if (!channel) {
        if (error)
            *error = QStringLiteral("无法打开 SSH 上传通道：%1").arg(m_ssh->lastError());
        return false;
    }
    const QByteArray command = QStringLiteral("cat > %1").arg(shellQuote(remote)).toUtf8();
    if (libssh2_channel_exec(channel, command.constData()) != 0) {
        if (error)
            *error = QStringLiteral("无法启动远程 cat：%1").arg(m_ssh->lastError());
        libssh2_channel_free(channel);
        return false;
    }

    const qint64 total = input.size();
    qint64 done = 0;
    bool cancelled = false;
    bool failed = false;
    QByteArray buffer;
    buffer.resize(128 * 1024);
    while (!input.atEnd()) {
        if (cancel && cancel->load()) {
            cancelled = true;
            break;
        }
        const qint64 read = input.read(buffer.data(), buffer.size());
        if (read < 0) {
            failed = true;
            if (error)
                *error = input.errorString();
            break;
        }
        if (read == 0)
            break;
        qint64 offset = 0;
        while (offset < read) {
            const ssize_t written = libssh2_channel_write(channel, buffer.constData() + offset,
                                                          static_cast<size_t>(read - offset));
            if (written <= 0) {
                if (error)
                    *error = QStringLiteral("写入远程文件失败：%1").arg(m_ssh->lastError());
                failed = true;
                break;
            }
            offset += written;
        }
        if (failed)
            break;
        done += read;
        if (progress)
            progress(TransferProgress{done, total, QFileInfo(localPath).fileName()});
    }
    libssh2_channel_send_eof(channel);
    QByteArray stderrData;
    if (!readChannel(channel, [](const char *, qint64) { return true; }, &stderrData, nullptr, error))
        failed = true;
    libssh2_channel_close(channel);
    const int exitCode = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);

    if (cancelled || failed) {
        if (error)
            *error = cancelled ? QStringLiteral("传输已取消。")
                               : QStringLiteral("上传失败。");
        return false;
    }
    if (exitCode != 0) {
        if (error)
            *error = commandError(QStringLiteral("cat > %1").arg(shellQuote(remote)), stderrData,
                                  exitCode);
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool ScpBackend::downloadFile(const QString &remotePath, const QString &localPath, qint64 totalHint,
                              const ProgressCallback &progress, CancelToken *cancel,
                              QString *error)
{
    return downloadStream(remotePath, localPath, totalHint, progress, cancel, error);
}

bool ScpBackend::uploadFile(const QString &localPath, const QString &remotePath,
                            const ProgressCallback &progress, CancelToken *cancel, QString *error)
{
    QString target = path::normalizeRemote(remotePath);
    FileInfo existing;
    bool exists = stat(target, &existing, nullptr);
    if (exists && existing.isDir) {
        if (error)
            *error = QStringLiteral("上传目标是一个目录。");
        return false;
    }
    if (exists && existing.isSymlink) {
        QByteArray resolved;
        if (!execOk(QStringLiteral("readlink -f -- %1").arg(shellQuote(target)), &resolved, error) ||
            !resolved.trimmed().startsWith('/')) {
            if (error)
                *error = QStringLiteral("无法解析上传目标的符号链接。");
            return false;
        }
        target = QString::fromUtf8(resolved).trimmed();
        exists = stat(target, &existing, nullptr);
    }
    const QString temporary = path::remoteJoin(path::remoteParent(target),
        QStringLiteral(".winscp-upload-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    auto cleanup = qScopeGuard([&] { removeFile(temporary, nullptr); });
    if (!uploadStream(localPath, temporary, progress, cancel, error) ||
        (cancel && cancel->load()))
        return false;
    if (exists && !chmod(temporary, existing.mode & 0777, error))
        return false;
    if (!rename(temporary, target, error))
        return false;
    cleanup.dismiss();
    return true;
}

bool ScpBackend::setModifiedTime(const QString &pathValue, const QDateTime &time, QString *error)
{
    if (!time.isValid())
        return true;
    const QString normalized = path::normalizeRemote(pathValue);
    const QString quoted = shellQuote(normalized);
    const qint64 seconds = time.toSecsSinceEpoch();
    QByteArray errorOutput;
    int exitCode = -1;
    if (exec(QStringLiteral("touch -d @%1 -- %2").arg(seconds).arg(quoted), nullptr, &errorOutput,
             &exitCode, error) &&
        exitCode == 0)
        return true;
    const QString stamp = time.toString(QStringLiteral("yyyyMMddHHmm.ss"));
    if (exec(QStringLiteral("touch -t %1 -- %2").arg(stamp, quoted), nullptr, &errorOutput,
             &exitCode, error) &&
        exitCode == 0)
        return true;
    if (error)
        *error = QStringLiteral("无法设置远程文件时间。");
    return false;
}

void ScpBackend::keepAlive()
{
    if (m_ssh)
        m_ssh->sendKeepAlive();
}

} // namespace wsc
