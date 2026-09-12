#include "engine/qtwebengine/qtwebengineprofile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQueue>
#include <QQuickWebEngineDownloadRequest>
#include <QQuickWebEngineProfile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QWebEngineCookieStore>

namespace eden::engine {

    class QtCookieBridge final : public QObject {
        Q_OBJECT
      public:
        using Callback = std::function<void(const QJsonObject &)>;
        QtCookieBridge(QObject *profile, QQmlEngine *engine, QObject *parent)
            : QObject(parent),
              m_token(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
            m_deadline.setSingleShot(true);
            m_deadline.setInterval(30000);
            connect(&m_deadline, &QTimer::timeout, this, [this] {
                delete std::exchange(m_host, nullptr);
                m_body.clear();
                auto callback = std::exchange(m_callback, {});
                if (callback) {
                    callback({{"error", "cookie_operation_interrupted"}});
                }
            });
            QQmlComponent component(engine);
            component.setData(
                R"QML(import QtQuick
import QtWebEngine
Item {
    property var bridge
    property var cookieProfile
    visible: false
    WebEngineProfilePrototype { id: inspectorProfile; storageName: "" }
    WebEngineView { id: target; profile: cookieProfile; url: "about:blank"; visible: false }
    WebEngineView {
        id: inspector
        profile: inspectorProfile.instance()
        inspectedView: target
        visible: false
        onLoadingChanged: request => { if (!loading && bridge) bridge.ready() }
        onJavaScriptConsoleMessage: (level, message, line, source) => { if (bridge) bridge.receive(message) }
    }
    function execute(script) { inspector.runJavaScript(script) }
}
)QML",
                QUrl()
            );
            m_host = component.createWithInitialProperties(
                {{"bridge", QVariant::fromValue(static_cast<QObject *>(this))},
                 {"cookieProfile", QVariant::fromValue(profile)}}
            );
            if (m_host) {
                m_host->setParent(this);
            }
        }

        ~QtCookieBridge() override {
            if (m_callback) {
                m_callback({{"error", "profile_destroyed"}});
            }
        }

        void execute(const QString &body, Callback callback) {
            if (!m_host || m_callback) {
                callback({{"error", m_host ? "cookie_store_busy" : "cookie_bridge_unavailable"}});
                return;
            }
            m_callback = std::move(callback);
            m_body = body;
            m_sent = false;
            m_deadline.start();
            if (m_ready) {
                send();
            }
        }

        Q_INVOKABLE void ready() {
            m_ready = true;
            if (m_callback) {
                send();
            }
        }

        Q_INVOKABLE void receive(const QString &message) {
            if (!m_callback || !message.startsWith(m_token)) {
                return;
            }
            auto callback = std::move(m_callback);
            m_callback = {};
            m_deadline.stop();
            const auto result = QJsonDocument::fromJson(message.mid(m_token.size()).toUtf8());
            callback(result.isObject() ? result.object() : QJsonObject{{"error", "invalid_cookie_response"}});
        }

