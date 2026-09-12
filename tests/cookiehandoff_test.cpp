#include "core/profiles/cookiehandoffcoordinator.h"
#include "engine/engineprofile.h"
#include "engine/portablecookie.h"

#include <QtTest>

using eden::core::CookieHandoffCoordinator;
using eden::core::ProfileError;
using eden::engine::CookieReplaceCallback;
using eden::engine::CookieReplaceResult;
using eden::engine::CookieSameSite;
using eden::engine::CookieSnapshotCallback;
using eden::engine::CookieSnapshotResult;
using eden::engine::PortableCookie;

static eden::engine::EngineProfileParameters
adapterParameters(const QString &profileId, eden::engine::Backend backend, bool privateProfile) {
    eden::engine::EngineProfileParameters parameters;
    parameters.profileId = profileId;
    parameters.backend = backend;
    parameters.privateProfile = privateProfile;
    return parameters;
}

class FakeCookieAdapter final : public eden::engine::EngineProfile {
  public:
    FakeCookieAdapter(const QString &profileId, eden::engine::Backend backend, bool privateProfile = false)
        : EngineProfile(adapterParameters(profileId, backend, privateProfile)) {}

    QObject *nativeProfile() const override {
        return nullptr;
    }

    void clearData() override {}

    bool supportsPortableCookies() const override {
        return supported;
    }

    void exportPortableCookies(CookieSnapshotCallback callback) override {
        ++exportCalls;
        if (deferExport) {
            pendingExport = std::move(callback);
            return;
        }
        CookieSnapshotResult result;
        result.errorCode = exportError;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (PortableCookie cookie : jar) {
            if (eden::engine::canonicalizePortableCookie(cookie, now)) {
                result.cookies.append(cookie);
            } else {
                ++result.skippedCookies;
            }
        }
        QMetaObject::invokeMethod(
            this,
            [callback = std::move(callback), result]() mutable { callback(std::move(result)); },
            Qt::QueuedConnection
        );
    }

    void replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) override {
        ++replaceCalls;
        CookieReplaceResult result;
        if (!replaceError.isEmpty()) {
            result.errorCode = replaceError;
        } else {
            jar = cookies;
            result.importedCookies = cookies.size();
        }
        QMetaObject::invokeMethod(
            this,
            [callback = std::move(callback), result]() mutable { callback(std::move(result)); },
            Qt::QueuedConnection
        );
    }

    QList<PortableCookie> jar;
    QString exportError;
    QString replaceError;
    CookieSnapshotCallback pendingExport;
    bool supported = true;
    bool deferExport = false;
    int exportCalls = 0;
    int replaceCalls = 0;
};

static PortableCookie
makeCookie(const char *name, const char *value, const char *domain, bool hostOnly = true, bool httpOnly = false) {
    PortableCookie cookie;
    cookie.name = name;
    cookie.value = value;
    cookie.domain = domain;
    cookie.path = "/";
    cookie.hostOnly = hostOnly;
    cookie.httpOnly = httpOnly;
    return cookie;
}

class CookieHandoffTest final : public QObject {
    Q_OBJECT

  private slots:
    void canonicalizationNormalizesDomainPathAndExpiry();
    void identityMatchesSpecTuple();
    void handoffReplacesDestinationJarExactly();
    void logoutPropagatesEmptyJar();
    void rejectsPrivateEqualBackendCrossProfileAndUnsupported();
    void serializesRequestsInOrder();
    void deadlineCompletesExactlyOnceAndDropsLateCallback();
    void blockedCoordinatorRejectsNewRequests();
    void wipeClearsCookieValues();
};

void CookieHandoffTest::canonicalizationNormalizesDomainPathAndExpiry() {
    PortableCookie cookie = makeCookie("session", "value", ".Example.COM", false);
    cookie.path = "";
    QVERIFY(eden::engine::canonicalizePortableCookie(cookie, 1000));
    QCOMPARE(cookie.domain, QByteArray("example.com"));
    QCOMPARE(cookie.path, QByteArray("/"));

    PortableCookie expired = makeCookie("old", "value", "example.com");
    expired.expires = QDateTime::fromMSecsSinceEpoch(500, Qt::UTC);
    QVERIFY(!eden::engine::canonicalizePortableCookie(expired, 1000));

    PortableCookie future = makeCookie("fresh", "value", "example.com");
    future.expires = QDateTime::fromMSecsSinceEpoch(5000, Qt::UTC);
    QVERIFY(eden::engine::canonicalizePortableCookie(future, 1000));

    PortableCookie badDomain = makeCookie("bad", "value", "exa mple.com");
    QVERIFY(!eden::engine::canonicalizePortableCookie(badDomain, 1000));

    PortableCookie unicodeDomain = makeCookie("bad", "value", "\xC3\xA9xample.com");
    QVERIFY(!eden::engine::canonicalizePortableCookie(unicodeDomain, 1000));
}

