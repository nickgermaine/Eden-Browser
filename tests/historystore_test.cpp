#include "core/history/historystore.h"
#include "core/profiles/sqlcipherdatabase.h"

#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

#include <sqlcipher/sqlite3.h>

using eden::core::HistoryStore;
using eden::core::ProfileError;
using eden::core::SqlCipherDatabase;

class HistoryFixture final {
  public:
    HistoryFixture() {
        if (directory.isValid() && SqlCipherDatabase::initialize(path(), key)) {
            database = SqlCipherDatabase::open(path(), key, false);
        }
    }

    ~HistoryFixture() {
        QThreadPool::globalInstance()->waitForDone();
        sqlite3_close_v2(database);
    }

    QString path() const {
        return directory.filePath("history.sqlite");
    }

    bool execute(const QByteArray &sql) const {
        return SqlCipherDatabase::execute(database, sql);
    }

    bool matches(const HistoryStore &store) const {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                database,
                "SELECT url,title,visited_at,visit_count FROM history ORDER BY visited_at DESC,url ASC LIMIT 1000",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return false;
        }
        const auto cleanup = qScopeGuard([statement] { sqlite3_finalize(statement); });
        int row = 0;
        int step;
        while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
            const auto text = [statement](int column) {
                return QString::fromUtf8(
                    reinterpret_cast<const char *>(sqlite3_column_text(statement, column)),
                    sqlite3_column_bytes(statement, column)
                );
            };
            const QModelIndex index = store.index(row++);
            if (!index.isValid() || store.data(index, HistoryStore::UrlRole).toUrl() != QUrl(text(0)) ||
                store.data(index, HistoryStore::TitleRole).toString() != text(1) ||
                store.data(index, HistoryStore::VisitedAtRole).toDateTime().toMSecsSinceEpoch() !=
                    sqlite3_column_int64(statement, 2) ||
                store.data(index, HistoryStore::VisitCountRole).toLongLong() != sqlite3_column_int64(statement, 3)) {
                return false;
            }
        }
        return step == SQLITE_DONE && row == store.rowCount();
    }

    QTemporaryDir directory;
    const QByteArray key = QByteArray(32, 'h');
    sqlite3 *database = nullptr;
};

class HistoryStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void keepsQtResponsiveWhileDatabaseIsLocked();
    void updatesRowsWithoutResettingPersistentIndexes();
    void preservesQueuedMutationOrder();
    void finishesQueuedWritesAfterStoreDestruction();
    void closesAfterPendingWritesAndRejectsLaterVisits();
    void preservesTheNewestThousandRows_data();
    void preservesTheNewestThousandRows();
    void reportsFailedWritesAndRecovers_data();
    void reportsFailedWritesAndRecovers();
    void rejectsFailedInitialReads();
    void usesTheHistoryOrderingIndex();
    void handlesReentrantCompletion();
};

void HistoryStoreTest::keepsQtResponsiveWhileDatabaseIsLocked() {
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    HistoryStore history(fixture.path());
    history.initialize(fixture.key);
    QVERIFY(history.isInitialized());
    QVERIFY(fixture.execute("BEGIN IMMEDIATE"));
    const auto unlock = qScopeGuard([&fixture] { fixture.execute("ROLLBACK"); });
    QElapsedTimer elapsed;
    elapsed.start();
    history.recordVisit(QUrl("https://responsive.invalid/"), "Responsive");
    QVERIFY2(elapsed.elapsed() < 150, "Recording a visit blocked the Qt thread on the database lock");
    QVERIFY(history.hasPendingWrites());
    QCOMPARE(history.rowCount(), 0);
    bool timerDelivered = false;
    QTimer::singleShot(40, &history, [&fixture, &timerDelivered] {
        timerDelivered = true;
        fixture.execute("ROLLBACK");
    });
    QTRY_VERIFY(!history.hasPendingWrites());
    QVERIFY(timerDelivered);
    QCOMPARE(history.error(), ProfileError::None);
    QCOMPARE(history.rowCount(), 1);
    QVERIFY(fixture.matches(history));
}

