#pragma once

#include "engine/engineprofile.h"

class QQuickWebEngineProfile;

namespace eden::engine {

class QtWebEngineProfile final : public EngineProfile {
    Q_OBJECT

  public:
    explicit QtWebEngineProfile(bool privateProfile, QObject *parent = nullptr);
    ~QtWebEngineProfile() override;

    QObject *nativeProfile() const override;

  private:
    QQuickWebEngineProfile *m_profile;
};

}
