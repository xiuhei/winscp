// SPDX-License-Identifier: MIT
#pragma once

#include "core/SessionData.h"

#include <QDialog>

class QCheckBox;

namespace wsc {

class SessionEditor;

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(const SessionData &initial, QWidget *parent = nullptr);

    SessionData session() const;
    bool saveAsSite() const;

protected:
    void accept() override;

private:
    SessionEditor *m_editor = nullptr;
    QCheckBox *m_saveSiteCheck = nullptr;
};

} // namespace wsc
