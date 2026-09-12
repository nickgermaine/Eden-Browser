#pragma once

#include "core/profiles/profilecontext.h"
#include "core/profiles/profileid.h"
#include "core/profiles/profilepaths.h"
#include "core/profiles/profiletypes.h"

#include <QTemporaryDir>

#include <memory>

class QQmlEngine;

namespace eden::test {

    struct ProfileHarness {
        QTemporaryDir dataRoot;
        QTemporaryDir cacheRoot;
        std::shared_ptr<eden::core::ProfileContext> context;

        eden::core::ProfilePaths::Roots roots() const {
            return eden::core::ProfilePaths::Roots{dataRoot.path(), cacheRoot.path()};
        }

        bool create(QQmlEngine *qmlEngine = nullptr, const QString &displayName = QStringLiteral("Test")) {
            if (!dataRoot.isValid() || !cacheRoot.isValid()) {
                return false;
            }
            eden::core::ProfileRecord record;
            record.id = eden::core::ProfileId::generate();
            record.displayName = displayName;
            record.lifecycle = eden::core::ProfileLifecycle::Ready;
            const eden::core::ProfilePaths paths(roots(), record.id);
            eden::core::ProfileError error = eden::core::ProfileError::None;
            context = eden::core::ProfileContext::create(record, paths, qmlEngine, &error);
            return static_cast<bool>(context);
        }
    };

}
