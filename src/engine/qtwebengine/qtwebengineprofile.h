#pragma once

#include "engine/engineprofile.h"

class QQuickWebEngineProfile;
class QQmlEngine;

namespace eden::engine {

class QtWebEngineProfile final : public EngineProfile {
    Q_OBJECT

  public:
    explicit QtWebEngineProfile(bool privateProfile, QQmlEngine *engine, QObject *parent = nullptr);
    ~QtWebEngineProfile() override;

    QObject *nativeProfile() const override;

  private:
    QObject *m_profilePrototype;
    QQuickWebEngineProfile *m_profile;
};

}
