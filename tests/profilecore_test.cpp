#include "core/profiles/avatarprocessor.h"
#include "core/profiles/profilecolors.h"
#include "core/profiles/profileid.h"
#include "core/profiles/profilepaths.h"
#include "core/profiles/profilevalidation.h"

#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

class ProfileCoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void profileIdGenerationIsCanonical();
    void profileIdParsingRejectsMalformedInput();
    void displayNameValidation();
    void passwordValidation();
    void pathsDeriveFromRoots();
    void pathContainmentRejectsTraversal();
    void deletionStagingNames();
    void generatedColorsMeetContrast();
    void firstGraphemeHandlesSurrogates();
    void avatarNormalizesAndStripsMetadata();
    void avatarRejectsOversizedAndMissingSources();
};

void ProfileCoreTest::profileIdGenerationIsCanonical() {
    const eden::core::ProfileId first = eden::core::ProfileId::generate();
    const eden::core::ProfileId second = eden::core::ProfileId::generate();
    QVERIFY(first.isValid());
    QVERIFY(first.toString() != second.toString());
    QVERIFY(eden::core::ProfileId::isCanonical(first.toString()));
    QCOMPARE(first.toString(), first.toString().toLower());
    QCOMPARE(first.toString().size(), 36);
}

void ProfileCoreTest::profileIdParsingRejectsMalformedInput() {
    QVERIFY(eden::core::ProfileId::parse("0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c"));
    QVERIFY(!eden::core::ProfileId::parse("0C5A9B7E-1F2D-4E3A-8B6C-9D0E1F2A3B4C"));
    QVERIFY(!eden::core::ProfileId::parse("{0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c}"));
    QVERIFY(!eden::core::ProfileId::parse("0c5a9b7e1f2d4e3a8b6c9d0e1f2a3b4c"));
    QVERIFY(!eden::core::ProfileId::parse(""));
    QVERIFY(!eden::core::ProfileId::parse("../../../etc/passwd"));
    QVERIFY(!eden::core::ProfileId::parse("0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4g"));
}

void ProfileCoreTest::displayNameValidation() {
    QCOMPARE(eden::core::normalizedDisplayName("  Nick  ").value_or(QString()), QString("Nick"));
    QVERIFY(!eden::core::normalizedDisplayName(""));
    QVERIFY(!eden::core::normalizedDisplayName("   "));
    QVERIFY(!eden::core::normalizedDisplayName(QString(65, QLatin1Char('a'))));
    QVERIFY(eden::core::normalizedDisplayName(QString(64, QLatin1Char('a'))));
    QVERIFY(!eden::core::normalizedDisplayName(QString("line\nbreak")));
    QVERIFY(!eden::core::normalizedDisplayName(QString("tab\tname")));
    QVERIFY(!eden::core::normalizedDisplayName("path/name"));
    QVERIFY(!eden::core::normalizedDisplayName("path\\name"));
    QVERIFY(!eden::core::normalizedDisplayName(QString("para") + QChar(0x2029) + QString("graph")));
    const QString emoji = QString::fromUtf8("\xF0\x9F\xA6\x8A");
    QString longEmoji;
    for (int index = 0; index < 64; ++index) {
        longEmoji += emoji;
    }
    QVERIFY(eden::core::normalizedDisplayName(longEmoji));
    QVERIFY(!eden::core::normalizedDisplayName(longEmoji + emoji));
}

void ProfileCoreTest::passwordValidation() {
    using eden::core::ProfileError;
    QCOMPARE(eden::core::validateProfilePassword("password1", "password1"), ProfileError::None);
    QCOMPARE(eden::core::validateProfilePassword("password1", "password2"), ProfileError::InvalidPassword);
    QCOMPARE(eden::core::validateProfilePassword("short", "short"), ProfileError::InvalidPassword);
    QCOMPARE(
        eden::core::validateProfilePassword(QString(257, QLatin1Char('a')), QString(257, QLatin1Char('a'))),
        ProfileError::InvalidPassword
    );
    QCOMPARE(
        eden::core::validateProfilePassword(QString(256, QLatin1Char('a')), QString(256, QLatin1Char('a'))),
        ProfileError::None
    );
    const QString wide = QString::fromUtf8("\xF0\x9F\xA6\x8A");
    QString oversizedUtf8;
    for (int index = 0; index < 200; ++index) {
        oversizedUtf8 += wide + QLatin1Char('a');
    }
    QCOMPARE(eden::core::validateProfilePassword(oversizedUtf8, oversizedUtf8), ProfileError::InvalidPassword);
}

