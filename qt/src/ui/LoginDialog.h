#pragma once

#include "core/SessionData.h"
#include <QDialog>

class QListWidget;
class QPushButton;

namespace wsc {

class SessionEditor;

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(const SessionData &initial, QWidget *parent = nullptr);
    SessionData session() const;

protected:
    void accept() override;

private:
    void refreshSites(const QString &selected = {});
    void saveSite();
    void deleteSite();

    SessionEditor *m_editor = nullptr;
    QListWidget *m_sites = nullptr;
    QPushButton *m_deleteButton = nullptr;
};

}
