#include "engine/qtwebengine/qtwebengineview.h"
#include "engine/engineprofile.h"
#include "engine/mediaactivityscript.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaEnum>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QUuid>
#include <QWebChannel>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePermission>
#include <QWebEngineScript>

#include <algorithm>
#include <cmath>

namespace eden::engine {

    class QtWebEngineNewViewRequest final : public EngineNewViewRequest {
      public:
        QtWebEngineNewViewRequest(QWebEngineNewWindowRequest *request, QQmlEngine *engine)
            : EngineNewViewRequest(
                  request->requestedUrl(),
                  request->destination() == QWebEngineNewWindowRequest::InNewWindow ? EngineView::Disposition::NewWindow
                  : request->destination() == QWebEngineNewWindowRequest::InNewBackgroundTab
                      ? EngineView::Disposition::NewBackgroundTab
                      : EngineView::Disposition::NewForegroundTab,
                  request->isUserInitiated()
              ),
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

    class QtFormReportBridge final : public QObject {
        Q_OBJECT

      public:
        explicit QtFormReportBridge(QtWebEngineView *view)
            : QObject(view),
              m_view(view) {}

        Q_INVOKABLE QVariantMap registerDocument(const QString &origin, const QString &documentId) {
            return m_view->registerFormDocument(origin, documentId);
        }

        Q_INVOKABLE void report(
            const QString &token,
            const QString &documentId,
            const QString &origin,
            const QString &kind,
            const QVariantList &values
        ) {
            m_view->handleFormReport(token, documentId, origin, kind, values);
        }

      private:
        QtWebEngineView *m_view;
    };

