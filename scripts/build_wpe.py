import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

from wpe.canvas_replay import prepare_canvas_replay
from wpe.engine_experiments import prepare_engine_experiments
from wpe.pgo import prepare_profile
from wpe.jsc_experiments import prepare_jsc_experiments


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError("The pinned WebKit source does not match the embedding patch")
    return text.replace(before, after, 1)


def prepare_source(root, version, digest, variant=""):
    dependency = root / "third_party" / "wpe"
    archive = dependency / "packages" / f"wpewebkit-{version}.tar.xz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        urllib.request.urlretrieve(f"https://wpewebkit.org/releases/wpewebkit-{version}.tar.xz", temporary)
        temporary.rename(archive)
    if hashlib.file_digest(archive.open("rb"), "sha256").hexdigest() != digest:
        raise RuntimeError("The WebKit archive failed SHA-256 verification")
    source = dependency / "source" / (f"wpewebkit-{version}" + ("-" + variant if variant else ""))
    if not (source / "CMakeLists.txt").exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=source.parent) as staging:
            with tarfile.open(archive) as package:
                package.extractall(staging, filter="data")
            (Path(staging) / f"wpewebkit-{version}").rename(source)
    paths = [
        "Source/WebKit/UIProcess/API/glib/WebKitUIClient.cpp",
        "Source/WebKit/UIProcess/Inspector/wpe/WebInspectorUIProxyWPE.cpp",
        "Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp",
        "Source/WebKit/UIProcess/API/glib/WebKitUserMediaPermissionRequest.cpp",
        "Source/WebCore/platform/mediastream/gstreamer/GStreamerDisplayCaptureDeviceManager.cpp",
        "Source/WebKit/UIProcess/UserMediaPermissionRequestProxy.h",
    ]
    with tarfile.open(archive) as package:
        original = {name: package.extractfile(f"wpewebkit-{version}/{name}").read().decode() for name in paths}
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
        '#include "APINavigationAction.h"\n#include "APIContextMenuClient.h"\n#include "WebContextMenuProxy.h"\n#include "WebKitContextMenuPrivate.h"\n#include "WebKitHitTestResultPrivate.h"',
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
    for name, content in zip(paths, [client, inspector, view, media, capture, request]):
        destination = source / name
        if destination.read_text() != content:
            destination.write_text(content)
    return source


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
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--link-jobs", type=int)
    parser.add_argument("--cpus", default="0-7,16-23")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--runtime-only", action="store_true")
    parser.add_argument("--mode", choices=["baseline", "profile-generate", "performance"], default="performance")
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--variant", default="")
    parser.add_argument("--canvas-replay", action="store_true")
    parser.add_argument("--engine-experiments", action="store_true")
    parser.add_argument("--jsc-experiments", action="store_true")
    parser.add_argument("--huge-page-alignment", action="store_true")
    arguments = parser.parse_args()
    if arguments.variant and not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", arguments.variant):
        parser.error("--variant accepts lowercase letters, digits and hyphens")
    if arguments.canvas_replay and (not arguments.variant or arguments.mode == "baseline"):
        parser.error("--canvas-replay requires an isolated --variant and a performance source build")
    if arguments.engine_experiments and (not arguments.variant or arguments.mode == "baseline"):
        parser.error("--engine-experiments requires an isolated --variant and a performance source build")
    if arguments.jsc_experiments and (not arguments.variant or arguments.mode == "baseline"):
        parser.error("--jsc-experiments requires an isolated --variant and a performance source build")
    if arguments.huge_page_alignment and not arguments.engine_experiments:
        parser.error("--huge-page-alignment requires --engine-experiments")
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
    cores = {actual[cpu] for cpu in selected}
    if not 1 <= arguments.jobs <= min(len(actual) // 2, len(selected)) or len(cores) > len(set(actual.values())) // 2:
        raise RuntimeError("The requested CPU set or worker count exceeds the half-machine build budget")
    if arguments.link_jobs is None:
        arguments.link_jobs = min(4, max(1, arguments.jobs - 1))
    if arguments.mode != "baseline" and not 1 <= arguments.link_jobs < arguments.jobs:
        parser.error("--link-jobs must leave at least one other slot in the total --jobs budget")
    build_jobs = arguments.jobs if arguments.mode == "baseline" else arguments.jobs - arguments.link_jobs
    root = Path(__file__).resolve().parents[1]
    dependency = root / "third_party/wpe"
    suffix = {"baseline": "", "profile-generate": "-profile", "performance": "-performance"}[arguments.mode]
    variant_suffix = "-" + arguments.variant if arguments.variant else ""
    prefix = dependency / f"local{suffix}{variant_suffix}"
    if arguments.runtime_only:
        stage_runtime_libraries(dependency, prefix)
        return
    if arguments.profile and (arguments.mode != "performance" or not arguments.profile.is_file()):
        parser.error("--profile requires performance mode and an existing merged profile")
    versions = (root / "cmake" / "WpeVersion.cmake").read_text()
    version_key = "EDEN_WPE_MINIMUM_VERSION" if arguments.mode == "baseline" else "EDEN_WPE_PERFORMANCE_VERSION"
    digest_key = "EDEN_WPE_SOURCE_SHA256" if arguments.mode == "baseline" else "EDEN_WPE_PERFORMANCE_SHA256"
    version = re.search(version_key + r' "([^"]+)"', versions)[1]
    digest = re.search(digest_key + r' "([^"]+)"', versions)[1]
    build = dependency / (("build" if arguments.mode == "baseline" else f"build-{version}{suffix}") + variant_suffix)
    if arguments.mode == "performance" and not arguments.profile:
        profiles = [dependency / "profiles" / f"{version}{variant_suffix}.profdata"]
        if arguments.variant:
            profiles.append(dependency / "profiles" / f"{version}.profdata")
        for saved_profile in profiles:
            if saved_profile.is_file():
                arguments.profile = saved_profile
                break
    source = prepare_source(root, version, digest, arguments.variant)
    if arguments.variant and arguments.mode != "baseline":
        prepare_canvas_replay(root, source, version, arguments.canvas_replay)
        prepare_engine_experiments(root, source, version, arguments.engine_experiments)
        prepare_jsc_experiments(root, source, version, arguments.jsc_experiments)
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
    environment["RUBYLIB"] = os.pathsep.join(
        [str(tools / "lib" / "ruby" / "3.4.0"), str(tools / "lib" / "ruby" / "3.4.0" / "x86_64-linux")]
    )
    environment["WEBKIT_NINJA_LINK_MAX"] = "1"
    environment["WEBKIT_NINJA_COMPILE_MAX"] = str(build_jobs)
    environment["GEM_PATH"] = os.pathsep.join(
        [str(tools / "lib" / "ruby" / "gems" / "3.4.0"), "/usr/lib/ruby/gems/3.4.0"]
    )
    affinity = ["taskset", "--cpu-list", arguments.cpus]
    effective_profile = arguments.profile
    profile_paths = {}
    if arguments.profile:
        effective_profile, profile_paths = prepare_profile(arguments.profile, source, build, affinity, environment)
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
        "-DENABLE_WEB_RTC=ON",
        "-DUSE_GSTREAMER_WEBRTC=ON",
        "-DUSE_LIBWEBRTC=OFF",
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
        linker_flags = f'-fuse-ld=lld -Wl,--threads=1,--thinlto-jobs={arguments.link_jobs},--thinlto-cache-dir={build / "lto-cache"}'
        if arguments.huge_page_alignment:
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
    compiler = "gcc" if arguments.mode == "baseline" else "clang"
    manifest = {
        "version": version,
        "sourceSHA256": digest,
        "mode": arguments.mode,
        "variant": arguments.variant,
        "canvasReplayPatch": arguments.canvas_replay,
        "engineExperiments": arguments.engine_experiments,
        "jscExperiments": arguments.jsc_experiments,
        "jscExperimentSources": {"jsc_experiments.py": file_digest(root / "scripts/wpe/jsc_experiments.py").hex()} if arguments.jsc_experiments else {},
        "hugePageAlignment": arguments.huge_page_alignment,
        "engineExperimentSources": {path.name: file_digest(path).hex() for path in [root / "scripts/wpe/engine_experiments.py"]} if arguments.engine_experiments else {},
        "canvasReplaySources": {
            path.name: file_digest(path).hex()
            for path in [root / "scripts/wpe/canvas_replay.py", root / "scripts/wpe/EdenSkiaCanvasReplay.h"]
        } if arguments.canvas_replay else {},
        "compiler": subprocess.check_output([compiler, "--version"], env=environment, text=True).splitlines()[0],
        "profileSHA256": file_digest(arguments.profile).hex() if arguments.profile else None,
        "effectiveProfileSHA256": file_digest(effective_profile).hex() if effective_profile else None,
        "profilePaths": profile_paths,
        "configure": configure,
    }
    manifest_path = prefix / "share" / "eden-wpe-build.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(prefix)


if __name__ == "__main__":
    main()
