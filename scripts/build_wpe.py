import argparse
import hashlib
import inspect
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

from wpe.adopted_patches import adopted_sources, load as load_adopted_patches, prepare_adopted_patches, render
from wpe import autoplay_activation, capture_device_enumeration, capture_frames, display_capture, dmabuf_video_frames, hang_recorder, libwebrtc_glue, mediastream_playback, video_encoder_tuning, video_frame_conversion, voice_processing, webcodecs_bitstreams, webgl_teardown, webrtc_interop, webrtc_source_identity
from wpe.patch_support import without_cmake_comments
from wpe.canvas_replay import prepare_canvas_replay
from wpe.bolt import STAGES as BOLT_STAGES, run_stage as run_bolt_stage
from wpe.rendering_experiments import prepare_rendering_experiments
from wpe.pgo import prepare_profile, profile_identity
from wpe.inspector_theme import apply_inspector_theme
from wpe.performance_patches import (
    DEFAULT_PATCHES,
    DEFAULT_PROFILE_VARIANT,
    EXPERIMENTAL_PATCHES,
    names as patch_names,
    resolve_patches,
    selections as patch_selections,
)


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError("The pinned WebKit source does not match the embedding patch")
    return text.replace(before, after, 1)


def prepare_libwebrtc_source(root, source, version, digest):
    archive = root / "third_party" / "wpe" / "packages" / f"libwebrtc-wpewebkit-{version}.tar.xz"
    if not archive.is_file():
        raise RuntimeError(f"The pinned libwebrtc archive is missing; run scripts/wpe/libwebrtc_fetch.py first ({archive})")
    with archive.open("rb") as stream:
        archive_digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if archive_digest != digest:
        raise RuntimeError("The libwebrtc archive failed SHA-256 verification")
    if (source / LIBWEBRTC_SOURCE_SUBTREE / "CMakeLists.txt").is_file():
        return
    third_party = source / "Source" / "ThirdParty"
    third_party.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=third_party) as staging:
        with tarfile.open(archive) as package:
            package.extractall(staging, filter="data")
        (Path(staging) / f"libwebrtc-wpewebkit-{version}" / "Source" / "ThirdParty" / "libwebrtc").rename(
            source / LIBWEBRTC_SOURCE_SUBTREE
        )


def libwebrtc_subtree_patches(archive, version):
    with tarfile.open(archive) as package:
        originals = {
            name: package.extractfile(f"libwebrtc-wpewebkit-{version}/{name}").read().decode()
            for name in libwebrtc_glue.SUBTREE_PATCHES
        }
    return {name: without_cmake_comments(patch(originals[name])) for name, patch in libwebrtc_glue.SUBTREE_PATCHES.items()}


def apply_libwebrtc_subtree_patches(root, source, version):
    archive = root / "third_party" / "wpe" / "packages" / f"libwebrtc-wpewebkit-{version}.tar.xz"
    for name, content in libwebrtc_subtree_patches(archive, version).items():
        destination = source / name
        if destination.read_text() != content:
            destination.write_text(content)


def prepare_source(root, version, digest, variant="", reset_performance=False, webrtc_backend="gstreamer", libwebrtc_digest=None):
    dependency = root / "third_party" / "wpe"
    archive = dependency / "packages" / f"wpewebkit-{version}.tar.xz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        urllib.request.urlretrieve(f"https://wpewebkit.org/releases/wpewebkit-{version}.tar.xz", temporary)
        temporary.rename(archive)
    with archive.open("rb") as stream:
        archive_digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if archive_digest != digest:
        raise RuntimeError("The WebKit archive failed SHA-256 verification")
    source = dependency / "source" / (f"wpewebkit-{version}" + ("-" + variant if variant else ""))
    if not (source / "CMakeLists.txt").exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=source.parent) as staging:
            with tarfile.open(archive) as package:
                package.extractall(staging, filter="data")
            (Path(staging) / f"wpewebkit-{version}").rename(source)
    if reset_performance:
        prepare_canvas_replay(root, source, version, False)
        prepare_rendering_experiments(root, source, version, False)
        prepare_adopted_patches(root, source, version, False)
    if webrtc_backend == "libwebrtc":
        prepare_libwebrtc_source(root, source, version, libwebrtc_digest)
        apply_libwebrtc_subtree_patches(root, source, version)
    with tarfile.open(archive) as package:
        original = archive_originals(package, version)
    for name, content in embedded_patches(original, version).items():
        destination = source / name
        if destination.read_text() != content:
            destination.write_text(content)
    active = {path for module in correctness_modules(webrtc_backend) for path in module.PATCHES}
    inactive = {path for module in GSTREAMER_CORRECTNESS_MODULES + LIBWEBRTC_CORRECTNESS_MODULES for path in module.PATCHES} - active
    for name in sorted(inactive):
        destination = source / name
        if name in original and destination.is_file() and destination.read_text() != original[name]:
            destination.write_text(original[name])
    restore_dropped_embedded_paths(source, archive, version, EMBEDDED_PATHS)
    return source


def archive_originals(package, version):
    members = set(package.getnames())
    module_paths = {path for module in GSTREAMER_CORRECTNESS_MODULES + LIBWEBRTC_CORRECTNESS_MODULES for path in module.PATCHES}
    originals = {}
    for name in EMBEDDED_PATHS:
        member = f"wpewebkit-{version}/{name}"
        if member not in members and version != "2.53.92" and name in module_paths:
            continue
        originals[name] = package.extractfile(member).read().decode()
    return originals


def restore_dropped_embedded_paths(source, archive, version, current):
    manifest = source / EMBEDDED_MANIFEST
    previous = set(json.loads(manifest.read_text())) if manifest.is_file() else set(RETIRED_EMBEDDED_PATHS)
    dropped = sorted(previous - set(current))
    if dropped:
        with tarfile.open(archive) as package:
            for name in dropped:
                destination = source / name
                if not destination.is_file():
                    continue
                try:
                    member = package.getmember(f"wpewebkit-{version}/{name}")
                except KeyError:
                    destination.unlink()
                    continue
                pristine = package.extractfile(member).read()
                if destination.read_bytes() != pristine:
                    destination.write_bytes(pristine)
    manifest.write_text(json.dumps(sorted(set(current)), indent=1) + "\n")
    return dropped


EMBEDDED_PATHS = [
    "Source/WebKit/UIProcess/API/glib/WebKitUIClient.cpp",
    "Source/WebKit/UIProcess/Inspector/wpe/WebInspectorUIProxyWPE.cpp",
    "Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp",
    "Source/WebKit/UIProcess/API/glib/WebKitUserMediaPermissionRequest.cpp",
    "Source/WebCore/platform/mediastream/gstreamer/GStreamerDisplayCaptureDeviceManager.cpp",
    "Source/WebKit/UIProcess/UserMediaPermissionRequestProxy.h",
    "Source/WebKit/UIProcess/API/wpe/PageClientImpl.cpp",
    "Source/WebKit/UIProcess/API/wpe/WebKitPopupMenu.cpp",
    "Source/WebInspectorUI/UserInterface/Views/Main.css",
    "Source/WebKit/UIProcess/glib/SystemSettingsManagerProxy.cpp",
    "Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp",
]

