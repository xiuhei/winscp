#include "core/PasswordStore.h"

#include <QByteArray>
#include <QString>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dpapi.h>
#endif

namespace wsc {

namespace {
constexpr auto kDpapiPrefix = "dpapi:";
constexpr auto kBase64Prefix = "b64:";

QString fallbackProtect(const QString &plainText)
{
    return QString::fromLatin1(kBase64Prefix) +
           QString::fromLatin1(plainText.toUtf8().toBase64());
}

QString fallbackUnprotect(const QString &storedText)
{
    const QString payload = storedText.mid(QString::fromLatin1(kBase64Prefix).size());
    return QString::fromUtf8(QByteArray::fromBase64(payload.toLatin1()));
}
} // namespace

bool PasswordStore::isProtected(const QString &text)
{
    return text.startsWith(QString::fromLatin1(kDpapiPrefix)) ||
           text.startsWith(QString::fromLatin1(kBase64Prefix));
}

QString PasswordStore::protect(const QString &plainText)
{
    if (plainText.isEmpty())
        return QString();
#ifdef Q_OS_WIN
    QByteArray utf8 = plainText.toUtf8();
    QByteArray entropy("WinSCPQt::PasswordStore");
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE *>(utf8.data());
    input.cbData = static_cast<DWORD>(utf8.size());
    DATA_BLOB entropyBlob;
    entropyBlob.pbData = reinterpret_cast<BYTE *>(entropy.data());
    entropyBlob.cbData = static_cast<DWORD>(entropy.size());
    DATA_BLOB output{};
    if (CryptProtectData(&input, L"WinSCPQt", &entropyBlob, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        const QByteArray encrypted(reinterpret_cast<const char *>(output.pbData),
                                   static_cast<int>(output.cbData));
        LocalFree(output.pbData);
        return QString::fromLatin1(kDpapiPrefix) + QString::fromLatin1(encrypted.toBase64());
    }
#endif
    return fallbackProtect(plainText);
}

QString PasswordStore::unprotect(const QString &storedText)
{
    if (storedText.isEmpty())
        return QString();
    if (storedText.startsWith(QString::fromLatin1(kBase64Prefix)))
        return fallbackUnprotect(storedText);
#ifdef Q_OS_WIN
    if (storedText.startsWith(QString::fromLatin1(kDpapiPrefix))) {
        const QByteArray encrypted =
            QByteArray::fromBase64(storedText.mid(QString::fromLatin1(kDpapiPrefix).size()).toLatin1());
        QByteArray entropy("WinSCPQt::PasswordStore");
        DATA_BLOB input;
        input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.data()));
        input.cbData = static_cast<DWORD>(encrypted.size());
        DATA_BLOB entropyBlob;
        entropyBlob.pbData = reinterpret_cast<BYTE *>(entropy.data());
        entropyBlob.cbData = static_cast<DWORD>(entropy.size());
        DATA_BLOB output{};
        if (CryptUnprotectData(&input, nullptr, &entropyBlob, nullptr, nullptr,
                               CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            const QString result = QString::fromUtf8(reinterpret_cast<const char *>(output.pbData),
                                                     static_cast<int>(output.cbData));
            LocalFree(output.pbData);
            return result;
        }
        return QString();
    }
#endif
    // Values written by older versions were plain text.
    return storedText;
}

} // namespace wsc
