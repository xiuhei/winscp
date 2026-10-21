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
#include "ui/SessionWorkspace.h"
#include "core/FileSystemController.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueModel.h"
#include "proto/LocalBackend.h"

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
#include <QScopeGuard>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTableView>
#include <QTabBar>
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

    void directoryRefreshPreservesRows()
    {
        FileInfo removed;
        removed.name = QStringLiteral("removed");
        removed.path = QStringLiteral("/removed");
        FileInfo changed;
        changed.name = QStringLiteral("changed");
        changed.path = QStringLiteral("/changed");
        FileInfo unchanged;
        unchanged.name = QStringLiteral("unchanged");
        unchanged.path = QStringLiteral("/unchanged");
        FileInfo added;
        added.name = QStringLiteral("added");
        added.path = QStringLiteral("/added");
        FileTableModel model(false);
        model.setEntries({removed, changed, unchanged});
        const QPersistentModelIndex retained(model.index(2, 0));
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy updates(&model, &QAbstractItemModel::dataChanged);
        changed.size = 123;
        model.updateEntries({added, unchanged, changed});
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.entryAt(0), changed);
        QCOMPARE(model.entryAt(1), unchanged);
        QCOMPARE(model.entryAt(2), added);
        QVERIFY(retained.isValid());
        QCOMPARE(retained.row(), 1);
        QCOMPARE(retained.data().toString(), unchanged.name);
        QCOMPARE(updates.size(), 1);
        QVERIFY(reset.isEmpty());
        model.updateEntries({added, unchanged, changed});
        QCOMPARE(updates.size(), 1);
        model.updateEntries({});
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!retained.isValid());
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

    void sessionTabsKeepViewsAndShareTransfers()
    {
        const bool showLogin = Settings::instance().showLoginDialog;
        const auto restore = qScopeGuard([&] { Settings::instance().showLoginDialog = showLogin; });
        Settings::instance().showLoginDialog = false;
        MainWindow window;
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("SessionTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QString());
        QVERIFY(!tabs->tabButton(0, QTabBar::RightSide));
        SessionData first;
        first.siteName = QStringLiteral("生产服务器");
        first.hostName = QStringLiteral("first.example");
        window.setInitialSession(first, false);
        auto *firstView = window.activeWorkspace();
        SessionData second = first;
        second.siteName = QStringLiteral("测试服务器");
        second.hostName = QStringLiteral("second.example");
        window.setInitialSession(second, false);
        QPointer<SessionWorkspace> secondView = window.activeWorkspace();
        QVERIFY(firstView != secondView);
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(0), first.siteName);
        QCOMPARE(tabs->tabText(1), second.siteName);
        QCOMPARE(tabs->tabText(2), QString());
        QVERIFY(!tabs->tabButton(2, QTabBar::RightSide));
        QCOMPARE(window.findChildren<TransferManager *>().size(), 1);
        QCOMPARE(window.findChildren<QDialog *>(QStringLiteral("TransferQueueWindow")).size(), 1);
        QCOMPARE(window.findChildren<FileSystemController *>().size(), 2);
        tabs->setCurrentIndex(0);
        QCOMPARE(window.activeWorkspace(), firstView);
        QVERIFY(firstView->isVisible());
        QVERIFY(!secondView->isVisible());
        tabs->tabCloseRequested(1);
        QTRY_VERIFY(secondView.isNull());
        QCOMPARE(window.activeWorkspace(), firstView);
        QCOMPARE(tabs->count(), 2);
        tabs->tabCloseRequested(0);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QString());
        QVERIFY(window.activeWorkspace());
        QVERIFY(window.activeWorkspace()->session().hostName.isEmpty());
    }

    void blankSessionTabOpensLoginWithoutLosingSelection()
    {
        MainWindow window;
        window.show();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("SessionTabs"));
        int shown = 0;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, &window, [&] {
            auto *login = window.findChild<LoginDialog *>();
            if (login && login->isVisible()) {
                ++shown;
                login->reject();
            }
        });
        dismiss.start(10);
        QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
        QTRY_COMPARE(shown, 1);
        QCOMPARE(tabs->count(), 1);
        SessionData first;
        first.hostName = QStringLiteral("first.example");
        window.setInitialSession(first, false);
        auto *view = window.activeWorkspace();
        QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(1).center());
        QTRY_COMPARE(shown, 2);
        QTest::qWait(50);
        QCOMPARE(shown, 2);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(window.activeWorkspace(), view);
    }

    void closingSessionDoesNotStopSharedTransfer()
    {
        const bool showLogin = Settings::instance().showLoginDialog;
        const auto restoreLogin = qScopeGuard([&] { Settings::instance().showLoginDialog = showLogin; });
        Settings::instance().showLoginDialog = false;
        MainWindow window;
        SessionData session;
        session.hostName = QStringLiteral("first.example");
        window.setInitialSession(session, false);
        QPointer<SessionWorkspace> view = window.activeWorkspace();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("SessionTabs"));
        auto *transfers = window.findChild<TransferManager *>();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath(QStringLiteral("payload.txt"));
        const QString targetDirectory = directory.filePath(QStringLiteral("target"));
        QVERIFY(QDir().mkpath(targetDirectory));
        LocalBackend files;
        QString error;
        const QByteArray payload("transfer survives closing its session tab");
        QVERIFY(files.writeFile(source, payload, &error));
        const QString target = QDir(targetDirectory).filePath(QStringLiteral("payload.txt"));
        QVERIFY(files.writeFile(target, "original", &error));
        const QString policy = Settings::instance().conflictPolicy;
        const auto restore = qScopeGuard([&] { Settings::instance().conflictPolicy = policy; });
        Settings::instance().conflictPolicy = QStringLiteral("ask");
        bool closed = false;
        transfers->setConflictResolver([&](const QString &, TransferDirection, qint64, qint64) {
            tabs->tabCloseRequested(0);
            closed = true;
            return ConflictAction::Overwrite;
        });
        QSignalSpy finished(transfers, &TransferManager::allTransfersFinished);
        transfers->enqueueDownload(SessionData::localSession(), {LocalBackend::infoForPath(source)}, targetDirectory);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(closed);
        QTRY_VERIFY(view.isNull());
        QCOMPARE(transfers->model()->rows().last().status, TransferStatus::Completed);
        QByteArray actual;
        QVERIFY(files.readFile(target, &actual, &error));
        QCOMPARE(actual, payload);
        QCOMPARE(window.findChildren<QDialog *>(QStringLiteral("TransferQueueWindow")).size(), 1);
    }

    void closingLastSessionHonorsLoginPreference()
    {
        const bool showLogin = Settings::instance().showLoginDialog;
        const auto restore = qScopeGuard([&] { Settings::instance().showLoginDialog = showLogin; });
        Settings::instance().showLoginDialog = true;
        MainWindow window;
        window.show();
        SessionData session;
        session.hostName = QStringLiteral("first.example");
        window.setInitialSession(session, false);
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("SessionTabs"));
        int shown = 0;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, &window, [&] {
            auto *login = window.findChild<LoginDialog *>();
            if (login && login->isVisible()) {
                ++shown;
                login->reject();
            }
        });
        dismiss.start(10);
        tabs->tabCloseRequested(0);
        QTRY_COMPARE(shown, 1);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(tabs->tabText(0).isEmpty());
        QTest::qWait(50);
        QCOMPARE(shown, 1);
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
