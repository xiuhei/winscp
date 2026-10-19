// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace wsc {

/// Small DPAPI-backed password helper. Encrypted values are prefixed with
/// "dpapi:" and can only be decrypted by the same Windows user.
class PasswordStore {
public:
    static QString protect(const QString &plainText);
    static QString unprotect(const QString &storedText);
    static bool isProtected(const QString &text);
};

} // namespace wsc
