#include "core/AppPaths.h"
#include "core/Settings.h"
#include "ui/FilePane.h"
#include "ui/FileTableModel.h"
#include "ui/IconProvider.h"
#include "ui/LoginDialog.h"
#include "ui/SessionEditor.h"
#include "ui/Theme.h"
#include "ui/TextEditorDialog.h"
#include "ui/MainWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QJsonObject>
#include <QLineEdit>
#include <QFile>
#include <QMessageBox>
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

    void sessionConfigurationRoundTrip()
    {
        SessionData session;
        QString error;
        QVERIFY2(SessionData::parseUrl(QStringLiteral("tester@example.test:2222/logs"), &session, &error),
                 qPrintable(error));
        QCOMPARE(session.protocol, Protocol::Sftp);
        QCOMPARE(session.hostName, QStringLiteral("example.test"));
        QCOMPARE(session.port, 2222);
        QCOMPARE(session.userName, QStringLiteral("tester"));
        QCOMPARE(session.remoteDirectory, QStringLiteral("/logs"));
        QCOMPARE(session.url(), QStringLiteral("sftp://tester@example.test:2222/logs"));

        session.privateKeyFile = QStringLiteral("id_ed25519");
        session.timeoutSeconds = 45;
        session.keepAliveSeconds = 60;
        QJsonObject stored = session.toJson();
        stored.insert(QStringLiteral("compression"), true);
        stored.insert(QStringLiteral("passiveMode"), false);
        stored.insert(QStringLiteral("maxConnections"), 8);
        stored.insert(QStringLiteral("agentForwarding"), true);
        stored.insert(QStringLiteral("rawSettings"), QStringLiteral("unused"));
        const SessionData restored = SessionData::fromJson(stored);
        QCOMPARE(restored.url(), session.url());
        QCOMPARE(restored.privateKeyFile, session.privateKeyFile);
        QCOMPARE(restored.timeoutSeconds, 45);
        QCOMPARE(restored.keepAliveSeconds, 60);
        QCOMPARE(restored.toJson(), session.toJson());

        QVERIFY(SessionData::parseUrl(QStringLiteral("ftps://tester@example.test:990/?insecure=1&implicit=1"),
                                     &session, &error));
        QCOMPARE(session.protocol, Protocol::Ftps);
        QVERIFY(session.implicitTls);
        QVERIFY(!session.tlsVerifyCertificate);
        const SessionData ftps = SessionData::fromJson(session.toJson());
        QCOMPARE(ftps.url(), session.url());
        QVERIFY(ftps.implicitTls);
        QVERIFY(!ftps.tlsVerifyCertificate);
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

    void editorRetainsChangesAfterFailedUpload()
    {
        TextEditorDialog dialog(QStringLiteral("/log"), "original\n");
        auto *editor = dialog.findChild<QPlainTextEdit *>();
        editor->appendPlainText(QStringLiteral("unsaved"));
        const QByteArray edited = dialog.content();
        QSignalSpy requested(&dialog, &TextEditorDialog::saveRequested);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QPushButton *save = nullptr;
        for (auto *button : buttons->buttons())
            if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole)
                save = qobject_cast<QPushButton *>(button);
        QVERIFY(save);
        save->click();
        QCOMPARE(requested.size(), 1);
        QVERIFY(editor->isReadOnly());
        QCOMPARE(accepted.size(), 0);
        dialog.reject();
        QCOMPARE(dialog.content(), edited);
        dialog.finishSave(false, QStringLiteral("权限不足"));
        QVERIFY(!editor->isReadOnly());
        QVERIFY(save->isEnabled());
        QCOMPARE(dialog.content(), edited);
        QVERIFY(editor->document()->isModified());
        save->click();
        QCOMPARE(requested.size(), 2);
        dialog.finishSave(true);
        QCOMPARE(accepted.size(), 1);
    }

    void localCopyRejectsSelfAndDescendants()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath(QStringLiteral("source.txt"));
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original");
        file.close();
        MainWindow window;
        FilePane *local = nullptr;
        for (auto *pane : window.findChildren<FilePane *>())
            if (!pane->isRemote())
                local = pane;
        QVERIFY(local);
        int warnings = 0;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, &window, [&] {
            for (auto *message : window.findChildren<QMessageBox *>()) {
                ++warnings;
                message->accept();
            }
        });
        dismiss.start(10);
        local->externalFilesDropped({QUrl::fromLocalFile(source)}, directory.path(), false);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
        file.close();
        const QString tree = directory.filePath(QStringLiteral("tree"));
        const QString nested = QDir(tree).filePath(QStringLiteral("sub"));
        QVERIFY(QDir().mkpath(nested));
        local->externalFilesDropped({QUrl::fromLocalFile(tree)}, nested, false);
        QVERIFY(!QFileInfo::exists(QDir(nested).filePath(QStringLiteral("tree"))));
        QCOMPARE(warnings, 2);
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
