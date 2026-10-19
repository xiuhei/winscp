#include "ui/PropertiesDialog.h"

#include "core/PathUtils.h"
#include "ui/IconProvider.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace wsc {

PropertiesDialog::PropertiesDialog(const FileInfo &info, bool remote, bool canEditPermissions,
                                   QWidget *parent)
    : QDialog(parent)
    , m_info(info)
    , m_remote(remote)
    , m_canEditPermissions(canEditPermissions)
{
    setWindowTitle(QStringLiteral("属性 - %1").arg(info.name));
    setWindowIcon(IconProvider::instance().appIcon());
    resize(540, m_canEditPermissions ? 560 : 420);
    setMinimumWidth(480);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto *header = new QHBoxLayout();
    header->setSpacing(12);
    auto *icon = new QLabel(this);
    icon->setPixmap(IconProvider::instance().fileIcon(info, !remote).pixmap(48, 48));
    auto *name = new QLabel(info.name, this);
    QFont nameFont = name->font();
    nameFont.setPointSize(13);
    nameFont.setBold(true);
    name->setFont(nameFont);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *type = new QLabel(info.typeDescription(), this);
    type->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::muted().name()));
    auto *nameColumn = new QVBoxLayout();
    nameColumn->setSpacing(2);
    nameColumn->addWidget(name);
    nameColumn->addWidget(type);
    header->addWidget(icon);
    header->addLayout(nameColumn, 1);
    layout->addLayout(header);

    auto *details = new QGroupBox(QStringLiteral("常规"), this);
    auto *form = new QFormLayout(details);
    form->setContentsMargins(14, 18, 14, 12);
    form->setSpacing(8);
    auto addValue = [&](const QString &label, const QString &value) {
        auto *field = new QLabel(value.isEmpty() ? QStringLiteral("—") : value, details);
        field->setTextInteractionFlags(Qt::TextSelectableByMouse);
        field->setWordWrap(true);
        form->addRow(label, field);
    };
    addValue(QStringLiteral("位置："), remote ? info.path : QDir::toNativeSeparators(info.path));
    addValue(QStringLiteral("大小："), info.isDir ? QStringLiteral("—") : path::formatSize(info.size));
    addValue(QStringLiteral("修改时间："), path::formatDateTime(info.modified));
    addValue(QStringLiteral("访问时间："), path::formatDateTime(info.accessed));
    addValue(QStringLiteral("权限："), info.permissionsString());
    if (!info.owner.isEmpty())
        addValue(QStringLiteral("所有者："), info.owner);
    if (!info.group.isEmpty())
        addValue(QStringLiteral("用户组："), info.group);
    if (info.isSymlink)
        addValue(QStringLiteral("链接目标："), info.linkTarget);
    layout->addWidget(details);

    if (m_canEditPermissions) {
        auto *permissions = new QGroupBox(QStringLiteral("权限"), this);
        auto *permissionLayout = new QVBoxLayout(permissions);
        permissionLayout->setContentsMargins(14, 18, 14, 12);
        auto *grid = new QGridLayout();
        grid->setHorizontalSpacing(24);
        grid->setVerticalSpacing(8);
        grid->addWidget(new QLabel(QStringLiteral(""), permissions), 0, 0);
        grid->addWidget(new QLabel(QStringLiteral("读取"), permissions), 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("写入"), permissions), 0, 2);
        grid->addWidget(new QLabel(QStringLiteral("执行"), permissions), 0, 3);
        const QStringList rows = {QStringLiteral("所有者"), QStringLiteral("用户组"),
                                  QStringLiteral("其他")};
        const quint32 mode = info.mode != 0 ? info.mode
                                            : (info.isDir ? 040755u : 0100644u);
        for (int row = 0; row < 3; ++row) {
            grid->addWidget(new QLabel(rows.at(row), permissions), row + 1, 0);
            for (int column = 0; column < 3; ++column) {
                auto *check = new QCheckBox(permissions);
                const int shift = (2 - row) * 3 + (2 - column);
                check->setChecked((mode >> shift) & 1);
                connect(check, &QCheckBox::toggled, this,
                        &PropertiesDialog::updatePermissionText);
                m_permissions[row][column] = check;
                grid->addWidget(check, row + 1, column + 1);
            }
        }
        permissionLayout->addLayout(grid);
        auto *octalRow = new QHBoxLayout();
        octalRow->addWidget(new QLabel(QStringLiteral("八进制："), permissions));
        m_permissionValue = new QLabel(permissions);
        QFont mono(QStringLiteral("Cascadia Mono"));
        mono.setStyleHint(QFont::Monospace);
        m_permissionValue->setFont(mono);
        octalRow->addWidget(m_permissionValue);
        octalRow->addStretch(1);
        permissionLayout->addLayout(octalRow);
        layout->addWidget(permissions);
        updatePermissionText();
        if (remote) {
            auto *note = new QLabel(
                QStringLiteral("提示：SFTP 和部分 FTP 服务器支持 Unix 权限。修改后立即生效。"),
                permissions);
            note->setStyleSheet(QStringLiteral("color: %1; font-size: 9pt;").arg(Theme::muted().name()));
            note->setWordWrap(true);
            permissionLayout->addWidget(note);
        }
    }

    layout->addStretch(1);
    auto *buttons = new QDialogButtonBox(this);
    if (m_canEditPermissions) {
        buttons->addButton(QStringLiteral("保存"), QDialogButtonBox::AcceptRole);
        buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    } else {
        buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
    }
    for (QPushButton *button : buttons->findChildren<QPushButton *>()) {
        if (button->text() == QLatin1String("保存"))
            button->setProperty("accent", true);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

quint32 PropertiesDialog::mode() const
{
    quint32 result = 0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (m_permissions[row][column] && m_permissions[row][column]->isChecked()) {
                const int shift = (2 - row) * 3 + (2 - column);
                result |= (1u << shift);
            }
        }
    }
    return result;
}

void PropertiesDialog::updatePermissionText()
{
    if (!m_permissionValue)
        return;
    m_permissionValue->setText(QStringLiteral("0%1").arg(mode(), 3, 8, QLatin1Char('0')));
}

} // namespace wsc
