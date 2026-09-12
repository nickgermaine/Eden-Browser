#pragma once

#include <QString>

namespace eden::core {

    enum class ProfileError {
        None,
        RegistryOpenFailed,
        RegistryBusy,
        RegistryCorrupt,
        RegistryWriteFailed,
        InvalidProfileId,
        InvalidDisplayName,
        InvalidPassword,
        WrongPassword,
        VerifierFailed,
        ProfileMissing,
        ProfileNotReady,
        OperationInProgress,
        LastProfile,
        DirectoryCreateFailed,
        PermissionRestricted,
        AvatarUnreadable,
        AvatarTooLarge,
        AvatarDecodeFailed,
        AvatarWriteFailed,
        StagingFailed,
        CleanupFailed,
        MigrationFailed,
        LockHeld,
        SignOutBlocked,
        HandoffUnsupported,
        HandoffFailed,
        Cancelled,
        SettingsReadFailed,
        SettingsInvalid,
        SettingsWriteFailed,
        SessionReadFailed,
        SessionInvalid,
        SessionWriteFailed,
        VaultOpenFailed,
        DatabaseOpenFailed,
        HistoryReadFailed,
        HistoryWriteFailed
    };

    QString profileErrorCode(ProfileError error);
    QString profileErrorMessage(ProfileError error);

}
