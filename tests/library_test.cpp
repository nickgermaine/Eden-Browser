#include "core/bookmarks/bookmarkstore.h"
#include "core/favicons/faviconstore.h"
#include "core/history/historystore.h"
#include "core/history/recentlyclosedstore.h"
#include "core/library/librarylistmodel.h"
#include "core/profiles/sqlcipherdatabase.h"

#include <QAbstractItemModelTester>
#include <QBuffer>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

using eden::core::BookmarkStore;
using eden::core::FaviconStore;
using eden::core::HistoryStore;
using eden::core::LibraryListModel;
using eden::core::RecentlyClosedStore;
using eden::core::SqlCipherDatabase;

static QUrl iconDataUrl(int extent, const QColor &color) {
    QImage image(extent, extent, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QUrl(QString("data:image/png;base64,%1").arg(QString::fromLatin1(encoded.toBase64())));
}

class LibraryTest final : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        m_directory = std::make_unique<QTemporaryDir>();
        QVERIFY(m_directory->isValid());
        m_path = m_directory->filePath("profile.sqlite");
        QVERIFY(SqlCipherDatabase::initialize(m_path, m_key));
    }

    void cleanup() {
        QThreadPool::globalInstance()->waitForDone();
        m_directory.reset();
    }

    void bookmarksKeepManualOrder() {
        {
            BookmarkStore store(m_path);
            store.initialize(m_key);
            store.add(QUrl("https://a.example/"), "A");
            store.add(QUrl("https://b.example/"), "B");
            store.add(QUrl("https://c.example/"), "C");
            QCOMPARE(store.data(store.index(0), BookmarkStore::TitleRole).toString(), QString("C"));
            QVERIFY(store.move(0, 2));
            QVERIFY(!store.move(0, 0));
            QVERIFY(!store.move(0, 7));
            QCOMPARE(store.data(store.index(0), BookmarkStore::TitleRole).toString(), QString("B"));
            QCOMPARE(store.data(store.index(2), BookmarkStore::TitleRole).toString(), QString("C"));
            store.add(QUrl("https://d.example/"), "D");
            QCOMPARE(store.data(store.index(0), BookmarkStore::TitleRole).toString(), QString("D"));
        }
        BookmarkStore reopened(m_path);
        reopened.initialize(m_key);
        QStringList titles;
        for (int row = 0; row < reopened.rowCount(); ++row) {
            titles.append(reopened.data(reopened.index(row), BookmarkStore::TitleRole).toString());
        }
        QCOMPARE(titles, QStringList({"D", "B", "A", "C"}));
    }

    void recentlyClosedPersistsAndCaps() {
        {
            RecentlyClosedStore store(m_path);
            QVERIFY(store.initialize(m_key));
            QAbstractItemModelTester tester(&store);
            store.add(QUrl("eden://settings"), "Settings");
            store.add(QUrl("about:blank"), "Blank");
            QCOMPARE(store.rowCount(), 0);
            for (int index = 0; index < RecentlyClosedStore::capacity + 5; ++index) {
                store.add(QUrl(QString("https://site%1.example/").arg(index)), QString::number(index));
            }
            QCOMPARE(store.rowCount(), RecentlyClosedStore::capacity);
            QCOMPARE(store.data(store.index(0), RecentlyClosedStore::TitleRole).toString(), QString("29"));
            const qint64 id = store.data(store.index(1), RecentlyClosedStore::ClosedIdRole).toLongLong();
            QCOMPARE(store.take(id), QUrl("https://site28.example/"));
            QVERIFY(store.take(id).isEmpty());
            store.removeLatest(QUrl("https://site29.example/"));
            QCOMPARE(store.data(store.index(0), RecentlyClosedStore::TitleRole).toString(), QString("27"));
        }
        RecentlyClosedStore reopened(m_path);
        QVERIFY(reopened.initialize(m_key));
        QCOMPARE(reopened.rowCount(), RecentlyClosedStore::capacity - 2);
        QCOMPARE(reopened.data(reopened.index(0), RecentlyClosedStore::TitleRole).toString(), QString("27"));
        reopened.clear();
        QCOMPARE(reopened.rowCount(), 0);
    }

    void faviconsAreSharedPerHostAndDownscaled() {
        {
            FaviconStore store(m_path);
            QVERIFY(store.initialize(m_key));
            QSignalSpy changed(&store, &FaviconStore::changed);
            store.record(QUrl("https://Example.com/page"), iconDataUrl(128, Qt::red));
            store.record(QUrl("https://example.com/other"), iconDataUrl(128, Qt::red));
            store.record(QUrl("file:///tmp/a.html"), iconDataUrl(16, Qt::red));
            store.record(QUrl("https://plain.example/"), QUrl("https://plain.example/favicon.ico"));
            QCOMPARE(changed.count(), 1);
            const QUrl icon = store.iconFor(QUrl("https://example.com/deep/link?x=1"));
            QVERIFY(icon.toString().startsWith("data:image/png;base64,"));
            const QImage decoded = QImage::fromData(QByteArray::fromBase64(icon.toString().section(',', 1).toLatin1()));
            QCOMPARE(decoded.width(), 48);
            QVERIFY(store.iconFor(QUrl("https://plain.example/")).isEmpty());
        }
        FaviconStore reopened(m_path);
        QVERIFY(reopened.initialize(m_key));
        QVERIFY(!reopened.iconFor(QUrl("http://example.com/")).isEmpty());
    }

    void timestampsFallIntoSections() {
        const qint64 now = QDateTime(QDate(2026, 9, 20), QTime(15, 0)).toMSecsSinceEpoch();
        const auto at = [](int day, int hour) { return QDateTime(QDate(2026, 9, day), QTime(hour, 0)).toMSecsSinceEpoch(); };
        QCOMPARE(LibraryListModel::sectionForTimestamp(at(20, 0), now), QString("recent"));
        QCOMPARE(LibraryListModel::sectionForTimestamp(at(19, 23), now), QString("yesterday"));
        QCOMPARE(LibraryListModel::sectionForTimestamp(at(18, 23), now), QString("week"));
        QCOMPARE(LibraryListModel::sectionForTimestamp(at(13, 0), now), QString("week"));
        QCOMPARE(LibraryListModel::sectionForTimestamp(at(12, 23), now), QString("earlier"));
    }

    void historyModelSectionsCollapseAndFilter() {
        HistoryStore history(m_path);
        history.initialize(m_key);
        RecentlyClosedStore closed(m_path);
        QVERIFY(closed.initialize(m_key));
        LibraryListModel model(LibraryListModel::Mode::History);
        QAbstractItemModelTester tester(&model);
        model.setHistory(&history, &closed);
        QCOMPARE(model.rowCount(), 0);

        QSignalSpy finished(&history, &HistoryStore::writesFinished);
        history.recordVisit(QUrl("https://news.example/"), "News");
        history.recordVisit(QUrl("https://docs.example/guide"), "Guide");
        QTRY_VERIFY(finished.count() > 0 && !history.hasPendingWrites());
        closed.add(QUrl("https://closed.example/"), "Closed tab");
        QTRY_COMPARE(model.rowCount(), 5);

        const auto header = [&model](int row) { return model.data(model.index(row), LibraryListModel::HeaderRole).toBool(); };
        const auto section = [&model](int row) {
            return model.data(model.index(row), LibraryListModel::SectionIdRole).toString();
        };
        QVERIFY(header(0));
        QCOMPARE(section(0), QString("closed"));
        QCOMPARE(model.data(model.index(0), LibraryListModel::SectionTitleRole).toString(), QString("Recently closed"));
        QVERIFY(!header(1));
        QVERIFY(model.data(model.index(1), LibraryListModel::ClosedIdRole).toLongLong() != 0);
        QVERIFY(header(2));
        QCOMPARE(section(2), QString("recent"));
        QCOMPARE(model.itemCount(), 3);

        model.toggleSection("recent");
        QCOMPARE(model.rowCount(), 3);
        QVERIFY(model.data(model.index(2), LibraryListModel::CollapsedRole).toBool());
        model.toggleSection("recent");
        QCOMPARE(model.rowCount(), 5);

        model.setQuery("guide");
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(1), LibraryListModel::TitleRole).toString(), QString("Guide"));
        model.setQuery("");
        QCOMPARE(model.rowCount(), 5);
    }

    void bookmarkModelReordersOnlyWithoutQuery() {
        BookmarkStore store(m_path);
        store.initialize(m_key);
        store.add(QUrl("https://a.example/"), "A");
        store.add(QUrl("https://b.example/"), "B");
        store.add(QUrl("https://c.example/"), "C");
        LibraryListModel model(LibraryListModel::Mode::Bookmarks);
        QAbstractItemModelTester tester(&model);
        model.setBookmarks(&store);
        QCOMPARE(model.rowCount(), 3);
        QVERIFY(model.reorderable());
        QVERIFY(model.moveItem(2, 0));
        QCOMPARE(model.data(model.index(0), LibraryListModel::TitleRole).toString(), QString("A"));
        QCOMPARE(store.data(store.index(0), BookmarkStore::TitleRole).toString(), QString("A"));
        model.setQuery("b");
        QVERIFY(!model.reorderable());
        QVERIFY(!model.moveItem(0, 0));
        QCOMPARE(model.rowCount(), 1);
        store.remove(QUrl("https://b.example/"));
        QTRY_COMPARE(model.rowCount(), 0);
    }

  private:
    const QByteArray m_key = QByteArray(32, 'k');
    std::unique_ptr<QTemporaryDir> m_directory;
    QString m_path;
};

QTEST_MAIN(LibraryTest)
#include "library_test.moc"