    static const QString formHookScript = QStringLiteral(R"JS((function(){
'use strict';
if (window.top !== window || globalThis.origin === 'null') { return; }
if (window.__edenReconnectFormReports) { window.__edenReconnectFormReports(); return; }
var currentTransport = null;
var channel = null;
var initializing = false;
var authorizing = false;
var registration = null;
var pendingCredential = null;
var pendingReport = null;
var transportNow = function(){ return globalThis.qt && globalThis.qt.webChannelTransport; };
var documentId = function(){ return globalThis.__edenAutofillDocument || ''; };
var send = function(kind, values){
    if (!registration || currentTransport !== transportNow()) { return false; }
    channel.objects.forms.report(registration.token, documentId(), globalThis.origin, kind, values);
    return true;
};
var authorize = function(transport){
    if (!channel || authorizing) { return; }
    authorizing = true;
    var id = documentId();
    channel.objects.forms.registerDocument(globalThis.origin, id, function(result){
        if (currentTransport !== transport || transportNow() !== transport) { return; }
        authorizing = false;
        if (documentId() !== id) { authorize(transport); return; }
        if (!result || !result.token || result.origin !== globalThis.origin || !result.documentId) {
            pendingCredential = null;
            pendingReport = null;
            return;
        }
        if (!id) {
            Object.defineProperty(globalThis, '__edenAutofillDocument', {value:result.documentId});
        }
        registration = result;
        if (pendingCredential) { send('credential', pendingCredential); pendingCredential = null; }
        if (pendingReport) { send('field', pendingReport); pendingReport = null; }
        var field = document.activeElement;
        if (document.hasFocus() && relevantField(field)) {
            pendingField = field;
            if (pendingFrame === null) { pendingFrame = requestFrame(flushField); }
        }
    });
};
var connectBridge = function(force){
    var transport = transportNow();
    if (currentTransport !== transport) {
        currentTransport = transport;
        channel = null;
        initializing = false;
        authorizing = false;
        registration = null;
        pendingCredential = null;
        pendingReport = null;
        lastReport = null;
    }
    if (!transport || initializing || authorizing) { return; }
    if (force) { registration = null; }
    if (channel) {
        if (!registration) { authorize(transport); }
        return;
    }
    initializing = true;
    new QWebChannel(transport, function(connected){
        if (currentTransport !== transport || transportNow() !== transport) { return; }
        initializing = false;
        channel = connected;
        authorize(transport);
    });
};
window.__edenReconnectFormReports = connectBridge;
var reportCredential = function(username, password){
    connectBridge();
    var values = [username, password];
    if (!send('credential', values)) { pendingCredential = values; }
};
var reportFormField = function(type, name, autocomplete, value, x, y, width, height){
    connectBridge();
    var values = [type, name, autocomplete, value, x, y, width, height];
    if (!send('field', values)) { pendingReport = values; }
};
var visible = function(field){ return field && !field.disabled && !field.readOnly && field.getClientRects().length > 0; };
var fieldsFor = function(root){ return Array.prototype.slice.call((root || document).querySelectorAll('input,textarea')); };
var autocompleteHas = function(field, token){ return String(field.autocomplete || '').toLowerCase().split(/\s+/).indexOf(token) >= 0; };
var fieldIdentity = function(field){
    return [field.name, field.id, field.placeholder, field.getAttribute('aria-label')].filter(Boolean).join(' ').toLowerCase();
};
var usernameField = function(field){
    return field && field.value &&
        (autocompleteHas(field, 'username') || autocompleteHas(field, 'email') || field.type === 'email' ||
         /user|email|login|identifier/.test(fieldIdentity(field))) && visible(field);
};
var secretType = function(field){
    if (!field || autocompleteHas(field, 'one-time-code')) { return false; }
    if (field.type === 'password' || autocompleteHas(field, 'current-password') || autocompleteHas(field, 'new-password')) {
        return true;
    }
    return /password|passwd|passphrase|token|api[ _-]*key|secret|access[ _-]*key/.test(fieldIdentity(field));
};
var secretField = function(field){ return field && field.value && secretType(field) && visible(field); };
var usernameCandidate = function(root){
    var field = fieldsFor(root).find(usernameField);
    return field ? field.value || '' : '';
};
var usernameFor = function(secret){
    var form = secret.form || document;
    var explicit = usernameCandidate(form);
    if (explicit) { return explicit; }
    var fields = fieldsFor(form);
    var index = fields.indexOf(secret);
    for (var position = index - 1; position >= 0; --position) {
        var field = fields[position];
        if (visible(field) && /^(text|email|tel|$)/.test(field.type || '')) { return field.value || ''; }
    }
    return '';
};
var capture = function(root){
    var secret = fieldsFor(root).find(secretField);
    var username = secret ? usernameFor(secret) : usernameCandidate(root);
    if (username || secret) { try { reportCredential(username, secret ? secret.value : ''); } catch (error) {} }
};
document.addEventListener('submit', function(event){ if (event.isTrusted) { capture(event.target); } }, true);
document.addEventListener('click', function(event){
    if (!event.isTrusted) { return; }
    var target = event.target && event.target.closest ? event.target.closest('button,input,[role="button"],a') : null;
    if (!target) { return; }
    var label = [target.textContent, target.value, target.getAttribute('aria-label')].filter(Boolean).join(' ');
    var root = target.form || (target.closest ? target.closest('form') : null) || document;
    var buttonLike = /^(BUTTON|INPUT)$/.test(target.tagName) || target.getAttribute('role') === 'button';
    var activatesLogin = /sign.?in|log.?in|continue|next|submit|authenticate|unlock/i.test(label);
    var secondaryControl = /show|hide|reveal|visibility|cancel|back/i.test(label);
    if (target.type === 'submit' || activatesLogin || (buttonLike && !secondaryControl && fieldsFor(root).some(secretField))) {
        capture(root);
    }
}, true);
document.addEventListener('change', function(event){
    if (!event.isTrusted) { return; }
    var field = event.target;
    if (field && /^(INPUT|TEXTAREA)$/.test(field.tagName) && usernameField(field)) {
        try { reportCredential(field.value || '', ''); } catch (error) {}
    }
}, true);
document.addEventListener('keydown', function(event){
    if (event.isTrusted && event.key === 'Enter') { capture(event.target.form || document); }
}, true);
var relevantField = function(field){
    if (!field || !/^(INPUT|TEXTAREA)$/.test(field.tagName) || field.disabled || field.readOnly) { return false; }
    if (/^(password|email|tel)$/.test(field.type)) { return true; }
    var identity = fieldIdentity(field) + ' ' + String(field.autocomplete || '').toLowerCase();
    return /name|email|user|login|identifier|pass|token|secret|access|phone|address|city|state|province|postal|zip|country/
        .test(identity);
};
var requestFrame = window.requestAnimationFrame.bind(window);
var cancelFrame = window.cancelAnimationFrame.bind(window);
var pendingFrame = null;
var pendingField = null;
var lastReport = null;
var clearField = function(){
    if (pendingFrame !== null) { cancelFrame(pendingFrame); }
    pendingFrame = null;
    pendingField = null;
    if (lastReport) {
        lastReport = null;
        try { reportFormField('', '', '', '', 0, 0, 0, 0); } catch (error) {}
    }
};
var flushField = function(){
    pendingFrame = null;
    var field = pendingField;
    pendingField = null;
    if (!relevantField(field) || document.activeElement !== field) { clearField(); return; }
    var rect = field.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) { clearField(); return; }
    var type = field.type || 'text';
    var value = type === 'password' || secretType(field) ? '' : field.value || '';
    var name = field.name || field.id || '';
    var autocomplete = field.autocomplete || '';
    var values = [type, name, autocomplete, value, rect.x, rect.y, rect.width, rect.height];
    if (lastReport) {
        var changed = false;
        for (var index = 0; index < values.length; ++index) {
            if (values[index] !== lastReport[index]) { changed = true; break; }
        }
        if (!changed) { return; }
    }
    lastReport = values;
    try {
        reportFormField(type, name, autocomplete, value, rect.x, rect.y, rect.width, rect.height);
    } catch (error) {}
};
var queueField = function(event){
    if (!event.isTrusted) { return; }
    if (!relevantField(event.target)) { clearField(); return; }
    pendingField = event.target;
    if (pendingFrame === null) { pendingFrame = requestFrame(flushField); }
};
document.addEventListener('focusin', function(event){
    queueField(event);
}, true);
document.addEventListener('focusout', function(event){ if (event.isTrusted) { clearField(); } }, true);
document.addEventListener('input', function(event){
    queueField(event);
}, true);
window.addEventListener('blur', clearField);
window.addEventListener('pageshow', function(event){
    if (!event.isTrusted) { return; }
    connectBridge(event.persisted);
    if (event.persisted) { requestFrame(function(){ connectBridge(); }); }
});
connectBridge();
})();)JS");

    static QString formReportBootstrap() {
        static const QString bootstrap = [] {
            QFile source(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
            return source.open(QIODevice::ReadOnly) ? QString::fromUtf8(source.readAll()) + '\n' + formHookScript
                                                    : QString();
        }();
        return bootstrap;
    }

    QtWebEngineView::QtWebEngineView(EngineProfile *profile, QObject *parent)
        : EngineView(parent),
          m_profile(profile) {}

    static QUrl aliasEngineInternalUrl(QUrl url) {
        if (url.scheme() == "chrome") {
            url.setScheme("eden");
        }
        return url;
    }

    QtWebEngineView::~QtWebEngineView() {
        delete m_devToolsView.data();
        if (m_view) {
            QObject::disconnect(m_view, nullptr, this, nullptr);
            m_view->setProperty("edenBridge", QVariant::fromValue<QObject *>(nullptr));
            delete m_view.data();
        }
    }

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
            entry.insert(
                "title",
                item.title().isEmpty() ? item.url().toDisplayString(QUrl::RemovePassword) : item.title()
            );
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
            component.setData(
                R"QML(import QtQuick
import QtWebEngine
WebEngineView {
    anchors.fill: parent
}
)QML",
                QUrl()
            );
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
            if (auto *item = qobject_cast<QQuickItem *>(m_view.data())) {
                item->setVisible(false);
                item->setParentItem(nullptr);
            }
            return;
        }
        if (!ensureView(qmlEngine(viewport))) {
            return;
        }
        if (QQuickItem *item = qobject_cast<QQuickItem *>(m_view.data())) {
            item->setParentItem(viewport);
            item->setVisible(true);
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
        component.setData(
            R"QML(import QtQuick
import QtWebEngine
import QtWebChannel
WebEngineView {
    id: webView
    anchors.fill: parent
    property var edenBridge
    webChannelWorld: WebEngineScript.ApplicationWorld
    webChannel: WebChannel {}
    function edenFind(text, flags) {
        findText(text, flags)
    }
    function edenContextCommand(action) {
        triggerWebAction(action)
    }
    function edenRunJavaScript(script) {
        runJavaScript(script)
    }
    function edenProbeAutofill(id, script) {
        runJavaScript(script, 1, result => { if (edenBridge) edenBridge.handleAutofillTarget(id, result) })
    }
    function edenFillCredential(script) {
        runJavaScript(script, 1)
    }
    function edenInstallFormReports(script) {
        runJavaScript(script, WebEngineScript.ApplicationWorld)
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
    onCertificateError: error => { if (edenBridge) edenBridge.handleCertificateError() }
    onJavaScriptConsoleMessage: (level, message, lineNumber, sourceID) => { if (edenBridge) edenBridge.handleMediaActivity(message) }
    onRenderProcessTerminated: (status, code) => { if (edenBridge) edenBridge.clearMediaActivity() }
    onPermissionRequested: permission => { if (edenBridge) edenBridge.handlePermission(permission) }
}
)QML",
            QUrl()
        );
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
        const QString bootstrap = formReportBootstrap();
        auto *channel = qobject_cast<QWebChannel *>(item->property("webChannel").value<QObject *>());
        QObject *scripts = item->property("userScripts").value<QObject *>();
        if (!channel || !scripts || bootstrap.isEmpty()) {
            delete item;
            return false;
        }
        QWebEngineScript script;
        script.setName(QStringLiteral("eden-form-reports"));
        script.setInjectionPoint(QWebEngineScript::DocumentReady);
        script.setWorldId(QWebEngineScript::ApplicationWorld);
        script.setRunsOnSubFrames(false);
        script.setSourceCode(bootstrap);
        if (!QMetaObject::invokeMethod(scripts, "insert", Qt::DirectConnection, Q_ARG(QWebEngineScript, script))) {
            delete item;
            return false;
        }
        m_mediaReportToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QWebEngineScript mediaScript;
        mediaScript.setName(QStringLiteral("eden-media-activity"));
        mediaScript.setInjectionPoint(QWebEngineScript::DocumentCreation);
        mediaScript.setWorldId(QWebEngineScript::MainWorld);
        mediaScript.setRunsOnSubFrames(true);
        mediaScript.setSourceCode(QStringLiteral(
                                      "(function(){const send=console.debug.bind(console);const id=crypto.randomUUID();"
                                      "const main=window===window.top;let first=true;const "
                                      "report=function(value){send('%1'+JSON.stringify([id,value,main&&first]));first="
                                      "false;};%2(report);})();"
        )
                                      .arg(m_mediaReportToken, QString::fromUtf8(mediaActivityScript)));
        if (!QMetaObject::invokeMethod(scripts, "insert", Qt::DirectConnection, Q_ARG(QWebEngineScript, mediaScript))) {
            delete item;
            return false;
        }
        channel->registerObject(QStringLiteral("forms"), new QtFormReportBridge(this));
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
        connect(m_view, SIGNAL(windowCloseRequested()), this, SIGNAL(closeRequested()));
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
        return QMetaObject::invokeMethod(
            m_view,
            "acceptAsNewWindow",
            Qt::DirectConnection,
            Q_ARG(QWebEngineNewWindowRequest *, request)
        );
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

    void QtWebEngineView::requestAutofillTarget(AutofillTargetCallback callback) {
        if (!callback) {
            return;
        }
        if (!m_view) {
            callback({});
            return;
        }
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QByteArray encoded = QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact);
        const QString script =
            QStringLiteral(
                "(function(values){if(!Object.hasOwn(globalThis,'__edenAutofillDocument')){"
                "Object.defineProperty(globalThis,'__edenAutofillDocument',{value:values[0]});}"
                "return "
                "{documentId:globalThis.__edenAutofillDocument,origin:globalThis.origin,url:location.href};})(%1)"
            )
                .arg(QString::fromUtf8(encoded));
        m_autofillRequests.insert(id, std::move(callback));
        if (!QMetaObject::invokeMethod(m_view, "edenProbeAutofill", Q_ARG(QVariant, id), Q_ARG(QVariant, script))) {
            handleAutofillTarget(id, {});
        }
    }

    void QtWebEngineView::handleAutofillTarget(const QString &requestId, const QVariant &result) {
        const auto found = m_autofillRequests.find(requestId);
        if (found == m_autofillRequests.end()) {
            return;
        }
        AutofillTargetCallback callback = std::move(found.value());
        m_autofillRequests.erase(found);
        const QVariantMap values = result.toMap();
        AutofillTarget target{
            QUrl(values.value("url").toString()),
            QUrl(values.value("origin").toString()),
            values.value("documentId").toString()
        };
        if (!target.isValid() || target.origin != autofillOrigin(value("url").toUrl())) {
            target = {};
        }
        callback(std::move(target));
    }

    void
    QtWebEngineView::fillCredential(const AutofillTarget &target, const QString &username, const QString &password) {
        if (!m_view || !target.isValid()) {
            return;
        }
        const QByteArray values =
            QJsonDocument(QJsonArray{username, password, target.documentId, target.origin.toString(QUrl::FullyEncoded)})
                .toJson(QJsonDocument::Compact);
        const QString script =
            QStringLiteral(
                "(function(values){if(globalThis.__edenAutofillDocument!==values[2]||globalThis.origin!==values[3])"
                "return;var fields=Array.from(document.querySelectorAll('input,textarea'));var tokens="
                "function(f){return String(f.autocomplete||'').toLowerCase().split(/\\s+/);};var has=function(f,v)"
                "{return tokens(f).indexOf(v)>=0;};var identity=function(f){return[f.name,f.id,f.placeholder,f."
                "getAttribute('aria-label')].filter(Boolean).join(' ').toLowerCase();};var secret=fields.find(function"
                "(f){return!f.disabled&&!f.readOnly&&!has(f,'one-time-code')&&(f.type==='password'||has(f,'current-"
                "password')||/password|passwd|passphrase|token|api[ _-]*key|secret|access[ _-"
                "]*key/.test(identity(f)));});if(!secret)return;var username=fields.find(function(f){return!f.disabled"
                "&&(has(f,'username')||has(f,'email')||f.type==='email'||/user|email|login|identifier/.test(identity"
                "(f)));});if(!username){var i=fields.indexOf(secret);for(var p=i-1;p>=0;--p){if(/^(text|email|tel|$)/"
                ".test(fields[p].type||'')){username=fields[p];break;}}}var set=function(field,value){if(!field)return;"
                "var prototype=field.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;"
                "var setter=Object.getOwnPropertyDescriptor(prototype,'value').set;setter.call(field,value);"
                "field.dispatchEvent(new Event('input',{bubbles:true}));field.dispatchEvent(new Event('change',"
                "{bubbles:true}));};set(username,values[0]);set(secret,values[1]);})(%1)"
            )
                .arg(QString::fromUtf8(values));
        QMetaObject::invokeMethod(m_view, "edenFillCredential", Q_ARG(QVariant, script));
    }

    void QtWebEngineView::exitFullscreen() {
        triggerWebAction("ExitFullScreen");
    }

    void QtWebEngineView::fillForm(const AutofillTarget &target, const QVariantMap &fields) {
        if (!m_view || !target.isValid()) {
            return;
        }
        const QByteArray values = QJsonDocument(
                                      QJsonArray{
                                          QJsonObject::fromVariantMap(fields),
                                          target.documentId,
                                          target.origin.toString(QUrl::FullyEncoded)
                                      }
        )
                                      .toJson(QJsonDocument::Compact);
        const QString script =
            QStringLiteral(
                "(function(payload){if(globalThis.__edenAutofillDocument!==payload[1]||globalThis.origin!==payload[2])"
                "return;var values=payload[0];var "
                "aliases={name:['name','full-name'],email:['email'],phone:['tel','phone'],"
                "addressLine1:['address-line1','street-address'],addressLine2:['address-line2'],city:["
                "'address-level2','city'],region:['address-level1','state','province'],postalCode:['postal-code',"
                "'zip'],country:['country','country-name']};var normalize=function(v){return String(v||'')."
                "toLowerCase().replace(/[^a-z0-9]/g,'');};var set=function(field,value){if(!field||!value)return;"
                "var prototype=field.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;"
                "var setter=Object.getOwnPropertyDescriptor(prototype,'value').set;setter.call(field,value);field."
                "dispatchEvent(new Event('input',{bubbles:true}));field.dispatchEvent(new Event('change',{bubbles:"
                "true}));};Object.keys(aliases).forEach(function(key){var names=aliases[key].map(normalize);var field="
                "Array.from(document.querySelectorAll('input,textarea')).find(function(item){var candidates=[item."
                "autocomplete,item.name,item.id,item.placeholder].map(normalize);return candidates.some(function("
                "candidate){return names.some(function(name){return candidate.indexOf(name)>=0;});});});set(field,"
                "values[key]);});})(%1)"
            )
                .arg(QString::fromUtf8(values));
        QMetaObject::invokeMethod(m_view, "edenFillCredential", Q_ARG(QVariant, script));
    }

    void QtWebEngineView::resolvePermissionRequest(quint64 id, bool allowed) {
        const auto found = m_permissions.find(id);
        if (found == m_permissions.end()) {
            return;
        }
        const QWebEnginePermission permission = found.value().value<QWebEnginePermission>();
        m_permissions.erase(found);
        if (allowed) {
            permission.grant();
        } else {
            permission.deny();
        }
    }

    void QtWebEngineView::dismissPermissionRequest(quint64 id) {
        const auto found = m_permissions.find(id);
        if (found == m_permissions.end()) {
            return;
        }
        const QWebEnginePermission permission = found.value().value<QWebEnginePermission>();
        m_permissions.erase(found);
        permission.deny();
        permission.reset();
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
        const bool finishedLoading = m_loading && !nextLoading;
        if (m_loading != nextLoading) {
            m_loading = nextLoading;
            if (m_loading) {
                QHash<QString, AutofillTargetCallback> callbacks;
                callbacks.swap(m_autofillRequests);
                for (auto &callback : callbacks) {
                    callback({});
                }
            }
            emit loadingChanged();
        }
        if (finishedLoading) {
            installFormHooks();
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

    void QtWebEngineView::clearMediaActivity() {
        clearCaptureDetails();
    }

    void QtWebEngineView::handleMediaActivity(const QString &message) {
        if (m_mediaReportToken.isEmpty() || !message.startsWith(m_mediaReportToken) || message.size() > 256) {
            return;
        }
        const QJsonArray values = QJsonDocument::fromJson(message.mid(m_mediaReportToken.size()).toUtf8()).array();
        if (values.size() != 3 || !values[0].isString() || !values[1].isDouble() || !values[2].isBool()) {
            return;
        }
        if (values[2].toBool()) {
            clearCaptureDetails();
        }
        setCaptureDetails(values[0].toString(), values[1].toInt());
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

    void QtWebEngineView::handlePermission(const QVariant &permissionValue) {
        const QWebEnginePermission permission = permissionValue.value<QWebEnginePermission>();
        if (!permission.isValid()) {
            return;
        }
        QStringList names;
        switch (permission.permissionType()) {
        case QWebEnginePermission::PermissionType::MediaAudioCapture:
            names.append("microphone");
            break;
        case QWebEnginePermission::PermissionType::MediaVideoCapture:
            names.append("camera");
            break;
        case QWebEnginePermission::PermissionType::MediaAudioVideoCapture:
            names.append({"microphone", "camera"});
            break;
        case QWebEnginePermission::PermissionType::DesktopVideoCapture:
        case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture:
            names.append("screen sharing");
            break;
        case QWebEnginePermission::PermissionType::Notifications:
            names.append("notifications");
            break;
        case QWebEnginePermission::PermissionType::Geolocation:
            names.append("location");
            break;
        case QWebEnginePermission::PermissionType::ClipboardReadWrite:
            names.append("clipboard");
            break;
        case QWebEnginePermission::PermissionType::LocalFontsAccess:
            names.append("local fonts");
            break;
        case QWebEnginePermission::PermissionType::MouseLock:
            names.append("pointer lock");
            break;
        case QWebEnginePermission::PermissionType::Unsupported:
            permission.deny();
            return;
        }
        const quint64 id = m_nextPermissionId++;
        m_permissions.insert(id, QVariant::fromValue(permission));
        PermissionRequestInfo info;
        info.id = id;
        info.origin = permission.origin();
        info.permissions = names;
        emit permissionRequested(info);
    }

    QUrl QtWebEngineView::committedMainFrameUrl() const {
        const QVariant frame = value("mainFrame");
        const QMetaObject *metaObject = frame.metaType().metaObject();
        const int index = metaObject ? metaObject->indexOfProperty("url") : -1;
        return index >= 0 ? metaObject->property(index).readOnGadget(frame.constData()).toUrl() : QUrl();
    }

    QVariantMap QtWebEngineView::registerFormDocument(const QString &originText, const QString &documentId) {
        const QUrl origin(originText);
        if (origin.isEmpty() || origin != autofillOrigin(origin) || origin != autofillOrigin(committedMainFrameUrl()) ||
            (!documentId.isEmpty() && QUuid(documentId).isNull())) {
            return {};
        }
        const QString nextDocumentId =
            documentId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : documentId;
        const bool changedDocument =
            !m_formDocumentId.isEmpty() && (m_formDocumentId != nextDocumentId || m_formOrigin != origin);
        m_formDocumentId = nextDocumentId;
        m_formOrigin = origin;
        m_formReportToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (changedDocument) {
            emit formFieldFocused({});
        }
        return {
            {"token", m_formReportToken},
            {"documentId", m_formDocumentId},
            {"origin", m_formOrigin.toString(QUrl::FullyEncoded)}
        };
    }

    void QtWebEngineView::handleFormReport(
        const QString &token,
        const QString &documentId,
        const QString &originText,
        const QString &kind,
        const QVariantList &values
    ) {
        const QUrl origin(originText);
        if (token.isEmpty() || token != m_formReportToken || documentId != m_formDocumentId || origin != m_formOrigin ||
            origin != autofillOrigin(committedMainFrameUrl())) {
            return;
        }
        if (kind == QStringLiteral("credential") && values.size() == 2) {
            if (values.at(0).metaType().id() != QMetaType::QString ||
                values.at(1).metaType().id() != QMetaType::QString) {
                return;
            }
            CredentialSubmissionInfo info;
            info.origin = origin;
            info.username = values.at(0).toString();
            info.password = values.at(1).toString();
            emit credentialSubmitted(info);
        } else if (kind == QStringLiteral("field") && values.size() == 8) {
            for (int index = 0; index < 4; ++index) {
                if (values.at(index).metaType().id() != QMetaType::QString) {
                    return;
                }
            }
            for (int index = 4; index < 8; ++index) {
                bool valid = false;
                const double value = values.at(index).toDouble(&valid);
                if (!valid || !std::isfinite(value)) {
                    return;
                }
            }
            const QString type = values.at(0).toString();
            if (type.isEmpty()) {
                emit formFieldFocused({});
                return;
            }
            emit formFieldFocused(
                {{"origin", origin},
                 {"type", type},
                 {"name", values.at(1).toString()},
                 {"autocomplete", values.at(2).toString()},
                 {"value", type == QStringLiteral("password") ? QString() : values.at(3).toString()},
                 {"x", values.at(4).toDouble()},
                 {"y", values.at(5).toDouble()},
                 {"width", values.at(6).toDouble()},
                 {"height", values.at(7).toDouble()}}
            );
        }
    }

    void QtWebEngineView::handleContextMenu(
        const QPoint &position,
        const QUrl &linkUrl,
        const QUrl &mediaUrl,
        const QString &selectedText,
        bool editable
    ) {
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

    void QtWebEngineView::installFormHooks() {
        if (m_view) {
            QMetaObject::invokeMethod(m_view, "edenInstallFormReports", Q_ARG(QVariant, formReportBootstrap()));
        }
    }

    void QtWebEngineView::runJavaScript(const QString &script) {
        if (m_view) {
            QMetaObject::invokeMethod(m_view, "edenRunJavaScript", Q_ARG(QVariant, script));
        }
    }

    void QtWebEngineView::invoke(const char *method) {
        if (m_view) {
            QMetaObject::invokeMethod(m_view, method);
        }
    }

}

#include "qtwebengineview.moc"
