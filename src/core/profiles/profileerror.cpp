#include "core/profiles/profileerror.h"

namespace eden::core {

    QString profileErrorCode(ProfileError error) {
        switch (error) {
        case ProfileError::None:
            return QStringLiteral("ok");
        case ProfileError::RegistryOpenFailed:
            return QStringLiteral("registry_open_failed");
        case ProfileError::RegistryBusy:
            return QStringLiteral("registry_busy");
        case ProfileError::RegistryCorrupt:
            return QStringLiteral("registry_corrupt");
        case ProfileError::RegistryWriteFailed:
            return QStringLiteral("registry_write_failed");
        case ProfileError::InvalidProfileId:
            return QStringLiteral("invalid_profile_id");
        case ProfileError::InvalidDisplayName:
            return QStringLiteral("invalid_display_name");
        case ProfileError::InvalidPassword:
            return QStringLiteral("invalid_password");
        case ProfileError::WrongPassword:
            return QStringLiteral("wrong_password");
        case ProfileError::VerifierFailed:
            return QStringLiteral("verifier_failed");
        case ProfileError::ProfileMissing:
            return QStringLiteral("profile_missing");
        case ProfileError::ProfileNotReady:
            return QStringLiteral("profile_not_ready");
        case ProfileError::OperationInProgress:
            return QStringLiteral("operation_in_progress");
        case ProfileError::LastProfile:
            return QStringLiteral("last_profile");
        case ProfileError::DirectoryCreateFailed:
            return QStringLiteral("directory_create_failed");
        case ProfileError::PermissionRestricted:
            return QStringLiteral("permission_restricted");
        case ProfileError::AvatarUnreadable:
            return QStringLiteral("avatar_unreadable");
        case ProfileError::AvatarTooLarge:
            return QStringLiteral("avatar_too_large");
        case ProfileError::AvatarDecodeFailed:
            return QStringLiteral("avatar_decode_failed");
        case ProfileError::AvatarWriteFailed:
            return QStringLiteral("avatar_write_failed");
        case ProfileError::StagingFailed:
            return QStringLiteral("staging_failed");
        case ProfileError::CleanupFailed:
            return QStringLiteral("cleanup_failed");
        case ProfileError::MigrationFailed:
            return QStringLiteral("migration_failed");
        case ProfileError::LockHeld:
            return QStringLiteral("lock_held");
        case ProfileError::SignOutBlocked:
            return QStringLiteral("sign_out_blocked");
        case ProfileError::HandoffUnsupported:
            return QStringLiteral("handoff_unsupported");
        case ProfileError::HandoffFailed:
            return QStringLiteral("handoff_failed");
        case ProfileError::Cancelled:
            return QStringLiteral("cancelled");
        case ProfileError::SettingsReadFailed:
            return QStringLiteral("settings_read_failed");
        case ProfileError::SettingsInvalid:
            return QStringLiteral("settings_invalid");
        case ProfileError::SettingsWriteFailed:
            return QStringLiteral("settings_write_failed");
        case ProfileError::SessionReadFailed:
            return QStringLiteral("session_read_failed");
        case ProfileError::SessionInvalid:
            return QStringLiteral("session_invalid");
        case ProfileError::SessionWriteFailed:
            return QStringLiteral("session_write_failed");
        case ProfileError::VaultOpenFailed:
            return QStringLiteral("vault_open_failed");
        case ProfileError::DatabaseOpenFailed:
            return QStringLiteral("database_open_failed");
        case ProfileError::HistoryReadFailed:
            return QStringLiteral("history_read_failed");
        case ProfileError::HistoryWriteFailed:
            return QStringLiteral("history_write_failed");
        }
        return QStringLiteral("unknown");
    }

