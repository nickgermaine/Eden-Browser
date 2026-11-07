import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

SOCKET = "eden-share"
WINDOW_TITLE = "eden-share-probe"
PORTAL_BUS_NAME = "org.freedesktop.impl.portal.desktop.gnome"
PORTAL_OBJECT = "/org/freedesktop/portal/desktop"
WINDOW_LOGICAL_SIZE = (400, 300)
MONITOR_SIZE = (1600, 1000)
MUTTER_CURSOR_MODES = {1: 0, 2: 1, 4: 2}
POINTER_RATE = 125
POINTER_RADIUS = 150
WIREPLUMBER_CONFIG = """
wireplumber.profiles = {
    main = {
        monitor.alsa = disabled
        monitor.alsa-midi = disabled
        monitor.bluez = disabled
        monitor.bluez-midi = disabled
        monitor.v4l2 = disabled
        monitor.libcamera = disabled
    }
}
"""
PORTAL_INTERFACES = """
<node>
  <interface name="org.freedesktop.impl.portal.ScreenCast">
    <method name="CreateSession">
      <arg type="o" name="handle" direction="in"/>
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SelectSources">
      <arg type="o" name="handle" direction="in"/>
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="Start">
      <arg type="o" name="handle" direction="in"/>
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
    <property name="AvailableSourceTypes" type="u" access="read"/>
    <property name="AvailableCursorModes" type="u" access="read"/>
    <property name="version" type="u" access="read"/>
  </interface>
</node>
"""
SESSION_INTERFACE = """
<node>
  <interface name="org.freedesktop.impl.portal.Session">
    <method name="Close"/>
    <signal name="Closed"/>
    <property name="version" type="u" access="read"/>
  </interface>
</node>
"""


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait_for(check, message, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(message)


def portal_backend(arguments):
    import gi

    from gi.repository import Gio, GLib

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    owned = bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                          GLib.Variant("(su)", (PORTAL_BUS_NAME, 4)), GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
    require(owned == 1, "Another process owns the GNOME portal backend name")
    ready = Path(arguments.stage) / "backend.ready"
    log = (Path(arguments.stage) / "backend.log").open("w")
    sessions = {}
    screencast_info = Gio.DBusNodeInfo.new_for_xml(PORTAL_INTERFACES).interfaces[0]
    session_info = Gio.DBusNodeInfo.new_for_xml(SESSION_INTERFACE).interfaces[0]

    def call(name, path, interface, method, parameters=None, reply=None):
        return bus.call_sync(name, path, interface, method, parameters, GLib.VariantType(reply) if reply else None, Gio.DBusCallFlags.NONE, 15000, None)

    def mutter(path, interface, method, parameters=None, reply=None):
        return call("org.gnome.Mutter.ScreenCast", path, interface, method, parameters, reply)

    def probe_window():
        windows = call("org.gnome.Shell", "/org/gnome/Shell/Introspect", "org.gnome.Shell.Introspect", "GetWindows", None, "(a{ta{sv}})").unpack()[0]
        return next((window_id for window_id, properties in windows.items() if properties.get("title") == WINDOW_TITLE), None)

    def close_session(path):
        session = sessions.pop(path, None)
        if not session:
            return
        if session.get("mutter"):
            try:
                mutter(session["mutter"], "org.gnome.Mutter.ScreenCast.Session", "Stop")
            except GLib.Error:
                pass
        bus.unregister_object(session["registration"])

    def session_call(connection, sender, path, interface, method, parameters, invocation):
        if method == "Close":
            close_session(path)
            invocation.return_value(None)

    def session_property(connection, sender, path, interface, name):
        return GLib.Variant("u", 1)

    def start(session_path, invocation):
        session = sessions[session_path]
        mutter_session = mutter("/org/gnome/Mutter/ScreenCast", "org.gnome.Mutter.ScreenCast", "CreateSession", GLib.Variant("(a{sv})", ({},)), "(o)").unpack()[0]
        session["mutter"] = mutter_session
        properties = {"cursor-mode": GLib.Variant("u", MUTTER_CURSOR_MODES.get(session["cursor"], 0))}
        if session["types"] & 2:
            window_id = probe_window()
            require(window_id is not None, "The shared probe window is not mapped")
            properties["window-id"] = GLib.Variant("t", window_id)
            stream = mutter(mutter_session, "org.gnome.Mutter.ScreenCast.Session", "RecordWindow", GLib.Variant("(a{sv})", (properties,)), "(o)").unpack()[0]
            source_type = 2
        else:
            stream = mutter(mutter_session, "org.gnome.Mutter.ScreenCast.Session", "RecordMonitor", GLib.Variant("(sa{sv})", ("", properties)), "(o)").unpack()[0]
            source_type = 1

        def added(connection, sender, path, interface, signal, parameters):
            node = parameters.unpack()[0]
            bus.signal_unsubscribe(session["subscription"])
            stream_parameters = call("org.gnome.Mutter.ScreenCast", stream, "org.freedesktop.DBus.Properties", "Get",
                                     GLib.Variant("(ss)", ("org.gnome.Mutter.ScreenCast.Stream", "Parameters")), "(v)").unpack()[0]
            description = {"source_type": GLib.Variant("u", source_type), "id": GLib.Variant("s", "0")}
            if "size" in stream_parameters:
                description["size"] = GLib.Variant("(ii)", stream_parameters["size"])
            if "position" in stream_parameters:
                description["position"] = GLib.Variant("(ii)", stream_parameters["position"])
            print(json.dumps({"event": "started", "node": node, "sourceType": source_type, "parameters": {k: list(v) if isinstance(v, tuple) else v for k, v in stream_parameters.items()}}), file=log, flush=True)
            invocation.return_value(GLib.Variant("(ua{sv})", (0, {"streams": GLib.Variant("a(ua{sv})", [(node, description)])})))

        session["subscription"] = bus.signal_subscribe("org.gnome.Mutter.ScreenCast", "org.gnome.Mutter.ScreenCast.Stream", "PipeWireStreamAdded",
                                                       stream, None, Gio.DBusSignalFlags.NONE, added)
        mutter(mutter_session, "org.gnome.Mutter.ScreenCast.Session", "Start")

    def screencast_call(connection, sender, path, interface, method, parameters, invocation):
        try:
            if method == "CreateSession":
                handle, session_path, app_id, options = parameters.unpack()
                registration = bus.register_object(session_path, session_info, session_call, session_property, None)
                sessions[session_path] = {"registration": registration, "types": 1, "cursor": 1}
                print(json.dumps({"event": "session", "app": app_id}), file=log, flush=True)
                invocation.return_value(GLib.Variant("(ua{sv})", (0, {})))
            elif method == "SelectSources":
                handle, session_path, app_id, options = parameters.unpack()
                sessions[session_path]["types"] = options.get("types", 1)
                sessions[session_path]["cursor"] = options.get("cursor_mode", 1)
                print(json.dumps({"event": "select", "types": sessions[session_path]["types"], "cursorMode": sessions[session_path]["cursor"]}), file=log, flush=True)
                invocation.return_value(GLib.Variant("(ua{sv})", (0, {})))
            elif method == "Start":
                handle, session_path, app_id, parent_window, options = parameters.unpack()
                start(session_path, invocation)
        except Exception as error:
            print(json.dumps({"event": "error", "method": method, "error": str(error)}), file=log, flush=True)
            invocation.return_value(GLib.Variant("(ua{sv})", (2, {})))

    def screencast_property(connection, sender, path, interface, name):
        return GLib.Variant("u", {"AvailableSourceTypes": 3, "AvailableCursorModes": 7, "version": 5}[name])

    bus.register_object(PORTAL_OBJECT, screencast_info, screencast_call, screencast_property, None)
    ready.write_text("ready")
    GLib.MainLoop().run()


def probe_window(arguments):
    import gi

    gi.require_version("Gtk", "4.0")
    from gi.repository import GLib, Gtk

    control = Path(arguments.control)

    def draw(area, context, width, height, data):
        half_width, half_height = width / 2, height / 2
        for x, y, color in ((0, 0, (1, 0, 0)), (half_width, 0, (0, 1, 0)), (0, half_height, (0, 0, 1)), (half_width, half_height, (1, 1, 0))):
            context.set_source_rgb(*color)
            context.rectangle(x, y, half_width, half_height)
            context.fill()

    def activate(application):
        window = Gtk.ApplicationWindow(application=application, title=WINDOW_TITLE)
        window.set_decorated(False)
        window.set_default_size(*WINDOW_LOGICAL_SIZE)
        area = Gtk.DrawingArea()
        area.set_draw_func(draw, None)
        area.add_tick_callback(lambda widget, clock: (widget.queue_draw(), True)[1])
        window.set_child(area)
        window.present()

        def watch():
            if (control / "maximize").exists() and not window.is_maximized():
                window.maximize()
            return True

        GLib.timeout_add(100, watch)

    application = Gtk.Application(application_id="org.eden.ShareProbe")
    application.connect("activate", activate)
    application.run([sys.argv[0]])


def move_pointer(stop, scale, log):
    import gi

    from gi.repository import Gio, GLib

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(path, interface, method, parameters=None, reply=None):
        return bus.call_sync("org.gnome.Mutter.RemoteDesktop", path, interface, method, parameters,
                             GLib.VariantType(reply) if reply else None, Gio.DBusCallFlags.NONE, 5000, None)

    session = call("/org/gnome/Mutter/RemoteDesktop", "org.gnome.Mutter.RemoteDesktop", "CreateSession", None, "(o)").unpack()[0]
    interface = "org.gnome.Mutter.RemoteDesktop.Session"
    call(session, interface, "Start")
    center = (MONITOR_SIZE[0] / scale / 2, MONITOR_SIZE[1] / scale / 2)
    call(session, interface, "NotifyPointerMotionRelative", GLib.Variant("(dd)", (-10000.0, -10000.0)))
    call(session, interface, "NotifyPointerMotionRelative", GLib.Variant("(dd)", (center[0] + POINTER_RADIUS, center[1])))
    moves = 0
    angle = 0.0
    step = 2 * math.pi / POINTER_RATE
    started = time.monotonic()
    try:
        while not stop.is_set():
            dx = POINTER_RADIUS * (math.cos(angle + step) - math.cos(angle))
            dy = POINTER_RADIUS * (math.sin(angle + step) - math.sin(angle))
            call(session, interface, "NotifyPointerMotionRelative", GLib.Variant("(dd)", (dx, dy)))
            angle += step
            moves += 1
            time.sleep(1 / POINTER_RATE)
    finally:
        print(json.dumps({"moves": moves, "seconds": round(time.monotonic() - started, 2)}), file=log, flush=True)
        call(session, interface, "Stop")


def apply_scale(scale):
    from gi.repository import Gio, GLib

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    config = ("org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig", "org.gnome.Mutter.DisplayConfig")
    serial, monitors, logical, properties = bus.call_sync(*config, "GetCurrentState", None, None, Gio.DBusCallFlags.NONE, 10000, None).unpack()
    connector = monitors[0][0][0]
    mode = next(item[0] for item in monitors[0][1] if item[6].get("is-current"))
    bus.call_sync(*config, "ApplyMonitorsConfig",
                  GLib.Variant("(uua(iiduba(ssa{sv}))a{sv})", (serial, 1, [(0, 0, scale, 0, True, [(connector, mode, {})])], {})),
                  None, Gio.DBusCallFlags.NONE, 10000, None)
    state = bus.call_sync(*config, "GetCurrentState", None, None, Gio.DBusCallFlags.NONE, 10000, None).unpack()
    return state[2][0][2]


def name_has_owner(name):
    from gi.repository import Gio, GLib

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    return bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                         GLib.Variant("(s)", (name,)), None, Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]


