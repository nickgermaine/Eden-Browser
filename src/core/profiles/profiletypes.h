#pragma once

#include "core/profiles/profileid.h"

#include <QString>

namespace eden::core {

    enum class ProfileLifecycle { Creating, Ready, Deleting };

    enum class ProfileSessionState { Locked, Unlocking, Unlocked, SigningOut, Deleting };

    enum class StartupDisposition { ResumeLast, ChooseProfile };

    struct ProfileRecord {
        ProfileId id;
        QString displayName;
        quint32 colorSeed = 0;
        int avatarRevision = 0;
        QString passwordVerifier;
        ProfileLifecycle lifecycle = ProfileLifecycle::Creating;
        qint64 createdAt = 0;
        qint64 lastUsedAt = 0;
    };

    struct ProfileSummary {
        QString profileId;
        QString displayName;
        quint32 colorSeed = 0;
        int avatarRevision = 0;
        bool protectedProfile = false;
        ProfileLifecycle lifecycle = ProfileLifecycle::Creating;
        qint64 createdAt = 0;
        qint64 lastUsedAt = 0;
    };

    inline ProfileSummary summaryFromRecord(const ProfileRecord &record) {
        return ProfileSummary{
            record.id.toString(),
            record.displayName,
            record.colorSeed,
            record.avatarRevision,
            !record.passwordVerifier.isEmpty(),
            record.lifecycle,
            record.createdAt,
            record.lastUsedAt,
        };
    }

    QString profileLifecycleName(ProfileLifecycle lifecycle);
    std::optional<ProfileLifecycle> profileLifecycleFromName(const QString &name);
    QString profileSessionStateName(ProfileSessionState state);

}
