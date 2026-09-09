#include "core/bookmarks/bookmarkstore.h"
#include "core/history/historystore.h"
#include "core/omni/omniboxcontroller.h"
#include "core/profiles/profiledatabase.h"
#include "core/window/tabmodel.h"
#include "engine/engineview.h"

#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtTest>

class OmniboxEngineView final : public eden::engine::EngineView {
    Q_OBJECT

  public:
    QUrl url() const override {
        return m_url;
    }
    QString title() const override {
        return m_url.host();
    }
    QUrl faviconUrl() const override {
        return {};
    }
    int loadProgress() const override {
        return 100;
    }
    bool isLoading() const override {
        return false;
    }
    bool canGoBack() const override {
        return false;
    }
    bool canGoForward() const override {
        return false;
    }
    bool isAudible() const override {
        return false;
    }
    bool isMuted() const override {
        return false;
    }
    QString securityState() const override {
        return "secure";
    }
    QString backendName() const override {
        return "Fake";
    }
    Capabilities capabilities() const override {
        return {};
    }
    void load(const QUrl &url) override {
        m_url = url;
        emit urlChanged();
        emit titleChanged();
    }
    void back() override {}
    void forward() override {}
    QVariantList navigationHistory(int, int) const override {
        return {};
    }
    void goToHistoryOffset(int) override {}
    void reload() override {}
    void stop() override {}
    void openDevTools() override {}
    void findInPage(const QString &, FindFlags) override {}
    void attach(QQuickItem *) override {}
    void setMuted(bool) override {}

  private:
    QUrl m_url;
};

class OmniboxControllerTest final : public QObject {
    Q_OBJECT

  private slots:
    void strongHistoryPrefixOutranksWeakOpenTabMatch();
    void plaintextDatabaseMigratesToCiphertext();
    void corruptDatabaseIsNotReplaced();
};

void OmniboxControllerTest::strongHistoryPrefixOutranksWeakOpenTabMatch() {
    QTemporaryDir dataDirectory;
    QVERIFY(dataDirectory.isValid());
    const QString databasePath = dataDirectory.path() + "/eden.sqlite";
    const QByteArray databaseKey(32, 'k');
    eden::core::ProfileDatabase database(databasePath);
    QVERIFY(database.initialize(databaseKey));
    QFile encryptedDatabase(databasePath);
    QVERIFY(encryptedDatabase.open(QIODevice::ReadOnly));
    QVERIFY(!encryptedDatabase.read(16).startsWith("SQLite format 3"));
    QVERIFY(!QFileInfo::exists(databasePath + ".plaintext-migration"));
    QVERIFY(!QFileInfo::exists(databasePath + ".encrypting"));
    eden::core::TabModel tabs([] { return std::make_unique<OmniboxEngineView>(); }, false);
    tabs.addTab(QUrl("https://forging.it/help"));
    eden::core::HistoryStore history(databasePath);
    history.initialize(databaseKey);
    eden::core::BookmarkStore bookmarks(databasePath);
    bookmarks.initialize(databaseKey);
    const QUrl github("https://github.com/");
    history.recordVisit(github, "GitHub");

    eden::core::OmniboxController controller(&tabs, &history, &bookmarks);
    controller.setQuery("gith");
    QTRY_VERIFY_WITH_TIMEOUT(controller.rowCount() > 0, 200);
    QCOMPARE(controller.data(controller.index(0), eden::core::OmniboxController::UrlRole).toUrl(), github);
    QCOMPARE(controller.completionSuffix(), QString("ub.com/"));

    controller.setQuery("different words");
    QCOMPARE(controller.completionSuffix(), QString());
    QCOMPARE(controller.suggestionUrl(0), controller.destination("different words"));

    controller.setQuery({});
    QCOMPARE(controller.rowCount(), 0);
    QCOMPARE(controller.completionSuffix(), QString());
}

void OmniboxControllerTest::plaintextDatabaseMigratesToCiphertext() {
    QTemporaryDir dataDirectory;
    QVERIFY(dataDirectory.isValid());
    const QString databasePath = dataDirectory.path() + "/eden.sqlite";
    {
        QSqlDatabase plaintext = QSqlDatabase::addDatabase("QSQLITE", "plaintext-migration");
        plaintext.setDatabaseName(databasePath);
        QVERIFY(plaintext.open());
        QSqlQuery query(plaintext);
        QVERIFY(query.exec(
            "CREATE TABLE history(url TEXT PRIMARY KEY,title TEXT NOT NULL,visited_at INTEGER NOT NULL,"
            "visit_count INTEGER NOT NULL DEFAULT 1)"
        ));
        QVERIFY(query.exec("INSERT INTO history VALUES('https://work.example','Work',42,3)"));
        plaintext.close();
    }
    QSqlDatabase::removeDatabase("plaintext-migration");
    const QByteArray databaseKey(32, 'm');
    eden::core::ProfileDatabase database(databasePath);
    QVERIFY(database.initialize(databaseKey));
    QFile encryptedDatabase(databasePath);
    QVERIFY(encryptedDatabase.open(QIODevice::ReadOnly));
    QVERIFY(!encryptedDatabase.read(16).startsWith("SQLite format 3"));
    QVERIFY(!QFileInfo::exists(databasePath + ".plaintext-migration"));
    QVERIFY(!QFileInfo::exists(databasePath + ".encrypting"));
    eden::core::HistoryStore history(databasePath);
    history.initialize(databaseKey);
    QCOMPARE(history.rowCount(), 1);
    QCOMPARE(history.data(history.index(0), eden::core::HistoryStore::TitleRole).toString(), QString("Work"));
}

void OmniboxControllerTest::corruptDatabaseIsNotReplaced() {
    QTemporaryDir dataDirectory;
    QVERIFY(dataDirectory.isValid());
    const QString databasePath = dataDirectory.path() + "/eden.sqlite";
    const QByteArray original("not-a-database");
    QFile file(databasePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();
    eden::core::ProfileDatabase database(databasePath);
    QVERIFY(!database.initialize(QByteArray(32, 'x')));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
}

QTEST_GUILESS_MAIN(OmniboxControllerTest)

#include "omniboxcontroller_test.moc"
