#include "engine/qtwebengine/qtwebengineview.h"
#include "engine/engineprofile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaEnum>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QUuid>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePermission>

#include <algorithm>

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

    static const QString formHookScript = QStringLiteral(R"JS((function(){
if(window.__edenFormHooksInstalled)return;window.__edenFormHooksInstalled=true;
var visible=function(field){return field&&!field.disabled&&!field.readOnly&&field.getClientRects().length>0;};
var fieldsFor=function(root){return Array.from((root||document).querySelectorAll('input,textarea'));};
var autocompleteHas=function(field,token){return String(field.autocomplete||'').toLowerCase().split(/\s+/).indexOf(token)>=0;};
var fieldIdentity=function(field){return[field.name,field.id,field.placeholder,field.getAttribute('aria-label')].filter(Boolean).join(' ').toLowerCase();};
var usernameField=function(field){return visible(field)&&field.value&&(autocompleteHas(field,'username')||autocompleteHas(field,'email')||field.type==='email'||/user|email|login|identifier/.test(fieldIdentity(field)));};
var secretField=function(field){if(!visible(field)||!field.value||autocompleteHas(field,'one-time-code'))return false;if(field.type==='password'||autocompleteHas(field,'current-password')||autocompleteHas(field,'new-password'))return true;return/password|passwd|passphrase|token|api[ _-]*key|secret|access[ _-]*key/.test(fieldIdentity(field));};
var usernameCandidate=function(root){var field=fieldsFor(root).find(usernameField);return field?field.value||'':'';};
var usernameFor=function(secret){var form=secret.form||document;var explicit=usernameCandidate(form);if(explicit)return explicit;var fields=fieldsFor(form);var index=fields.indexOf(secret);for(var position=index-1;position>=0;--position){var field=fields[position];if(visible(field)&&/^(text|email|tel|$)/.test(field.type||''))return field.value||'';}return'';};
var capture=function(root){var secret=fieldsFor(root).find(secretField);var username=secret?usernameFor(secret):usernameCandidate(root);if(username||secret)console.info('EDEN_CREDENTIAL:'+JSON.stringify([username,secret?secret.value:'']));};
document.addEventListener('submit',function(event){capture(event.target);},true);
document.addEventListener('click',function(event){var target=event.target&&event.target.closest?event.target.closest('button,input,[role="button"],a'):null;if(!target)return;var label=[target.textContent,target.value,target.getAttribute('aria-label')].filter(Boolean).join(' ');var root=target.form||(target.closest?target.closest('form'):null)||document;var buttonLike=/^(BUTTON|INPUT)$/.test(target.tagName)||target.getAttribute('role')==='button';var activatesLogin=/sign.?in|log.?in|continue|next|submit|authenticate|unlock/i.test(label);var secondaryControl=/show|hide|reveal|visibility|cancel|back/i.test(label);if(target.type==='submit'||activatesLogin||(buttonLike&&!secondaryControl&&fieldsFor(root).some(secretField)))capture(root);},true);
document.addEventListener('change',function(event){var field=event.target;if(field&&/^(INPUT|TEXTAREA)$/.test(field.tagName)&&usernameField(field))console.info('EDEN_CREDENTIAL:'+JSON.stringify([field.value||'','']));},true);
document.addEventListener('keydown',function(event){if(event.key==='Enter')capture(event.target.form||document);},true);
var reportField=function(field){if(!field||!/^(INPUT|TEXTAREA)$/.test(field.tagName))return;var rect=field.getBoundingClientRect();var value=secretField(field)?'':field.value||'';console.info('EDEN_FIELD:'+JSON.stringify([field.type||'text',field.name||field.id||'',field.autocomplete||'',value,rect.x,rect.y,rect.width,rect.height]));};
document.addEventListener('focusin',function(event){reportField(event.target);},true);
document.addEventListener('input',function(event){reportField(event.target);},true);
})();)JS");

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
        if (m_view) {
            m_view->setProperty("edenBridge", QVariant::fromValue<QObject *>(nullptr));
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
        component.setData(
            R"QML(import QtQuick
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
    function edenRunJavaScript(script) {
        runJavaScript(script)
    }
    function edenProbeAutofill(id, script) {
        runJavaScript(script, 1, result => { if (edenBridge) edenBridge.handleAutofillTarget(id, result) })
    }
    function edenFillCredential(script) {
        runJavaScript(script, 1)
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
    onJavaScriptConsoleMessage: (level, message, lineNumber, sourceId) => { if (edenBridge) edenBridge.handleJavaScriptConsoleMessage(message) }
    onCertificateError: error => { if (edenBridge) edenBridge.handleCertificateError() }
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

    void QtWebEngineView::fillForm(const QVariantMap &fields) {
        const QByteArray values = QJsonDocument(QJsonObject::fromVariantMap(fields)).toJson(QJsonDocument::Compact);
        runJavaScript(
            QStringLiteral(
                "(function(values){var aliases={name:['name','full-name'],email:['email'],phone:['tel','phone'],"
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
                .arg(QString::fromUtf8(values))
        );
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

    void QtWebEngineView::handleJavaScriptConsoleMessage(const QString &message) {
        const qsizetype separator = message.indexOf(':');
        if (separator <= 0) {
            return;
        }
        const QString kind = message.first(separator);
        const QJsonArray values = QJsonDocument::fromJson(message.sliced(separator + 1).toUtf8()).array();
        QUrl origin = m_url;
        origin.setPath(QString());
        origin.setQuery(QString());
        origin.setFragment(QString());
        if (kind == QStringLiteral("EDEN_CREDENTIAL") && values.size() == 2) {
            CredentialSubmissionInfo info;
            info.origin = origin;
            info.username = values.at(0).toString();
            info.password = values.at(1).toString();
            emit credentialSubmitted(info);
        } else if (kind == QStringLiteral("EDEN_FIELD") && values.size() == 8) {
            emit formFieldFocused(
                {{"origin", origin},
                 {"type", values.at(0).toString()},
                 {"name", values.at(1).toString()},
                 {"autocomplete", values.at(2).toString()},
                 {"value", values.at(3).toString()},
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
        runJavaScript(formHookScript);
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