def desktop_state():
    from gi.repository import Gio, GLib

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    state = {}
    try:
        overview = bus.call_sync("org.gnome.Shell", "/org/gnome/Shell", "org.freedesktop.DBus.Properties", "Get",
                                 GLib.Variant("(ss)", ("org.gnome.Shell", "OverviewActive")), None, Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
        state["overviewActive"] = overview
    except GLib.Error as error:
        state["overviewError"] = error.message
    return state


def hide_overview():
    from gi.repository import Gio, GLib

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    try:
        bus.call_sync("org.gnome.Shell", "/org/gnome/Shell", "org.freedesktop.DBus.Properties", "Set",
                      GLib.Variant("(ssv)", ("org.gnome.Shell", "OverviewActive", GLib.Variant("b", False))), None, Gio.DBusCallFlags.NONE, 5000, None)
        return None
    except GLib.Error as error:
        return error.message


def inside(arguments):
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    stage = Path(arguments.stage)
    control = stage / "control"
    control.mkdir()
    processes = []

    def spawn(name, command, environment=None):
        process = subprocess.Popen(command, env=environment, stdout=(stage / f"{name}.log").open("w"), stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    try:
        spawn("pipewire", ["pipewire"])
        wait_for(lambda: (runtime / "pipewire-0").exists(), "PipeWire did not start")
        spawn("wireplumber", ["wireplumber"])
        backend = spawn("backend-process", [sys.executable, __file__, "--portal-backend", "--stage", str(stage)])
        wait_for(lambda: (stage / "backend.ready").exists() or backend.poll() is not None, "The portal backend did not start")
        require(backend.poll() is None, "The portal backend exited: " + (stage / "backend-process.log").read_text())
        shell = spawn("shell", ["gnome-shell", "--headless", "--wayland", "--no-x11", "--virtual-monitor", "%dx%d" % MONITOR_SIZE, "--wayland-display", SOCKET])
        wait_for(lambda: (runtime / SOCKET).exists() or shell.poll() is not None, "gnome-shell did not start")
        require(shell.poll() is None, "gnome-shell exited during startup")
        wait_for(lambda: name_has_owner("org.gnome.Mutter.ScreenCast") and name_has_owner("org.gnome.Shell.Introspect"), "gnome-shell did not export its screen cast API")
        applied = apply_scale(arguments.scale)
        require(abs(applied - arguments.scale) < 0.01, f"The monitor scale is {applied} instead of {arguments.scale}")
        window_environment = dict(os.environ, WAYLAND_DISPLAY=SOCKET, GDK_BACKEND="wayland")
        spawn("window", [sys.executable, __file__, "--probe-window", "--control", str(control)], window_environment)
        test_environment = dict(os.environ, WAYLAND_DISPLAY=SOCKET, QT_QPA_PLATFORM="wayland", EDEN_WPE_PREWARM="0",
                                EDEN_TEST_DISPLAY_FRAMES=arguments.source, EDEN_TEST_DISPLAY_FRAMES_CONTROL=str(control),
                                EDEN_TEST_DISPLAY_FRAMES_SCALE=str(arguments.scale))
        if arguments.codec:
            test_environment["EDEN_TEST_DISPLAY_FRAMES_CODEC"] = arguments.codec
        if arguments.hint:
            test_environment["EDEN_TEST_DISPLAY_FRAMES_HINT"] = arguments.hint
        if arguments.pointer:
            test_environment["EDEN_TEST_DISPLAY_FRAMES_MAXIMIZE_FIRST"] = "1"
        command = [str(Path(arguments.build) / "eden-wpe-tests"), "-nocrashhandler", "displayCaptureShowsTheSharedSurface"]
        if arguments.webkit_library:
            command = ["bwrap", "--dev-bind", "/", "/", "--ro-bind", arguments.webkit_library, arguments.webkit_target, "--", *command]
        (stage / "desktop-before.json").write_text(json.dumps(desktop_state()))
        import threading
        stop = threading.Event()
        started = time.monotonic()

        def watch_overview():
            last = None
            with (stage / "overview.log").open("w") as log:
                while not stop.is_set():
                    active = desktop_state().get("overviewActive")
                    if active != last:
                        print(f"{time.monotonic() - started:.2f} {active}", file=log, flush=True)
                        last = active
                    if active:
                        failure = hide_overview()
                        if failure:
                            print(f"{time.monotonic() - started:.2f} hide failed: {failure}", file=log, flush=True)
                    time.sleep(0.1)

        watcher = threading.Thread(target=watch_overview, daemon=True)
        watcher.start()
        if arguments.pointer:
            pointer_log = (stage / "pointer.log").open("w")
            pointer = threading.Thread(target=move_pointer, args=(stop, arguments.scale, pointer_log), daemon=True)
            pointer.start()
        with (stage / "test.log").open("w") as log:
            result = subprocess.run(command, env=test_environment, stdout=log, stderr=subprocess.STDOUT, timeout=arguments.timeout_seconds)
        stop.set()
        (stage / "desktop-after.json").write_text(json.dumps(desktop_state()))
        return result.returncode
    finally:
        for process in reversed(processes):
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()


def quadrants_fill_frame(grid):
    expected = ["RRGG", "RRGG", "BBYY", "BBYY"]
    return grid == expected


MINIMUM_SHARED_FPS = 15


def validate(summary, source, scale, pointer=False):
    require(summary.get("testExit") == 0, f"The display capture test program exited with {summary.get('testExit')}")
    selections = [event for event in summary.get("backend", []) if event.get("event") == "select"]
    require(selections and all(event.get("cursorMode") == 2 for event in selections), f"WebKit did not ask the portal for an embedded cursor: {selections}")
    frames = summary.get("frames") or {}
    require(not frames.get("error"), f"getDisplayMedia failed at '{frames.get('stage')}': {frames.get('error')}")
    local = frames.get("local") or {}
    remote = frames.get("remote") or {}
    settings = local.get("settings") or {}
    for label, sample in (("local", local), ("remote", remote)):
        require(not sample.get("frameError"), f"Reading a {label} frame failed: {sample.get('frameError')}")
    if source == "window" and pointer:
        require(local.get("videoWidth") == MONITOR_SIZE[0] and 0 < (local.get("videoHeight") or 0) <= MONITOR_SIZE[1],
                f"The maximized window arrived at {local.get('videoWidth')}x{local.get('videoHeight')}")
        require([settings.get("width"), settings.get("height")] == [local.get("videoWidth"), local.get("videoHeight")],
                f"The track reported {settings.get('width')}x{settings.get('height')} for {local.get('videoWidth')}x{local.get('videoHeight')} frames")
        require(settings.get("displaySurface") == "window", f"The track reported the surface {settings.get('displaySurface')}")
        require(quadrants_fill_frame(local.get("grid")), f"The shared window did not fill the frame: {local.get('grid')}")
        require([remote.get("videoWidth"), remote.get("videoHeight")] == [local.get("videoWidth"), local.get("videoHeight")],
                f"The encoded window arrived at {remote.get('videoWidth')}x{remote.get('videoHeight')}")
        require(quadrants_fill_frame(remote.get("grid")), f"The encoded window did not fill the frame: {remote.get('grid')}")
    elif source == "window":
        size = [int(WINDOW_LOGICAL_SIZE[0] * scale), int(WINDOW_LOGICAL_SIZE[1] * scale)]
        require([local.get("videoWidth"), local.get("videoHeight")] == size, f"The shared window arrived at {local.get('videoWidth')}x{local.get('videoHeight')} instead of {size[0]}x{size[1]}")
        require([settings.get("width"), settings.get("height")] == size, f"The track reported {settings.get('width')}x{settings.get('height')} instead of {size[0]}x{size[1]}")
        require(settings.get("displaySurface") == "window", f"The track reported the surface {settings.get('displaySurface')}")
        require(quadrants_fill_frame(local.get("grid")), f"The shared window did not fill the frame: {local.get('grid')}")
        require([remote.get("videoWidth"), remote.get("videoHeight")] == size, f"The encoded window arrived at {remote.get('videoWidth')}x{remote.get('videoHeight')}")
        require(quadrants_fill_frame(remote.get("grid")), f"The encoded window did not fill the frame: {remote.get('grid')}")
        resized = frames.get("resized") or {}
        resized_settings = resized.get("settings") or {}
        require(resized.get("videoWidth") == MONITOR_SIZE[0] and 0 < (resized.get("videoHeight") or 0) <= MONITOR_SIZE[1],
                f"After maximizing, frames were {resized.get('videoWidth')}x{resized.get('videoHeight')}")
        require([resized_settings.get("width"), resized_settings.get("height")] == [resized.get("videoWidth"), resized.get("videoHeight")],
                f"After maximizing, the track reported {resized_settings.get('width')}x{resized_settings.get('height')} for {resized.get('videoWidth')}x{resized.get('videoHeight')} frames")
        require(quadrants_fill_frame(resized.get("grid")), f"The maximized window did not fill the frame: {resized.get('grid')}")
    else:
        require([local.get("videoWidth"), local.get("videoHeight")] == list(MONITOR_SIZE), f"The shared screen arrived at {local.get('videoWidth')}x{local.get('videoHeight')}")
        require([settings.get("width"), settings.get("height")] == list(MONITOR_SIZE), f"The track reported {settings.get('width')}x{settings.get('height')}")
        require(settings.get("displaySurface") == "monitor", f"The track reported the surface {settings.get('displaySurface')}")
        require(any(cell != "." for row in local.get("grid") or [] for cell in row), f"The shared screen has no recognizable content: {local.get('grid')}")
        require([remote.get("videoWidth"), remote.get("videoHeight")] == list(MONITOR_SIZE), f"The encoded screen arrived at {remote.get('videoWidth')}x{remote.get('videoHeight')}")
        require(local.get("grid") == remote.get("grid"), f"The encoded screen differs from the captured one: {local.get('grid')} against {remote.get('grid')}")
    require(local.get("canvasGrid") == local.get("grid"), f"Drawing the shared video into a canvas gave {local.get('canvasGrid')} instead of {local.get('grid')}")
    if summary.get("codec"):
        require((remote.get("codec") or "").lower() == "video/" + summary["codec"].lower(),
                f"The share asked for {summary['codec']} but was sent as {remote.get('codec')}")
    require((remote.get("encodedFps") or 0) >= MINIMUM_SHARED_FPS, f"The share encoded at {remote.get('encodedFps')} fps")
    if pointer:
        require((remote.get("decodedFps") or 0) >= MINIMUM_SHARED_FPS, f"The receiver decoded {remote.get('decodedFps')} fps")
    else:
        require((remote.get("presentedFps") or 0) >= MINIMUM_SHARED_FPS, f"The receiver presented {remote.get('presentedFps')} fps")


def run(build, source, scale, timeout_seconds, stage, webkit_library=None, webkit_target=None, pointer=False, codec=None, hint=None):
    require((build / "eden-wpe-tests").is_file(), f"Missing eden-wpe-tests in {build}")
    for tool in ("dbus-run-session", "gnome-shell", "pipewire", "wireplumber", "bwrap"):
        require(shutil.which(tool), f"The display capture frame check requires {tool}")
    runtime = Path(tempfile.mkdtemp(prefix="eden-share-", dir="/tmp"))
    try:
        runtime.chmod(0o700)
        configuration = runtime / "config"
        (configuration / "wireplumber/wireplumber.conf.d").mkdir(parents=True)
        (configuration / "wireplumber/wireplumber.conf.d/eden-test.conf").write_text(WIREPLUMBER_CONFIG)
        environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("DBUS_", "WAYLAND_", "JSC_", "WEBKIT_", "EDEN_WPE_", "GST_", "LLVM_PROFILE", "PIPEWIRE_", "PULSE_"))
            and key not in ("DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME")
        }
        environment.update({key: os.environ[key] for key in ("GST_DEBUG", "GST_DEBUG_NO_COLOR") if key in os.environ})
        environment.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_DATA_HOME": str(runtime / "data"),
            "XDG_CONFIG_HOME": str(configuration),
            "XDG_CACHE_HOME": str(runtime / "cache"),
            "XDG_STATE_HOME": str(runtime / "state"),
            "XDG_CURRENT_DESKTOP": "GNOME",
            "XDG_SESSION_TYPE": "wayland",
        })
        command = [
            "dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()), "--inside",
            "--build", str(build), "--source", source, "--scale", str(scale), "--stage", str(stage), "--timeout-seconds", str(timeout_seconds),
        ]
        if pointer:
            command.append("--pointer")
        if codec:
            command += ["--codec", codec]
        if hint:
            command += ["--hint", hint]
        if webkit_library:
            command += ["--webkit-library", str(webkit_library), "--webkit-target", str(webkit_target)]
        completed = subprocess.run(command, env=environment, timeout=timeout_seconds + 90, check=False)
        test_log = (stage / "test.log").read_text() if (stage / "test.log").exists() else ""
        outcome = re.search(r"Display capture frames: (.*)", test_log)
        summary = {
            "source": source,
            "scale": scale,
            "pointer": pointer,
            "codec": codec,
            "hint": hint,
            "testExit": completed.returncode,
            "frames": json.loads(outcome[1]) if outcome else None,
            "backend": [json.loads(line) for line in (stage / "backend.log").read_text().splitlines()] if (stage / "backend.log").exists() else [],
        }
        summary["passed"] = False
        try:
            validate(summary, source, scale, pointer)
            summary["passed"] = True
        finally:
            (stage / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        return summary
    finally:
        shutil.rmtree(runtime, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "cmake-build-release")
    parser.add_argument("--source", choices=["window", "screen"])
    parser.add_argument("--scale", type=float)
    parser.add_argument("--timeout-seconds", type=int, default=240)
    parser.add_argument("--output-directory", type=Path)
    parser.add_argument("--webkit-library")
    parser.add_argument("--webkit-target")
    parser.add_argument("--stage")
    parser.add_argument("--control")
    parser.add_argument("--inside", action="store_true")
    parser.add_argument("--portal-backend", action="store_true")
    parser.add_argument("--probe-window", action="store_true")
    parser.add_argument("--pointer", action="store_true", help="Maximize the shared window and keep the pointer moving over it")
    parser.add_argument("--codec", choices=["VP8", "VP9", "AV1", "H264"], help="Prefer this codec for the loopback call")
    parser.add_argument("--hint", choices=["motion", "detail", "text"], help="Set this contentHint on the shared track")
    arguments = parser.parse_args()
    if arguments.portal_backend:
        return portal_backend(arguments)
    if arguments.probe_window:
        return probe_window(arguments)
    if arguments.inside:
        return inside(arguments)
    cases = [(arguments.source, arguments.scale)] if arguments.source and arguments.scale else [("window", 1.0), ("window", 2.0), ("screen", 2.0)]
    failures = []
    for source, scale in cases:
        with tempfile.TemporaryDirectory(prefix="eden-share-check-") as temporary:
            stage = Path(temporary)
            if arguments.output_directory:
                stage = arguments.output_directory / f"{source}-{scale:g}"
                stage.mkdir(parents=True, exist_ok=True)
            try:
                summary = run(arguments.build.resolve(), source, scale, arguments.timeout_seconds, stage, arguments.webkit_library, arguments.webkit_target,
                              arguments.pointer, arguments.codec, arguments.hint)
                print(f"Display capture frames {source} at {scale:g}x: passed {json.dumps(summary['frames'])}")
            except (AssertionError, subprocess.TimeoutExpired) as error:
                failures.append(f"{source} at {scale:g}x: {error}")
                print(f"Display capture frames {source} at {scale:g}x: failed: {error}")
    if failures:
        raise SystemExit("Display capture frame check failed: " + "; ".join(failures))


if __name__ == "__main__":
    main()
