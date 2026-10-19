// SPDX-License-Identifier: MIT
#pragma once

#include "core/SessionData.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QSpinBox;
class QToolButton;

namespace wsc {

class SessionEditor : public QWidget {
    Q_OBJECT
public:
    explicit SessionEditor(QWidget *parent = nullptr);

    void setSession(const SessionData &session);
    SessionData session() const;
    QString validate() const;
    void setSiteNameVisible(bool visible);
    void focusFirstField();
    void focusPassword();

private:
    void buildUi();
    void updateVisibility();
    void updatePortForProtocol();
    void setFormRowVisible(QFormLayout *form, QWidget *field, bool visible);
    Protocol currentProtocol() const;

    QWidget *m_siteGroup = nullptr;
    QLineEdit *m_siteNameEdit = nullptr;
    QLineEdit *m_noteEdit = nullptr;
    QComboBox *m_protocolCombo = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QToolButton *m_passwordToggle = nullptr;
    QWidget *m_keyRow = nullptr;
    QLineEdit *m_keyEdit = nullptr;
    QWidget *m_passphraseRow = nullptr;
    QLineEdit *m_passphraseEdit = nullptr;
    QLineEdit *m_remoteDirectoryEdit = nullptr;
    QLineEdit *m_localDirectoryEdit = nullptr;
    QCheckBox *m_savePasswordCheck = nullptr;
    QCheckBox *m_passiveCheck = nullptr;
    QCheckBox *m_implicitTlsCheck = nullptr;
    QCheckBox *m_verifyTlsCheck = nullptr;
    QCheckBox *m_webDavHttpsCheck = nullptr;
    QCheckBox *m_compressionCheck = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QSpinBox *m_keepAliveSpin = nullptr;
    QFormLayout *m_connectionForm = nullptr;
    QFormLayout *m_optionsForm = nullptr;
    Protocol m_previousProtocol = Protocol::Sftp;
};

} // namespace wsc
