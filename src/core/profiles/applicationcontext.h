#pragma once

#include "core/profiles/profilepaths.h"

#include <memory>

namespace eden::core {

    class ProfileManager;
    class ProfileRegistry;
    class WindowRegistry;
    class WindowLaunchServer;
    struct WindowLaunchRequest;

    class ApplicationContext final {
      public:
        ApplicationContext();
        explicit ApplicationContext(const ProfilePaths::Roots &roots);
        ~ApplicationContext();

        const ProfilePaths::Roots &roots() const;
        ProfileRegistry *registry() const;
        WindowRegistry *windows() const;
        ProfileManager *profiles() const;
        bool acquireSingleInstanceLock();
        bool listenForLaunchRequests();
        bool forwardLaunchRequest(const WindowLaunchRequest &request);

      private:
        ProfilePaths::Roots m_roots;
        std::unique_ptr<ProfileRegistry> m_registry;
        std::unique_ptr<WindowRegistry> m_windows;
        std::unique_ptr<ProfileManager> m_profiles;
        std::unique_ptr<WindowLaunchServer> m_launchServer;
    };

}
