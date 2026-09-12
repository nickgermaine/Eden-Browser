#include "core/profiles/profiletypes.h"

namespace eden::core {

    QString profileLifecycleName(ProfileLifecycle lifecycle) {
        switch (lifecycle) {
        case ProfileLifecycle::Creating:
            return QStringLiteral("Creating");
        case ProfileLifecycle::Ready:
            return QStringLiteral("Ready");
        case ProfileLifecycle::Deleting:
            return QStringLiteral("Deleting");
        }
        return QStringLiteral("Ready");
    }

    std::optional<ProfileLifecycle> profileLifecycleFromName(const QString &name) {
        if (name == QLatin1String("Creating")) {
            return ProfileLifecycle::Creating;
        }
        if (name == QLatin1String("Ready")) {
            return ProfileLifecycle::Ready;
        }
        if (name == QLatin1String("Deleting")) {
            return ProfileLifecycle::Deleting;
        }
        return std::nullopt;
    }

    QString profileSessionStateName(ProfileSessionState state) {
        switch (state) {
        case ProfileSessionState::Locked:
            return QStringLiteral("locked");
        case ProfileSessionState::Unlocking:
            return QStringLiteral("unlocking");
        case ProfileSessionState::Unlocked:
            return QStringLiteral("unlocked");
        case ProfileSessionState::SigningOut:
            return QStringLiteral("signing_out");
        case ProfileSessionState::Deleting:
            return QStringLiteral("deleting");
        }
        return QStringLiteral("locked");
    }

}
