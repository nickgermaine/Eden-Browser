from wpe.patch_support import replace_once


AUDIO_CONTEXT = "Source/WebCore/Modules/webaudio/AudioContext.cpp"
MEDIA_ELEMENT_SESSION = "Source/WebCore/html/MediaElementSession.cpp"
PERMISSIONS_POLICY_HEADER = "Source/WebCore/html/PermissionsPolicy.h"
PERMISSIONS_POLICY = "Source/WebCore/html/PermissionsPolicy.cpp"
ARGUMENT_CODERS = "Source/WebKit/Shared/WebCoreArgumentCoders.serialization.in"


DOCUMENT_ALLOWED_TO_AUTOPLAY = r'''bool PermissionsPolicy::isDocumentAllowedToAutoplay(const Document& document)
{
    if (document.requiresUserGestureForVideoPlayback())
        return false;

    RefPtr page = document.page();
    if (page && page->requiresUserGestureForVideoPlayback())
        return false;

    if (document.isCapturing())
        return true;

    bool autoplayIsEnabled = isFeatureEnabled(Feature::Autoplay, document, ShouldReportViolation::No);
    for (RefPtr frame = document.frame(); frame; frame = dynamicDowncast<LocalFrame>(frame->tree().parent())) {
        if (RefPtr window = frame->window(); window && window->hasStickyActivation())
            return true;
        if (!autoplayIsEnabled)
            return false;
    }
    return false;
}

'''


def audio_context_patch(text):
    text = replace_once(
        text,
        '#include "Performance.h"\n',
        '#include "Performance.h"\n#include "PermissionsPolicy.h"\n',
    )
    text = replace_once(
        text,
        "    RefPtr window = document.window();\n    return window && window->hasTransientActivation();\n}\n",
        "    RefPtr window = document.window();\n"
        "    if (window && window->hasTransientActivation())\n"
        "        return true;\n"
        "    return PermissionsPolicy::isDocumentAllowedToAutoplay(document);\n"
        "}\n",
    )
    return replace_once(
        text,
        "    m_wasSuspendedByScript = true;\n\n"
        "    if (!willPausePlayback()) {\n"
        "        addReaction(State::Suspended, WTF::move(promise));\n"
        "        return;\n"
        "    }\n",
        "    m_wasSuspendedByScript = true;\n\n"
        "    if (!willPausePlayback()) {\n"
        "        if (state() == State::Suspended) {\n"
        "            promise.resolve();\n"
        "            return;\n"
        "        }\n"
        "        addReaction(State::Suspended, WTF::move(promise));\n"
        "        return;\n"
        "    }\n",
    )


def media_element_session_patch(text):
    text = replace_once(
        text,
        '#include "Page.h"\n',
        '#include "Page.h"\n#include "PermissionsPolicy.h"\n',
    )
    return replace_once(
        text,
        "    if (m_restrictions & RequireUserGestureForAudioRateChange && (!element->isVideo() || element->hasAudio()) && !element->muted() && element->volume() && !document->processingUserGestureForMedia())\n",
        "    if (m_restrictions & RequireUserGestureForAudioRateChange && (!element->isVideo() || element->hasAudio()) && !element->muted() && element->volume() && !document->processingUserGestureForMedia() && !PermissionsPolicy::isDocumentAllowedToAutoplay(document.get()))\n",
    )


def permissions_policy_header_patch(text):
    text = replace_once(
        text,
        "        PrivateToken,\n        StorageAccess,\n        Invalid\n",
        "        PrivateToken,\n        StorageAccess,\n        Autoplay,\n        Invalid\n",
    )
    return replace_once(
        text,
        "    static bool isFeatureEnabled(Feature, const Document&, ShouldReportViolation = ShouldReportViolation::Yes);\n",
        "    static bool isFeatureEnabled(Feature, const Document&, ShouldReportViolation = ShouldReportViolation::Yes);\n"
        "    static bool isDocumentAllowedToAutoplay(const Document&);\n",
    )


def permissions_policy_patch(text):
    text = replace_once(
        text,
        '#include "ElementInlines.h"\n',
        '#include "ElementInlines.h"\n#include "FrameDestructionObserverInlines.h"\n#include "FrameTree.h"\n',
    )
    text = replace_once(
        text,
        '#include "LocalDOMWindow.h"\n',
        '#include "LocalDOMWindow.h"\n#include "LocalFrame.h"\n',
    )
    text = replace_once(
        text,
        '#include "OwnerPermissionsPolicyData.h"\n',
        '#include "OwnerPermissionsPolicyData.h"\n#include "Page.h"\n',
    )
    text = replace_once(
        text,
        '    case PermissionsPolicy::Feature::StorageAccess:\n        return "StorageAccess"_s;\n',
        '    case PermissionsPolicy::Feature::StorageAccess:\n        return "StorageAccess"_s;\n'
        '    case PermissionsPolicy::Feature::Autoplay:\n        return "Autoplay"_s;\n',
    )
    text = replace_once(
        text,
        '    constexpr auto storageAccessToken { "storage-access"_s };\n',
        '    constexpr auto storageAccessToken { "storage-access"_s };\n'
        '    constexpr auto autoplayToken { "autoplay"_s };\n',
    )
    text = replace_once(
        text,
        "    } else if (value.startsWith(storageAccessToken)) {\n"
        "        feature = PermissionsPolicy::Feature::StorageAccess;\n"
        "        remainingValue = value.substring(storageAccessToken.length());\n"
        "    }\n",
        "    } else if (value.startsWith(storageAccessToken)) {\n"
        "        feature = PermissionsPolicy::Feature::StorageAccess;\n"
        "        remainingValue = value.substring(storageAccessToken.length());\n"
        "    } else if (value.startsWith(autoplayToken)) {\n"
        "        feature = PermissionsPolicy::Feature::Autoplay;\n"
        "        remainingValue = value.substring(autoplayToken.length());\n"
        "    }\n",
    )
    text = replace_once(
        text,
        "    case PermissionsPolicy::Feature::PrivateToken:\n        return \"'self'\"_s;\n",
        "    case PermissionsPolicy::Feature::PrivateToken:\n    case PermissionsPolicy::Feature::Autoplay:\n        return \"'self'\"_s;\n",
    )
    return replace_once(
        text,
        "bool PermissionsPolicy::isFeatureEnabled(Feature feature, const Document& document, ShouldReportViolation shouldReportViolation)\n",
        DOCUMENT_ALLOWED_TO_AUTOPLAY + "bool PermissionsPolicy::isFeatureEnabled(Feature feature, const Document& document, ShouldReportViolation shouldReportViolation)\n",
    )


def argument_coders_patch(text):
    return replace_once(
        text,
        "    PrivateToken,\n    StorageAccess,\n    Invalid\n};\n\nclass WebCore::PermissionsPolicy {\n",
        "    PrivateToken,\n    StorageAccess,\n    Autoplay,\n    Invalid\n};\n\nclass WebCore::PermissionsPolicy {\n",
    )


PATCHES = {
    AUDIO_CONTEXT: audio_context_patch,
    MEDIA_ELEMENT_SESSION: media_element_session_patch,
    PERMISSIONS_POLICY_HEADER: permissions_policy_header_patch,
    PERMISSIONS_POLICY: permissions_policy_patch,
    ARGUMENT_CODERS: argument_coders_patch,
}