GSTREAMER_CORRECTNESS_MODULES = (webrtc_source_identity, webrtc_interop, mediastream_playback, display_capture, voice_processing, video_frame_conversion, video_encoder_tuning, capture_frames, autoplay_activation, capture_device_enumeration, hang_recorder, webgl_teardown, dmabuf_video_frames, webcodecs_bitstreams)
LIBWEBRTC_CORRECTNESS_MODULES = (mediastream_playback, display_capture, voice_processing, video_frame_conversion, video_encoder_tuning, capture_frames, libwebrtc_glue, autoplay_activation, capture_device_enumeration, hang_recorder, webgl_teardown, dmabuf_video_frames, webcodecs_bitstreams)
EMBEDDED_CORRECTNESS_MODULES = GSTREAMER_CORRECTNESS_MODULES
EMBEDDED_MANIFEST = ".eden-embedded-paths.json"
RETIRED_EMBEDDED_PATHS = (
    "Source/WebCore/platform/mediastream/gstreamer/GStreamerMockDeviceProvider.cpp",
)
EMBEDDED_PATHS += list(dict.fromkeys(path for module in GSTREAMER_CORRECTNESS_MODULES + LIBWEBRTC_CORRECTNESS_MODULES for path in module.PATCHES))
LIBWEBRTC_SOURCE_SUBTREE = "Source/ThirdParty/libwebrtc"


def correctness_modules(backend):
    if backend == "libwebrtc":
        return LIBWEBRTC_CORRECTNESS_MODULES
    return GSTREAMER_CORRECTNESS_MODULES


def embedded_generator_sha256(modules):
    generator_source = (
        inspect.getsource(replace_once).encode()
        + inspect.getsource(embedded_patches).encode()
        + b"".join(Path(module.__file__).read_bytes() for module in modules)
    )
    generator_paths = json.dumps(EMBEDDED_PATHS, separators=(",", ":")).encode()
    return hashlib.sha256(generator_source + b"\0" + generator_paths).hexdigest()


def profile_matches_backend(profile, generator_sha256):
    sidecar = profile.with_suffix(profile.suffix + ".json")
    if not sidecar.is_file():
        return False
    metadata = json.loads(sidecar.read_text())
    identity = metadata.get("trainingIdentity")
    return isinstance(identity, dict) and identity.get("embeddedPatchGeneratorSHA256") == generator_sha256


def select_profile(dependency, version, variant, variant_suffix, webrtc_backend, generator_sha256, no_profile):
    if no_profile:
        return None
    profiles = []
    if variant:
        profiles.append(dependency / "profiles" / f"{version}{variant_suffix}.profdata")
    profiles.extend(
        [
            dependency / "profiles" / f"{version}-{DEFAULT_PROFILE_VARIANT}.profdata",
            dependency / "profiles" / f"{version}.profdata",
        ]
    )
    if webrtc_backend == "libwebrtc":
        profiles = [profile for profile in profiles if profile_matches_backend(profile, generator_sha256)]
    return next((profile for profile in profiles if profile.is_file()), None)


def automatic_variant(experiments, frame_pointers, webrtc_backend):
    variant = ""
    if experiments and frame_pointers:
        variant = "experiments-frame-profile"
    elif experiments:
        variant = "experiments"
    elif frame_pointers:
        variant = "frame-profile"
    if webrtc_backend == "libwebrtc":
        variant = "-".join(["libwebrtc", variant] if variant else ["libwebrtc"])
    return variant


def webrtc_configure_flags(backend):
    if backend == "libwebrtc":
        return ["-DENABLE_WEB_RTC=ON", "-DUSE_GSTREAMER_WEBRTC=OFF", "-DUSE_LIBWEBRTC=ON"]
    return ["-DENABLE_WEB_RTC=ON", "-DUSE_GSTREAMER_WEBRTC=ON", "-DUSE_LIBWEBRTC=OFF"]


def managed_source_paths(source, adopted_patches, canvas_replay_enabled, webrtc_backend="gstreamer"):
    paths = set(EMBEDDED_PATHS)
    if adopted_patches:
        patches, files, _ = load_adopted_patches(adopted_patches)
        paths.update(patches)
        paths.update(files)
    if canvas_replay_enabled:
        paths.update({
            "Source/WebCore/platform/graphics/skia/ImageBufferSkiaAcceleratedBackend.cpp",
            "Source/WebCore/platform/graphics/skia/ImageBufferSkiaAcceleratedBackend.h",
            "Source/WebCore/platform/graphics/skia/EdenSkiaCanvasReplay.h",
            "Source/ThirdParty/skia/src/gpu/ganesh/GrDDLTask.cpp",
        })
    if webrtc_backend == "libwebrtc":
        subtree = source / LIBWEBRTC_SOURCE_SUBTREE
        if subtree.is_dir():
            paths.update(str(path.relative_to(source)) for path in sorted(subtree.rglob("*")) if path.is_file())
    return paths


def snapshot_managed_sources(source, paths):
    snapshot = {}
    for relative_path in paths:
        path = source / relative_path
        if path.is_file():
            snapshot[relative_path] = (path.stat().st_mtime_ns, file_digest(path))
    return snapshot


def restore_unchanged_source_timestamps(source, snapshot):
    for relative_path, (modified, digest) in snapshot.items():
        path = source / relative_path
        if path.is_file() and file_digest(path) == digest:
            os.utime(path, ns=(path.stat().st_atime_ns, modified))


def prepared_managed_source_digest(source, adopted_patches, canvas_replay_enabled, webrtc_backend="gstreamer"):
    paths = managed_source_paths(source, adopted_patches, canvas_replay_enabled, webrtc_backend)
    result = hashlib.sha256()
    for path in sorted(paths):
        result.update(path.encode())
        result.update(b"\0")
        result.update(file_digest(source / path))
    return result.hexdigest()


