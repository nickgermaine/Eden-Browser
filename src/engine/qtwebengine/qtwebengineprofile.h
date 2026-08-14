#pragma once

#include "engine/engineprofile.h"

class QQuickWebEngineProfile;
class QQmlEngine;
class QString;

namespace eden::engine {

    class QtWebEngineProfile final : public EngineProfile {
        Q_OBJECT

      public:
        explicit QtWebEngineProfile(
            bool privateProfile,
            const QString &userAgent,
            QQmlEngine *engine,
            QObject *parent = nullptr
        );
        ~QtWebEngineProfile() override;

        QObject *nativeProfile() const override;
        void clearData() override;

      private:
        QObject *m_profilePrototype;
        QQuickWebEngineProfile *m_profile;
    };

}
