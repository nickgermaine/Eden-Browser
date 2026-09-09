#include "engine/qtwebengine/qtwebengineprofile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWebEngineDownloadRequest>
#include <QQuickWebEngineProfile>
#include <QStandardPaths>
#include <QTimer>
#include <QWebEngineCookieStore>

namespace eden::engine {

    static QByteArray mirrorKey(const QNetworkCookie &cookie) {
        QByteArray domain = cookie.domain().toLatin1().toLower();
        const bool hostOnly = !domain.startsWith('.');
        while (domain.startsWith('.')) {
            domain.remove(0, 1);
        }
        QByteArray key;
        key.reserve(cookie.name().size() + domain.size() + cookie.path().size() + 4);
        key.append(cookie.name());
        key.append('\x1f');
        key.append(domain);
        key.append('\x1f');
        key.append(cookie.path().toUtf8());
        key.append('\x1f');
        key.append(hostOnly ? '1' : '0');
        return key;
    }

    static PortableCookie portableFromNetworkCookie(const QNetworkCookie &cookie) {
        PortableCookie portable;
        portable.name = cookie.name();
        portable.value = cookie.value();
        QByteArray domain = cookie.domain().toLatin1();
        portable.hostOnly = !domain.startsWith('.');
        portable.domain = domain;
        portable.path = cookie.path().toUtf8();
        portable.secure = cookie.isSecure();
        portable.httpOnly = cookie.isHttpOnly();
        if (cookie.expirationDate().isValid()) {
            portable.expires = cookie.expirationDate().toUTC();
        }
        switch (cookie.sameSitePolicy()) {
        case QNetworkCookie::SameSite::None:
            portable.sameSite = CookieSameSite::None;
            break;
        case QNetworkCookie::SameSite::Lax:
            portable.sameSite = CookieSameSite::Lax;
            break;
        case QNetworkCookie::SameSite::Strict:
            portable.sameSite = CookieSameSite::Strict;
            break;
        default:
            portable.sameSite = CookieSameSite::Unspecified;
            break;
        }
        return portable;
    }

