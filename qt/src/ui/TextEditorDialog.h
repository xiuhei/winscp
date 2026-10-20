// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>

class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QWidget;

namespace wsc {

class TextEditorDialog : public QDialog {
    Q_OBJECT
public:
    TextEditorDialog(const QString &remotePath, const QByteArray &data, QWidget *parent = nullptr, bool readOnly = false);

    QByteArray content() const;

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
    bool m_hasBom = false;
    bool m_crlf = false;
};

} // namespace wsc
