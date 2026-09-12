#include <libsecret/secret.h>

#include "core/automation/performancemetrics.h"
#include "core/permissions/permissionstore.h"
#include "core/profiles/profilecontext.h"
#include "core/profiles/profilepaths.h"
#include "core/profiles/windowregistry.h"
#include "core/window/tabmodel.h"
#include "core/window/windowcontroller.h"
#include "engine/engineregistry.h"
#include "engine/engineview.h"
#include "passwords/credentialvault.h"

#include <QQuickWindow>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

extern "C" gchar *secret_password_lookup_sync(const SecretSchema *, GCancellable *, GError **, ...) {
    return g_strdup(QByteArray(32, 'p').toBase64().constData());
}

extern "C" gboolean secret_password_store_sync(
    const SecretSchema *,
    const gchar *,
    const gchar *,
    const gchar *,
    GCancellable *,
    GError **,
    ...
) {
    return false;
}

class PermissionEngine final : public eden::engine::EngineView {
  public:
    QUrl url() const override {
        return m_url;
    }
    QString title() const override {
        return QStringLiteral("Permission fixture");
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
        return QStringLiteral("secure");
    }
    QString backendName() const override {
        return QStringLiteral("Fixture");
    }
    Capabilities capabilities() const override {
        return thumbnails ? Capabilities(ThumbnailCapture) : Capabilities();
    }
    qint64 rendererProcessId() const override {
        ++processQueries;
        return QCoreApplication::applicationPid();
    }
    void requestThumbnail(const QSize &, ThumbnailCallback callback) override {
        ++captures;
        if (captureDelayMs > 0) {
            QTest::qSleep(captureDelayMs);
        }
        completion = std::move(callback);
    }
    void finishThumbnail() {
        QImage image(420, 236, QImage::Format_ARGB32);
        image.fill(Qt::red);
        auto callback = std::move(completion);
        if (callback) {
            callback(image);
        }
    }

    void setActivity(int flags) {
        setCaptureDetails("fixture", flags);
    }

    bool thumbnails = false;
    mutable int processQueries = 0;
    int captures = 0;
    int captureDelayMs = 0;
    ThumbnailCallback completion;
    void load(const QUrl &url) override {
        m_url = url;
        emit urlChanged();
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

    void exitFullscreen() override {
        ++fullscreenExits;
    }
    void requestAutofillTarget(AutofillTargetCallback callback) override {
        autofill = std::move(callback);
    }
    void fillForm(const eden::engine::AutofillTarget &target, const QVariantMap &fields) override {
        filledTarget = target;
        filledFields = fields;
    }
    int fullscreenExits = 0;
    AutofillTargetCallback autofill;
    eden::engine::AutofillTarget filledTarget;
    QVariantMap filledFields;

    void requestPermission(quint64 id) {
        eden::engine::PermissionRequestInfo request;
        request.id = id;
        request.origin = eden::engine::autofillOrigin(m_url);
        request.permissions = {QStringLiteral("camera")};
        emit permissionRequested(request);
    }

    void resolvePermissionRequest(quint64 id, bool allowed) override {
        resolutions.append({id, allowed});
    }

    void dismissPermissionRequest(quint64 id) override {
        dismissals.append(id);
        resolvePermissionRequest(id, false);
    }

    QList<QPair<quint64, bool>> resolutions;
    QList<quint64> dismissals;

  private:
    QUrl m_url;
};

class PermissionHarness {
  public:
    bool create(bool privateWindow = false, QObject *parent = nullptr) {
        if (!m_data.isValid() || !m_cache.isValid()) {
            return false;
        }
        eden::core::ProfileRecord record;
        record.id = eden::core::ProfileId::generate();
        record.displayName = QStringLiteral("Permission fixture");
        record.lifecycle = eden::core::ProfileLifecycle::Ready;
        const eden::core::ProfilePaths paths({m_data.path(), m_cache.path()}, record.id);
        context = eden::core::ProfileContext::create(record, paths, nullptr, nullptr);
        if (!context) {
            return false;
        }
        context->activate();
        if (!context->isActivated()) {
            return false;
        }
        auto *registry = eden::engine::EngineRegistry::instance();
        if (registry->descriptors().isEmpty()) {
            return false;
        }
        const auto backend = registry->descriptors().constFirst().backend;
        controller = std::make_unique<eden::core::WindowController>(parent);
        controller->initialize(context, privateWindow, registry->idForBackend(backend), false);
        source = std::make_unique<eden::core::TabModel>(
            [](eden::engine::Backend, eden::engine::EngineProfile *) { return std::make_unique<PermissionEngine>(); },
            eden::core::TabModel::ProfileResolver(),
            registry,
            backend,
            privateWindow,
            context->idString()
        );
        return true;
    }

