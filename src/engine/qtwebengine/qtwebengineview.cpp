#include "engine/qtwebengine/qtwebengineview.h"
#include "engine/engineprofile.h"

#include <QMetaEnum>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>

#include <algorithm>

namespace eden::engine {

class QtWebEngineNewViewRequest final : public EngineNewViewRequest {
  public:
    QtWebEngineNewViewRequest(QWebEngineNewWindowRequest *request, QQmlEngine *engine)
        : EngineNewViewRequest(request->requestedUrl(),
                               request->destination() == QWebEngineNewWindowRequest::InNewWindow ? EngineView::Disposition::NewWindow
                               : request->destination() == QWebEngineNewWindowRequest::InNewBackgroundTab
                                   ? EngineView::Disposition::NewBackgroundTab
                                   : EngineView::Disposition::NewForegroundTab,
                               request->isUserInitiated()),
          m_request(request),
          m_engine(engine) {}

    bool openIn(EngineView *target) override {
        QtWebEngineView *qtTarget = qobject_cast<QtWebEngineView *>(target);
        return qtTarget && m_request && m_engine && qtTarget->acceptNewWindowRequest(m_request, m_engine);
    }

  private:
    QPointer<QWebEngineNewWindowRequest> m_request;
    QPointer<QQmlEngine> m_engine;
};

QtWebEngineView::QtWebEngineView(EngineProfile *profile, QObject *parent)
    : EngineView(parent),
      m_profile(profile) {}

static QUrl aliasEngineInternalUrl(QUrl url) {
    if (url.scheme() == "chrome") {
        url.setScheme("eden");
    }
    return url;
}

QtWebEngineView::~QtWebEngineView() = default;

QUrl QtWebEngineView::url() const {
    return aliasEngineInternalUrl(m_view ? m_url : m_pendingUrl);
}

QString QtWebEngineView::title() const {
    return m_title;
}

QUrl QtWebEngineView::faviconUrl() const {
    return m_faviconUrl;
}

int QtWebEngineView::loadProgress() const {
    return m_loadProgress;
}

bool QtWebEngineView::isLoading() const {
    return m_loading;
}

bool QtWebEngineView::canGoBack() const {
    return m_canGoBack;
}

bool QtWebEngineView::canGoForward() const {
    return m_canGoForward;
}

bool QtWebEngineView::isAudible() const {
    return m_audible;
}

bool QtWebEngineView::isMuted() const {
    return m_muted;
}

QString QtWebEngineView::securityState() const {
    const QUrl current = m_view ? m_url : QUrl();
    const QString scheme = current.scheme();
    if (current.isEmpty() || scheme == "eden" || scheme == "about" || scheme == "chrome") {
        return "local";
    }
    if (scheme == "https" && !m_certificateError) {
        return "secure";
    }
    return "insecure";
}

QString QtWebEngineView::backendName() const {
    return "Blink (Qt)";
}

EngineView::Capabilities QtWebEngineView::capabilities() const {
    return DockedDevtools | ThumbnailCapture | OsrCompositing | PerTabMute;
}

qint64 QtWebEngineView::rendererProcessId() const {
    return value("renderProcessPid").toLongLong();
}

bool QtWebEngineView::containsPageScenePoint(const QPointF &windowScenePoint) const {
    QQuickItem *item = qobject_cast<QQuickItem *>(m_view.data());
    if (!item) {
        return false;
    }
    const QPointF localPoint = item->mapFromScene(windowScenePoint);
    return QRectF(QPointF(), item->size()).contains(localPoint);
}

QUrl QtWebEngineView::internalUrlFor(const QUrl &url) const {
    if (url.scheme() != "eden" || url.host().isEmpty()) {
        return {};
    }
    const QString host = url.host();
    if (host == "newtab" || host == "new-tab" || host == "new-tab-page") {
        return QUrl("about:blank");
    }
    QUrl mapped = url;
    mapped.setScheme("chrome");
    return mapped;
}

void QtWebEngineView::load(const QUrl &url) {
    m_pendingUrl = url;
    if (m_view) {
        m_view->setProperty("url", url);
    } else {
        emit urlChanged();
    }
}

void QtWebEngineView::back() {
    invoke("goBack");
}

void QtWebEngineView::forward() {
    invoke("goForward");
}

QVariantList QtWebEngineView::navigationHistory(int direction, int maximumItems) const {
    QVariantList entries;
    QWebEngineHistory *history = value("history").value<QWebEngineHistory *>();
    if (!history || direction == 0 || maximumItems <= 0) {
        return entries;
    }
    const int normalizedDirection = direction < 0 ? -1 : 1;
    const int currentIndex = history->currentItemIndex();
    const int boundedMaximum = std::min(maximumItems, 100);
    entries.reserve(boundedMaximum);
    for (int distance = 1; distance <= boundedMaximum; ++distance) {
        const int offset = normalizedDirection * distance;
        const int itemIndex = currentIndex + offset;
        if (itemIndex < 0 || itemIndex >= history->count()) {
            break;
        }
        const QWebEngineHistoryItem item = history->itemAt(itemIndex);
        QVariantMap entry;
        entry.insert("id", offset);
        entry.insert("title", item.title().isEmpty() ? item.url().toDisplayString(QUrl::RemovePassword) : item.title());
        entry.insert("subtitle", item.url().toDisplayString(QUrl::RemovePassword));
        entries.append(entry);
    }
    return entries;
}

void QtWebEngineView::goToHistoryOffset(int offset) {
    QWebEngineHistory *history = value("history").value<QWebEngineHistory *>();
    if (!history || offset == 0) {
        return;
    }
    const int itemIndex = history->currentItemIndex() + offset;
    if (itemIndex >= 0 && itemIndex < history->count()) {
        history->goToItem(history->itemAt(itemIndex));
    }
}

void QtWebEngineView::reload() {
    invoke("reload");
}

void QtWebEngineView::stop() {
    invoke("stop");
}

void QtWebEngineView::openDevTools() {
    if (devToolsOpen()) {
        closeDevTools();
        return;
    }
    setDevToolsOpen(true);
}

void QtWebEngineView::closeDevTools() {
    m_pendingInspect = false;
    m_devToolsHost.clear();
    if (m_devToolsView) {
        if (QQuickItem *item = qobject_cast<QQuickItem *>(m_devToolsView.data())) {
            item->setParentItem(nullptr);
        }
        m_devToolsView->deleteLater();
        m_devToolsView.clear();
    }
    setDevToolsOpen(false);
}

void QtWebEngineView::attachDevTools(QQuickItem *viewport) {
    if (!viewport) {
        m_devToolsHost.clear();
        if (QQuickItem *item = qobject_cast<QQuickItem *>(m_devToolsView.data())) {
            item->setParentItem(nullptr);
            item->setVisible(false);
        }
        return;
    }
    if (!devToolsOpen()) {
        return;
    }
    m_devToolsHost = viewport;
    if (!ensureView(qmlEngine(viewport))) {
        return;
    }
    if (!m_devToolsView) {
        QQmlEngine *engine = qmlEngine(viewport);
        if (!engine) {
            return;
        }
        QQmlComponent component(engine);
        component.setData(R"QML(import QtQuick
import QtWebEngine
WebEngineView {
    anchors.fill: parent
}
)QML",
                          QUrl());
        if (component.isError()) {
            qWarning().noquote() << component.errorString();
            return;
        }
        QObject *created = component.create();
        QQuickItem *item = qobject_cast<QQuickItem *>(created);
        if (!item) {
            delete created;
            return;
        }
        item->setParent(this);
        if (m_profile) {
            item->setProperty("profile", QVariant::fromValue(m_profile->nativeProfile()));
        }
        item->setProperty("inspectedView", QVariant::fromValue(m_view.data()));
        m_devToolsView = item;
    }
    if (QQuickItem *item = qobject_cast<QQuickItem *>(m_devToolsView.data())) {
        item->setParentItem(viewport);
        item->setVisible(true);
    }
    if (m_pendingInspect) {
        m_pendingInspect = false;
        triggerWebAction("InspectElement");
    }
}