    QString profileErrorMessage(ProfileError error) {
        switch (error) {
        case ProfileError::HistoryReadFailed:
            return QStringLiteral(
                "Your browsing history could not be read. Check access to the profile and try again."
            );
        case ProfileError::HistoryWriteFailed:
            return QStringLiteral(
                "Your browsing history could not be saved. Check available space and folder permissions."
            );
        case ProfileError::VaultOpenFailed:
            return QStringLiteral(
                "Your profile vault could not be opened. Check access to your keyring and try again."
            );
        case ProfileError::DatabaseOpenFailed:
            return QStringLiteral(
                "Your browsing data could not be opened. Check access to the profile or restore a working copy."
            );
        case ProfileError::SessionReadFailed:
            return QStringLiteral("Your saved tabs could not be read. Check access to the profile and try again.");
        case ProfileError::SessionInvalid:
            return QStringLiteral("Your saved tabs could not be verified. Restore a working copy before trying again.");
        case ProfileError::SessionWriteFailed:
            return QStringLiteral("Your tabs could not be saved. Check available space and folder permissions.");
        case ProfileError::None:
            return {};
        case ProfileError::RegistryOpenFailed:
        case ProfileError::RegistryCorrupt:
            return QStringLiteral("The profile list could not be read.");
        case ProfileError::RegistryBusy:
        case ProfileError::RegistryWriteFailed:
            return QStringLiteral("The profile list could not be updated.");
        case ProfileError::InvalidProfileId:
        case ProfileError::ProfileMissing:
            return QStringLiteral("That profile no longer exists.");
        case ProfileError::InvalidDisplayName:
            return QStringLiteral("Enter a name between 1 and 64 characters without line breaks or slashes.");
        case ProfileError::InvalidPassword:
            return QStringLiteral("Passwords need 8 to 256 characters, and both entries must match.");
        case ProfileError::WrongPassword:
            return QStringLiteral("That password is not right.");
        case ProfileError::VerifierFailed:
            return QStringLiteral("The profile could not be opened.");
        case ProfileError::ProfileNotReady:
            return QStringLiteral("This profile needs repair before it can open.");
        case ProfileError::OperationInProgress:
            return QStringLiteral("Another change to this profile is still finishing.");
        case ProfileError::LastProfile:
            return QStringLiteral("Create another profile before deleting this one.");
        case ProfileError::DirectoryCreateFailed:
        case ProfileError::PermissionRestricted:
            return QStringLiteral("Eden could not use this profile's folder. Check its permissions and try again.");
        case ProfileError::AvatarUnreadable:
            return QStringLiteral("That image could not be read.");
        case ProfileError::AvatarTooLarge:
            return QStringLiteral("Choose an image under 20 MB and 8192 by 8192 pixels.");
        case ProfileError::AvatarDecodeFailed:
            return QStringLiteral("That image format is not supported.");
        case ProfileError::AvatarWriteFailed:
            return QStringLiteral("The picture could not be saved. Your previous picture is unchanged.");
        case ProfileError::StagingFailed:
            return QStringLiteral("The profile could not be deleted. Its data is unchanged.");
        case ProfileError::CleanupFailed:
            return QStringLiteral(
                "Some deleted profile data is still waiting for cleanup. Eden will retry on the next launch."
            );
        case ProfileError::MigrationFailed:
            return QStringLiteral("Your existing browsing data could not be moved into a profile. Nothing was lost.");
        case ProfileError::LockHeld:
            return QStringLiteral("Another Eden window is already managing profiles.");
        case ProfileError::SignOutBlocked:
            return QStringLiteral("This profile is signing out.");
        case ProfileError::HandoffUnsupported:
        case ProfileError::HandoffFailed:
            return QStringLiteral(
                "Eden could not carry all sign-in data to the new engine. You may need to sign in again."
            );
        case ProfileError::Cancelled:
            return QStringLiteral("The operation was cancelled.");
        case ProfileError::SettingsReadFailed:
            return QStringLiteral("Your settings could not be read. Check access to the profile and try again.");
        case ProfileError::SettingsInvalid:
            return QStringLiteral("Your settings could not be verified. Restore a working copy before trying again.");
        case ProfileError::SettingsWriteFailed:
            return QStringLiteral("Your settings could not be saved. Check available space and folder permissions.");
        }
        return {};
    }

}
