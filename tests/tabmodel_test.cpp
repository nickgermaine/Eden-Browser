#include "core/window/tabmodel.h"
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

class FakeEngineView final : public eden::engine::EngineView {
    Q_OBJECT

  public:
    FakeEngineView()
        : EngineView() {}

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
    void openDevTools() override {}
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

  private:
    QUrl m_url;
    QString m_title;
    QUrl m_favicon;
    bool m_muted = false;
};

class TabModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void addUsesSingleInsertRange();
    void closeUsesSingleRemoveRange();
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
        false);
    const int row = model.addTab(QUrl("eden://settings"));
    QCOMPARE(engineCount, 0);
    QCOMPARE(model.data(model.index(row), eden::core::TabModel::TitleRole).toString(), QString("Settings"));
    QCOMPARE(model.data(model.index(row), eden::core::TabModel::UrlRole).toUrl(), QUrl("eden://settings"));
    QCOMPARE(model.data(model.index(row), eden::core::TabModel::InternalPageRole).toString(), QString("settings"));
    QVERIFY(model.engineAt(row) == nullptr);
    const int editorRow = model.addTab(QUrl("eden://theme-editor"));
    QCOMPARE(engineCount, 0);
    QCOMPARE(model.data(model.index(editorRow), eden::core::TabModel::TitleRole).toString(), QString("Theme Editor"));
    QCOMPARE(model.data(model.index(editorRow), eden::core::TabModel::InternalPageRole).toString(), QString("theme-editor"));
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
    connect(&model, &eden::core::TabModel::externalViewRequested, &model,
            [&received, &destination](eden::engine::EngineNewViewRequest *request) {
                received = request && request->requestedUrl() == destination && request->isUserInitiated() &&
                           request->disposition() == eden::engine::EngineView::Disposition::NewBackgroundTab;
            });
    view->requestNewView(destination, eden::engine::EngineView::Disposition::NewBackgroundTab);
    QVERIFY(received);
}

void TabModelTest::qtWebEngineBridgeMethodsArePublic() {
    const QMetaObject &metaObject = eden::engine::QtWebEngineView::staticMetaObject;
    const QList<QByteArray> methods = {"handleNewWindow(QObject*)", "handleFullScreen(bool)", "handleContextMenu(QPoint,QUrl,QString,bool)",
                                       "handleCertificateError()"};
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
    component.setData(R"QML(import QtQml
QtObject {
    required property var bridge
    Component.onCompleted: {
        bridge.handleNewWindow(null)
        bridge.handleContextMenu(Qt.point(0, 0), "", "", false)
        bridge.handleFullScreen(true)
    }
}
)QML",
                      QUrl());
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{"bridge", QVariant::fromValue(&view)}}));
    QVERIFY2(object, qPrintable(component.errorString()));
    QCOMPARE(fullscreenRequested.size(), 1);
    QVERIFY(fullscreenRequested.first().at(0).toBool());
    QCOMPARE(contextMenuRequested.size(), 1);
    const eden::engine::ContextMenuInfo info = contextMenuRequested.first().at(0).value<eden::engine::ContextMenuInfo>();
    QCOMPARE(info.position, QPoint(0, 0));
}

QTEST_GUILESS_MAIN(TabModelTest)

#include "tabmodel_test.moc"