    static QNetworkCookie networkCookieFromPortable(const PortableCookie &portable) {
        QNetworkCookie cookie(portable.name, portable.value);
        cookie.setDomain(
            portable.hostOnly ? QString::fromLatin1(portable.domain)
                              : QStringLiteral(".") + QString::fromLatin1(portable.domain)
        );
        cookie.setPath(QString::fromUtf8(portable.path));
        cookie.setSecure(portable.secure);
        cookie.setHttpOnly(portable.httpOnly);
        if (portable.expires) {
            cookie.setExpirationDate(*portable.expires);
        }
        switch (portable.sameSite) {
        case CookieSameSite::None:
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::None);
            break;
        case CookieSameSite::Lax:
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::Lax);
            break;
        case CookieSameSite::Strict:
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::Strict);
            break;
        case CookieSameSite::Unspecified:
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::Default);
            break;
        }
        return cookie;
    }

    static QUrl cookieOriginFor(const QNetworkCookie &cookie) {
        QString domain = cookie.domain();
        while (domain.startsWith(QLatin1Char('.'))) {
            domain.remove(0, 1);
        }
        QUrl origin;
        origin.setScheme(cookie.isSecure() ? QStringLiteral("https") : QStringLiteral("http"));
        origin.setHost(domain);
        origin.setPath(cookie.path());
        return origin;
    }

    QtWebEngineProfile::QtWebEngineProfile(
        const EngineProfileParameters &parameters,
        const QString &userAgent,
        QQmlEngine *engine,
        QObject *parent
    )
        : EngineProfile(parameters, parent),
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
                download->setDownloadDirectory(directory);
                const int id = static_cast<int>(download->id());
                emit downloadStarted(
                    id,
                    download->suggestedFileName(),
                    download->url(),
                    directory + "/" + download->suggestedFileName(),
                    download->totalBytes()
                );
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
                connect(download, &QWebEngineDownloadRequest::stateChanged, this, update);
                download->accept();
            }
        );
        attachCookieMirror();
    }

    QtWebEngineProfile::~QtWebEngineProfile() {
        if (m_replace && m_replace->callback) {
            CookieReplaceResult result;
            result.errorCode = QStringLiteral("profile_destroyed");
            m_replace->callback(std::move(result));
        }
    }

    QObject *QtWebEngineProfile::nativeProfile() const {
        return m_profile;
    }

    QWebEngineCookieStore *QtWebEngineProfile::cookieStore() const {
        return m_profile ? m_profile->cookieStore() : nullptr;
    }

    void QtWebEngineProfile::attachCookieMirror() {
        QWebEngineCookieStore *store = cookieStore();
        if (!store) {
            return;
        }
        connect(store, &QWebEngineCookieStore::cookieAdded, this, [this](const QNetworkCookie &cookie) {
            m_cookieSignalsObserved = true;
            m_cookieMirror.insert(mirrorKey(cookie), cookie);
        });
        connect(store, &QWebEngineCookieStore::cookieRemoved, this, [this](const QNetworkCookie &cookie) {
            m_cookieSignalsObserved = true;
            m_cookieMirror.remove(mirrorKey(cookie));
        });
        store->loadAllCookies();
        m_mirrorReady = true;
    }

    void QtWebEngineProfile::clearData() {
        if (!m_profile) {
            return;
        }
        m_profile->cookieStore()->deleteAllCookies();
        m_profile->clearHttpCache();
    }

    bool QtWebEngineProfile::supportsPortableCookies() const {
        return !isPrivate() && m_mirrorReady;
    }

    void QtWebEngineProfile::exportPortableCookies(CookieSnapshotCallback callback) {
        CookieSnapshotResult result;
        if (!m_mirrorReady) {
            result.errorCode = QStringLiteral("store_unavailable");
            QMetaObject::invokeMethod(
                this,
                [callback = std::move(callback), result = std::move(result)]() mutable { callback(std::move(result)); },
                Qt::QueuedConnection
            );
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        result.cookies.reserve(m_cookieMirror.size());
        for (const QNetworkCookie &cookie : std::as_const(m_cookieMirror)) {
            PortableCookie portable = portableFromNetworkCookie(cookie);
            if (canonicalizePortableCookie(portable, now)) {
                result.cookies.append(std::move(portable));
            } else {
                wipePortableCookie(portable);
                ++result.skippedCookies;
            }
        }
        QMetaObject::invokeMethod(
            this,
            [callback = std::move(callback), result = std::move(result)]() mutable { callback(std::move(result)); },
            Qt::QueuedConnection
        );
    }

    void
    QtWebEngineProfile::replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) {
        QWebEngineCookieStore *store = cookieStore();
        if (!store || !m_mirrorReady) {
            CookieReplaceResult result;
            result.errorCode = QStringLiteral("store_unavailable");
            QMetaObject::invokeMethod(
                this,
                [callback = std::move(callback), result = std::move(result)]() mutable { callback(std::move(result)); },
                Qt::QueuedConnection
            );
            return;
        }
        if (m_replace && m_replace->callback) {
            CookieReplaceResult superseded;
            superseded.errorCode = QStringLiteral("superseded");
            m_replace->callback(std::move(superseded));
        }
        m_replace = std::make_unique<ReplaceOperation>();
        m_replace->callback = std::move(callback);
        m_replace->generation = ++m_replaceGeneration;
        const QList<QNetworkCookie> current = m_cookieMirror.values();
        for (const QNetworkCookie &cookie : current) {
            store->deleteCookie(cookie, cookieOriginFor(cookie));
        }
        for (const PortableCookie &portable : cookies) {
            const QNetworkCookie cookie = networkCookieFromPortable(portable);
            m_replace->expected.insert(mirrorKey(cookie), cookie);
            store->setCookie(cookie, cookieOriginFor(cookie));
        }
        settleReplace(m_replace->generation, 0);
    }

    void QtWebEngineProfile::settleReplace(quint64 generation, int attempt) {
        QPointer<QtWebEngineProfile> guard(this);
        QTimer::singleShot(attempt == 0 ? 60 : 250, this, [guard, generation, attempt] {
            if (!guard || !guard->m_replace || guard->m_replace->generation != generation) {
                return;
            }
            ReplaceOperation &operation = *guard->m_replace;
            bool settled = true;
            for (auto iterator = operation.expected.cbegin(); iterator != operation.expected.cend(); ++iterator) {
                if (!guard->m_cookieMirror.contains(iterator.key())) {
                    settled = false;
                    break;
                }
            }
            const bool signalsAvailable = guard->m_cookieSignalsObserved;
            if (!settled && signalsAvailable && attempt < 16) {
                guard->settleReplace(generation, attempt + 1);
                return;
            }
            CookieReplaceResult result;
            if (settled) {
                result.importedCookies = operation.expected.size();
            } else if (!signalsAvailable) {
                result.importedCookies = operation.expected.size();
                for (auto iterator = operation.expected.cbegin(); iterator != operation.expected.cend(); ++iterator) {
                    guard->m_cookieMirror.insert(iterator.key(), iterator.value());
                }
            } else {
                for (auto iterator = operation.expected.cbegin(); iterator != operation.expected.cend(); ++iterator) {
                    if (guard->m_cookieMirror.contains(iterator.key())) {
                        ++result.importedCookies;
                    }
                }
                result.skippedCookies = operation.expected.size() - result.importedCookies;
                result.errorCode = QStringLiteral("settle_incomplete");
            }
            CookieReplaceCallback callback = std::move(operation.callback);
            guard->m_replace.reset();
            if (callback) {
                callback(std::move(result));
            }
        });
    }

}
