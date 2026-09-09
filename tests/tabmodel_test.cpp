#include "core/window/tabmodel.h"
#include "engine/engineprofile.h"
#include "engine/engineprofilemap.h"
#include "engine/engineregistry.h"
#include "engine/engineview.h"
#include "engine/qtwebengine/qtwebengineview.h"

#include <QMetaMethod>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QtTest>

class FakeNewViewRequest final : public eden::engine::EngineNewViewRequest {
  public:
    FakeNewViewRequest(const QUrl &url, eden::engine::EngineView::Disposition disposition)
        : EngineNewViewRequest(url, disposition, true) {}

    bool openIn(eden::engine::EngineView *target) override {
        if (!target) {
            return false;
        }
        target->load(requestedUrl());
        return true;
    }
};

class FakeEngineView : public eden::engine::EngineView {
    Q_OBJECT

  public:
    explicit FakeEngineView(QString name = "Blink (Qt)", Capabilities capabilityFlags = {})
        : EngineView(),
          m_name(std::move(name)),
          m_capabilities(capabilityFlags) {}

    QUrl url() const override {
        return m_url;
    }
    QString title() const override {
        return m_title;
    }
    QUrl faviconUrl() const override {
        return m_favicon;
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
        return m_muted;
    }
    QString securityState() const override {
        return m_url.scheme() == "https" ? "secure" : "local";
    }
    QString backendName() const override {
        return m_name;
    }
    Capabilities capabilities() const override {
        return m_capabilities;
    }

    void load(const QUrl &url) override {
        m_url = url;
        m_title = url.host();
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
    void openDevTools() override {
        setDevToolsOpen(!devToolsOpen());
    }
    void findInPage(const QString &, FindFlags) override {}
    void attach(QQuickItem *) override {}
    void setMuted(bool muted) override {
        if (m_muted == muted) {
            return;
        }
        m_muted = muted;
        emit mutedChanged();
    }
    void setFavicon(const QUrl &favicon) {
        if (m_favicon == favicon) {
            return;
        }
        m_favicon = favicon;
        emit faviconUrlChanged();
    }
    void requestNewView(const QUrl &url, Disposition disposition) {
        FakeNewViewRequest request(url, disposition);
        emit newViewRequested(&request);
    }
    QVariantMap serializeState() const override {
        QVariantMap state = EngineView::serializeState();
        state.insert("scroll", m_scroll);
        return state;
    }
    void restoreState(const QVariantMap &state) override {
        m_scroll = state.value("scroll").toInt();
        EngineView::restoreState(state);
    }
    void setScroll(int scroll) {
        m_scroll = scroll;
    }
    int scroll() const {
        return m_scroll;
    }

  private:
    QUrl m_url;
    QString m_title;
    QUrl m_favicon;
    bool m_muted = false;
    QString m_name;
    Capabilities m_capabilities;
    int m_scroll = 0;
};

static eden::engine::EngineProfileParameters fakeProfileParameters(bool privateProfile, eden::engine::Backend backend) {
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = backend;
    parameters.privateProfile = privateProfile;
    return parameters;
}

class FakeEngineProfile final : public eden::engine::EngineProfile {
  public:
    FakeEngineProfile(bool privateProfile, eden::engine::Backend backend)
        : EngineProfile(fakeProfileParameters(privateProfile, backend)),
          m_backend(backend) {}

    QObject *nativeProfile() const override {
        return nullptr;
    }
    void clearData() override {
        ++clearCount;
    }

