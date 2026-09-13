<p align="center">
  <img src="resources/icons/logo/eden-logo-128.png" alt="Eden Browser Logo" width="128">
</p>

<h1 align="center">Eden Browser</h1>

<h4 align="center">Linux-first, performance-obsessed web browser</h4>

---

<br />
Project was initially written in python/qt5, then reimplemented in cpp, in 2017.

![Eden 0.3.0](screenshots/screen-0.3.8.png?raw=true "Eden 0.3.8")


## Features
- Linux-first
- Multi-engine, for real: pick the browser engine globally or per tab, and convert a live tab from one engine to another from its context menu. Ships with CEF (Blink) and QtWebEngine today, with a pluggable registry built for more.
- Extensive theme configurations
- Performance-obsessed: Explicit CEF GC on tab renderers, preventing resource bloat that exists inherently in CEF.
- Tab previews: accurate and realtime resource usage reporting
- Tabs your way: horizontal strip or vertical sidebar, pinning, drag a tab out into its own window, undo close, and session restore.
- Unified DevTools: one docked inspector pane for every engine, docked right or bottom, resizable, pops out into its own window, and remembers where you left it.
- Private windows.
- Bookmarks, history, and downloads panes built into the shell.
- Native desktop integration: your desktop's own file picker via XDG portals, Wayland-native tab tear-off.

## Roadmap at a high level:
- *v0.4.0*: Servo & Webkit engine integration
- *v0.5.0*: Site permissions & Notifications
- *v0.6.0*: Records Office - COBOL
- *v0.7.0*: Extensions (I honestly have no idea yet how I'ma do this, with multiple engines)
- *v0.8.0*: Performance hacks

<br /><br />

---


## Requirements

### Building today

- CMake ≥ 3.21, GCC 13+ or Clang 16+ (C++20)
- Qt ≥ 6.8: Core, Gui, Network, Qml, Quick, QuickControls2, QuickEffects, QuickShapes, Sql, Test
- Qt WebEngine ≥ 6.8 for the Blink (Qt) engine; on by default, skip with `-DEDEN_ENGINE_QTWEBENGINE=OFF`
- libpsl for recognizing complete domain suffixes in the address bar
- Network access on first configure: CMake fetches the pinned CEF distribution (~300 MB) and the Solar icon set into `third_party/`, checksum-verified, no manual steps
- Optional: Rust toolchain for the experimental Servo scaffold (`-DEDEN_ENGINE_SERVO=ON`, off by default), Python ≥ 3.10 for the benchmark harness in `scripts/bench/`

### Runtime (Linux)

- A Wayland or X11 desktop session; developed and tested on GNOME Wayland
- A GPU worth having

### Coming with the roadmap

- GnuCOBOL ≥ 3.1 (`cobc`) for the Records Office batch programs (v0.6.0)
- libsodium + qtkeychain for the password vault, NSS for Firefox password import
- Rust adblock engine, md4c for AI pane markdown
- Secret Service keyring, D-Bus notifications, GeoClue2 at runtime as those features land

### Building

```
cmake -S . -B build
cmake --build build -j$(nproc)
./build/eden-browser
```

<br /><br />

---


## License

Eden Browser is free software, licensed under the [GNU General Public License v3.0](LICENSE). It bundles open source components under their own licenses: Qt 6 (LGPLv3), Chromium and CEF (BSD 3-Clause), and the Solar Icon Set (CC BY 4.0). The full texts ship with the app and are viewable at eden://settings/about.

### Old Screenshots (2017)

Moved to [screenshots/](screenshots/)