void HistoryStoreTest::updatesRowsWithoutResettingPersistentIndexes() {
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    QVERIFY(fixture.execute(
        "INSERT INTO history VALUES('https://a.invalid/','A',1,1),"
        "('https://b.invalid/','B',2,3)"
    ));
    HistoryStore history(fixture.path());
    history.initialize(fixture.key);
    QAbstractItemModelTester tester(&history, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const QPersistentModelIndex a(history.index(1));
    const QPersistentModelIndex b(history.index(0));
    QSignalSpy resets(&history, &QAbstractItemModel::modelReset);
    QSignalSpy moves(&history, &QAbstractItemModel::rowsMoved);
    QSignalSpy changes(&history, &QAbstractItemModel::dataChanged);
    const QString title = QStringLiteral("New\0title");
    history.recordVisit(QUrl("https://a.invalid/"), title);
    QTRY_VERIFY(!history.hasPendingWrites());
    QCOMPARE(a.row(), 0);
    QCOMPARE(b.row(), 1);
    QCOMPARE(a.data(HistoryStore::TitleRole).toString(), title);
    QCOMPARE(a.data(HistoryStore::VisitCountRole).toLongLong(), 2);
    QCOMPARE(moves.count(), 1);
    QCOMPARE(changes.count(), 1);
    const QList<int> roles = qvariant_cast<QList<int>>(changes.at(0).at(2));
    QVERIFY(roles.contains(HistoryStore::TitleRole));
    QVERIFY(roles.contains(HistoryStore::VisitedAtRole));
    QVERIFY(roles.contains(HistoryStore::VisitCountRole));
    history.remove(QUrl("https://a.invalid/"));
    QTRY_VERIFY(!history.hasPendingWrites());
    QVERIFY(!a.isValid());
    QCOMPARE(b.row(), 0);
    history.clear();
    QTRY_VERIFY(!history.hasPendingWrites());
    QVERIFY(!b.isValid());
    QCOMPARE(history.rowCount(), 0);
    QCOMPARE(resets.count(), 0);
    QVERIFY(fixture.matches(history));
}

void HistoryStoreTest::preservesQueuedMutationOrder() {
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    HistoryStore history(fixture.path());
    history.initialize(fixture.key);
    QAbstractItemModelTester tester(&history, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy resets(&history, &QAbstractItemModel::modelReset);
    const QUrl a("https://a.invalid/");
    const QUrl b("https://b.invalid/");
    for (int visit = 0; visit < 100; ++visit) {
        history.recordVisit(a, "A");
        history.recordVisit(b, "B");
        history.remove(a);
    }
    history.clear();
    history.recordVisit(a, "First");
    history.recordVisit(a, "Last");
    QTRY_VERIFY_WITH_TIMEOUT(!history.hasPendingWrites(), 15000);
    QCOMPARE(history.rowCount(), 1);
    QCOMPARE(history.data(history.index(0), HistoryStore::TitleRole).toString(), QString("Last"));
    QCOMPARE(history.data(history.index(0), HistoryStore::VisitCountRole).toLongLong(), 2);
    QCOMPARE(resets.count(), 0);
    QVERIFY(fixture.matches(history));
}

void HistoryStoreTest::finishesQueuedWritesAfterStoreDestruction() {
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    auto history = std::make_unique<HistoryStore>(fixture.path());
    history->initialize(fixture.key);
    QVERIFY(fixture.execute("BEGIN IMMEDIATE"));
    const auto unlock = qScopeGuard([&fixture] { fixture.execute("ROLLBACK"); });
    const QUrl url("https://shutdown.invalid/");
    history->recordVisit(url, "First");
    history->recordVisit(url, "Last");
    QElapsedTimer elapsed;
    elapsed.start();
    history.reset();
    QVERIFY2(elapsed.elapsed() < 150, "Destroying the history model waited for disk writes");
    QVERIFY(fixture.execute("ROLLBACK"));
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    HistoryStore restored(fixture.path());
    restored.initialize(fixture.key);
    QCOMPARE(restored.rowCount(), 1);
    QCOMPARE(restored.data(restored.index(0), HistoryStore::TitleRole).toString(), QString("Last"));
    QCOMPARE(restored.data(restored.index(0), HistoryStore::VisitCountRole).toLongLong(), 2);
    QVERIFY(fixture.matches(restored));
}

void HistoryStoreTest::closesAfterPendingWritesAndRejectsLaterVisits() {
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    HistoryStore history(fixture.path());
    history.initialize(fixture.key);
    QVERIFY(fixture.execute("BEGIN IMMEDIATE"));
    const auto unlock = qScopeGuard([&fixture] { fixture.execute("ROLLBACK"); });
    history.recordVisit(QUrl("https://close.invalid/"), "Preserved");
    int completions = 0;
    history.close([&completions] { ++completions; });
    history.close([&completions] { ++completions; });
    history.recordVisit(QUrl("https://late.invalid/"), "Late");
    QCOMPARE(completions, 0);
    QVERIFY(!history.isInitialized());
    QVERIFY(fixture.execute("ROLLBACK"));
    QTRY_COMPARE(completions, 2);
    QVERIFY(!history.hasPendingWrites());
    QCOMPARE(history.rowCount(), 1);
    QVERIFY(fixture.matches(history));
    history.close([&completions] { ++completions; });
    QCOMPARE(completions, 3);
    HistoryStore restored(fixture.path());
    restored.initialize(fixture.key);
    QCOMPARE(restored.rowCount(), 1);
}

void HistoryStoreTest::preservesTheNewestThousandRows_data() {
    QTest::addColumn<qint64>("timestamp");
    QTest::newRow("ordinary-clock") << qint64(1);
    QTest::newRow("clock-moved-backwards") << QDateTime::currentMSecsSinceEpoch() + 86400000;
}

void HistoryStoreTest::preservesTheNewestThousandRows() {
    QFETCH(qint64, timestamp);
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    QVERIFY(fixture.execute("BEGIN IMMEDIATE"));
    for (int row = 0; row < 1005; ++row) {
        const QByteArray suffix = QByteArray::number(row).rightJustified(4, '0');
        QVERIFY(fixture.execute(
            "INSERT INTO history VALUES('https://row.invalid/" + suffix + "','Row'," + QByteArray::number(timestamp) +
            ",1)"
        ));
    }
    QVERIFY(fixture.execute("COMMIT"));
    HistoryStore history(fixture.path());
    history.initialize(fixture.key);
    QAbstractItemModelTester tester(&history, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QCOMPARE(history.rowCount(), 1000);
    QVERIFY(fixture.matches(history));
    const QList<QUrl> visits{
        QUrl("https://row.invalid/0500"),
        QUrl("https://row.invalid/1004"),
        QUrl("https://new.invalid/"),
        QUrl(QString::fromUtf8("https://unicode.invalid/\xee\x80\x80")),
        QUrl(QString::fromUtf8("https://unicode.invalid/\xf0\x90\x80\x80"))
    };
    for (const QUrl &url : visits) {
        history.recordVisit(url, "Visited");
        QTRY_VERIFY(!history.hasPendingWrites());
        QVERIFY(fixture.matches(history));
    }
    const QList<QUrl> removals{
        QUrl("https://row.invalid/0000"),
        QUrl("https://row.invalid/1003"),
        QUrl("https://missing.invalid/"),
        QUrl("https://row.invalid/0999")
    };
    for (const QUrl &url : removals) {
        history.remove(url);
        QTRY_VERIFY(!history.hasPendingWrites());
        QVERIFY(fixture.matches(history));
    }
}

void HistoryStoreTest::reportsFailedWritesAndRecovers_data() {
    QTest::addColumn<bool>("remove");
    QTest::newRow("insert") << false;
    QTest::newRow("delete") << true;
}

void HistoryStoreTest::reportsFailedWritesAndRecovers() {
    QFETCH(bool, remove);
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    const QUrl url("https://failure.invalid/");
    QVERIFY(fixture.execute("INSERT INTO history VALUES('https://failure.invalid/','Preserved',1,1)"));
    HistoryStore history(fixture.path());
    history.initialize(fixture.key);
    const QByteArray event = remove ? "DELETE" : "INSERT";
    QVERIFY(fixture.execute(
        "CREATE TRIGGER reject_history BEFORE " + event + " ON history BEGIN SELECT RAISE(ABORT,'fixture failure'); END"
    ));
    QSignalSpy changes(&history, &HistoryStore::changed);
    QSignalSpy errors(&history, &HistoryStore::errorChanged);
    if (remove) {
        history.remove(url);
    } else {
        history.recordVisit(url, "Replacement");
    }
    QTRY_VERIFY(!history.hasPendingWrites());
    QCOMPARE(history.error(), ProfileError::HistoryWriteFailed);
    QCOMPARE(changes.count(), 0);
    QCOMPARE(errors.count(), 1);
    QCOMPARE(history.data(history.index(0), HistoryStore::TitleRole).toString(), QString("Preserved"));
    QVERIFY(fixture.matches(history));
    QVERIFY(fixture.execute("DROP TRIGGER reject_history"));
    if (remove) {
        history.clear();
    } else {
        history.recordVisit(url, "Replacement");
    }
    QTRY_VERIFY(!history.hasPendingWrites());
    QCOMPARE(history.error(), ProfileError::None);
    QCOMPARE(changes.count(), 1);
    QCOMPARE(errors.count(), 2);
    QVERIFY(fixture.matches(history));
}

void HistoryStoreTest::rejectsFailedInitialReads() {
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    HistoryStore history(fixture.path());
    history.initialize(QByteArray(31, 'h'));
    QVERIFY(!history.isInitialized());
    QCOMPARE(history.error(), ProfileError::HistoryReadFailed);
    history.initialize(QByteArray(32, 'x'));
    QVERIFY(!history.isInitialized());
    QCOMPARE(history.error(), ProfileError::HistoryReadFailed);
    QVERIFY(fixture.execute("ALTER TABLE history RENAME TO preserved_history"));
    history.initialize(fixture.key);
    QVERIFY(!history.isInitialized());
    QCOMPARE(history.error(), ProfileError::HistoryReadFailed);
    QVERIFY(fixture.execute("ALTER TABLE preserved_history RENAME TO history"));
    history.initialize(fixture.key);
    QVERIFY(history.isInitialized());
    QCOMPARE(history.error(), ProfileError::None);
}

void HistoryStoreTest::usesTheHistoryOrderingIndex() {
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    sqlite3_stmt *statement = nullptr;
    QCOMPARE(
        sqlite3_prepare_v2(
            fixture.database,
            "EXPLAIN QUERY PLAN SELECT title,url,visited_at,visit_count FROM history "
            "ORDER BY visited_at DESC,url ASC LIMIT 1000",
            -1,
            &statement,
            nullptr
        ),
        SQLITE_OK
    );
    const auto cleanup = qScopeGuard([statement] { sqlite3_finalize(statement); });
    QByteArray plan;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        plan += reinterpret_cast<const char *>(sqlite3_column_text(statement, 3));
    }
    QVERIFY2(plan.contains("history_visited_at"), plan.constData());
    QVERIFY2(!plan.contains("TEMP B-TREE"), plan.constData());
}

void HistoryStoreTest::handlesReentrantCompletion() {
    HistoryFixture fixture;
    QVERIFY(fixture.database);
    HistoryStore history(fixture.path());
    history.initialize(fixture.key);
    int changes = 0;
    connect(&history, &HistoryStore::changed, &history, [&history, &changes] {
        ++changes;
        if (changes == 1) {
            history.recordVisit(QUrl("https://nested.invalid/"), "Nested");
            QEventLoop loop;
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
            loop.exec();
        }
    });
    history.recordVisit(QUrl("https://first.invalid/"), "First");
    QTRY_VERIFY(!history.hasPendingWrites());
    QCOMPARE(changes, 2);
    QCOMPARE(history.rowCount(), 2);
    QVERIFY(fixture.matches(history));
}

QTEST_GUILESS_MAIN(HistoryStoreTest)

#include "historystore_test.moc"