void CookieHandoffTest::identityMatchesSpecTuple() {
    const PortableCookie hostCookie = makeCookie("a", "1", "example.com", true);
    PortableCookie domainCookie = makeCookie("a", "1", "example.com", false);
    QVERIFY(eden::engine::portableCookieIdentity(hostCookie) != eden::engine::portableCookieIdentity(domainCookie));
    PortableCookie otherPath = hostCookie;
    otherPath.path = "/deep";
    QVERIFY(eden::engine::portableCookieIdentity(hostCookie) != eden::engine::portableCookieIdentity(otherPath));
    PortableCookie differentValue = hostCookie;
    differentValue.value = "2";
    QCOMPARE(eden::engine::portableCookieIdentity(hostCookie), eden::engine::portableCookieIdentity(differentValue));
}

void CookieHandoffTest::handoffReplacesDestinationJarExactly() {
    const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    FakeCookieAdapter source(profileId, eden::engine::Backend::Cef);
    FakeCookieAdapter destination(profileId, eden::engine::Backend::QtWebEngine);
    source.jar = {makeCookie("session", "abc", "slack.com", true, true), makeCookie("theme", "dark", "slack.com")};
    destination.jar = {makeCookie("stale", "old", "slack.com")};
    CookieHandoffCoordinator coordinator(profileId);
    QSignalSpy finished(&coordinator, &CookieHandoffCoordinator::handoffFinished);
    bool completed = false;
    coordinator.requestHandoff(
        std::shared_ptr<eden::engine::EngineProfile>(&source, [](eden::engine::EngineProfile *) {}),
        std::shared_ptr<eden::engine::EngineProfile>(&destination, [](eden::engine::EngineProfile *) {}),
        [&completed](ProfileError error, const QString &) {
            QCOMPARE(error, ProfileError::None);
            completed = true;
        }
    );
    QTRY_VERIFY(completed);
    QCOMPARE(destination.jar.size(), 2);
    QCOMPARE(destination.jar.constFirst().name, QByteArray("session"));
    QVERIFY(destination.jar.constFirst().httpOnly);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.constFirst().at(2).toLongLong(), 2);
    QCOMPARE(finished.constFirst().at(5).toString(), QString("ok"));
}

void CookieHandoffTest::logoutPropagatesEmptyJar() {
    const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    FakeCookieAdapter source(profileId, eden::engine::Backend::Cef);
    FakeCookieAdapter destination(profileId, eden::engine::Backend::QtWebEngine);
    destination.jar = {makeCookie("session", "still-signed-in", "slack.com")};
    CookieHandoffCoordinator coordinator(profileId);
    bool completed = false;
    coordinator.requestHandoff(
        std::shared_ptr<eden::engine::EngineProfile>(&source, [](eden::engine::EngineProfile *) {}),
        std::shared_ptr<eden::engine::EngineProfile>(&destination, [](eden::engine::EngineProfile *) {}),
        [&completed](ProfileError error, const QString &) {
            QCOMPARE(error, ProfileError::None);
            completed = true;
        }
    );
    QTRY_VERIFY(completed);
    QVERIFY(destination.jar.isEmpty());
}

void CookieHandoffTest::rejectsPrivateEqualBackendCrossProfileAndUnsupported() {
    const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString otherProfileId = QStringLiteral("22222222-2222-4222-8222-222222222222");
    CookieHandoffCoordinator coordinator(profileId);
    const auto expectRejection = [&coordinator](
                                     const std::shared_ptr<eden::engine::EngineProfile> &source,
                                     const std::shared_ptr<eden::engine::EngineProfile> &destination,
                                     const QString &expectedCode
                                 ) {
        bool rejected = false;
        coordinator
            .requestHandoff(source, destination, [&rejected, expectedCode](ProfileError error, const QString &code) {
                QCOMPARE(error, ProfileError::HandoffUnsupported);
                QCOMPARE(code, expectedCode);
                rejected = true;
            });
        QVERIFY(rejected);
    };
    const auto lease = [](FakeCookieAdapter *adapter) {
        return std::shared_ptr<eden::engine::EngineProfile>(adapter, [](eden::engine::EngineProfile *) {});
    };
    FakeCookieAdapter privateSource(profileId, eden::engine::Backend::Cef, true);
    FakeCookieAdapter destination(profileId, eden::engine::Backend::QtWebEngine);
    expectRejection(lease(&privateSource), lease(&destination), QStringLiteral("private_mode"));

    FakeCookieAdapter sameBackendSource(profileId, eden::engine::Backend::QtWebEngine);
    expectRejection(lease(&sameBackendSource), lease(&destination), QStringLiteral("equal_backend"));

    FakeCookieAdapter foreignSource(otherProfileId, eden::engine::Backend::Cef);
    expectRejection(lease(&foreignSource), lease(&destination), QStringLiteral("cross_profile"));

    FakeCookieAdapter unsupportedSource(profileId, eden::engine::Backend::Cef);
    unsupportedSource.supported = false;
    expectRejection(lease(&unsupportedSource), lease(&destination), QStringLiteral("unsupported_adapter"));
}