      private:
        void send() {
            if (m_sent) {
                return;
            }
            m_sent = true;
            const QString bootstrap = QStringLiteral(R"JS((function(){
const report=value=>console.debug('%1'+JSON.stringify(value));
if (!globalThis.InspectorFrontendHost || !globalThis.DevToolsAPI) { report({error:'cookie_bridge_unavailable'}); return; }
const pending=new Map();
let next=100000000;
const original=DevToolsAPI.dispatchMessage;
DevToolsAPI.dispatchMessage=function(message){
    const data=typeof message==='string'?JSON.parse(message):message;
    const entry=pending.get(data.id);
    if (!entry) { return original.call(this,message); }
    pending.delete(data.id);
    clearTimeout(entry.timeout);
    if (data.error) { entry.reject(new Error('cookie_protocol_failed')); } else { entry.resolve(data.result); }
};
const send=(method,params={})=>new Promise((resolve,reject)=>{
    const id=++next;
    const timeout=setTimeout(()=>{pending.delete(id);reject(new Error('cookie_protocol_timeout'));},5000);
    pending.set(id,{resolve,reject,timeout});
    InspectorFrontendHost.sendMessageToBackend(JSON.stringify({id,method,params}));
});
const ordinary=cookie=>!cookie.partitionKey&&!cookie.partitionKeyOpaque;
const parameters=cookie=>{
    const value={name:cookie.name,value:cookie.value,domain:cookie.domain,path:cookie.path,secure:cookie.secure,httpOnly:cookie.httpOnly};
    if (cookie.sameSite) { value.sameSite=cookie.sameSite; }
    if (!cookie.session&&cookie.expires>0) { value.expires=cookie.expires; }
    return value;
};
const clear=async()=>{
    const snapshot=await send('Network.getAllCookies');
    for (const cookie of snapshot.cookies.filter(ordinary)) {
        await send('Network.deleteCookies',{name:cookie.name,domain:cookie.domain,path:cookie.path});
    }
};
(async()=>{%2})().then(report,()=>report({error:'cookie_protocol_failed'})).finally(()=>{DevToolsAPI.dispatchMessage=original;});
})();)JS")
                                          .arg(m_token, std::exchange(m_body, {}));
            QMetaObject::invokeMethod(m_host, "execute", Q_ARG(QVariant, bootstrap));
        }
        QObject *m_host = nullptr;
        QString m_token;
        QString m_body;
        Callback m_callback;
        bool m_ready = false;
        bool m_sent = false;
        QTimer m_deadline;
    };

    QtWebEngineProfile::QtWebEngineProfile(
        const EngineProfileParameters &parameters,
        const QString &userAgent,
        QQmlEngine *engine,
        QObject *parent
    )
        : EngineProfile(parameters, parent),
          m_engine(engine),
          m_profilePrototype(nullptr),
          m_profile(nullptr) {
        if (!engine) {
            qFatal("A QML engine is required to create a web profile");
        }
        QQmlComponent component(engine);
        component.setData("import QtWebEngine\nWebEngineProfilePrototype {}", QUrl());
        if (component.isError()) {
            qFatal("The Qt WebEngine profile prototype is unavailable: %s", qPrintable(component.errorString()));
        }
        QVariantMap properties;
        properties.insert(
            "persistentPermissionsPolicy",
            QVariant::fromValue(QQuickWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime)
        );
        if (parameters.privateProfile) {
            properties.insert("storageName", QString());
            properties.insert("httpCacheType", QQuickWebEngineProfile::MemoryHttpCache);
            properties.insert("persistentCookiesPolicy", QQuickWebEngineProfile::NoPersistentCookies);
        } else {
            const QString dataPath = parameters.dataPath;
            const QString cachePath = parameters.cachePath;
            QDir().mkpath(dataPath);
            QDir().mkpath(cachePath);
            QFile::setPermissions(dataPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            QFile::setPermissions(cachePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            properties.insert("storageName", QStringLiteral("profile-") + parameters.profileId);
            properties.insert("persistentStoragePath", dataPath);
            properties.insert("cachePath", cachePath);
            properties.insert("persistentCookiesPolicy", QQuickWebEngineProfile::AllowPersistentCookies);
            properties.insert("httpCacheType", QQuickWebEngineProfile::DiskHttpCache);
        }
        m_profilePrototype = component.createWithInitialProperties(properties);
        if (!m_profilePrototype) {
            qFatal("The Qt WebEngine profile prototype could not be created: %s", qPrintable(component.errorString()));
        }
        m_profilePrototype->setParent(this);
        if (!QMetaObject::invokeMethod(
                m_profilePrototype,
                "instance",
                Q_RETURN_ARG(QQuickWebEngineProfile *, m_profile)
            ) ||
            !m_profile) {
            qFatal("The Qt WebEngine profile could not be created");
        }
        m_profile->setHttpUserAgent(userAgent);
        connect(
            m_profile,
            &QQuickWebEngineProfile::downloadRequested,
            this,
            [this](QQuickWebEngineDownloadRequest *download) {
                const QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
                auto destination = reserveDownloadPath(directory, download->suggestedFileName());
                const QString fileName = QFileInfo(*destination).fileName();
                download->setDownloadDirectory(directory);
                download->setDownloadFileName(fileName);
                const quint64 id = downloadIdentifier(download->id());
                emit downloadStarted(id, fileName, download->url(), *destination, download->totalBytes());
                auto update = [this, download, id] {
                    QString state = "downloading";
                    if (download->state() == QWebEngineDownloadRequest::DownloadCompleted) {
                        state = "completed";
                    } else if (download->state() == QWebEngineDownloadRequest::DownloadCancelled) {
                        state = "cancelled";
                    } else if (download->state() == QWebEngineDownloadRequest::DownloadInterrupted) {
                        state = "failed";
                    }
                    emit downloadUpdated(id, download->receivedBytes(), download->totalBytes(), state);
                };
                connect(download, &QWebEngineDownloadRequest::receivedBytesChanged, this, update);
                connect(download, &QWebEngineDownloadRequest::totalBytesChanged, this, update);
                connect(
                    download,
                    &QWebEngineDownloadRequest::stateChanged,
                    this,
                    [download, update, destination = std::move(destination)]() mutable {
                        update();
                        if (download->state() != QWebEngineDownloadRequest::DownloadInProgress) {
                            destination.reset();
                        }
                    }
                );
                download->accept();
            }
        );
    }

    QtWebEngineProfile::~QtWebEngineProfile() {
        delete std::exchange(m_cookieBridge, nullptr);
    }

    QObject *QtWebEngineProfile::nativeProfile() const {
        return m_profile;
    }

    QtCookieBridge *QtWebEngineProfile::cookieBridge() {
        if (!m_cookieBridge) {
            m_cookieBridge = new QtCookieBridge(m_profile, m_engine, this);
        }
        return m_cookieBridge;
    }

    bool QtWebEngineProfile::supportsPortableCookies() const {
        return !isPrivate() && m_profile;
    }

    void QtWebEngineProfile::exportPortableCookies(CookieSnapshotCallback callback) {
        if (!supportsPortableCookies()) {
            EngineProfile::exportPortableCookies(std::move(callback));
            return;
        }
        cookieBridge()->execute(
            "return await send('Network.getAllCookies');",
            [callback = std::move(callback)](const QJsonObject &response) {
                CookieSnapshotResult result;
                result.errorCode = response.value("error").toString();
                if (result.errorCode.isEmpty() && !response.value("cookies").isArray()) {
                    result.errorCode = QStringLiteral("invalid_cookie_response");
                }
                for (const QJsonValue &entry : response.value("cookies").toArray()) {
                    const QJsonObject native = entry.toObject();
                    if (native.contains("partitionKey") || native.value("partitionKeyOpaque").toBool()) {
                        ++result.skippedCookies;
                        continue;
                    }
                    PortableCookie cookie;
                    cookie.name = native.value("name").toString().toUtf8();
                    cookie.value = native.value("value").toString().toUtf8();
                    cookie.domain = native.value("domain").toString().toLatin1();
                    cookie.hostOnly = !cookie.domain.startsWith('.');
                    cookie.path = native.value("path").toString().toUtf8();
                    cookie.secure = native.value("secure").toBool();
                    cookie.httpOnly = native.value("httpOnly").toBool();
                    const QString sameSite = native.value("sameSite").toString();
                    cookie.sameSite = sameSite == "Strict" ? CookieSameSite::Strict
                                      : sameSite == "Lax"  ? CookieSameSite::Lax
                                      : sameSite == "None" ? CookieSameSite::None
                                                           : CookieSameSite::Unspecified;
                    if (!native.value("session").toBool()) {
                        cookie.expires = QDateTime::fromMSecsSinceEpoch(
                            static_cast<qint64>(native.value("expires").toDouble() * 1000),
                            Qt::UTC
                        );
                    }
                    if (canonicalizePortableCookie(cookie, QDateTime::currentMSecsSinceEpoch())) {
                        result.cookies.append(std::move(cookie));
                    } else {
                        wipePortableCookie(cookie);
                        ++result.skippedCookies;
                    }
                }
                callback(std::move(result));
            }
        );
    }

    void
    QtWebEngineProfile::replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) {
        if (!supportsPortableCookies()) {
            EngineProfile::replacePortableCookies(cookies, std::move(callback));
            return;
        }
        QJsonArray values;
        for (PortableCookie cookie : cookies) {
            if (!canonicalizePortableCookie(cookie, QDateTime::currentMSecsSinceEpoch())) {
                wipePortableCookie(cookie);
                callback({0, 0, "invalid_cookie"});
                return;
            }
            QJsonObject value{
                {"name", QString::fromUtf8(cookie.name)},
                {"value", QString::fromUtf8(cookie.value)},
                {"domain", QString::fromLatin1((cookie.hostOnly ? QByteArray() : QByteArray(".")) + cookie.domain)},
                {"path", QString::fromUtf8(cookie.path)},
                {"secure", cookie.secure},
                {"httpOnly", cookie.httpOnly}
            };
            if (cookie.expires) {
                value.insert("expires", static_cast<double>(cookie.expires->toMSecsSinceEpoch()) / 1000.0);
            }
            if (cookie.sameSite != CookieSameSite::Unspecified) {
                value.insert(
                    "sameSite",
                    cookie.sameSite == CookieSameSite::Strict ? "Strict"
                    : cookie.sameSite == CookieSameSite::Lax  ? "Lax"
                                                              : "None"
                );
            }
            values.append(value);
            wipePortableCookie(cookie);
        }
        const QString body = QStringLiteral(R"JS(
const incoming=%1;
const before=(await send('Network.getAllCookies')).cookies.filter(ordinary).map(parameters);
try {
    await clear();
    await send('Network.setCookies',{cookies:incoming});
    return {imported:incoming.length};
} catch (error) {
    try { await clear(); await send('Network.setCookies',{cookies:before}); return {error:'rolled_back'}; }
    catch (failure) { return {error:'rollback_failed'}; }
}
)JS")
                                 .arg(QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact)));
        cookieBridge()->execute(body, [callback = std::move(callback)](const QJsonObject &result) {
            callback({result.value("imported").toInt(), 0, result.value("error").toString()});
        });
    }

    void QtWebEngineProfile::clearData() {
        if (!m_profile) {
            return;
        }
        m_profile->cookieStore()->deleteAllCookies();
        m_profile->clearHttpCache();
    }

}

#include "qtwebengineprofile.moc"