def embedded_patches(original, version):
    paths = EMBEDDED_PATHS
    client = original[paths[0]]
    client = replace_once(
        client,
        '#include "DrawingAreaProxy.h"',
        '#include "DrawingAreaProxy.h"\n#include "APISecurityOrigin.h"\n#include "FrameInfoData.h"',
    )
    client = replace_once(
        client,
        "const WebKit::FrameInfoData&, Function<void(bool)>& completionHandler",
        "const WebKit::FrameInfoData& frameInfo, Function<void(bool)>& completionHandler",
    )
    client = replace_once(
        client,
        "API::SecurityOrigin&, const WTF::String& keySystem",
        "API::SecurityOrigin& origin, const WTF::String& keySystem",
    )
    client = replace_once(
        client,
        "API::SecurityOrigin&, CompletionHandler<void(bool allowed)>&& completionHandler",
        "API::SecurityOrigin& origin, CompletionHandler<void(bool allowed)>&& completionHandler",
    )
    for name, origin in [
        ("geolocationPermissionRequest", "frameInfo.securityOrigin.toString()"),
        ("permissionRequest", "origin.securityOrigin().toString()"),
        ("userMediaPermissionRequest", "userMediaDocumentOrigin.securityOrigin().toString()"),
        ("notificationPermissionRequest", "origin.securityOrigin().toString()"),
    ]:
        before = f"        webkitWebViewMakePermissionRequest(m_webView, WEBKIT_PERMISSION_REQUEST({name}.get()));"
        after = (
            f'        g_object_set_data_full(G_OBJECT({name}.get()), "eden-permission-origin-v1", g_strdup({origin}.utf8().data()), g_free);\n'
            + before
        )
        if name == "permissionRequest":
            start = client.index("void decidePolicyForMediaKeySystemPermissionRequest")
            end = client.index("void decidePolicyForUserMediaPermissionRequest", start)
            client = client[:start] + replace_once(client[start:end], before, after) + client[end:]
        else:
            client = replace_once(client, before, after)
    client = replace_once(
        client,
        "#if PLATFORM(GTK)\n    bool takeFocus",
        """#if PLATFORM(WPE)
    bool takeFocus(WebPageProxy*, WKFocusDirection direction) final {
        g_signal_emit_by_name(m_webView, "eden-focus-v1", direction == kWKFocusDirectionBackward ? -1 : 1);
        return true;
    }
    void focus(WebPageProxy*) final {
        g_signal_emit_by_name(m_webView, "eden-focus-v1", 0);
    }
#endif
#if PLATFORM(GTK)
    bool takeFocus""",
    )
    for method in ["runJavaScriptAlert", "runJavaScriptConfirm", "runJavaScriptPrompt", "runBeforeUnloadConfirmPanel"]:
        start = client.index("    void " + method + "(")
        end = client.index("\n    }", start)
        body = client[start:end]
        body = replace_once(body, "WebKit::FrameInfoData&&,", "WebKit::FrameInfoData&& frameInfo,")
        call = body.index("        webkitWebViewRunJavaScript")
        body = (
            body[:call]
            + '        g_object_set_data_full(G_OBJECT(m_webView), "eden-dialog-origin-v1", g_strdup(frameInfo.securityOrigin.toString().utf8().data()), g_free);\n'
            + body[call:]
        )
        client = client[:start] + body + client[end:]
    inspector = original[paths[1]]
    start = inspector.index(
        "    static constexpr auto versionedDirectory",
        inspector.index("static Ref<WebsiteDataStore> inspectorWebsiteDataStore()"),
    )
    end = inspector.index("\n}", start)
    inspector = inspector[:start] + "    return WebsiteDataStore::createNonPersistent();" + inspector[end:]
    inspector = inspector.replace("Web Inspector " + chr(0x2014) + " %s", "Web Inspector (%s)")
    close_start = inspector.index("void WebInspectorUIProxy::platformCloseFrontendPageAndWindow()")
    close_end = inspector.index("void WebInspectorUIProxy::platformDidCloseForCrash()", close_start)
    close_body = inspector[close_start:close_end]
    close_body = replace_once(
        close_body,
        "    if (m_inspectorView)\n        g_signal_handlers_disconnect_by_data(m_inspectorView->wpeView(), this);",
        "    if (m_inspectorView) {\n        g_signal_handlers_disconnect_by_data(m_inspectorView->wpeView(), this);\n        wpe_view_closed(m_inspectorView->wpeView());\n    }",
    )
    inspector = inspector[:close_start] + close_body + inspector[close_end:]
    inspector = replace_once(
        inspector,
        '#include "APINavigationAction.h"',
        '#include "APINavigationAction.h"\n#include "APIContextMenuClient.h"\n#include "WebContextMenuProxy.h"\n#include "WebKitContextMenuPrivate.h"\n#include "WebKitHitTestResultPrivate.h"\n#include "WebKitOptionMenuPrivate.h"\n#include "WebKitUserContentPrivate.h"\n#include "WebUserContentControllerProxy.h"\n#include <WebCore/Color.h>',
    )
    inspector = replace_once(
        inspector,
        "class InspectorNavigationClient final : public API::NavigationClient {",
        """class EdenInspectorContextMenuClient final : public API::ContextMenuClient {
public:
    explicit EdenInspectorContextMenuClient(WPEView* view) : m_view(view) { }
private:
    void getContextMenuFromProposedMenu(WebPageProxy& page, Vector<Ref<WebContextMenuItem>>&& proposedMenu, WebContextMenuListenerProxy& listener, const WebHitTestResultData& hitTest, API::Object*) final {
        auto items = WTF::map(proposedMenu, [](auto& item) { return item->data(); });
        GRefPtr<WebKitContextMenu> menu = adoptGRef(webkitContextMenuCreate(items));
        webkitContextMenuSetPage(menu.get(), &page);
        if (auto* proxy = page.activeContextMenu()) {
            webkitContextMenuSetPosition(menu.get(), proxy->menuLocation());
        }
        GRefPtr<WebKitHitTestResult> result = adoptGRef(webkitHitTestResultCreate(hitTest));
        g_signal_emit_by_name(m_view, "eden-context-menu-v1", menu.get(), result.get());
        listener.useContextMenuItems({ });
    }
    WPEView* m_view;
};

class InspectorNavigationClient final : public API::NavigationClient {""",
    )
    inspector = replace_once(
        inspector,
        "    page->setNavigationClient(makeUniqueRef<InspectorNavigationClient>(*this));",
        """    if (!g_signal_lookup("eden-context-menu-v1", WPE_TYPE_VIEW)) {
        g_signal_new("eden-context-menu-v1", WPE_TYPE_VIEW, G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 2, WEBKIT_TYPE_CONTEXT_MENU, WEBKIT_TYPE_HIT_TEST_RESULT);
    }
    if (!g_signal_lookup("eden-option-menu-v1", WPE_TYPE_VIEW)) {
        g_signal_new("eden-option-menu-v1", WPE_TYPE_VIEW, G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 5, WEBKIT_TYPE_OPTION_MENU, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);
    }
    if (!g_signal_lookup("eden-inspector-style-v1", WPE_TYPE_VIEW)) {
        g_signal_new("eden-inspector-style-v1", WPE_TYPE_VIEW, G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_STRING);
    }
    g_signal_connect(wpeView, "eden-inspector-style-v1", G_CALLBACK(+[](WPEView*, const char* css, WebInspectorUIProxy* proxy) {
        if (!proxy->m_inspectorView || !css)
            return;
        Ref inspectorPage = proxy->m_inspectorView->page();
        inspectorPage->userContentController().removeAllUserStyleSheets();
        WebKitUserStyleSheet* sheet = webkit_user_style_sheet_new(css, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_STYLE_LEVEL_USER, nullptr, nullptr);
        inspectorPage->userContentController().addUserStyleSheet(webkitUserStyleSheetGetUserStyleSheet(sheet));
        webkit_user_style_sheet_unref(sheet);
    }), this);
    page->setBackgroundColor(WebCore::Color::transparentBlack);
    page->setContextMenuClient(makeUnique<EdenInspectorContextMenuClient>(wpeView));
    page->setNavigationClient(makeUniqueRef<InspectorNavigationClient>(*this));""",
    )
    view = original[paths[2]]
    start = view.index("static void webkit_web_view_class_init(")
    brace = view.index("{", start)
    view = (
        view[: brace + 1]
        + '\n    g_type_set_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-embedding-api-v1"), GINT_TO_POINTER(1));'
        + view[brace + 1 :]
    )
    view = replace_once(
        view,
        '#include "APIData.h"',
        '#include "APIData.h"\n#include "APIIconLoadingClient.h"\n#include "NetworkProcessMessages.h"\n#include "NetworkProcessProxy.h"\n#include "WebInspectorUIProxy.h"\n#include <wtf/glib/GWeakPtr.h>',
    )
    icon_client = """class EdenIconLoadingClient final : public API::IconLoadingClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(EdenIconLoadingClient);
public:
    explicit EdenIconLoadingClient(WebKitWebView* view) : m_view(view) { }
private:
    void getLoadDecisionForIcon(const WebCore::LinkIcon& icon, CompletionHandler<void(CompletionHandler<void(API::Data*)>&&)>&& completionHandler) final {
        if (icon.type != WebCore::LinkIconType::Favicon || !icon.url.protocolIsInHTTPFamily()) {
            completionHandler(nullptr);
            return;
        }
        auto pageURL = getPage(m_view).pageLoadState().activeURL();
        completionHandler([view = GWeakPtr<WebKitWebView>(m_view), pageURL = WTF::move(pageURL)](API::Data* data) {
            if (!view || !data || data->size() > 262144 || getPage(view.get()).pageLoadState().activeURL() != pageURL) {
                return;
            }
            GRefPtr<GBytes> bytes = adoptGRef(g_bytes_new(data->span().data(), data->size()));
            g_signal_emit_by_name(view.get(), "eden-favicon-v1", bytes.get());
        });
    }
    WebKitWebView* m_view;
};

"""
    if version == "2.53.92":
        icon_client = """class EdenIconLoadingClient final : public API::IconLoadingClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(EdenIconLoadingClient);
public:
    explicit EdenIconLoadingClient(WebKitWebView* view) : m_view(view) { }
private:
    void getLoadDecisionForIcons(const HashMap<WebKit::CallbackID, WebCore::LinkIcon>& icons, CompletionHandler<void(HashSet<WebKit::CallbackID>&&)>&& completionHandler) final {
        m_pageURLs.clear();
        HashSet<WebKit::CallbackID> accepted;
        if (auto* view = m_view.get()) {
            auto pageURL = getPage(view).pageLoadState().activeURL();
            for (const auto& [identifier, icon] : icons) {
                if (icon.type == WebCore::LinkIconType::Favicon && icon.url.protocolIsInHTTPFamily()) {
                    accepted.add(identifier);
                    m_pageURLs.add(identifier, pageURL);
                }
            }
        }
        completionHandler(WTF::move(accepted));
    }
    void iconLoaded(const WebKit::CallbackID& identifier, const WebCore::LinkIcon&, API::Data* data) final {
        auto pageURL = m_pageURLs.take(identifier);
        auto* view = m_view.get();
        if (!view || pageURL.isEmpty() || !data || data->size() > 262144 || getPage(view).pageLoadState().activeURL() != pageURL) {
            return;
        }
        GRefPtr<GBytes> bytes = adoptGRef(g_bytes_new(data->span().data(), data->size()));
        g_signal_emit_by_name(view, "eden-favicon-v1", bytes.get());
    }
    GWeakPtr<WebKitWebView> m_view;
    HashMap<WebKit::CallbackID, WTF::URL> m_pageURLs;
};

"""
    view = replace_once(
        view,
        "static void webkit_web_view_class_init(WebKitWebViewClass* webViewClass)",
        """static gint64 edenRendererProcessID(WebKitWebView* view) {
    auto& page = getPage(view);
    return page.hasRunningProcess() ? page.legacyMainFrameProcessID() : 0;
}

static void edenShowPageSource(WebKitWebView* view) {
    auto& page = getPage(view);
    if (auto* frame = page.mainFrame()) {
        if (auto* inspector = page.inspector())
            inspector->showMainResourceForFrame(frame->frameID());
    }
}

static void webkit_web_view_class_init(WebKitWebViewClass* webViewClass)""",
    )
    view = replace_once(
        view,
        '    g_type_set_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-embedding-api-v1"), GINT_TO_POINTER(1));',
        '    g_type_set_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-renderer-process-id-v1"), reinterpret_cast<gpointer>(edenRendererProcessID));\n    g_type_set_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-show-page-source-v1"), reinterpret_cast<gpointer>(edenShowPageSource));\n    g_type_set_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-embedding-api-v1"), GINT_TO_POINTER(1));',
    )
    flush_session = """static void edenFlushNetworkSession(WebKitNetworkSession* session, GAsyncReadyCallback callback, gpointer userData) {
    GRefPtr<GTask> task = adoptGRef(g_task_new(session, nullptr, callback, userData));
    Ref store = webkitWebsiteDataManagerGetDataStore(webkit_network_session_get_website_data_manager(session));
    store->syncLocalStorage([store, task = WTF::move(task)]() mutable {
        Ref networkProcess = store->networkProcess();
        networkProcess->sendWithAsyncReply(Messages::NetworkProcess::FlushCookies(store->sessionID()), [store, task = WTF::move(task)]() mutable {
            g_task_return_boolean(task.get(), TRUE);
        });
    });
}

"""
    view = replace_once(
        view,
        "static void webkit_web_view_class_init(WebKitWebViewClass* webViewClass)",
        flush_session + "static void webkit_web_view_class_init(WebKitWebViewClass* webViewClass)",
    )
    view = replace_once(
        view,
        "static void webkitWebViewConstructed(GObject* object)",
        icon_client + "static void webkitWebViewConstructed(GObject* object)",
    )
    if version == "2.53.92":
        view = replace_once(
            view,
            "    attachIconLoadingClientToView(webView);",
            "    getPage(webView).setIconLoadingClient(makeUnique<EdenIconLoadingClient>(webView));",
        )
    else:
        view = replace_once(
            view,
            "    attachFormClientToView(webView);",
            "    attachFormClientToView(webView);\n    getPage(webView).setIconLoadingClient(makeUnique<EdenIconLoadingClient>(webView));",
        )
    marker = '    g_type_set_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-embedding-api-v1"), GINT_TO_POINTER(1));'
    view = replace_once(
        view,
        marker,
        marker
        + '\n    g_type_set_qdata(WEBKIT_TYPE_NETWORK_SESSION, g_quark_from_static_string("eden-flush-storage-v1"), reinterpret_cast<gpointer>(edenFlushNetworkSession));\n    g_signal_new("eden-favicon-v1", G_TYPE_FROM_CLASS(webViewClass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_BYTES);\n    g_signal_new("eden-focus-v1", G_TYPE_FROM_CLASS(webViewClass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_INT);',
    )
    view = replace_once(
        view,
        "    if (state & WebCore::MediaProducer::MediaState::HasActiveScreenCaptureDevice)",
        "    if (state.containsAny({ WebCore::MediaProducer::MediaState::HasActiveScreenCaptureDevice, WebCore::MediaProducer::MediaState::HasActiveWindowCaptureDevice }))",
    )
    view = replace_once(
        view,
        "    if (state & WebCore::MediaProducer::MediaState::HasMutedScreenCaptureDevice)",
        "    if (state.containsAny({ WebCore::MediaProducer::MediaState::HasMutedScreenCaptureDevice, WebCore::MediaProducer::MediaState::HasMutedWindowCaptureDevice }))",
    )
    notification_start = view.index(
        "    if (mediaStateFlags.isEmpty())", view.index("void webkitWebViewMediaCaptureStateDidChange(")
    )
    notification_end = view.index("\n}", notification_start)
    view = (
        view[:notification_start]
        + """    UNUSED_PARAM(mediaStateFlags);
    g_object_notify_by_pspec(G_OBJECT(webView), sObjProperties[PROP_CAMERA_CAPTURE_STATE]);
    g_object_notify_by_pspec(G_OBJECT(webView), sObjProperties[PROP_DISPLAY_CAPTURE_STATE]);
    g_object_notify_by_pspec(G_OBJECT(webView), sObjProperties[PROP_MICROPHONE_CAPTURE_STATE]);"""
        + view[notification_end:]
    )
    media = original[paths[3]]
    media = replace_once(
        media,
        "    priv->request->allow(audioDevice, videoDevice);",
        """    if (priv->request->requiresDisplayCapture()) {
        auto type = g_object_get_data(G_OBJECT(request), "eden-display-window-v1") ? WebCore::CaptureDevice::DeviceType::Window : WebCore::CaptureDevice::DeviceType::Screen;
        videoDevice = emptyString();
        for (const auto& device : priv->request->eligibleVideoDevices()) {
            if (device.type() == type) {
                videoDevice = device.persistentId();
                break;
            }
        }
        if (videoDevice.isEmpty()) {
            priv->request->deny(UserMediaPermissionRequestProxy::UserMediaAccessDenialReason::NoCaptureDevices);
            return;
        }
    }
    priv->request->allow(audioDevice, videoDevice);""",
    )
    capture = original[paths[4]]
    capture = replace_once(
        capture,
        "auto outputType = GStreamerDisplayCaptureDeviceManager::PipeWireOutputType::Monitor | GStreamerDisplayCaptureDeviceManager::PipeWireOutputType::Window;",
        "auto outputType = device.type() == CaptureDevice::DeviceType::Window ? GStreamerDisplayCaptureDeviceManager::PipeWireOutputType::Window : GStreamerDisplayCaptureDeviceManager::PipeWireOutputType::Monitor;",
    )
    capture = replace_once(
        capture,
        "    m_devices.append(WTF::move(screenCaptureDevice));",
        """    m_devices.append(WTF::move(screenCaptureDevice));
    CaptureDevice windowCaptureDevice(createVersion4UUIDString(), CaptureDevice::DeviceType::Window, "Capture Window"_s);
    windowCaptureDevice.setEnabled(true);
    m_devices.append(WTF::move(windowCaptureDevice));""",
    )
    request = replace_once(
        original[paths[5]],
        "    Vector<String> videoDeviceUIDs() const;",
        "    Vector<String> videoDeviceUIDs() const;\n    const Vector<WebCore::CaptureDevice>& eligibleVideoDevices() const { return m_eligibleVideoDevices; }",
    )
    page_client = replace_once(
        original[paths[6]],
        """    if (!m_view.client().isGLibBasedAPI())
        return nullptr;
    return WebKitPopupMenu::create(m_view, page.popupMenuClient());""",
        """#if ENABLE(WPE_PLATFORM)
    if (!m_view.client().isGLibBasedAPI() && !m_view.wpeView())
        return nullptr;
#else
    if (!m_view.client().isGLibBasedAPI())
        return nullptr;
#endif
    return WebKitPopupMenu::create(m_view, page.popupMenuClient());""",
    )
    popup = replace_once(
        original[paths[7]],
        """    ASSERT(view.client().isGLibBasedAPI());
    return adoptRef(*new WebKitPopupMenu(view, client));""",
        """    return adoptRef(*new WebKitPopupMenu(view, client));""",
    )
    popup = replace_once(
        popup,
        """    GRefPtr<WebKitOptionMenu> menu = static_cast<WebKitWebViewClient&>(m_view.client()).showOptionMenu(*this, rect, items, selectedIndex);
    if (menu) {""",
        """    GRefPtr<WebKitOptionMenu> menu;
    if (m_view.client().isGLibBasedAPI())
        menu = static_cast<WebKitWebViewClient&>(m_view.client()).showOptionMenu(*this, rect, items, selectedIndex);
#if ENABLE(WPE_PLATFORM)
    else if (auto* wpeView = m_view.wpeView()) {
        const guint signal = g_signal_lookup("eden-option-menu-v1", WPE_TYPE_VIEW);
        if (signal && g_signal_has_handler_pending(wpeView, signal, 0, TRUE)) {
            menu = adoptGRef(webkitOptionMenuCreate(*this, items, selectedIndex));
            g_signal_emit_by_name(wpeView, "eden-option-menu-v1", menu.get(), rect.x(), rect.y(), rect.width(), rect.height());
        }
    }
#endif
    if (menu) {""",
    )
    popup = replace_once(
        popup,
        '#include "WebKitWebViewClient.h"',
        """#include "WebKitWebViewClient.h"

#if ENABLE(WPE_PLATFORM)
#include <wpe/wpe-platform.h>
#endif""",
    )
    inspector_style = replace_once(
        original[paths[8]],
        """    width: 1em;
    height: 1em;
    vertical-align: -2px;
}""",
        """    width: 1em;
    height: 1em;
    font: inherit;
    line-height: 1;
    vertical-align: -2px;
}""",
    )
    system_settings = replace_once(
        original[paths[9]],
        """    changedState.fontName = fontName;
}
#endif""",
        """    changedState.fontName = fontName;
    String family = fontName;
    double points = 10;
    if (auto index = fontName.reverseFind(' '); index != notFound) {
        auto sizeText = fontName.substring(index + 1).utf8();
        char* end = nullptr;
        double parsed = g_ascii_strtod(sizeText.data(), &end);
        if (end != sizeText.data() && parsed > 0) {
            points = parsed;
            family = fontName.left(index);
        }
    }
    if (family.endsWith(','))
        family = family.left(family.length() - 1);
    changedState.fontFamily = family;
    changedState.fontSize = static_cast<float>(points * (xftDPI() / 1024.0) / 72.0);
}
#endif""",
    )
    system_settings = replace_once(
        system_settings,
        "#include <WebCore/SystemSettings.h>",
        "#include <WebCore/SystemSettings.h>\n#include <glib.h>",
    )
    launcher = replace_once(
        original[paths[10]],
        """    if (processType == ProcessLauncher::ProcessType::DBusProxy)
        return false;""",
        """    if (processType == ProcessLauncher::ProcessType::DBusProxy)
        return false;

#if USE(GSTREAMER_WEBRTC)
    if (processType == ProcessLauncher::ProcessType::Web && !g_getenv("EDEN_WEBKIT_ISOLATE_WEB_PROCESS_NETWORK"))
        return false;
#endif""",
    )
    launcher = replace_once(
        launcher,
        """static int createFlatpakInfo(const char* instanceID)
{
    static NeverDestroyed<GUniquePtr<char>> data;
    static size_t size;

    if (!data.get()) {
        GUniquePtr<GKeyFile> keyFile(g_key_file_new());
        g_key_file_set_string(keyFile.get(), "Application", "name", WTF::applicationID().data());
        g_key_file_set_string(keyFile.get(), "Instance", "instance-id", instanceID);

        if (remoteInspectorEnabled())
            g_key_file_set_string(keyFile.get(), "Context", "shared", "network;");

        data->reset(g_key_file_to_data(keyFile.get(), &size, nullptr));
    }

    return createSealedMemFdWithData("flatpak-info", data->get(), size);
}""",
        """static int createFlatpakInfo(const char* instanceID)
{
    GUniquePtr<GKeyFile> keyFile(g_key_file_new());
    g_key_file_set_string(keyFile.get(), "Application", "name", WTF::applicationID().data());
    g_key_file_set_string(keyFile.get(), "Instance", "instance-id", instanceID);

    if (remoteInspectorEnabled())
        g_key_file_set_string(keyFile.get(), "Context", "shared", "network;");

    size_t size;
    GUniquePtr<char> data(g_key_file_to_data(keyFile.get(), &size, nullptr));
    return createSealedMemFdWithData("flatpak-info", data.get(), size);
}""",
    )
    launcher = replace_once(
        launcher,
        "#include <sys/mman.h>\n",
        "#include <sys/mman.h>\n#include <sys/stat.h>\n#include <sys/sysmacros.h>\n",
    )
    launcher = replace_once(
        launcher,
        """        CString path = FileSystem::pathByAppendingComponent("/dev"_s, fileName).utf8();
""",
        """        CString path = FileSystem::pathByAppendingComponent("/dev"_s, fileName).utf8();
        struct stat deviceStatus;
        if (!stat(path.data(), &deviceStatus) && S_ISCHR(deviceStatus.st_mode)) {
            CString udevEntry = makeString("/run/udev/data/c"_s, major(deviceStatus.st_rdev), ':', minor(deviceStatus.st_rdev)).utf8();
            args.append(CString("--ro-bind-try"));
            args.append(udevEntry);
            args.append(udevEntry);
        }
""",
    )
    patched = dict(
        zip(
            paths,
            [
                client,
                inspector,
                view,
                media,
                capture,
                request,
                page_client,
                popup,
                inspector_style,
                system_settings,
                launcher,
            ],
        )
    )
    root = Path(__file__).resolve().parents[1]
    module_patches = {}
    for module in EMBEDDED_CORRECTNESS_MODULES:
        for name, patch in module.PATCHES.items():
            module_patches.setdefault(name, []).append(patch)
    for name, patches in module_patches.items():
        if version == "2.53.92":
            patched[name] = render(root, original[name], patches, name)
        elif name in original:
            patched[name] = original[name]
    return patched