    PermissionEngine *addTab() {
        const int row = source->addTab(QUrl(QStringLiteral("https://permissions.example/page")));
        auto *view = static_cast<PermissionEngine *>(source->engineViewAt(row));
        if (!view || !source->transferTabTo(row, controller->tabs(), controller->tabs()->rowCount())) {
            return nullptr;
        }
        return view;
    }

    eden::core::PermissionStore::Verdict verdict() const {
        return context->permissions()->verdict(QUrl("https://permissions.example"), QStringLiteral("camera"));
    }

  private:
    eden::core::WindowRegistry m_registry;
    QTemporaryDir m_data;
    QTemporaryDir m_cache;

  public:
    std::shared_ptr<eden::core::ProfileContext> context;
    std::unique_ptr<eden::core::WindowController> controller;
    std::unique_ptr<eden::core::TabModel> source;
};

class WindowControllerTest final : public QObject {
    Q_OBJECT

  private slots:
    void fullscreenTracksItsOwner();
    void personalDataRejectsReplacedTargets();
    void displayChoicesRemainTemporary();
    void automaticDismissal_data();
    void automaticDismissal();
    void explicitChoice_data();
    void explicitChoice();
    void backgroundRequestsRemainTemporary();
    void previewCaptureSkipsMemorySampling();
    void previewActivityUpdatesWhileOpen();
    void previewCapturePreservesPendingTabs();
    void previewCaptureRejectsStaleImages();
    void tabSwitchMetricIncludesPreviewWork();
};

void WindowControllerTest::fullscreenTracksItsOwner() {
    QQuickWindow window;
    PermissionHarness harness;
    QVERIFY(harness.create(false, &window));
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    emit view->fullscreenRequested(true);
    QVERIFY(harness.controller->contentFullscreen());
    QCOMPARE(harness.controller->fullscreenOrigin(), QString("https://permissions.example"));
    harness.controller->exitContentFullscreen();
    QVERIFY(!harness.controller->contentFullscreen());
    QCOMPARE(view->fullscreenExits, 1);
    emit view->fullscreenRequested(true);
    view->load(QUrl("https://replacement.example"));
    QVERIFY(!harness.controller->contentFullscreen());
    QCOMPARE(view->fullscreenExits, 2);
    emit view->fullscreenRequested(true);
    QVERIFY(harness.addTab());
    harness.controller->setActiveIndex(1);
    QVERIFY(!harness.controller->contentFullscreen());
    QCOMPARE(view->fullscreenExits, 3);
    emit view->fullscreenRequested(true);
    QVERIFY(!harness.controller->contentFullscreen());
    QCOMPARE(view->fullscreenExits, 4);
}

void WindowControllerTest::personalDataRejectsReplacedTargets() {
    PermissionHarness harness;
    QVERIFY(harness.create());
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    auto *vault = qobject_cast<eden::passwords::CredentialVault *>(harness.controller->credentialVault());
    QVERIFY(vault && vault->available());
    const qint64 id =
        vault->saveFormProfile(0, {{"label", "Work"}, {"name", "Test Person"}, {"email", "person@example.test"}});
    QVERIFY(id > 0);
    harness.controller->fillSavedForm(id);
    QVERIFY(view->autofill);
    const eden::engine::AutofillTarget target{view->url(), eden::engine::autofillOrigin(view->url()), "document"};
    auto callback = std::move(view->autofill);
    view->load(QUrl("https://replacement.example"));
    callback(target);
    QVERIFY(view->filledFields.isEmpty());
    view->load(target.url);
    harness.controller->fillSavedForm(id);
    QVERIFY(view->autofill);
    std::move(view->autofill)(target);
    QCOMPARE(view->filledFields.value("email").toString(), QString("person@example.test"));
    QCOMPARE(view->filledTarget.documentId, target.documentId);
}

void WindowControllerTest::displayChoicesRemainTemporary() {
    PermissionHarness harness;
    QVERIFY(harness.create());
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    eden::engine::DisplayCaptureRequestInfo request;
    request.id = 42;
    request.origin = eden::engine::autofillOrigin(view->url());
    request.audioRequested = true;
    emit view->displayCaptureRequested(request);
    QVERIFY(!harness.controller->displayCaptureRequest().isEmpty());
    harness.controller->resolveDisplayCaptureRequest("screen");
    QCOMPARE(harness.context->permissions()->state(request.origin, "screen sharing"), QString("ask"));
    emit view->displayCaptureRequested(request);
    QVERIFY(!harness.controller->displayCaptureRequest().isEmpty());
}

void WindowControllerTest::automaticDismissal_data() {
    QTest::addColumn<QString>("action");
    for (const char *action : {"dismiss", "tab-switch", "tab-transfer", "replacement", "native-close"}) {
        QTest::newRow(action) << QString::fromLatin1(action);
    }
}

void WindowControllerTest::automaticDismissal() {
    QFETCH(QString, action);
    PermissionHarness harness;
    QVERIFY(harness.create());
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    view->requestPermission(1);
    QCOMPARE(harness.controller->permissionRequest().value("id").toULongLong(), quint64(1));
    QCOMPARE(harness.verdict(), eden::core::PermissionStore::Ask);
    if (action == "dismiss") {
        QVERIFY(QMetaObject::invokeMethod(harness.controller.get(), "dismissPermissionRequest"));
    } else if (action == "tab-switch") {
        QVERIFY(harness.addTab());
    } else if (action == "tab-transfer") {
        QVERIFY(harness.controller->tabs()->transferTabTo(0, harness.source.get(), 0));
    } else if (action == "replacement") {
        view->requestPermission(2);
        emit view->permissionRequestClosed(1);
    } else {
        emit view->permissionRequestClosed(1);
    }
    QCOMPARE(harness.verdict(), eden::core::PermissionStore::Ask);
    if (action == "native-close") {
        QVERIFY(view->resolutions.isEmpty());
        QVERIFY(view->dismissals.isEmpty());
    } else {
        QCOMPARE(view->resolutions, (QList<QPair<quint64, bool>>{{1, false}}));
        QCOMPARE(view->dismissals, QList<quint64>{1});
    }
    if (action == "replacement") {
        QCOMPARE(harness.controller->permissionRequest().value("id").toULongLong(), quint64(2));
    } else {
        QVERIFY(harness.controller->permissionRequest().isEmpty());
    }
    if (action == "tab-transfer") {
        QVERIFY(harness.source->transferTabTo(0, harness.controller->tabs(), 0));
    } else if (action == "tab-switch") {
        harness.controller->setActiveIndex(0);
    }
    view->requestPermission(3);
    QCOMPARE(harness.controller->permissionRequest().value("id").toULongLong(), quint64(3));
    QCOMPARE(harness.verdict(), eden::core::PermissionStore::Ask);
}

void WindowControllerTest::explicitChoice_data() {
    QTest::addColumn<bool>("allowed");
    QTest::addColumn<bool>("privateWindow");
    QTest::newRow("allow") << true << false;
    QTest::newRow("block") << false << false;
    QTest::newRow("private-allow") << true << true;
    QTest::newRow("private-block") << false << true;
}

void WindowControllerTest::explicitChoice() {
    QFETCH(bool, allowed);
    QFETCH(bool, privateWindow);
    PermissionHarness harness;
    QVERIFY(harness.create(privateWindow));
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    view->requestPermission(1);
    harness.controller->resolvePermissionRequest(allowed);
    QVERIFY(harness.controller->permissionRequest().isEmpty());
    QVERIFY(view->dismissals.isEmpty());
    QCOMPARE(view->resolutions, (QList<QPair<quint64, bool>>{{1, allowed}}));
    const auto expected = privateWindow ? eden::core::PermissionStore::Ask
                          : allowed     ? eden::core::PermissionStore::Granted
                                        : eden::core::PermissionStore::Denied;
    QCOMPARE(harness.verdict(), expected);
    harness.controller->resolvePermissionRequest(!allowed);
    QCOMPARE(harness.verdict(), expected);
    QCOMPARE(view->resolutions.size(), 1);
    view->requestPermission(2);
    if (privateWindow) {
        QCOMPARE(harness.controller->permissionRequest().value("id").toULongLong(), quint64(2));
        QCOMPARE(view->resolutions.size(), 1);
    } else {
        QVERIFY(harness.controller->permissionRequest().isEmpty());
        QCOMPARE(view->resolutions.constLast(), qMakePair(quint64(2), allowed));
    }
}

void WindowControllerTest::backgroundRequestsRemainTemporary() {
    PermissionHarness harness;
    QVERIFY(harness.create());
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    QVERIFY(harness.addTab());
    view->requestPermission(1);
    QVERIFY(harness.controller->permissionRequest().isEmpty());
    QCOMPARE(view->dismissals, QList<quint64>{1});
    QCOMPARE(harness.verdict(), eden::core::PermissionStore::Ask);
    harness.controller->setActiveIndex(0);
    view->requestPermission(2);
    QCOMPARE(harness.controller->permissionRequest().value("id").toULongLong(), quint64(2));
    QCOMPARE(view->resolutions.size(), 1);
}

void WindowControllerTest::previewActivityUpdatesWhileOpen() {
    PermissionHarness harness;
    QVERIFY(harness.create());
    auto *view = harness.addTab();
    QVERIFY(view);
    QVERIFY(harness.controller->tabPreview(0).value("activityIndicators").toList().isEmpty());
    QSignalSpy revision(harness.controller.get(), &eden::core::WindowController::tabPreviewRevisionChanged);
    view->setActivity(6);
    QVERIFY(!revision.isEmpty());
    const auto indicators = harness.controller->tabPreview(0).value("activityIndicators").toList();
    QCOMPARE(indicators.size(), 2);
    QCOMPARE(indicators[0].toMap().value("icon").toString(), QString("microphone"));
    QCOMPARE(indicators[1].toMap().value("icon").toString(), QString("monitor"));
    view->setActivity(0);
    QVERIFY(harness.controller->tabPreview(0).value("activityIndicators").toList().isEmpty());
}

void WindowControllerTest::previewCaptureSkipsMemorySampling() {
    PermissionHarness harness;
    QVERIFY(harness.create());
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    view->thumbnails = true;
    const int revision = harness.controller->tabPreviewRevision();
    QVERIFY(harness.addTab());
    QCOMPARE(view->captures, 1);
    QCOMPARE(view->processQueries, 0);
    QCOMPARE(harness.controller->tabPreviewRevision(), revision);
    for (int count = 0; count < 10; ++count) {
        harness.controller->setActiveIndex(0);
        harness.controller->setActiveIndex(1);
    }
    QCOMPARE(view->captures, 1);
    QCOMPARE(view->processQueries, 0);
    view->finishThumbnail();
    QCOMPARE(view->processQueries, 0);
    QCOMPARE(harness.controller->tabPreviewRevision(), revision + 1);
    const QVariantMap preview = harness.controller->tabPreview(0);
    QVERIFY(view->processQueries > 0);
    QCOMPARE(QUrl(preview.value("thumbnail").toString()).scheme(), QString("image"));
    view->processQueries = 0;
    QTRY_COMPARE_WITH_TIMEOUT(view->captures, 2, 3000);
    QCOMPARE(view->processQueries, 0);
    view->finishThumbnail();
}

void WindowControllerTest::previewCapturePreservesPendingTabs() {
    PermissionHarness harness;
    QVERIFY(harness.create());
    PermissionEngine *first = harness.addTab();
    QVERIFY(first);
    first->thumbnails = true;
    PermissionEngine *second = harness.addTab();
    QVERIFY(second);
    second->thumbnails = true;
    first->finishThumbnail();
    harness.controller->setActiveIndex(0);
    second->finishThumbnail();
    for (int count = 0; count < 10; ++count) {
        harness.controller->setActiveIndex(1);
        harness.controller->setActiveIndex(0);
    }
    QCOMPARE(first->captures, 1);
    QCOMPARE(second->captures, 1);
    QTRY_COMPARE_WITH_TIMEOUT(first->captures, 2, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(second->captures, 2, 3000);
    QCOMPARE(first->processQueries, 0);
    QCOMPARE(second->processQueries, 0);
    first->finishThumbnail();
    second->finishThumbnail();
}

void WindowControllerTest::previewCaptureRejectsStaleImages() {
    PermissionHarness harness;
    QVERIFY(harness.create());
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    view->thumbnails = true;
    QVERIFY(harness.addTab());
    QCOMPARE(view->captures, 1);
    view->load(QUrl("https://permissions.example/different"));
    const int revision = harness.controller->tabPreviewRevision();
    view->finishThumbnail();
    QCOMPARE(harness.controller->tabPreviewRevision(), revision);
    QVERIFY(!harness.controller->tabPreview(0).contains("thumbnail"));
}

void WindowControllerTest::tabSwitchMetricIncludesPreviewWork() {
#if EDEN_ENABLE_AUTOMATION
    const QByteArray previous = qgetenv("EDEN_PERF");
    const auto restoreEnvironment = qScopeGuard([previous] {
        if (previous.isNull()) {
            qunsetenv("EDEN_PERF");
        } else {
            qputenv("EDEN_PERF", previous);
        }
    });
    qputenv("EDEN_PERF", "1");
    QQuickWindow window;
    PermissionHarness harness;
    QVERIFY(harness.create(false, &window));
    PermissionEngine *view = harness.addTab();
    QVERIFY(view);
    QVERIFY(QMetaObject::invokeMethod(&window, "frameSwapped", Qt::DirectConnection));
    const quint64 sequence = eden::core::PerformanceMetrics::sequence();
    view->thumbnails = true;
    view->captureDelayMs = 40;
    QVERIFY(harness.addTab());
    QCOMPARE(view->captures, 1);
    QVERIFY(QMetaObject::invokeMethod(&window, "frameSwapped", Qt::DirectConnection));
    const QJsonArray samples = eden::core::PerformanceMetrics::samplesAfter(sequence);
    QCOMPARE(samples.size(), 1);
    QCOMPARE(samples.first().toObject().value("name").toString(), QString("tabswitch.input_to_frame_ms"));
    QVERIFY(samples.first().toObject().value("value").toDouble() >= 35.0);
    view->finishThumbnail();
#else
    QSKIP("Tab switch metrics require automation support");
#endif
}

QTEST_MAIN(WindowControllerTest)

#include "windowcontroller_test.moc"
