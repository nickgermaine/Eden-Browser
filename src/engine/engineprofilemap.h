#pragma once

#include "engine/enginebackend.h"

#include <QHash>
#include <QList>
#include <functional>
#include <memory>

namespace eden::engine {

    class EngineProfile;

    class EngineProfileMap final {
      public:
        using Factory = std::function<std::shared_ptr<EngineProfile>(Backend backend)>;

        EngineProfileMap(bool privateProfile, Factory factory);

        std::shared_ptr<EngineProfile> profile(Backend backend);
        std::shared_ptr<EngineProfile> existingProfile(Backend backend) const;
        QList<std::shared_ptr<EngineProfile>> liveProfiles() const;
        void clearData();
        void clear();
        qsizetype size() const;
        bool isPrivate() const;

      private:
        bool m_privateProfile;
        Factory m_factory;
        QHash<Backend, std::shared_ptr<EngineProfile>> m_profiles;
    };

}