void QtWebEngineView::detachDevTools(QQuickItem *viewport) {
    if (viewport && m_devToolsHost == viewport) {
        attachDevTools(nullptr);
    }
}

void QtWebEngineView::findInPage(const QString &text, FindFlags flags) {
    if (!m_view) {
        return;
    }
    QMetaObject::invokeMethod(m_view, "edenFind", Q_ARG(QVariant, text), Q_ARG(QVariant, static_cast<int>(flags)));
}

void QtWebEngineView::attach(QQuickItem *viewport) {
    if (!viewport) {
        return;
    }
    if (!ensureView(qmlEngine(viewport))) {
        return;
    }
    if (QQuickItem *item = qobject_cast<QQuickItem *>(m_view.data())) {
        item->setParentItem(viewport);
    }
}

void QtWebEngineView::releaseFocus() {
    if (QQuickItem *item = qobject_cast<QQuickItem *>(m_view.data())) {
        item->setFocus(false, Qt::MouseFocusReason);
    }
}

bool QtWebEngineView::ensureView(QQmlEngine *engine) {
    if (m_view) {
        return true;
    }
    if (!engine) {
        return false;
    }
    QQmlComponent component(engine);
    component.setData(R"QML(import QtQuick
import QtWebEngine
WebEngineView {
    id: webView
    anchors.fill: parent
    property var edenBridge
    function edenFind(text, flags) {
        findText(text, flags)
    }
    function edenContextCommand(action) {
        triggerWebAction(action)
    }
    onNewWindowRequested: request => { if (edenBridge) edenBridge.handleNewWindow(request) }
    onFullScreenRequested: request => {
        request.accept()
        if (edenBridge) edenBridge.handleFullScreen(request.toggleOn)
    }
    onContextMenuRequested: request => {
        request.accepted = true
        if (edenBridge) edenBridge.handleContextMenu(request.position, request.linkUrl, request.mediaUrl, request.selectedText, request.isContentEditable)
    }
    onJavaScriptConsoleMessage: (level, message, lineNumber, sourceId) => {}
    onCertificateError: error => { if (edenBridge) edenBridge.handleCertificateError() }
}
)QML",
                      QUrl());
    if (component.isError()) {
        qWarning().noquote() << component.errorString();
    }
    QObject *created = component.create();
    QQuickItem *item = qobject_cast<QQuickItem *>(created);
    if (!item) {
        delete created;
        return false;
    }
    item->setParent(this);
    item->setProperty("edenBridge", QVariant::fromValue(static_cast<QObject *>(this)));
    if (m_profile) {
        item->setProperty("profile", QVariant::fromValue(m_profile->nativeProfile()));
    }
    m_view = item;
    connect(m_view, SIGNAL(urlChanged()), this, SLOT(syncState()));
    connect(m_view, SIGNAL(titleChanged()), this, SLOT(syncState()));
    connect(m_view, SIGNAL(iconChanged()), this, SLOT(syncState()));
    connect(m_view, SIGNAL(loadProgressChanged()), this, SLOT(syncState()));
    connect(m_view, SIGNAL(loadingChanged(QWebEngineLoadingInfo)), this, SLOT(syncState()));
    connect(m_view, SIGNAL(canGoBackChanged()), this, SLOT(syncState()));
    connect(m_view, SIGNAL(canGoForwardChanged()), this, SLOT(syncState()));
    connect(m_view, SIGNAL(recentlyAudibleChanged(bool)), this, SLOT(syncState()));
    connect(m_view, SIGNAL(audioMutedChanged(bool)), this, SLOT(syncState()));
    if (!m_pendingUrl.isEmpty()) {
        m_view->setProperty("url", m_pendingUrl);
    }
    syncState();
    return true;
}

