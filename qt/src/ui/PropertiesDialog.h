// SPDX-License-Identifier: MIT
#pragma once

#include "core/Types.h"

#include <QDialog>

class QCheckBox;
class QLabel;

namespace wsc {

class PropertiesDialog : public QDialog {
    Q_OBJECT
public:
    PropertiesDialog(const FileInfo &info, bool remote, bool canEditPermissions,
                     QWidget *parent = nullptr);

    quint32 mode() const;
    bool permissionsEdited() const { return m_canEditPermissions; }

private:
    void updatePermissionText();

    FileInfo m_info;
    bool m_remote = false;
    bool m_canEditPermissions = false;
    QCheckBox *m_permissions[3][3] = {};
    QLabel *m_permissionValue = nullptr;
};

} // namespace wsc
