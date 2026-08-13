#include "engine/engineregistry.h"

#include <algorithm>

namespace eden::engine {

EngineRegistry::EngineRegistry(QList<EngineDescriptor> descriptors, QObject *parent)
    : QObject(parent) {
    for (EngineDescriptor &descriptor : descriptors) {
        descriptor.id = descriptor.id.trimmed().toLower();
        if (descriptor.id.isEmpty() || descriptor.displayName.isEmpty() || contains(descriptor.backend) || contains(descriptor.id)) {
            continue;
        }
        m_descriptors.append(std::move(descriptor));
    }
}

EngineRegistry *EngineRegistry::instance() {
    static EngineRegistry registry({
#if EDEN_ENGINE_CEF
        {Backend::Cef, "cef", "Blink", false},
#endif
#if EDEN_ENGINE_QTWEBENGINE
        {Backend::QtWebEngine, "qtwebengine", "Blink (Qt)", false},
#endif
    });
    return &registry;
}

QVariantList EngineRegistry::engines() const {
    QVariantList result;
    result.reserve(m_descriptors.size());
    for (const EngineDescriptor &descriptor : m_descriptors) {
        QVariantMap entry;
        entry.insert("id", descriptor.id);
        entry.insert("name", descriptor.displayName);
        entry.insert("experimental", descriptor.experimental);
        result.append(entry);
    }
    return result;
}

const QList<EngineDescriptor> &EngineRegistry::descriptors() const {
    return m_descriptors;
}

bool EngineRegistry::contains(Backend backend) const {
    return std::any_of(m_descriptors.cbegin(), m_descriptors.cend(),
                       [backend](const EngineDescriptor &descriptor) { return descriptor.backend == backend; });
}

bool EngineRegistry::contains(const QString &id) const {
    return backendForId(id).has_value();
}

std::optional<Backend> EngineRegistry::backendForId(const QString &id) const {
    const QString normalized = id.trimmed().toLower();
    const auto found = std::find_if(m_descriptors.cbegin(), m_descriptors.cend(),
                                    [&normalized](const EngineDescriptor &descriptor) { return descriptor.id == normalized; });
    if (found == m_descriptors.cend()) {
        return std::nullopt;
    }
    return found->backend;
}

QString EngineRegistry::idForBackend(Backend backend) const {
    const auto found = std::find_if(m_descriptors.cbegin(), m_descriptors.cend(),
                                    [backend](const EngineDescriptor &descriptor) { return descriptor.backend == backend; });
    return found == m_descriptors.cend() ? QString() : found->id;
}

QString EngineRegistry::displayName(Backend backend) const {
    const auto found = std::find_if(m_descriptors.cbegin(), m_descriptors.cend(),
                                    [backend](const EngineDescriptor &descriptor) { return descriptor.backend == backend; });
    return found == m_descriptors.cend() ? QString() : found->displayName;
}

QVariantList EngineRegistry::selectionActions(const QString &selectedId) const {
    QVariantList actions;
    actions.reserve(m_descriptors.size());
    for (const EngineDescriptor &descriptor : m_descriptors) {
        QVariantMap action;
        action.insert("id", descriptor.id);
        action.insert("title", descriptor.displayName);
        action.insert("subtitle", descriptor.experimental ? QString("Experimental") : QString());
        action.insert("icon", descriptor.id == selectedId ? QString("check") : QString());
        actions.append(action);
    }
    return actions;
}

}