    eden::engine::Backend m_backend;
    int clearCount = 0;
};

class TabModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void addUsesSingleInsertRange();
    void closeUsesSingleRemoveRange();
    void closeClosesPerTabDevTools();
    void moveUsesMoveSignal();
    void transferPreservesEngineAndClosesEmptySource();
    void transferWithinModelUsesInsertionBoundary();
    void transferRejectsDifferentPrivacyModes();
    void undoRestoresClosedTab();
    void contextActionsWork();
    void pinningMovesTabToPinnedRegion();
    void duplicateInsertsNextToOriginal();
    void closeOthersSparesPinnedTabs();
    void undoCloseRestoresIntoMatchingRegion();
    void faviconChangesReachTheModel();
    void internalPageDoesNotCreateEngine();
    void urlCredentialsAreNotExposed();
    void newViewRequestCrossesTheEngineSeam();
    void qtWebEngineBridgeMethodsArePublic();
    void qmlCanInvokeQtWebEngineBridge();
    void registryExposesOnlyCompiledBackends();
    void registryBuildsSelectionActions();
    void backendOverrideCreatesMixedWindow();
    void profilesAreSeparatedByBackendAndPrivacy();
    void conversionRetagsAndPreservesState();
    void globalConversionRehydratesOnlyActiveTab();
    void discardedRoleTracksLazyConversion();
    void restoreKeepsPinnedRegionAndBackend();
};

static eden::core::TabModel createModel() {
    return eden::core::TabModel([] { return std::make_unique<FakeEngineView>(); }, false);
}

void TabModelTest::addUsesSingleInsertRange() {
    auto model = createModel();
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QCOMPARE(model.addTab(QUrl("https://example.com")), 0);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(inserted.size(), 1);
    QCOMPARE(inserted.first().at(1).toInt(), 0);
    QCOMPARE(inserted.first().at(2).toInt(), 0);
    QCOMPARE(reset.size(), 0);
}

void TabModelTest::closeUsesSingleRemoveRange() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QVERIFY(model.closeTab(0));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(removed.size(), 1);
    QCOMPARE(reset.size(), 0);
}

void TabModelTest::closeClosesPerTabDevTools() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    auto *view = qobject_cast<FakeEngineView *>(model.engineAt(0));
    QVERIFY(view);
    view->openDevTools();
    QVERIFY(view->devToolsOpen());
    QVERIFY(model.closeTab(0));
    QVERIFY(model.undoClose());
    QVERIFY(model.isDiscarded(0));
    QVERIFY(model.ensureEngine(0));
    view = qobject_cast<FakeEngineView *>(model.engineAt(0));
    QVERIFY(view);
    QVERIFY(!view->devToolsOpen());
}

