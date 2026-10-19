// SPDX-License-Identifier: MIT
#pragma once

#include "core/SessionData.h"

#include <QDialog>

class QLineEdit;
class QListWidget;
class QLabel;

namespace wsc {

class SessionEditor;

class SiteManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit SiteManagerDialog(const SessionData &initial, QWidget *parent = nullptr);

    SessionData selectedSession() const { return m_selectedSession; }

protected:
    void accept() override;

private:
    void refreshList(const QString &selectName = {});
    void loadSite(int index);
    void newSite();
    void deleteSite();
    void saveCurrent();
    void connectSelected();
    bool currentIndexSelected() const;

    QListWidget *m_list = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    SessionEditor *m_editor = nullptr;
    QLabel *m_statusLabel = nullptr;
    SessionData m_selectedSession;
    bool m_loaded = false;
};

} // namespace wsc
