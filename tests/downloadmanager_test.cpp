#include "core/downloads/downloadmanager.h"
#include "engine/engineprofile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

class DownloadTestProfile final : public eden::engine::EngineProfile {
  public:
    explicit DownloadTestProfile(eden::engine::Backend backend = eden::engine::Backend::Cef)
        : EngineProfile([backend] {
              eden::engine::EngineProfileParameters parameters;
              parameters.backend = backend;
              parameters.privateProfile = true;
              return parameters;
          }()) {}

    QObject *nativeProfile() const override {
        return nullptr;
    }

    void clearData() override {}

    void begin(quint32 nativeId, const QString &name) {
        emit downloadStarted(downloadIdentifier(nativeId), name, QUrl("https://example.test/file"), name, 100);
    }

    void update(quint32 nativeId, qint64 bytes, const QString &state = "downloading") {
        emit downloadUpdated(downloadIdentifier(nativeId), bytes, 100, state);
    }
};

class DownloadManagerTest final : public QObject {
    Q_OBJECT

  private slots:
    void separatesNativeIdentifiersAcrossProfiles();
    void keepsNativeIdentifiersStableWithinProfile();
    void preservesFullWidthIdentifiers();
    void reservesAndReleasesDestinationsAcrossBackends();
};

void DownloadManagerTest::reservesAndReleasesDestinationsAcrossBackends() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DownloadTestProfile cef;
    DownloadTestProfile qt(eden::engine::Backend::QtWebEngine);
    const QString existing = directory.filePath("report.txt");
    QFile file(existing);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("preserve"), qint64(8));
    file.close();
    auto first = cef.reserveDownloadPath(directory.path(), "report.txt");
    auto second = qt.reserveDownloadPath(directory.path() + "/unused/..", "report.txt");
    QCOMPARE(QFileInfo(*first).fileName(), QString("report (1).txt"));
    QCOMPARE(QFileInfo(*second).fileName(), QString("report (2).txt"));
    const QString released = *first;
    first.reset();
    auto reused = qt.reserveDownloadPath(directory.path(), "report.txt");
    QCOMPARE(*reused, released);
    const auto sanitized = cef.reserveDownloadPath(directory.path(), "../../report.txt");
    QCOMPARE(QFileInfo(*sanitized).absolutePath(), directory.path());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("preserve"));
}

static void connectDownloads(DownloadTestProfile &profile, eden::core::DownloadManager &downloads) {
    QObject::connect(
        &profile,
        &eden::engine::EngineProfile::downloadStarted,
        &downloads,
        &eden::core::DownloadManager::beginDownload
    );
    QObject::connect(
        &profile,
        &eden::engine::EngineProfile::downloadUpdated,
        &downloads,
        &eden::core::DownloadManager::updateDownload
    );
}

void DownloadManagerTest::separatesNativeIdentifiersAcrossProfiles() {
    using Manager = eden::core::DownloadManager;
    Manager downloads;
    DownloadTestProfile first;
    DownloadTestProfile second(eden::engine::Backend::QtWebEngine);
    connectDownloads(first, downloads);
    connectDownloads(second, downloads);
    first.begin(7, "first-file");
    second.begin(7, "second-file");
    QCOMPARE(downloads.rowCount(), 2);
    second.update(7, 80);
    first.update(7, 40);
    QCOMPARE(downloads.data(downloads.index(0), Manager::FileNameRole).toString(), QString("second-file"));
    QCOMPARE(downloads.data(downloads.index(0), Manager::ReceivedBytesRole).toLongLong(), 80);
    QCOMPARE(downloads.data(downloads.index(1), Manager::FileNameRole).toString(), QString("first-file"));
    QCOMPARE(downloads.data(downloads.index(1), Manager::ReceivedBytesRole).toLongLong(), 40);
    first.update(7, 100, "completed");
    QCOMPARE(downloads.activeCount(), 1);
    QCOMPARE(downloads.data(downloads.index(0), Manager::StateRole).toString(), QString("downloading"));

    DownloadTestProfile third;
    connectDownloads(third, downloads);
    third.begin(7, "third-file");
    QCOMPARE(downloads.rowCount(), 3);
    QCOMPARE(downloads.activeCount(), 2);
    third.update(7, 20);
    QCOMPARE(downloads.data(downloads.index(0), Manager::ReceivedBytesRole).toLongLong(), 20);
    QCOMPARE(downloads.data(downloads.index(1), Manager::ReceivedBytesRole).toLongLong(), 80);
}

void DownloadManagerTest::keepsNativeIdentifiersStableWithinProfile() {
    using Manager = eden::core::DownloadManager;
    Manager downloads;
    quint64 oldIdentifier = 0;
    {
        DownloadTestProfile profile;
        connectDownloads(profile, downloads);
        oldIdentifier = profile.downloadIdentifier(7);
        profile.begin(7, "original-file");
        profile.begin(7, "duplicate-file");
        QCOMPARE(profile.downloadIdentifier(7), oldIdentifier);
        QCOMPARE(downloads.rowCount(), 1);
        profile.update(7, 50);
        QCOMPARE(downloads.data(downloads.index(0), Manager::ReceivedBytesRole).toLongLong(), 50);
    }
    DownloadTestProfile replacement;
    connectDownloads(replacement, downloads);
    QVERIFY(replacement.downloadIdentifier(7) != oldIdentifier);
    replacement.begin(7, "replacement-file");
    QCOMPARE(downloads.rowCount(), 2);
    replacement.update(7, 25);
    QCOMPARE(downloads.data(downloads.index(0), Manager::ReceivedBytesRole).toLongLong(), 25);
    QCOMPARE(downloads.data(downloads.index(1), Manager::ReceivedBytesRole).toLongLong(), 50);
    QVERIFY(replacement.downloadIdentifier(0xffffffffU) != replacement.downloadIdentifier(7));
}

void DownloadManagerTest::preservesFullWidthIdentifiers() {
    using Manager = eden::core::DownloadManager;
    Manager downloads;
    DownloadTestProfile profile;
    connectDownloads(profile, downloads);
    const quint64 first = 17;
    const quint64 second = (quint64(1) << 40) | first;
    emit profile.downloadStarted(first, "first", QUrl(), QString(), 100);
    emit profile.downloadStarted(second, "second", QUrl(), QString(), 100);
    QCOMPARE(downloads.rowCount(), 2);
    emit profile.downloadUpdated(second, 80, 100, "downloading");
    QCOMPARE(downloads.data(downloads.index(0), Manager::ReceivedBytesRole).toLongLong(), 80);
    QCOMPARE(downloads.data(downloads.index(1), Manager::ReceivedBytesRole).toLongLong(), 0);
}

QTEST_GUILESS_MAIN(DownloadManagerTest)

#include "downloadmanager_test.moc"
