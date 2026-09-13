#include "core/profiles/applicationcontext.h"

#include "core/profiles/profilemanager.h"
#include "core/profiles/profileregistry.h"
#include "core/profiles/windowlaunchserver.h"
#include "core/profiles/windowregistry.h"

namespace eden::core {

    ApplicationContext::ApplicationContext()
        : ApplicationContext(ProfilePaths::standardRoots()) {}

    ApplicationContext::ApplicationContext(const ProfilePaths::Roots &roots)
        : m_roots(roots),
          m_registry(std::make_unique<ProfileRegistry>(ProfilePaths::registryDatabasePath(roots))),
          m_windows(std::make_unique<WindowRegistry>()),
          m_profiles(std::make_unique<ProfileManager>(m_registry.get(), m_windows.get(), roots)),
          m_launchServer(std::make_unique<WindowLaunchServer>(roots.dataRoot)) {}

    ApplicationContext::~ApplicationContext() = default;

    const ProfilePaths::Roots &ApplicationContext::roots() const {
        return m_roots;
    }

    ProfileRegistry *ApplicationContext::registry() const {
        return m_registry.get();
    }

    WindowRegistry *ApplicationContext::windows() const {
        return m_windows.get();
    }

    ProfileManager *ApplicationContext::profiles() const {
        return m_profiles.get();
    }

    bool ApplicationContext::acquireSingleInstanceLock() {
        return m_registry->acquireProcessLock(ProfilePaths::registryLockPath(m_roots));
    }

    bool ApplicationContext::listenForLaunchRequests() {
        return m_registry->processLockHeld() && m_launchServer->listen([this](const WindowLaunchRequest &request) {
            return m_profiles->requestWindow(request);
        });
    }

    bool ApplicationContext::forwardLaunchRequest(const WindowLaunchRequest &request) {
        return m_launchServer->forward(request);
    }

}
