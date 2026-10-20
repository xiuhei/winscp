// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>

class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QWidget;
class QPushButton;

namespace wsc {

class TextEditorDialog : public QDialog {
    Q_OBJECT
public:
    TextEditorDialog(const QString &remotePath, const QByteArray &data, QWidget *parent = nullptr, bool readOnly = false);

    QByteArray content() const;
    void finishSave(bool success, const QString &message = {});

signals:
    void saveRequested(const QByteArray &data);

public slots:
    void reject() override;

protected:
    void accept() override;

private:
    void updateStatus();
    void findNext(bool backward);
    void toggleFindBar(bool visible);

    QString m_path;
    QByteArray m_original;
    QPlainTextEdit *m_editor = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QWidget *m_findBar = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_modifiedLabel = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    bool m_saving = false;
    bool m_hasBom = false;
    bool m_crlf = false;
};

} // namespace wsc
