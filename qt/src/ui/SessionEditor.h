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
    void focusFirstField();
    void focusPassword();

signals:
    void saveRequested();

private:
    void buildUi();
    void updateVisibility();
    void updatePortForProtocol();
    Protocol currentProtocol() const;

    SessionData m_session;
    QComboBox *m_protocolCombo = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QToolButton *m_passwordToggle = nullptr;
    QWidget *m_keyRow = nullptr;
    QLineEdit *m_keyEdit = nullptr;
    QLineEdit *m_passphraseEdit = nullptr;
    QCheckBox *m_implicitTlsCheck = nullptr;
    QCheckBox *m_verifyTlsCheck = nullptr;
    QCheckBox *m_webDavHttpsCheck = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QSpinBox *m_keepAliveSpin = nullptr;
    QFormLayout *m_optionsForm = nullptr;
    Protocol m_previousProtocol = Protocol::Sftp;
};

}