void TabModelTest::moveUsesMoveSignal() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    model.addTab(QUrl("https://three.example"));
    QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy tabMoved(&model, &eden::core::TabModel::tabMoved);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QVERIFY(model.moveTab(0, 2));
    QCOMPARE(model.data(model.index(2), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://one.example"));
    QCOMPARE(moved.size(), 1);
    QCOMPARE(tabMoved.size(), 1);
    QCOMPARE(tabMoved.first().at(0).toInt(), 0);
    QCOMPARE(tabMoved.first().at(1).toInt(), 2);
    QCOMPARE(reset.size(), 0);
}

void TabModelTest::transferPreservesEngineAndClosesEmptySource() {
    auto source = createModel();
    auto destination = createModel();
    source.addTab(QUrl("https://one.example"));
    source.addTab(QUrl("https://two.example"));
    destination.addTab(QUrl("https://destination.example"));
    source.pinTab(0, true);
    QObject *transferredEngine = source.engineAt(0);
    QSignalSpy sourceRemoved(&source, &QAbstractItemModel::rowsRemoved);
    QSignalSpy destinationInserted(&destination, &QAbstractItemModel::rowsInserted);
    QSignalSpy closeRequested(&source, &eden::core::TabModel::tabCloseRequestedForWindow);

    QVERIFY(source.transferTabTo(0, &destination, 1));
    QCOMPARE(source.rowCount(), 1);
    QCOMPARE(destination.rowCount(), 2);
    QCOMPARE(destination.engineAt(0), transferredEngine);
    QVERIFY(destination.data(destination.index(0), eden::core::TabModel::PinnedRole).toBool());
    QCOMPARE(sourceRemoved.size(), 1);
    QCOMPARE(destinationInserted.size(), 1);
    QCOMPARE(closeRequested.size(), 0);

    QVERIFY(source.transferTabTo(0, &destination, destination.rowCount()));
    QCOMPARE(source.rowCount(), 0);
    QCOMPARE(destination.rowCount(), 3);
    QCOMPARE(closeRequested.size(), 1);
}

void TabModelTest::transferWithinModelUsesInsertionBoundary() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    model.addTab(QUrl("https://three.example"));

    QVERIFY(model.transferTabTo(0, &model, 3));
    QCOMPARE(model.data(model.index(2), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://one.example"));
    QVERIFY(model.transferTabTo(2, &model, 0));
    QCOMPARE(model.data(model.index(0), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://one.example"));
}

void TabModelTest::transferRejectsDifferentPrivacyModes() {
    auto normal = createModel();
    eden::core::TabModel privateModel([] { return std::make_unique<FakeEngineView>(); }, true);
    normal.addTab(QUrl("https://normal.example"));
    privateModel.addTab(QUrl("https://private.example"));

    QVERIFY(!normal.transferTabTo(0, &privateModel, 1));
    QCOMPARE(normal.rowCount(), 1);
    QCOMPARE(privateModel.rowCount(), 1);
}

void TabModelTest::undoRestoresClosedTab() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    QVERIFY(model.closeTab(0));
    QVERIFY(model.canUndoClose());
    QVERIFY(model.undoClose());
    QCOMPARE(model.data(model.index(0), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://one.example"));
    QVERIFY(!model.canUndoClose());
}

void TabModelTest::contextActionsWork() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    QSignalSpy pinnedCountChanged(&model, &eden::core::TabModel::pinnedCountChanged);
    model.pinTab(0, true);
    QVERIFY(model.data(model.index(0), eden::core::TabModel::PinnedRole).toBool());
    QCOMPARE(model.pinnedCount(), 1);
    QCOMPARE(pinnedCountChanged.size(), 1);
    QCOMPARE(model.duplicateTab(0), 1);
    QCOMPARE(model.rowCount(), 2);
    model.toggleMuted(0);
    QVERIFY(model.data(model.index(0), eden::core::TabModel::MutedRole).toBool());
    model.closeOthers(0);
    QCOMPARE(model.rowCount(), 1);
    model.pinTab(0, false);
    QVERIFY(!model.data(model.index(0), eden::core::TabModel::PinnedRole).toBool());
    QCOMPARE(model.pinnedCount(), 0);
    QCOMPARE(pinnedCountChanged.size(), 2);
}

void TabModelTest::pinningMovesTabToPinnedRegion() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    model.addTab(QUrl("https://three.example"));
    model.pinTab(2, true);
    QCOMPARE(model.data(model.index(0), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://three.example"));
    QVERIFY(model.data(model.index(0), eden::core::TabModel::PinnedRole).toBool());
    model.pinTab(1, true);
    QCOMPARE(model.data(model.index(1), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://one.example"));
    QCOMPARE(model.pinnedCount(), 2);
    model.pinTab(0, false);
    QCOMPARE(model.data(model.index(1), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://three.example"));
    QVERIFY(!model.data(model.index(1), eden::core::TabModel::PinnedRole).toBool());
    QCOMPARE(model.pinnedCount(), 1);
}

void TabModelTest::duplicateInsertsNextToOriginal() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    model.addTab(QUrl("https://three.example"));
    QCOMPARE(model.duplicateTab(0), 1);
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.data(model.index(1), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://one.example"));
    QCOMPARE(model.data(model.index(2), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://two.example"));
}

void TabModelTest::closeOthersSparesPinnedTabs() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    model.addTab(QUrl("https://three.example"));
    model.pinTab(0, true);
    model.closeOthers(2);
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(model.data(model.index(0), eden::core::TabModel::PinnedRole).toBool());
    QCOMPARE(model.data(model.index(1), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://three.example"));
}

void TabModelTest::undoCloseRestoresIntoMatchingRegion() {
    auto model = createModel();
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    model.pinTab(0, true);
    QVERIFY(model.closeTab(1));
    model.pinTab(0, false);
    QVERIFY(model.undoClose());
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(!model.data(model.index(1), eden::core::TabModel::PinnedRole).toBool());
}

void TabModelTest::faviconChangesReachTheModel() {
    auto model = createModel();
    const int row = model.addTab(QUrl("https://example.com"));
    auto *view = qobject_cast<FakeEngineView *>(model.engineAt(row));
    QVERIFY(view);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    const QUrl favicon("https://example.com/favicon.ico");
    view->setFavicon(favicon);
    QCOMPARE(model.data(model.index(row), eden::core::TabModel::FaviconRole).toUrl(), favicon);
    QCOMPARE(changed.size(), 1);
}

void TabModelTest::internalPageDoesNotCreateEngine() {
    int engineCount = 0;
    eden::core::TabModel model(
        [&engineCount] {
            ++engineCount;
            return std::make_unique<FakeEngineView>();
        },
        false
    );
    const int row = model.addTab(QUrl("eden://settings"));
    QCOMPARE(engineCount, 0);
    QCOMPARE(model.data(model.index(row), eden::core::TabModel::TitleRole).toString(), QString("Settings"));
    QCOMPARE(model.data(model.index(row), eden::core::TabModel::UrlRole).toUrl(), QUrl("eden://settings"));
    QCOMPARE(model.data(model.index(row), eden::core::TabModel::InternalPageRole).toString(), QString("settings"));
    QVERIFY(model.engineAt(row) == nullptr);
    const int editorRow = model.addTab(QUrl("eden://theme-editor"));
    QCOMPARE(engineCount, 0);
    QCOMPARE(model.data(model.index(editorRow), eden::core::TabModel::TitleRole).toString(), QString("Theme Editor"));
    QCOMPARE(
        model.data(model.index(editorRow), eden::core::TabModel::InternalPageRole).toString(),
        QString("theme-editor")
    );
    const int newTabRow = model.addTab();
    QCOMPARE(engineCount, 0);
    QCOMPARE(model.data(model.index(newTabRow), eden::core::TabModel::TitleRole).toString(), QString("New Tab"));
    QCOMPARE(model.data(model.index(newTabRow), eden::core::TabModel::UrlRole).toUrl(), QUrl("eden://newtab"));
    QCOMPARE(model.data(model.index(newTabRow), eden::core::TabModel::InternalPageRole).toString(), QString("newtab"));
}

void TabModelTest::urlCredentialsAreNotExposed() {
    auto model = createModel();
    const int row = model.addTab(QUrl("https://user:secret@example.com/private"));
    const QUrl exposedUrl = model.data(model.index(row), eden::core::TabModel::UrlRole).toUrl();
    QCOMPARE(exposedUrl, QUrl("https://example.com/private"));
    auto *view = qobject_cast<FakeEngineView *>(model.engineAt(row));
    QVERIFY(view);
    QCOMPARE(view->url().password(), QString("secret"));
}

void TabModelTest::newViewRequestCrossesTheEngineSeam() {
    auto model = createModel();
    const int row = model.addTab(QUrl("https://source.example"));
    auto *view = qobject_cast<FakeEngineView *>(model.engineAt(row));
    QVERIFY(view);
    const QUrl destination("https://destination.example/new-tab");
    bool received = false;
    connect(
        &model,
        &eden::core::TabModel::externalViewRequested,
        &model,
        [&received, &destination](eden::engine::EngineNewViewRequest *request) {
            received = request && request->requestedUrl() == destination && request->isUserInitiated() &&
                       request->disposition() == eden::engine::EngineView::Disposition::NewBackgroundTab;
        }
    );
    view->requestNewView(destination, eden::engine::EngineView::Disposition::NewBackgroundTab);
    QVERIFY(received);
}

void TabModelTest::qtWebEngineBridgeMethodsArePublic() {
    const QMetaObject &metaObject = eden::engine::QtWebEngineView::staticMetaObject;
    const QList<QByteArray> methods = {
        "handleNewWindow(QObject*)",
        "handleFullScreen(bool)",
        "handleContextMenu(QPoint,QUrl,QUrl,QString,bool)",
        "handleCertificateError()"
    };
    for (const QByteArray &signature : methods) {
        const int index = metaObject.indexOfMethod(signature);
        QVERIFY2(index >= 0, signature.constData());
        QCOMPARE(metaObject.method(index).access(), QMetaMethod::Public);
    }
}

void TabModelTest::qmlCanInvokeQtWebEngineBridge() {
    eden::engine::QtWebEngineView view(nullptr);
    QSignalSpy fullscreenRequested(&view, &eden::engine::EngineView::fullscreenRequested);
    QSignalSpy contextMenuRequested(&view, &eden::engine::EngineView::contextMenuRequested);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"QML(import QtQml
QtObject {
    required property var bridge
    Component.onCompleted: {
        bridge.handleNewWindow(null)
        bridge.handleContextMenu(Qt.point(0, 0), "https://link.example", "https://image.example/picture.png", "", false)
        bridge.handleFullScreen(true)
    }
}
)QML",
        QUrl()
    );
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{"bridge", QVariant::fromValue(&view)}}));
    QVERIFY2(object, qPrintable(component.errorString()));
    QCOMPARE(fullscreenRequested.size(), 1);
    QVERIFY(fullscreenRequested.first().at(0).toBool());
    QCOMPARE(contextMenuRequested.size(), 1);
    const eden::engine::ContextMenuInfo info =
        contextMenuRequested.first().at(0).value<eden::engine::ContextMenuInfo>();
    QCOMPARE(info.position, QPoint(0, 0));
    QCOMPARE(info.linkUrl, QUrl("https://link.example"));
    QCOMPARE(info.mediaUrl, QUrl("https://image.example/picture.png"));
}

void TabModelTest::registryExposesOnlyCompiledBackends() {
    eden::engine::EngineRegistry registry({
        {eden::engine::Backend::QtWebEngine, "qtwebengine", "Blink (Qt)", false},
        {eden::engine::Backend::Servo, "servo", "Servo", true},
    });
    QCOMPARE(registry.engines().size(), 2);
    QCOMPARE(registry.displayName(eden::engine::Backend::QtWebEngine), QString("Blink (Qt)"));
    QCOMPARE(registry.backendForId("SERVO"), std::optional(eden::engine::Backend::Servo));
    QVERIFY(!registry.contains(eden::engine::Backend::Cef));
}

void TabModelTest::registryBuildsSelectionActions() {
    eden::engine::EngineRegistry registry({
        {eden::engine::Backend::Cef, "cef", "Blink", false},
        {eden::engine::Backend::QtWebEngine, "qtwebengine", "Blink (Qt)", false},
    });
    const QVariantList actions = registry.selectionActions("cef");
    QCOMPARE(actions.size(), 2);
    QCOMPARE(actions.at(0).toMap().value("title").toString(), QString("Blink"));
    QCOMPARE(actions.at(0).toMap().value("icon").toString(), QString("check"));
    QCOMPARE(actions.at(1).toMap().value("title").toString(), QString("Blink (Qt)"));
    QCOMPARE(actions.at(1).toMap().value("icon").toString(), QString("programming"));
}

void TabModelTest::backendOverrideCreatesMixedWindow() {
    eden::engine::EngineRegistry registry({
        {eden::engine::Backend::QtWebEngine, "qtwebengine", "Blink (Qt)", false},
        {eden::engine::Backend::Servo, "servo", "Servo", true},
    });
    auto factory = [](eden::engine::Backend backend,
                      eden::engine::EngineProfile *) -> std::unique_ptr<eden::engine::EngineView> {
        if (backend == eden::engine::Backend::QtWebEngine) {
            return std::make_unique<eden::engine::QtWebEngineView>(nullptr);
        }
        return std::make_unique<FakeEngineView>("Servo");
    };
    eden::core::TabModel model(factory, {}, &registry, eden::engine::Backend::QtWebEngine, false);
    const int qt = model.addTab(QUrl("https://qt.example"));
    const int servo = model.addTab(QUrl("https://servo.example"), true, "servo");
    QCOMPARE(model.backendIdAt(qt), QString("qtwebengine"));
    QCOMPARE(model.backendIdAt(servo), QString("servo"));
    QCOMPARE(model.data(model.index(qt), eden::core::TabModel::EngineNameRole).toString(), QString("Blink (Qt)"));
    QCOMPARE(model.data(model.index(servo), eden::core::TabModel::EngineNameRole).toString(), QString("Servo"));
    auto *qtView = qobject_cast<eden::engine::QtWebEngineView *>(model.engineAt(qt));
    QVERIFY(qtView);
    QVERIFY(qtView->capabilities().testFlag(eden::engine::EngineView::ThumbnailCapture));
    QVERIFY(qtView->capabilities().testFlag(eden::engine::EngineView::PerTabMute));
    QCOMPARE(qobject_cast<FakeEngineView *>(model.engineAt(servo))->backendName(), QString("Servo"));
}

void TabModelTest::profilesAreSeparatedByBackendAndPrivacy() {
    eden::engine::EngineProfileMap normal(false, [](eden::engine::Backend backend) {
        return std::make_shared<FakeEngineProfile>(false, backend);
    });
    eden::engine::EngineProfileMap privateProfiles(true, [](eden::engine::Backend backend) {
        return std::make_shared<FakeEngineProfile>(true, backend);
    });
    auto normalQt = normal.profile(eden::engine::Backend::QtWebEngine);
    auto normalServo = normal.profile(eden::engine::Backend::Servo);
    auto privateQt = privateProfiles.profile(eden::engine::Backend::QtWebEngine);
    QCOMPARE(normal.profile(eden::engine::Backend::QtWebEngine), normalQt);
    QVERIFY(normalQt != normalServo);
    QVERIFY(normalQt != privateQt);
    QVERIFY(!normalQt->isPrivate());
    QVERIFY(privateQt->isPrivate());
    normal.clearData();
    QCOMPARE(static_cast<FakeEngineProfile *>(normalQt.get())->clearCount, 1);
    QCOMPARE(static_cast<FakeEngineProfile *>(normalServo.get())->clearCount, 1);
}

void TabModelTest::conversionRetagsAndPreservesState() {
    eden::engine::EngineRegistry registry({
        {eden::engine::Backend::QtWebEngine, "qtwebengine", "Blink (Qt)", false},
        {eden::engine::Backend::Servo, "servo", "Servo", true},
    });
    int created = 0;
    auto factory = [&created](eden::engine::Backend backend, eden::engine::EngineProfile *) {
        ++created;
        return std::make_unique<FakeEngineView>(backend == eden::engine::Backend::Servo ? "Servo" : "Blink (Qt)");
    };
    eden::core::TabModel model(factory, {}, &registry, eden::engine::Backend::QtWebEngine, false);
    const QUrl url("https://conversion.example/path");
    const int row = model.addTab(url);
    qobject_cast<FakeEngineView *>(model.engineAt(row))->setScroll(840);
    QCOMPARE(created, 1);
    QVERIFY(model.convertTab(row, "servo"));
    QCOMPARE(created, 2);
    QCOMPARE(model.backendIdAt(row), QString("servo"));
    QCOMPARE(model.data(model.index(row), eden::core::TabModel::UrlRole).toUrl(), url);
    QCOMPARE(qobject_cast<FakeEngineView *>(model.engineAt(row))->backendName(), QString("Servo"));
    QCOMPARE(qobject_cast<FakeEngineView *>(model.engineAt(row))->scroll(), 840);
}

void TabModelTest::globalConversionRehydratesOnlyActiveTab() {
    eden::engine::EngineRegistry registry({
        {eden::engine::Backend::QtWebEngine, "qtwebengine", "Blink (Qt)", false},
        {eden::engine::Backend::Servo, "servo", "Servo", true},
    });
    int created = 0;
    auto factory = [&created](eden::engine::Backend backend, eden::engine::EngineProfile *) {
        ++created;
        return std::make_unique<FakeEngineView>(backend == eden::engine::Backend::Servo ? "Servo" : "Blink (Qt)");
    };
    eden::core::TabModel model(factory, {}, &registry, eden::engine::Backend::QtWebEngine, false);
    model.addTab(QUrl("https://one.example"));
    model.addTab(QUrl("https://two.example"));
    model.addTab(QUrl("https://three.example"));
    QCOMPARE(created, 3);
    QVERIFY(model.convertAllTabs("servo", 1));
    QCOMPARE(created, 4);
    QVERIFY(model.engineAt(0) == nullptr);
    QVERIFY(model.engineAt(1) != nullptr);
    QVERIFY(model.engineAt(2) == nullptr);
    QVERIFY(model.ensureEngine(2));
    QCOMPARE(created, 5);
    QCOMPARE(model.data(model.index(2), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://three.example"));
}

void TabModelTest::discardedRoleTracksLazyConversion() {
    eden::engine::EngineRegistry registry({
        {eden::engine::Backend::QtWebEngine, "qtwebengine", "Blink (Qt)", false},
        {eden::engine::Backend::Servo, "servo", "Servo", true},
    });
    auto factory = [](eden::engine::Backend backend, eden::engine::EngineProfile *) {
        return std::make_unique<FakeEngineView>(backend == eden::engine::Backend::Servo ? "Servo" : "Blink (Qt)");
    };
    eden::core::TabModel model(factory, {}, &registry, eden::engine::Backend::QtWebEngine, false);
    model.addTab(QUrl("https://active.example"));
    model.addTab(QUrl("https://background.example"));
    const quint64 backgroundId = model.tabIdAt(1);
    QVERIFY(backgroundId > 0);
    QVERIFY(model.convertAllTabs("servo", 0));
    QVERIFY(!model.data(model.index(0), eden::core::TabModel::DiscardedRole).toBool());
    QVERIFY(model.data(model.index(1), eden::core::TabModel::DiscardedRole).toBool());
    QCOMPARE(model.tabIdAt(1), backgroundId);
    QVERIFY(model.ensureEngine(1));
    QVERIFY(!model.data(model.index(1), eden::core::TabModel::DiscardedRole).toBool());
}

void TabModelTest::restoreKeepsPinnedRegionAndBackend() {
    eden::engine::EngineRegistry registry({
        {eden::engine::Backend::QtWebEngine, "qtwebengine", "Blink (Qt)", false},
        {eden::engine::Backend::Servo, "servo", "Servo", true},
    });
    int created = 0;
    auto factory = [&created](eden::engine::Backend, eden::engine::EngineProfile *) {
        ++created;
        return std::make_unique<FakeEngineView>();
    };
    eden::core::TabModel model(factory, {}, &registry, eden::engine::Backend::QtWebEngine, false);
    QJsonArray tabs;
    tabs.append(
        QJsonObject{{"url", "https://normal.example"}, {"title", "Normal"}, {"pinned", false}, {"backend", "servo"}}
    );
    tabs.append(
        QJsonObject{
            {"url", "https://pinned.example"},
            {"title", "Pinned"},
            {"pinned", true},
            {"backend", "qtwebengine"}
        }
    );
    model.restoreTabs(tabs);
    QCOMPARE(created, 0);
    QCOMPARE(model.pinnedCount(), 1);
    QVERIFY(model.data(model.index(0), eden::core::TabModel::PinnedRole).toBool());
    QCOMPARE(model.data(model.index(0), eden::core::TabModel::UrlRole).toUrl(), QUrl("https://pinned.example"));
    QCOMPARE(model.backendIdAt(1), QString("servo"));
    QCOMPARE(model.sessionTabs().at(1).toObject().value("backend").toString(), QString("servo"));
    QVERIFY(model.ensureEngine(1));
    QCOMPARE(created, 1);
}

QTEST_GUILESS_MAIN(TabModelTest)

#include "tabmodel_test.moc"
