#pragma once

#include "engine/engineprofile.h"

#include <QHash>
#include <QNetworkCookie>

class QQuickWebEngineProfile;
class QQmlEngine;
class QString;
class QWebEngineCookieStore;

namespace eden::engine {

    class QtWebEngineProfile final : public EngineProfile {
        Q_OBJECT

      public:
        explicit QtWebEngineProfile(
            const EngineProfileParameters &parameters,
            const QString &userAgent,
            QQmlEngine *engine,
            QObject *parent = nullptr
        );
        ~QtWebEngineProfile() override;

        QObject *nativeProfile() const override;
        void clearData() override;

        bool supportsPortableCookies() const override;
        void exportPortableCookies(CookieSnapshotCallback callback) override;
        void replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) override;

      private:
        void attachCookieMirror();
        QWebEngineCookieStore *cookieStore() const;
        void settleReplace(quint64 generation, int attempt);

        struct ReplaceOperation {
            QHash<QByteArray, QNetworkCookie> expected;
            CookieReplaceCallback callback;
            qsizetype skipped = 0;
            quint64 generation = 0;
        };

        QObject *m_profilePrototype;
        QQuickWebEngineProfile *m_profile;
        QHash<QByteArray, QNetworkCookie> m_cookieMirror;
        std::unique_ptr<ReplaceOperation> m_replace;
        quint64 m_replaceGeneration = 0;
        bool m_mirrorReady = false;
        bool m_cookieSignalsObserved = false;
    };

}
