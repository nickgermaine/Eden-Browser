#include "engine/engineprofilemap.h"
#include "engine/engineprofile.h"

namespace eden::engine {

EngineProfileMap::EngineProfileMap(bool privateProfile, Factory factory)
    : m_privateProfile(privateProfile),
      m_factory(std::move(factory)) {}

std::shared_ptr<EngineProfile> EngineProfileMap::profile(Backend backend) {
    const auto found = m_profiles.constFind(backend);
    if (found != m_profiles.cend()) {
        return *found;
    }
    std::shared_ptr<EngineProfile> created = m_factory ? m_factory(backend, m_privateProfile) : nullptr;
    if (created) {
        m_profiles.insert(backend, created);
    }
    return created;
}

void EngineProfileMap::clearData() {
    for (const std::shared_ptr<EngineProfile> &profile : std::as_const(m_profiles)) {
        if (profile) {
            profile->clearData();
        }
    }
}

qsizetype EngineProfileMap::size() const {
    return m_profiles.size();
}

}
