#include "core/AppPaths.h"
#include "core/Settings.h"
#include "ui/FilePane.h"
#include "ui/FileTableModel.h"
#include "ui/IconProvider.h"
#include "ui/LoginDialog.h"
#include "ui/SessionEditor.h"
#include "ui/Theme.h"
#include "ui/TextEditorDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace wsc;

class UiWorkflowTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("WINSCPQT_HOME", m_home.path().toUtf8());
        AppPaths::ensureDirectories();
        Settings::instance().load();
        Theme::apply(QStringLiteral("light"));
    }

    void refreshPreservesView()
    {
        FilePane pane(FilePane::Role::Remote);
        pane.resize(700, 400);
        pane.showRemoteConnected(QStringLiteral("/"), QStringLiteral("/logs"));
        FileList files;
        for (int i = 0; i < 100; ++i) {
            FileInfo file;
            file.name = QStringLiteral("log%1").arg(i, 3, 10, QLatin1Char('0'));
            file.path = QStringLiteral("/logs/") + file.name;
            files.append(file);
        }
        pane.applyRemoteListing(QStringLiteral("/logs"), files);
        pane.show();
        QTest::qWait(50);
        auto *table = pane.findChild<QTableView *>();
        auto *model = pane.findChild<FileTableModel *>();
        pane.selectByName({QStringLiteral("log050")});
        table->selectionModel()->setCurrentIndex(table->model()->index(51, 0), QItemSelectionModel::NoUpdate);
        table->verticalScrollBar()->setValue(800);
        const int scroll = table->verticalScrollBar()->value();
        const QRect geometry = table->geometry();
        QSignalSpy reset(model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(model, &QAbstractItemModel::dataChanged);
        QSignalSpy selection(table->selectionModel(), &QItemSelectionModel::selectionChanged);
        pane.applyRemoteListing(QStringLiteral("/logs"), files);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(selection.size(), 0);
        QCOMPARE(table->geometry(), geometry);
        QCOMPARE(table->verticalScrollBar()->value(), scroll);
        files[50].size = 123;
        files.removeAt(1);
        FileInfo added;
        added.name = QStringLiteral("new-log");
        added.path = QStringLiteral("/logs/new-log");
        files.append(added);
        pane.applyRemoteListing(QStringLiteral("/logs"), files);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(pane.selectedEntries().size(), 1);
        QCOMPARE(pane.selectedEntries().first().name, QStringLiteral("log050"));
        QCOMPARE(pane.selectedEntries().first().size, 123);
        QCOMPARE(pane.currentEntry().name, QStringLiteral("log050"));
        QCOMPARE(table->geometry(), geometry);
        QCOMPARE(table->verticalScrollBar()->value(), scroll);
    }

    void loginAndSiteSaving()
    {
        LoginDialog dialog(SessionData{});
        dialog.show();
        auto *sites = dialog.findChild<QListWidget *>(QStringLiteral("LoginSites"));
        auto *editor = dialog.findChild<SessionEditor *>();
        auto *host = dialog.findChild<QLineEdit *>(QStringLiteral("SessionHost"));
        auto *user = dialog.findChild<QLineEdit *>(QStringLiteral("SessionUser"));
        auto *password = dialog.findChild<QLineEdit *>(QStringLiteral("SessionPassword"));
        QCOMPARE(sites->count(), 1);
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        host->setText(QStringLiteral("example.test"));
        user->setText(QStringLiteral("tester"));
        password->setText(QStringLiteral("test secret"));
        const auto save = [&](bool savePassword) {
            QTimer::singleShot(20, &dialog, [&] {
                auto *saveDialog = dialog.findChild<QDialog *>(QStringLiteral("SaveSiteDialog"));
                if (!saveDialog)
                    return;
                saveDialog->findChild<QLineEdit *>(QStringLiteral("SaveSiteName"))->setText(QStringLiteral("Test server"));
                saveDialog->findChild<QCheckBox *>(QStringLiteral("SaveSitePassword"))->setChecked(savePassword);
                const QString prefix = qEnvironmentVariable("WINSCPQT_UI_PREVIEW");
                if (!prefix.isEmpty())
                    saveDialog->grab().save(prefix + QStringLiteral("-save.png"));
                saveDialog->accept();
            });
            editor->saveRequested();
        };
        save(false);
        QCOMPARE(sites->count(), 2);
        QCOMPARE(dialog.result(), 0);
        SiteStore::instance().load();
        QVERIFY(SiteStore::instance().find(QStringLiteral("Test server")).password.isEmpty());
        password->setText(QStringLiteral("test secret"));
        save(true);
        SiteStore::instance().load();
        QCOMPARE(SiteStore::instance().find(QStringLiteral("Test server")).password, QStringLiteral("test secret"));
        sites->setCurrentRow(0);
        QVERIFY(host->text().isEmpty());
        QVERIFY(password->text().isEmpty());
        sites->setCurrentRow(1);
        QCOMPARE(host->text(), QStringLiteral("example.test"));
        QCOMPARE(password->text(), QStringLiteral("test secret"));
        const QString prefix = qEnvironmentVariable("WINSCPQT_UI_PREVIEW");
        if (!prefix.isEmpty()) {
            qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
            dialog.grab().save(prefix + QStringLiteral("-light.png"));
            Theme::apply(QStringLiteral("dark"));
            QApplication::processEvents();
            dialog.grab().save(prefix + QStringLiteral("-dark.png"));
        }
    }

    void credentialsRespectSaveChoice()
    {
        SessionData session;
        session.password = QStringLiteral("password");
        session.passphrase = QStringLiteral("private key secret");
        session.savePassword = false;
        const auto stored = sessionFromStoredJson(sessionToStoredJson(session, true));
        QVERIFY(stored.password.isEmpty());
        QVERIFY(stored.passphrase.isEmpty());
    }

    void editorPreservesTextFormat()
    {
        const QByteArray original = QByteArray::fromHex("efbbbf") + "first\r\nsecond\r\n";
        TextEditorDialog dialog(QStringLiteral("/log"), original);
        QCOMPARE(dialog.content(), original);
        dialog.findChild<QPlainTextEdit *>()->appendPlainText(QStringLiteral("third"));
        const QByteArray edited = dialog.content();
        QVERIFY(edited.startsWith(QByteArray::fromHex("efbbbf")));
        QVERIFY(edited.contains("second\r\n"));
        QVERIFY(!edited.contains("\r\r\n"));
    }

    void iconsFollowTheme()
    {
        Theme::apply(QStringLiteral("light"));
        const QIcon icon = IconProvider::instance().icon(QStringLiteral("refresh"));
        const QImage light = icon.pixmap(32).toImage();
        Theme::apply(QStringLiteral("dark"));
        const QImage dark = icon.pixmap(32).toImage();
        QVERIFY(light != dark);
        QVERIFY(Theme::iconColor().lightness() > 220);
        const QImage disabled = icon.pixmap(32, QIcon::Disabled).toImage();
        int brightPixels = 0;
        for (int y = 0; y < disabled.height(); ++y)
            for (int x = 0; x < disabled.width(); ++x)
                if (disabled.pixelColor(x, y).alpha() > 100 && disabled.pixelColor(x, y).lightness() > 140)
                    ++brightPixels;
        QVERIFY(brightPixels > 30);
    }

private:
    QTemporaryDir m_home;
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    Q_INIT_RESOURCE(resources);
    UiWorkflowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ui_workflow_test.moc"
