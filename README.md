# Eden Browser
#### Imagine, for a moment, a web browser crafted by someone who has *actually* used a web browser before

Project was initially written in python/qt5, then reimplemented in cpp, in 2017.

## Requirements

### Building today (Qt6 baseline)

- CMake ≥ 3.21, GCC 12+ or Clang 15+ (C++17)
- Qt ≥ 6.8 LTS (recommended: latest stable): Base, Declarative, WebEngine

### Full scope
#### *Some point in time that is not before now*

- Qt ≥ 6.8 modules: Core, Gui, Network, Sql, DBus, Qml/Quick/QuickControls2, WebEngine (temporary, removed when CEF becomes the default engine)
- GnuCOBOL ≥ 3.1 (`cobc`) for the Records Office batch programs
- libsodium for password vault encryption
- qtkeychain (Qt6) for OS keyring integration
- Rust toolchain for the adblock engine
- libpsl for public-suffix handling (per-site data scoping)
- md4c for AI pane markdown rendering
- NSS for Firefox password import
- Python ≥ 3.10 for the benchmark harness
- CEF is fetched automatically by CMake, no manual install

### Runtime (Linux)

- A Secret Service keyring (gnome-keyring / KWallet) for the password vault
- D-Bus desktop notifications (any compliant desktop)
- GeoClue2 (optional) as the geolocation backend

### Building

```
cmake -S . -B build
cmake --build build -j$(nproc)
./build/eden-browser
```


### Screenshots -- 2017

DevTools has been added.
![Alt text](screenshots/jan25.png?raw=true "Eden 0.1.3")

Private Windows
![Alt text](screenshots/private-window.png?raw=true "Eden 0.1.3")

I added support for some Google notifications.  A lot of improvements to be made on it.
![Alt text](screenshots/notification-shadow.png?raw=true "Eden 0.1.3")

Eden already has a partially working main menu.
![Alt text](screenshots/jan25-partial-menu.png?raw=true "Eden 0.1.3")
