#pragma once

#include "engine/engineprofile.h"

class QQuickWebEngineProfile;
class QQmlEngine;
class QString;

namespace eden::engine {

    class QtCookieBridge;

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
        QtCookieBridge *cookieBridge();
        QQmlEngine *m_engine;
        QtCookieBridge *m_cookieBridge = nullptr;
        QObject *m_profilePrototype;
        QQuickWebEngineProfile *m_profile;
    };

}
