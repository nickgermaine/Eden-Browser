#include "engine/engineprofile.h"
#include "engine/qtwebengine/qtwebengineprofile.h"

#include <QCoreApplication>

namespace eden::engine {

EngineProfile::EngineProfile(bool privateProfile, QObject *parent)
    : QObject(parent),
      m_privateProfile(privateProfile) {}

EngineProfile::~EngineProfile() = default;

bool EngineProfile::isPrivate() const {
    return m_privateProfile;
}

EngineProfile *EngineProfile::defaultProfile() {
    static QtWebEngineProfile *profile = new QtWebEngineProfile(false, QCoreApplication::instance());
    return profile;
}

std::unique_ptr<EngineProfile> EngineProfile::createPrivateProfile(QObject *parent) {
    return std::make_unique<QtWebEngineProfile>(true, parent);
}

}
