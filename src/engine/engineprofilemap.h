#pragma once

#include "engine/enginebackend.h"

#include <QHash>
#include <functional>
#include <memory>

namespace eden::engine {

class EngineProfile;

class EngineProfileMap final {
  public:
    using Factory = std::function<std::shared_ptr<EngineProfile>(Backend backend, bool privateProfile)>;

    EngineProfileMap(bool privateProfile, Factory factory);

    std::shared_ptr<EngineProfile> profile(Backend backend);
    void clearData();
    qsizetype size() const;

  private:
    bool m_privateProfile;
    Factory m_factory;
    QHash<Backend, std::shared_ptr<EngineProfile>> m_profiles;
};

}
