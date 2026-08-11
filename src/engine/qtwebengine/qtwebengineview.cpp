#include "engine/qtwebengine/qtwebengineview.h"
#include "engine/engineprofile.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
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

QtWebEngineView::~QtWebEngineView() = default;

QUrl QtWebEngineView::url() const {
    return m_view ? m_url : m_pendingUrl;
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
    if (current.isEmpty() || scheme == "eden" || scheme == "about") {
        return "local";
    }
    if (scheme == "https" && !m_certificateError) {
        return "secure";
    }
    return "insecure";
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
    invoke("edenOpenDevTools");
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
    function edenOpenDevTools() {
        devtoolsLoader.active = !devtoolsLoader.active
    }
    Loader {
        id: devtoolsLoader
        active: false
        z: 20
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.round(parent.height * 0.45)
        sourceComponent: WebEngineView {
            profile: webView.profile
            inspectedView: webView
        }
    }
    function edenFind(text, flags) {
        findText(text, flags)
    }
    onNewWindowRequested: request => { if (edenBridge) edenBridge.handleNewWindow(request) }
    onFullScreenRequested: request => {
        request.accept()
        if (edenBridge) edenBridge.handleFullScreen(request.toggleOn)
    }
    onContextMenuRequested: request => { if (edenBridge) edenBridge.handleContextMenu(request.position, request.linkUrl, request.selectedText, request.isContentEditable) }
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

void QtWebEngineView::handleContextMenu(const QPoint &position, const QUrl &linkUrl, const QString &selectedText, bool editable) {
    ContextMenuInfo info;
    info.position = position;
    info.linkUrl = linkUrl;
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
