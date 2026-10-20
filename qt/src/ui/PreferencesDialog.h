// SPDX-License-Identifier: MIT
#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLineEdit;
class QSpinBox;

namespace wsc {

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

protected:
    void accept() override;

private:
    QComboBox *m_themeCombo = nullptr;
    QCheckBox *m_showHiddenCheck = nullptr;
    QCheckBox *m_confirmDeleteCheck = nullptr;
    QCheckBox *m_confirmExitCheck = nullptr;
    QCheckBox *m_savePasswordsCheck = nullptr;
    QCheckBox *m_askHostKeyCheck = nullptr;
    QSpinBox *m_cacheDaysSpin = nullptr;
    QSpinBox *m_parallelSpin = nullptr;
    QComboBox *m_conflictCombo = nullptr;
    QCheckBox *m_preserveTimesCheck = nullptr;
    QCheckBox *m_preservePermissionsCheck = nullptr;
    QFontComboBox *m_terminalFontCombo = nullptr;
    QSpinBox *m_terminalSizeSpin = nullptr;
    QSpinBox *m_scrollbackSpin = nullptr;
    QLineEdit *m_shellEdit = nullptr;
    QLineEdit *m_editorEdit = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QSpinBox *m_keepAliveSpin = nullptr;
};

} // namespace wsc
