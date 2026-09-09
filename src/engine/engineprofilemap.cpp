#include "engine/engineprofilemap.h"
#include "engine/engineprofile.h"

namespace eden::engine {

    EngineProfileMap::EngineProfileMap(bool privateProfile, Factory factory)
        : m_privateProfile(privateProfile),
          m_factory(std::move(factory)) {}

    std::shared_ptr<EngineProfile> EngineProfileMap::profile(Backend backend) {
        if (const std::shared_ptr<EngineProfile> existing = m_profiles.value(backend)) {
            return existing;
        }
        std::shared_ptr<EngineProfile> created = m_factory ? m_factory(backend) : nullptr;
        if (created) {
            m_profiles.insert(backend, created);
        }
        return created;
    }

    std::shared_ptr<EngineProfile> EngineProfileMap::existingProfile(Backend backend) const {
        return m_profiles.value(backend);
    }

    QList<std::shared_ptr<EngineProfile>> EngineProfileMap::liveProfiles() const {
        QList<std::shared_ptr<EngineProfile>> profiles;
        profiles.reserve(m_profiles.size());
        for (const std::shared_ptr<EngineProfile> &profile : m_profiles) {
            if (profile) {
                profiles.append(profile);
            }
        }
        return profiles;
    }

    void EngineProfileMap::clearData() {
        for (const std::shared_ptr<EngineProfile> &profile : std::as_const(m_profiles)) {
            if (profile) {
                profile->clearData();
            }
        }
    }

    void EngineProfileMap::clear() {
        m_profiles.clear();
    }

    qsizetype EngineProfileMap::size() const {
        return m_profiles.size();
    }

    bool EngineProfileMap::isPrivate() const {
        return m_privateProfile;
    }

}
