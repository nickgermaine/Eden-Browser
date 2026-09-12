#pragma once

#include "core/profiles/profilepaths.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace eden::core {

    class EngineStorage final : public QObject {
        Q_OBJECT

      public:
        using Completion = std::function<void(const QString &)>;

        static EngineStorage *instance();
        explicit EngineStorage(QObject *parent = nullptr);
        ~EngineStorage() override;
        void prepare(const ProfilePaths::Roots &roots, Completion completion);
        bool protects(const QString &path) const;
        void shutdown();
        static bool migrateLegacyPath(const QString &source, const QString &destination);

      signals:
        void storageFailed(const QString &message);

      private:
        class Private;
        std::unique_ptr<Private> d;
    };

}