void ProfileCoreTest::pathsDeriveFromRoots() {
    const eden::core::ProfilePaths::Roots roots{QStringLiteral("/data/root"), QStringLiteral("/cache/root")};
    const eden::core::ProfileId id = *eden::core::ProfileId::parse("0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c");
    const eden::core::ProfilePaths paths(roots, id);
    QVERIFY(paths.isValid());
    QCOMPARE(paths.dataDirectory(), QString("/data/root/profiles/0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c"));
    QCOMPARE(paths.cacheDirectory(), QString("/cache/root/profiles/0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c"));
    QCOMPARE(paths.avatarPath(), paths.dataDirectory() + "/avatar.png");
    QCOMPARE(paths.databasePath(), paths.dataDirectory() + "/eden.sqlite");
    QCOMPARE(paths.settingsPath(), paths.dataDirectory() + "/settings.ini");
    QCOMPARE(paths.sessionPath(), paths.dataDirectory() + "/session.json");
    QCOMPARE(paths.vaultPath(), paths.dataDirectory() + "/vault.sqlite");
    QCOMPARE(paths.extensionsDirectory(), paths.dataDirectory() + "/extensions");
    QCOMPARE(
        paths.engineDataDirectory("cef"),
        QString("/data/root/engine-data/cef/profiles/0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c")
    );
    QCOMPARE(paths.engineCacheDirectory("cef"), paths.cacheDirectory() + "/cef");
    QCOMPARE(eden::core::ProfilePaths::registryDatabasePath(roots), QString("/data/root/profiles.sqlite"));
    const QStringList owned = paths.ownedDirectories({"cef", "qtwebengine"});
    QCOMPARE(owned.size(), 4);
    QVERIFY(owned.contains(paths.dataDirectory()));
    QVERIFY(owned.contains(paths.cacheDirectory()));
}

void ProfileCoreTest::pathContainmentRejectsTraversal() {
    const eden::core::ProfilePaths::Roots roots{QStringLiteral("/data/root"), QStringLiteral("/cache/root")};
    const eden::core::ProfileId id = *eden::core::ProfileId::parse("0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c");
    const eden::core::ProfilePaths paths(roots, id);
    QVERIFY(paths.contains(paths.avatarPath()));
    QVERIFY(paths.contains(paths.dataDirectory()));
    QVERIFY(!paths.contains("/data/root/profiles/other-profile/avatar.png"));
    QVERIFY(!paths.contains(paths.dataDirectory() + "/../other/eden.sqlite"));
    QVERIFY(!paths.contains("/etc/passwd"));
}

void ProfileCoreTest::deletionStagingNames() {
    const eden::core::ProfileId id = *eden::core::ProfileId::parse("0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c");
    const QString staging = eden::core::ProfilePaths::deletionStagingName(id, 3);
    QVERIFY(eden::core::ProfilePaths::isDeletionStagingName(staging));
    QVERIFY(!eden::core::ProfilePaths::isDeletionStagingName(id.toString()));
    QVERIFY(!eden::core::ProfilePaths::isDeletionStagingName("profiles"));
    const eden::core::ProfilePaths::Roots roots{QStringLiteral("/data/root"), QStringLiteral("/cache/root")};
    const QStringList parents = eden::core::ProfilePaths::deletionStagingParents(roots, {"cef", "qtwebengine"});
    QCOMPARE(parents.size(), 4);
    QVERIFY(parents.contains(QString("/data/root/profiles")));
    QVERIFY(parents.contains(QString("/data/root/engine-data/cef/profiles")));
    QVERIFY(parents.contains(QString("/cache/root/profiles")));
}