bool QtWebEngineView::acceptNewWindowRequest(QWebEngineNewWindowRequest *request, QQmlEngine *engine) {
    if (!request || !ensureView(engine)) {
        return false;
    }
    return QMetaObject::invokeMethod(m_view, "acceptAsNewWindow", Qt::DirectConnection, Q_ARG(QWebEngineNewWindowRequest *, request));
}

void QtWebEngineView::setMuted(bool muted) {
    if (m_view) {
        m_view->setProperty("audioMuted", muted);
    }
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    emit mutedChanged();
}

void QtWebEngineView::executeContextMenuCommand(const QString &command) {
    if (command == "inspect") {
        if (!devToolsOpen()) {
            m_pendingInspect = true;
            openDevTools();
        } else {
            triggerWebAction("InspectElement");
        }
        return;
    }
    static const QHash<QString, QByteArray> actions = {
        {"back", "Back"},
        {"forward", "Forward"},
        {"reload", "Reload"},
        {"cut", "Cut"},
        {"copy", "Copy"},
        {"paste", "Paste"},
        {"select_all", "SelectAll"},
        {"open_link_new_tab", "OpenLinkInNewTab"},
        {"copy_link", "CopyLinkToClipboard"},
        {"download_link", "DownloadLinkToDisk"},
        {"view_source", "ViewSource"},
    };
    const auto found = actions.constFind(command);
    if (found == actions.cend()) {
        return;
    }
    triggerWebAction(found.value());
}

