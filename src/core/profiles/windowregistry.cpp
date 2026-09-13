#include "core/profiles/windowregistry.h"

#include <QQuickWindow>

#include <algorithm>

namespace eden::core {

    static WindowRegistry *&registryInstance() {
        static WindowRegistry *instance = nullptr;
        return instance;
    }

    WindowRegistry::WindowRegistry(QObject *parent)
        : QObject(parent) {
        registryInstance() = this;
    }

    WindowRegistry::~WindowRegistry() {
        if (registryInstance() == this) {
            registryInstance() = nullptr;
        }
    }

    WindowRegistry *WindowRegistry::instance() {
        return registryInstance();
    }

    void WindowRegistry::registerWindow(
        QQuickWindow *window,
        WindowController *controller,
        const QString &profileId,
        bool privateWindow
    ) {
        if (!controller || entryFor(controller)) {
            return;
        }
        Entry entry;
        entry.window = window;
        entry.controller = controller;
        entry.profileId = profileId;
        entry.privateWindow = privateWindow;
        entry.activationOrdinal = ++m_activationCounter;
        m_entries.append(entry);
        emit windowsChanged();
    }

    void WindowRegistry::unregisterWindow(WindowController *controller) {
        const qsizetype removed =
            m_entries.removeIf([controller](const Entry &entry) { return entry.controller == controller; });
        if (removed > 0) {
            emit windowsChanged();
        }
    }

    void WindowRegistry::touchActivation(WindowController *controller) {
        for (Entry &entry : m_entries) {
            if (entry.controller == controller) {
                entry.activationOrdinal = ++m_activationCounter;
                return;
            }
        }
    }

    QList<WindowController *> WindowRegistry::allControllers() const {
        QList<WindowController *> controllers;
        controllers.reserve(m_entries.size());
        for (const Entry &entry : m_entries) {
            controllers.append(entry.controller);
        }
        return controllers;
    }

    QList<WindowController *>
    WindowRegistry::profileControllers(const QString &profileId, bool includeNormal, bool includePrivate) const {
        QList<WindowController *> controllers;
        for (const Entry &entry : m_entries) {
            if (entry.profileId != profileId) {
                continue;
            }
            if (entry.privateWindow ? includePrivate : includeNormal) {
                controllers.append(entry.controller);
            }
        }
        return controllers;
    }

    WindowController *WindowRegistry::mostRecentProfileController(const QString &profileId, bool privateOnly) const {
        const Entry *best = nullptr;
        for (const Entry &entry : m_entries) {
            if (entry.profileId != profileId || (privateOnly && !entry.privateWindow)) {
                continue;
            }
            if (!best || entry.activationOrdinal > best->activationOrdinal) {
                best = &entry;
            }
        }
        return best ? best->controller : nullptr;
    }

    QQuickWindow *WindowRegistry::windowFor(WindowController *controller) const {
        const Entry *entry = entryFor(controller);
        return entry ? entry->window.data() : nullptr;
    }

    QString WindowRegistry::profileIdFor(const WindowController *controller) const {
        const Entry *entry = entryFor(controller);
        return entry ? entry->profileId : QString();
    }

    bool WindowRegistry::isPrivateWindow(const WindowController *controller) const {
        const Entry *entry = entryFor(controller);
        return entry && entry->privateWindow;
    }

    int WindowRegistry::normalWindowCount() const {
        return static_cast<int>(std::count_if(m_entries.cbegin(), m_entries.cend(), [](const Entry &entry) {
            return !entry.privateWindow;
        }));
    }

    int WindowRegistry::normalWindowCountForProfile(const QString &profileId) const {
        return static_cast<int>(std::count_if(m_entries.cbegin(), m_entries.cend(), [&profileId](const Entry &entry) {
            return !entry.privateWindow && entry.profileId == profileId;
        }));
    }

    QString WindowRegistry::mostRecentActiveProfileId(bool includePrivate) const {
        const Entry *best = nullptr;
        for (const Entry &entry : m_entries) {
            if (entry.privateWindow && !includePrivate) {
                continue;
            }
            if (!best || entry.activationOrdinal > best->activationOrdinal) {
                best = &entry;
            }
        }
        return best ? best->profileId : QString();
    }

    const WindowRegistry::Entry *WindowRegistry::entryFor(const WindowController *controller) const {
        for (const Entry &entry : m_entries) {
            if (entry.controller == controller) {
                return &entry;
            }
        }
        return nullptr;
    }

}