void ProfileCoreTest::generatedColorsMeetContrast() {
    const auto luminance = [](const QColor &color) {
        const auto channel = [](double value) {
            return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
    };
    for (quint32 seed = 0; seed < 24; ++seed) {
        const QColor background = eden::core::profileColorForSeed(seed);
        const QColor foreground = eden::core::profileForegroundFor(background);
        const double lighter = std::max(luminance(background), luminance(foreground));
        const double darker = std::min(luminance(background), luminance(foreground));
        const double contrast = (lighter + 0.05) / (darker + 0.05);
        QVERIFY2(contrast >= 4.5, qPrintable(QString("seed %1 contrast %2").arg(seed).arg(contrast)));
    }
}

void ProfileCoreTest::firstGraphemeHandlesSurrogates() {
    QCOMPARE(eden::core::firstGrapheme("nick"), QString("N"));
    QCOMPARE(eden::core::firstGrapheme("  eden"), QString("E"));
    const QString fox = QString::fromUtf8("\xF0\x9F\xA6\x8A");
    QCOMPARE(eden::core::firstGrapheme(fox + "name"), fox);
    QCOMPARE(eden::core::firstGrapheme(""), QString(""));
}

void ProfileCoreTest::avatarNormalizesAndStripsMetadata() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath("source.jpg");
    QImage source(1200, 700, QImage::Format_RGB32);
    QPainter painter(&source);
    painter.fillRect(source.rect(), Qt::darkCyan);
    painter.fillRect(QRect(0, 0, 600, 700), Qt::yellow);
    painter.end();
    QImageWriter writer(sourcePath, "jpg");
    writer.setText("Description", "secret metadata");
    QVERIFY(writer.write(source));
    const QString targetPath = directory.filePath("avatar.png");
    QCOMPARE(
        eden::core::AvatarProcessor::processBlocking(QUrl::fromLocalFile(sourcePath), targetPath),
        eden::core::ProfileError::None
    );
    const QImage normalized(targetPath);
    QCOMPARE(normalized.width(), 512);
    QCOMPARE(normalized.height(), 512);
    QVERIFY(normalized.text("Description").isEmpty());
    QFile targetFile(targetPath);
    QVERIFY(targetFile.open(QIODevice::ReadOnly));
    QVERIFY(!targetFile.readAll().contains("secret metadata"));
    QVERIFY(QFile::remove(sourcePath));
    QVERIFY(QFileInfo::exists(targetPath));
}

void ProfileCoreTest::avatarRejectsOversizedAndMissingSources() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString targetPath = directory.filePath("avatar.png");
    QCOMPARE(
        eden::core::AvatarProcessor::processBlocking(QUrl("https://example.com/avatar.png"), targetPath),
        eden::core::ProfileError::AvatarUnreadable
    );
    QCOMPARE(
        eden::core::AvatarProcessor::processBlocking(
            QUrl::fromLocalFile(directory.filePath("missing.png")),
            targetPath
        ),
        eden::core::ProfileError::AvatarUnreadable
    );
    const QString hugePath = directory.filePath("huge.png");
    QFile huge(hugePath);
    QVERIFY(huge.open(QIODevice::WriteOnly));
    QVERIFY(huge.resize(eden::core::AvatarProcessor::maximumSourceBytes + 1));
    huge.close();
    QCOMPARE(
        eden::core::AvatarProcessor::processBlocking(QUrl::fromLocalFile(hugePath), targetPath),
        eden::core::ProfileError::AvatarTooLarge
    );
    const QString notImagePath = directory.filePath("fake.png");
    QFile notImage(notImagePath);
    QVERIFY(notImage.open(QIODevice::WriteOnly));
    notImage.write("this is not an image");
    notImage.close();
    QCOMPARE(
        eden::core::AvatarProcessor::processBlocking(QUrl::fromLocalFile(notImagePath), targetPath),
        eden::core::ProfileError::AvatarDecodeFailed
    );
    QVERIFY(!QFileInfo::exists(targetPath));
}

QTEST_GUILESS_MAIN(ProfileCoreTest)
#include "profilecore_test.moc"