void QtWebEngineView::triggerWebAction(const QByteArray &actionName) {
    if (!m_view) {
        return;
    }
    const int enumIndex = m_view->metaObject()->indexOfEnumerator("WebAction");
    if (enumIndex < 0) {
        return;
    }
    const int action = m_view->metaObject()->enumerator(enumIndex).keyToValue(actionName.constData());
    if (action >= 0) {
        QMetaObject::invokeMethod(m_view, "edenContextCommand", Q_ARG(QVariant, action));
    }
}

void QtWebEngineView::requestThumbnail(const QSize &size, ThumbnailCallback callback) {
    QQuickItem *item = qobject_cast<QQuickItem *>(m_view.data());
    if (!item || !size.isValid()) {
        EngineView::requestThumbnail(size, std::move(callback));
        return;
    }
    QSharedPointer<QQuickItemGrabResult> result = item->grabToImage(size);
    if (!result) {
        EngineView::requestThumbnail(size, std::move(callback));
        return;
    }
    connect(result.get(), &QQuickItemGrabResult::ready, this, [result, callback = std::move(callback)]() mutable {
        if (callback) {
            callback(result->image());
        }
        result.clear();
    });
}

void QtWebEngineView::syncState() {
    const QUrl nextUrl = value("url").toUrl();
    const QString nextTitle = value("title").toString();
    const QUrl nextFavicon = value("icon").toUrl();
    const int nextProgress = value("loadProgress").toInt();
    const bool nextLoading = value("loading").toBool();
    const bool nextBack = value("canGoBack").toBool();
    const bool nextForward = value("canGoForward").toBool();
    const bool nextAudible = value("recentlyAudible").toBool();
    const bool nextMuted = value("audioMuted").toBool();
    if (m_url != nextUrl) {
        m_url = nextUrl;
        m_certificateError = false;
        emit urlChanged();
        emit securityStateChanged();
    }
    if (m_title != nextTitle) {
        m_title = nextTitle;
        emit titleChanged();
    }
    if (m_faviconUrl != nextFavicon) {
        m_faviconUrl = nextFavicon;
        emit faviconUrlChanged();
    }
    if (m_loadProgress != nextProgress) {
        m_loadProgress = nextProgress;
        emit loadProgressChanged();
    }
    if (m_loading != nextLoading) {
        m_loading = nextLoading;
        emit loadingChanged();
    }
    if (m_canGoBack != nextBack) {
        m_canGoBack = nextBack;
        emit canGoBackChanged();
    }
    if (m_canGoForward != nextForward) {
        m_canGoForward = nextForward;
        emit canGoForwardChanged();
    }
    if (m_audible != nextAudible) {
        m_audible = nextAudible;
        emit audibleChanged();
    }
    if (m_muted != nextMuted) {
        m_muted = nextMuted;
        emit mutedChanged();
    }
}

void QtWebEngineView::handleNewWindow(QObject *requestObject) {
    QWebEngineNewWindowRequest *request = qobject_cast<QWebEngineNewWindowRequest *>(requestObject);
    QQuickItem *item = qobject_cast<QQuickItem *>(m_view.data());
    QQmlEngine *engine = item ? qmlEngine(item) : nullptr;
    if (!request || !engine) {
        return;
    }
    QtWebEngineNewViewRequest engineRequest(request, engine);
    emit newViewRequested(&engineRequest);
}

void QtWebEngineView::handleFullScreen(bool fullscreen) {
    emit fullscreenRequested(fullscreen);
}

void QtWebEngineView::handleCertificateError() {
    m_certificateError = true;
    emit securityStateChanged();
}

void QtWebEngineView::handleContextMenu(const QPoint &position, const QUrl &linkUrl, const QUrl &mediaUrl, const QString &selectedText,
                                        bool editable) {
    ContextMenuInfo info;
    info.position = position;
    info.linkUrl = linkUrl;
    info.mediaUrl = mediaUrl;
    info.selectedText = selectedText;
    info.editable = editable;
    emit contextMenuRequested(info);
}

QVariant QtWebEngineView::value(const char *name) const {
    return m_view ? m_view->property(name) : QVariant();
}

void QtWebEngineView::invoke(const char *method) {
    if (m_view) {
        QMetaObject::invokeMethod(m_view, method);
    }
}

}