void CookieHandoffTest::serializesRequestsInOrder() {
    const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    FakeCookieAdapter blink(profileId, eden::engine::Backend::Cef);
    FakeCookieAdapter qt(profileId, eden::engine::Backend::QtWebEngine);
    FakeCookieAdapter servo(profileId, eden::engine::Backend::Servo);
    blink.jar = {makeCookie("winner", "blink", "example.com")};
    servo.jar = {makeCookie("winner", "servo", "example.com")};
    CookieHandoffCoordinator coordinator(profileId);
    const auto lease = [](FakeCookieAdapter *adapter) {
        return std::shared_ptr<eden::engine::EngineProfile>(adapter, [](eden::engine::EngineProfile *) {});
    };
    QStringList completionOrder;
    coordinator.requestHandoff(lease(&blink), lease(&qt), [&completionOrder](ProfileError, const QString &) {
        completionOrder.append("first");
    });
    coordinator.requestHandoff(lease(&servo), lease(&qt), [&completionOrder](ProfileError, const QString &) {
        completionOrder.append("second");
    });
    QCOMPARE(coordinator.pendingCount(), 2);
    QTRY_COMPARE(completionOrder.size(), 2);
    QCOMPARE(completionOrder, QStringList({"first", "second"}));
    QCOMPARE(qt.jar.size(), 1);
    QCOMPARE(qt.jar.constFirst().value, QByteArray("servo"));
    QVERIFY(!coordinator.active());
}

void CookieHandoffTest::deadlineCompletesExactlyOnceAndDropsLateCallback() {
    const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    FakeCookieAdapter source(profileId, eden::engine::Backend::Cef);
    FakeCookieAdapter destination(profileId, eden::engine::Backend::QtWebEngine);
    source.deferExport = true;
    destination.jar = {makeCookie("existing", "kept", "example.com")};
    CookieHandoffCoordinator coordinator(profileId);
    int completions = 0;
    ProfileError observed = ProfileError::None;
    coordinator.requestHandoff(
        std::shared_ptr<eden::engine::EngineProfile>(&source, [](eden::engine::EngineProfile *) {}),
        std::shared_ptr<eden::engine::EngineProfile>(&destination, [](eden::engine::EngineProfile *) {}),
        [&completions, &observed](ProfileError error, const QString &) {
            observed = error;
            ++completions;
        }
    );
    QVERIFY(coordinator.active());
    QTRY_VERIFY_WITH_TIMEOUT(completions == 1, CookieHandoffCoordinator::deadlineMilliseconds + 2000);
    QCOMPARE(observed, ProfileError::HandoffFailed);
    QVERIFY(!coordinator.active());
    CookieSnapshotResult late;
    late.cookies = {makeCookie("late", "value", "example.com")};
    source.pendingExport(late);
    QTest::qWait(50);
    QCOMPARE(completions, 1);
    QCOMPARE(destination.replaceCalls, 0);
    QCOMPARE(destination.jar.constFirst().value, QByteArray("kept"));
}

void CookieHandoffTest::blockedCoordinatorRejectsNewRequests() {
    const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    FakeCookieAdapter source(profileId, eden::engine::Backend::Cef);
    FakeCookieAdapter destination(profileId, eden::engine::Backend::QtWebEngine);
    CookieHandoffCoordinator coordinator(profileId);
    coordinator.setBlocked(true);
    bool rejected = false;
    coordinator.requestHandoff(
        std::shared_ptr<eden::engine::EngineProfile>(&source, [](eden::engine::EngineProfile *) {}),
        std::shared_ptr<eden::engine::EngineProfile>(&destination, [](eden::engine::EngineProfile *) {}),
        [&rejected](ProfileError error, const QString &code) {
            QCOMPARE(error, ProfileError::SignOutBlocked);
            QCOMPARE(code, QString("blocked"));
            rejected = true;
        }
    );
    QVERIFY(rejected);
    QCOMPARE(source.exportCalls, 0);
}

void CookieHandoffTest::wipeClearsCookieValues() {
    QList<PortableCookie> cookies = {makeCookie("secret", "super-secret-value", "example.com")};
    eden::engine::wipePortableCookies(cookies);
    QVERIFY(cookies.isEmpty());
    PortableCookie cookie = makeCookie("secret", "super-secret-value", "example.com");
    eden::engine::wipePortableCookie(cookie);
    QVERIFY(cookie.value.isEmpty());
    QVERIFY(cookie.name.isEmpty());
}

QTEST_GUILESS_MAIN(CookieHandoffTest)
#include "cookiehandoff_test.moc"
