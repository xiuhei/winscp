#include "ui/TextEditorDialog.h"

#include "core/PathUtils.h"
#include "core/Logger.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace wsc {

TextEditorDialog::TextEditorDialog(const QString &remotePath, const QByteArray &data, QWidget *parent)
    : QDialog(parent)
    , m_path(remotePath)
    , m_original(data)
{
    setWindowTitle(QStringLiteral("%1 - 编辑器").arg(path::remoteName(remotePath)));
    setWindowIcon(IconProvider::instance().appIcon());
    resize(900, 680);
    setMinimumSize(560, 400);

    QByteArray textData = data;
    if (textData.startsWith("\xEF\xBB\xBF")) {
        m_hasBom = true;
        textData.remove(0, 3);
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 8);
    layout->setSpacing(6);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);
    auto *icon = new QLabel(this);
    icon->setPixmap(IconProvider::instance().icon(QStringLiteral("text")).pixmap(24, 24));
    auto *pathLabel = new QLabel(remotePath, this);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    m_modifiedLabel = new QLabel(this);
    m_modifiedLabel->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                       .arg(Theme::accent().name()));
    auto *findButton = new QToolButton(this);
    findButton->setIcon(IconProvider::instance().icon(QStringLiteral("search")));
    findButton->setToolTip(QStringLiteral("查找 (Ctrl+F)"));
    findButton->setAutoRaise(true);
    auto *reloadInfo = new QLabel(QStringLiteral("UTF-8"), this);
    reloadInfo->setStyleSheet(QStringLiteral("color: %1; font-size: 9pt;").arg(Theme::muted().name()));
    toolbar->addWidget(icon);
    toolbar->addWidget(pathLabel, 1);
    toolbar->addWidget(m_modifiedLabel);
    toolbar->addWidget(reloadInfo);
    toolbar->addWidget(findButton);
    layout->addLayout(toolbar);

    m_findBar = new QWidget(this);
    auto *findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(0, 0, 0, 0);
    findLayout->setSpacing(6);
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setPlaceholderText(QStringLiteral("查找文本…"));
    auto *previousButton = new QToolButton(m_findBar);
    previousButton->setText(QStringLiteral("上一个"));
    auto *nextButton = new QToolButton(m_findBar);
    nextButton->setText(QStringLiteral("下一个"));
    auto *closeFindButton = new QToolButton(m_findBar);
    closeFindButton->setIcon(IconProvider::instance().icon(QStringLiteral("close")));
    findLayout->addWidget(m_findEdit, 1);
    findLayout->addWidget(previousButton);
    findLayout->addWidget(nextButton);
    findLayout->addWidget(closeFindButton);
    m_findBar->hide();
    layout->addWidget(m_findBar);

    m_editor = new QPlainTextEdit(this);
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(10);
    m_editor->setFont(font);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabStopDistance(QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')) * 4);
    m_editor->setPlainText(QString::fromUtf8(textData));
    m_editor->document()->setModified(false);
    layout->addWidget(m_editor, 1);

    auto *statusRow = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    auto *sizeLabel = new QLabel(
        QStringLiteral("%1 · %2 行").arg(path::formatSize(data.size())).arg(m_editor->document()->blockCount()),
        this);
    sizeLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    statusRow->addWidget(m_statusLabel, 1);
    statusRow->addWidget(sizeLabel);
    layout->addLayout(statusRow);

    auto *buttons = new QDialogButtonBox(this);
    auto *saveButton = buttons->addButton(QStringLiteral("保存并上传"), QDialogButtonBox::AcceptRole);
    saveButton->setProperty("accent", true);
    auto *cancelButton = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &TextEditorDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &TextEditorDialog::reject);
    connect(m_editor->document(), &QTextDocument::modificationChanged, this, [this](bool modified) {
        m_modifiedLabel->setText(modified ? QStringLiteral("● 未保存") : QString());
        setWindowModified(modified);
        setWindowTitle(QStringLiteral("%1%2 - 编辑器")
                           .arg(path::remoteName(m_path), modified ? QStringLiteral(" *") : QString()));
    });
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this, &TextEditorDialog::updateStatus);
    connect(findButton, &QToolButton::clicked, this, [this] { toggleFindBar(!m_findBar->isVisible()); });
    connect(closeFindButton, &QToolButton::clicked, this, [this] { toggleFindBar(false); });
    connect(previousButton, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(nextButton, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(m_findEdit, &QLineEdit::returnPressed, this, [this] { findNext(false); });
    connect(m_findEdit, &QLineEdit::textChanged, this, [this](const QString &) { findNext(false); });

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, [this] { toggleFindBar(true); });
    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, &TextEditorDialog::accept);
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escapeShortcut, &QShortcut::activated, this, [this] {
        if (m_findBar->isVisible())
            toggleFindBar(false);
        else
            reject();
    });

    updateStatus();
    m_editor->setFocus();
}

QByteArray TextEditorDialog::content() const
{
    QByteArray result = m_editor->toPlainText().toUtf8();
    if (m_hasBom)
        result.prepend("\xEF\xBB\xBF");
    return result;
}

void TextEditorDialog::accept()
{
    if (m_editor->toPlainText().toUtf8() == m_original)
        QDialog::reject();
    else
        QDialog::accept();
}

void TextEditorDialog::reject()
{
    if (m_editor->document()->isModified()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("放弃更改"),
            QStringLiteral("文件尚未保存，确定要放弃更改吗？"),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard)
            return;
    }
    QDialog::reject();
}

void TextEditorDialog::updateStatus()
{
    const QTextCursor cursor = m_editor->textCursor();
    m_statusLabel->setText(QStringLiteral("行 %1，列 %2")
                               .arg(cursor.blockNumber() + 1)
                               .arg(cursor.positionInBlock() + 1));
}

void TextEditorDialog::toggleFindBar(bool visible)
{
    m_findBar->setVisible(visible);
    if (visible) {
        m_findEdit->setFocus();
        m_findEdit->selectAll();
    } else {
        m_editor->setFocus();
    }
}

void TextEditorDialog::findNext(bool backward)
{
    if (!m_findBar->isVisible())
        return;
    const QString term = m_findEdit->text();
    if (term.isEmpty())
        return;
    QTextDocument::FindFlags flags;
    if (backward)
        flags |= QTextDocument::FindBackward;
    if (!m_editor->find(term, flags)) {
        QTextCursor cursor = m_editor->textCursor();
        cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        m_editor->setTextCursor(cursor);
        m_editor->find(term, flags);
    }
}

} // namespace wsc
