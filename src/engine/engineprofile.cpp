#include "engine/engineprofile.h"
namespace eden::engine {

EngineProfile::EngineProfile(bool privateProfile, QObject *parent)
    : QObject(parent),
      m_privateProfile(privateProfile) {}

EngineProfile::~EngineProfile() = default;

bool EngineProfile::isPrivate() const {
    return m_privateProfile;
}

}
