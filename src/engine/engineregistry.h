#pragma once

#include "engine/enginebackend.h"

#include <QObject>
#include <QVariantList>
#include <optional>

namespace eden::engine {

    struct EngineDescriptor {
        Backend backend;
        QString id;
        QString displayName;
        bool experimental = false;
    };

    class EngineRegistry final : public QObject {
        Q_OBJECT
        Q_PROPERTY(QVariantList engines READ engines CONSTANT)

      public:
        explicit EngineRegistry(QList<EngineDescriptor> descriptors, QObject *parent = nullptr);

        static EngineRegistry *instance();

        QVariantList engines() const;
        const QList<EngineDescriptor> &descriptors() const;
        bool contains(Backend backend) const;
        bool contains(const QString &id) const;
        std::optional<Backend> backendForId(const QString &id) const;
        QString idForBackend(Backend backend) const;
        QString displayName(Backend backend) const;
        Q_INVOKABLE QVariantList selectionActions(const QString &selectedId = {}) const;

      private:
        QList<EngineDescriptor> m_descriptors;
    };

}