def file_digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").digest()


def stage_runtime_libraries(dependency, prefix):
    library_directory = prefix / "lib"
    webkit = library_directory / "libWPEWebKit-2.0.so"
    if not webkit.is_file():
        raise RuntimeError("Build and install the pinned WebKit library before staging its runtime")
    extra_directories = [dependency / "tools/usr/lib", dependency / "usr/lib"]
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(str(path) for path in extra_directories)
    helpers = sorted((prefix / "libexec/wpe-webkit-2.0").glob("WPE*Process"))
    if not helpers:
        raise RuntimeError("The installed WebKit runtime has no child-process executables")
    pending = [webkit.resolve(), *(helper.resolve() for helper in helpers)]
    visited = set()
    staged = set()
    while pending:
        library = pending.pop()
        if library in visited:
            continue
        visited.add(library)
        listing = subprocess.check_output(["ldd", str(library)], env=environment, text=True)
        if "=> not found" in listing:
            raise RuntimeError(f"Unresolved WebKit runtime dependency while examining {library}:\n{listing}")
        for line in listing.splitlines():
            match = re.match(r"\s*(\S+) => (/.+) \(0x[0-9a-fA-F]+\)", line)
            if not match:
                continue
            name, _ = match.groups()
            if Path(name).name != name or name.startswith("libWPEWebKit-2.0.so"):
                continue
            source = next((directory / name for directory in extra_directories if (directory / name).is_file()), None)
            if source is None:
                continue
            source = source.resolve()
            if not any(source.is_relative_to(directory.resolve()) for directory in extra_directories):
                raise RuntimeError(f"Runtime dependency escaped its package directory: {source}")
            destination = library_directory / name
            if not destination.is_file() or file_digest(source) != file_digest(destination):
                temporary = destination.with_name(destination.name + ".staging")
                shutil.copy2(source, temporary)
                temporary.replace(destination)
            staged.add(name)
            pending.append(source)
    verification_environment = os.environ.copy()
    verification_environment.pop("LD_LIBRARY_PATH", None)
    for helper in [webkit, *helpers]:
        listing = subprocess.check_output(["ldd", str(helper)], env=verification_environment, text=True)
        if "=> not found" in listing:
            raise RuntimeError(f"The installed WebKit runtime is incomplete: {helper}\n{listing}")
    print(f"Staged {len(staged)} runtime libraries inside WebKit's sandbox-visible library directory")


