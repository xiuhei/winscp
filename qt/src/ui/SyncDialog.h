// SPDX-License-Identifier: MIT
#pragma once

#include "core/Backend.h"
#include "core/SessionData.h"
#include "sync/SyncPlanner.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableView;

namespace wsc {

class SyncResultModel;

/// Compares a local directory with a remote directory and builds a transfer /
/// delete plan. Execution is handed back to MainWindow so it can reuse the
/// normal queue and browser refresh paths.
class SyncDialog : public QDialog {
    Q_OBJECT
public:
    SyncDialog(const SessionData &session, const QString &localRoot, const QString &remoteRoot,
               HostKeyPrompt hostKeyPrompt, QWidget *parent = nullptr);

signals:
    void uploadRequested(const QList<wsc::FileInfo> &files, const QString &remoteDirectory);
    void downloadRequested(const QList<wsc::FileInfo> &files, const QString &localDirectory);
    void remoteDeleteRequested(const QList<wsc::FileInfo> &files);
    void localDeleteRequested(const QList<wsc::FileInfo> &files);

private:
    void buildUi();
    void startCompare();
    void applyResult();
    void performSync();
    void updateControls();
    SyncDirection direction() const;
    QString directionName() const;

    SessionData m_session;
    QString m_localRoot;
    QString m_remoteRoot;
    HostKeyPrompt m_hostKeyPrompt;

    QComboBox *m_directionCombo = nullptr;
    QCheckBox *m_recursiveCheck = nullptr;
    QCheckBox *m_deleteExtrasCheck = nullptr;
    QCheckBox *m_compareTimeCheck = nullptr;
    QPushButton *m_compareButton = nullptr;
    QPushButton *m_syncButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    QLabel *m_localLabel = nullptr;
    QLabel *m_remoteLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QTableView *m_table = nullptr;
    SyncResultModel *m_model = nullptr;

    QList<SyncItem> m_items;
    bool m_running = false;
};

} // namespace wsc