def main():
    parser = argparse.ArgumentParser(
        description="Build WPE WebKit with the known-good performance patch registry"
    )
    parser.add_argument("--jobs", type=int, default=16, help="combined worker budget, default: 16")
    parser.add_argument("--link-jobs", type=int, help="workers reserved for linking, default: 4")
    parser.add_argument("--cpus", default="0-7,16-23")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--runtime-only", action="store_true")
    parser.add_argument("--mode", choices=["baseline", "profile-generate", "performance"], default="performance")
    parser.add_argument(
        "--webrtc-backend",
        choices=["libwebrtc", "gstreamer"],
        default="libwebrtc",
        help="WebRTC implementation inside WebKit, default: libwebrtc",
    )
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--no-profile", action="store_true", help="performance build without PGO, for verification runtimes")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--variant", default="")
    parser.add_argument(
        "--experiments",
        action="store_true",
        help="add every experimental registry patch in an isolated build variant",
    )
    parser.add_argument("--frame-pointers", action="store_true")
    parser.add_argument("--verify-patches", action="store_true")
    parser.add_argument("--bolt", choices=BOLT_STAGES)
    parser.add_argument("--bolt-only", action="store_true", help="operate on an existing runtime without rebuilding")
    parser.add_argument("--bolt-plan", type=Path, help="JSON workload commands for BOLT training or validation")
    parser.add_argument("--bolt-tool-dir", type=Path)
    arguments = parser.parse_args()
    if arguments.variant and not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", arguments.variant):
        parser.error("--variant accepts lowercase letters, digits and hyphens")
    if arguments.experiments and arguments.mode == "baseline":
        parser.error("--experiments requires a performance source build")
    if arguments.frame_pointers and arguments.mode == "baseline":
        parser.error("--frame-pointers requires a performance source build")
    if arguments.verify_patches and not arguments.experiments:
        parser.error("--verify-patches requires --experiments")
    if arguments.bolt and arguments.mode != "performance":
        parser.error("--bolt requires performance mode")
    if arguments.bolt and arguments.frame_pointers:
        parser.error("BOLT uses the ordinary optimized runtime, without frame pointers")
    if arguments.bolt_only and not arguments.bolt:
        parser.error("--bolt-only requires --bolt")
    if arguments.bolt and arguments.bolt != "instrument" and not arguments.bolt_only:
        parser.error("Later BOLT stages require --bolt-only to preserve the trained binary")
    if arguments.bolt in ("train", "validate", "validate-performance") and not arguments.bolt_plan:
        parser.error("BOLT training and validation require --bolt-plan")
    if arguments.bolt_plan and arguments.bolt not in ("train", "validate", "validate-performance"):
        parser.error("--bolt-plan is used only by BOLT training and validation")
    if arguments.bolt and (arguments.prepare_only or arguments.configure_only or arguments.runtime_only):
        parser.error("BOLT cannot be combined with preparation, configuration or runtime staging only")
    if arguments.webrtc_backend == "libwebrtc" and arguments.mode == "baseline":
        parser.error("--webrtc-backend libwebrtc requires a performance source build; the baseline tracks the GStreamer backend")
    global EMBEDDED_CORRECTNESS_MODULES
    EMBEDDED_CORRECTNESS_MODULES = correctness_modules(arguments.webrtc_backend)
    topology = subprocess.check_output(["lscpu", "-p=CPU,CORE,SOCKET"], text=True)
    actual = {
        int(row.split(",")[0]): tuple(row.split(",")[1:])
        for row in topology.splitlines()
        if row and not row.startswith("#")
    }
    selected = set()
    for interval in arguments.cpus.split(","):
        limits = [int(value) for value in interval.split("-")]
        selected.update(range(limits[0], limits[-1] + 1))
    if not selected.issubset(actual):
        parser.error("--cpus includes a CPU absent from this host")
    cores = {actual[cpu] for cpu in selected}
    if not 1 <= arguments.jobs <= min(len(actual) // 2, len(selected)) or len(cores) > len(set(actual.values())) // 2:
        raise RuntimeError("The requested CPU set or worker count exceeds the half-machine build budget")
    if arguments.link_jobs is None:
        arguments.link_jobs = min(4, max(1, arguments.jobs - 1))
    if arguments.mode != "baseline" and not 1 <= arguments.link_jobs < arguments.jobs:
        parser.error("--link-jobs must leave at least one other slot in the total --jobs budget")
    build_jobs = arguments.jobs if arguments.mode == "baseline" else arguments.jobs - arguments.link_jobs
    patches = () if arguments.mode == "baseline" else resolve_patches(arguments.experiments)
    enabled_names = patch_names(patches)
    adopted_patches = patch_selections(patches, "adopted")
    if not arguments.variant:
        arguments.variant = automatic_variant(arguments.experiments, arguments.frame_pointers, arguments.webrtc_backend)
    root = Path(__file__).resolve().parents[1]
    dependency = root / "third_party/wpe"
    print("Enabled performance patches: " + (", ".join(enabled_names) if enabled_names else "none"), flush=True)
    suffix = {"baseline": "", "profile-generate": "-profile", "performance": "-performance"}[arguments.mode]
    variant_suffix = "-" + arguments.variant if arguments.variant else ""
    prefix = dependency / f"local{suffix}{variant_suffix}"
    if arguments.bolt_only:
        run_bolt_stage(root, prefix, arguments.bolt, arguments.bolt_tool_dir,
                       ["taskset", "--cpu-list", arguments.cpus], arguments.jobs, arguments.bolt_plan)
        print(prefix / "share/eden-bolt/state.json")
        return
    if (prefix / "share/eden-bolt/state.json").exists() and not arguments.runtime_only:
        parser.error("This runtime has a BOLT journal; use a fresh --variant for a source rebuild")
    if arguments.runtime_only:
        stage_runtime_libraries(dependency, prefix)
        apply_inspector_theme(root, prefix)
        return
    if arguments.profile and (arguments.mode != "performance" or not arguments.profile.is_file()):
        parser.error("--profile requires performance mode and an existing merged profile")
    if arguments.no_profile and (arguments.mode != "performance" or arguments.profile):
        parser.error("--no-profile requires performance mode and no --profile")
    versions = (root / "cmake" / "WpeVersion.cmake").read_text()
    version_key = "EDEN_WPE_MINIMUM_VERSION" if arguments.mode == "baseline" else "EDEN_WPE_PERFORMANCE_VERSION"
    digest_key = "EDEN_WPE_SOURCE_SHA256" if arguments.mode == "baseline" else "EDEN_WPE_PERFORMANCE_SHA256"
    version = re.search(version_key + r' "([^"]+)"', versions)[1]
    digest = re.search(digest_key + r' "([^"]+)"', versions)[1]
    libwebrtc_digest = None
    if arguments.webrtc_backend == "libwebrtc":
        libwebrtc_digest = re.search(r'EDEN_LIBWEBRTC_SHA256 "([^"]+)"', versions)[1]
    build = dependency / (("build" if arguments.mode == "baseline" else f"build-{version}{suffix}") + variant_suffix)
    if arguments.mode == "performance" and not arguments.profile:
        arguments.profile = select_profile(
            dependency,
            version,
            arguments.variant,
            variant_suffix,
            arguments.webrtc_backend,
            embedded_generator_sha256(EMBEDDED_CORRECTNESS_MODULES) if arguments.webrtc_backend == "libwebrtc" else None,
            arguments.no_profile,
        )
    if arguments.mode == "performance":
        print(
            "Selected PGO profile: " + (str(arguments.profile) if arguments.profile else "none"),
            flush=True,
        )
    expected_source = dependency / "source" / (f"wpewebkit-{version}" + variant_suffix)
    source_snapshot = snapshot_managed_sources(
        expected_source,
        managed_source_paths(expected_source, adopted_patches, "canvas-replay" in enabled_names, arguments.webrtc_backend),
    )
    source = prepare_source(
        root,
        version,
        digest,
        arguments.variant,
        arguments.mode != "baseline",
        arguments.webrtc_backend,
        libwebrtc_digest,
    )
    if arguments.mode != "baseline":
        if "canvas-replay" in enabled_names:
            prepare_canvas_replay(root, source, version, True)
        if adopted_patches:
            prepare_adopted_patches(root, source, version, True, names=adopted_patches)
    restore_unchanged_source_timestamps(source, source_snapshot)
    if arguments.prepare_only:
        print(source)
        return
    environment = os.environ.copy()
    tools = dependency / "tools" / "usr"
    environment["PATH"] = str(tools / "bin") + os.pathsep + environment["PATH"]
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        [str(tools / "lib"), str(dependency / "usr" / "lib"), environment.get("LD_LIBRARY_PATH", "")]
    )
    environment["PKG_CONFIG_PATH"] = os.pathsep.join(
        [str(dependency / "usr" / "lib" / "pkgconfig"), environment.get("PKG_CONFIG_PATH", "")]
    )
    environment["CPATH"] = os.pathsep.join([str(tools / "include"), environment.get("CPATH", "")])
    if (tools / "bin" / "ruby").is_file():
        environment["RUBYLIB"] = os.pathsep.join(
            [str(tools / "lib" / "ruby" / "3.4.0"), str(tools / "lib" / "ruby" / "3.4.0" / "x86_64-linux")]
        )
        environment["GEM_PATH"] = os.pathsep.join(
            [str(tools / "lib" / "ruby" / "gems" / "3.4.0"), "/usr/lib/ruby/gems/3.4.0"]
        )
    environment["WEBKIT_NINJA_LINK_MAX"] = "1"
    environment["WEBKIT_NINJA_COMPILE_MAX"] = str(build_jobs)
    affinity = ["taskset", "--cpu-list", arguments.cpus]
    compiler = "gcc" if arguments.mode == "baseline" else "clang"
    identity_values = {
        "version": version,
        "sourceSHA256": digest,
        "patches": list(enabled_names),
        "patchCatalogSHA256": file_digest(root / "scripts/wpe/performance_patches.py").hex(),
        "adoptedPatchSources": {
            path.name: file_digest(path).hex() for path in adopted_sources(adopted_patches)
        } if adopted_patches else {},
        "canvasReplayPatch": "canvas-replay" in enabled_names,
        "canvasReplaySources": {
            path.name: file_digest(path).hex()
            for path in [root / "scripts/wpe/canvas_replay.py", root / "scripts/wpe/EdenSkiaCanvasReplay.h"]
        } if "canvas-replay" in enabled_names else {},
        "hugePageAlignment": "huge-page-alignment" in enabled_names,
        "verifyPatches": arguments.verify_patches,
        "embeddedPatchGeneratorSHA256": embedded_generator_sha256(EMBEDDED_CORRECTNESS_MODULES),
        "preparedManagedSourceSHA256": prepared_managed_source_digest(
            source, adopted_patches, "canvas-replay" in enabled_names, arguments.webrtc_backend
        ),
        "compiler": subprocess.check_output([compiler, "--version"], env=environment, text=True).splitlines()[0],
    }
    training_identity = profile_identity(identity_values) if arguments.mode != "baseline" else None
    effective_profile = arguments.profile
    profile_paths = {}
    if arguments.profile:
        effective_profile, profile_paths = prepare_profile(
            arguments.profile, source, build, affinity, environment, training_identity
        )
    webrtc_configure = webrtc_configure_flags(arguments.webrtc_backend)
    configure = [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        "Ninja",
        "-DPORT=WPE",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        f'-DCMAKE_INSTALL_RPATH={prefix / "lib"};{tools / "lib"};{dependency / "usr" / "lib"}',
        f'-DCMAKE_PREFIX_PATH={tools};{dependency / "usr"}',
        *webrtc_configure,
        "-DCMAKE_C_COMPILER=/usr/bin/gcc",
        "-DCMAKE_CXX_COMPILER=/usr/bin/g++",
        "-DENABLE_DOCUMENTATION=OFF",
        "-DENABLE_INTROSPECTION=OFF",
        "-DENABLE_MINIBROWSER=OFF",
        "-DENABLE_API_TESTS=OFF",
        "-DENABLE_WPE_PLATFORM=ON",
        "-DENABLE_BUBBLEWRAP_SANDBOX=ON",
        "-DUSE_LD_GOLD=OFF",
        "-DUSE_LD_LLD=OFF",
        "-DCMAKE_AUTOGEN_PARALLEL=1",
        f"-DNUMBER_OF_PROCESSORS={arguments.jobs}",
        "-DCMAKE_JOB_POOL_COMPILE=",
        "-DCMAKE_JOB_POOL_LINK=",
        "-DCMAKE_JOB_POOLS=",
    ]
    if arguments.mode != "baseline":
        for compiler in ["clang", "clang++"]:
            if not shutil.which(compiler, path=environment["PATH"]):
                raise RuntimeError(f"The performance build requires {compiler}")
        if not shutil.which("ld.lld", path=environment["PATH"]):
            raise RuntimeError("The performance build requires the matching LLVM linker")
        flags = "-march=native -mtune=native -fno-semantic-interposition -fno-plt"
        if arguments.frame_pointers:
            flags += " -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"
        if arguments.verify_patches:
            flags += " -DWEBKIT_EDEN_VERIFY_PATCHES=1"
        linker_flags = f'-fuse-ld=lld -Wl,--threads=1,--thinlto-jobs={arguments.link_jobs},--thinlto-cache-dir={build / "lto-cache"}'
        linker_flags += " -Wl,--emit-relocs"
        if "huge-page-alignment" in enabled_names:
            linker_flags += " -Wl,-z,max-page-size=0x200000"
        configure += [
            "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_CXX_COMPILER=clang++",
            f"-DCMAKE_C_FLAGS={flags}",
            f"-DCMAKE_CXX_FLAGS={flags}",
            f"-DCMAKE_EXE_LINKER_FLAGS={linker_flags}",
            f"-DCMAKE_SHARED_LINKER_FLAGS={linker_flags}",
            f"-DCMAKE_MODULE_LINKER_FLAGS={linker_flags}",
            "-DLTO_MODE=thin",
            "-DUSE_LD_LLD=ON",
            "-DENABLE_ASSERTS=OFF",
            "-DUSE_CXX_STDLIB_ASSERTIONS=OFF",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DENABLE_WPE_LEGACY_API=OFF",
            "-DENABLE_JAVASCRIPT_SHELL=OFF",
            f'-DENABLE_LLVM_PROFILE_GENERATION={"ON" if arguments.mode == "profile-generate" else "OFF"}',
            f'-DUSE_PGO_PROFILE={"ON" if arguments.profile else "OFF"}',
            f'-DPGO_PROFILE_PATH={effective_profile.resolve() if effective_profile else ""}',
        ]
    if arguments.mode == "performance":
        profile_hash = file_digest(effective_profile).hex() if effective_profile else "none"
        profile_stamp = build / "eden-profile-sha256"
        previous_hash = profile_stamp.read_text().strip() if profile_stamp.exists() else None
        if previous_hash != profile_hash and (build / "build.ninja").exists():
            print("The PGO profile changed or is untracked; rebuilding compiled outputs", flush=True)
            subprocess.run(
                affinity + ["cmake", "--build", str(build), "--target", "clean"], env=environment, check=True
            )
        build.mkdir(parents=True, exist_ok=True)
        profile_stamp.write_text(profile_hash + "\n")
    subprocess.run(affinity + configure, env=environment, check=True)
    if arguments.configure_only:
        return
    subprocess.run(
        affinity + ["cmake", "--build", str(build), "--parallel", str(build_jobs)], env=environment, check=True
    )
    subprocess.run(affinity + ["cmake", "--install", str(build)], env=environment, check=True)
    stage_runtime_libraries(dependency, prefix)
    apply_inspector_theme(root, prefix)
    manifest = {
        "version": version,
        "sourceSHA256": digest,
        "mode": arguments.mode,
        "variant": arguments.variant,
        "webrtcBackend": arguments.webrtc_backend,
        "libwebrtcSHA256": libwebrtc_digest,
        "patches": identity_values["patches"],
        "defaultPatches": list(patch_names(DEFAULT_PATCHES)),
        "experimentalPatches": list(patch_names(EXPERIMENTAL_PATCHES)) if arguments.experiments else [],
        "experiments": arguments.experiments,
        "patchCatalogSHA256": identity_values["patchCatalogSHA256"],
        "canvasReplayPatch": identity_values["canvasReplayPatch"],
        "hugePageAlignment": identity_values["hugePageAlignment"],
        "embeddedPatchGeneratorSHA256": identity_values["embeddedPatchGeneratorSHA256"],
        "preparedManagedSourceSHA256": identity_values["preparedManagedSourceSHA256"],
        "profileTrainingIdentity": training_identity,
        "framePointers": arguments.frame_pointers,
        "renderingExperiments": False,
        "adoptedPatches": bool(adopted_patches),
        "verifyPatches": arguments.verify_patches,
        "boltPipeline": "share/eden-bolt/state.json",
        "adoptedPatchSources": identity_values["adoptedPatchSources"],
        "renderingExperimentSources": {},
        "canvasReplaySources": identity_values["canvasReplaySources"],
        "compiler": identity_values["compiler"],
        "profileSHA256": file_digest(arguments.profile).hex() if arguments.profile else None,
        "effectiveProfileSHA256": file_digest(effective_profile).hex() if effective_profile else None,
        "profilePaths": profile_paths,
        "configure": configure,
    }
    manifest_path = prefix / "share" / "eden-wpe-build.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    if arguments.bolt:
        run_bolt_stage(root, prefix, arguments.bolt, arguments.bolt_tool_dir,
                       affinity, arguments.jobs, arguments.bolt_plan)
    print(prefix)


if __name__ == "__main__":
    main()
