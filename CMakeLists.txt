cmake_minimum_required(VERSION 3.21)

project(eden-browser VERSION 0.3.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EDEN_ENGINE_CEF "Acquire and build the CEF engine dependency" ON)
option(EDEN_ENGINE_QTWEBENGINE "Build the QtWebEngine backend module" ON)
option(EDEN_ENGINE_SERVO "Acquire and build the Servo engine dependency" OFF)
option(EDEN_ENABLE_AUTOMATION "Build the local benchmark automation hook" OFF)
option(EDEN_SOLAR_ICONS "Acquire the Solar icon source set for design work" ON)
set(EDEN_CEF_LOCAL_ARCHIVE "" CACHE FILEPATH "Use a local CEF archive instead of the pinned download")

if(EDEN_ENGINE_CEF)
    include(cmake/CefVersion.cmake)
    include(cmake/EdenCef.cmake)
endif()

if(EDEN_ENGINE_SERVO)
    include(cmake/ServoVersion.cmake)
    include(cmake/EdenServo.cmake)
endif()

if(EDEN_SOLAR_ICONS)
    include(cmake/SolarIconsVersion.cmake)
    include(cmake/EdenSolarIcons.cmake)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 QuickEffects QuickShapes Sql Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(SQLCIPHER REQUIRED IMPORTED_TARGET sqlcipher)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)

if(EDEN_ENGINE_QTWEBENGINE)
    find_package(Qt6 6.8 REQUIRED COMPONENTS WebEngineCore WebEngineQuick)
endif()

qt_standard_project_setup(REQUIRES 6.8)

set(EDEN_CORE_SOURCES
    src/core/bookmarks/bookmarkstore.cpp
    src/core/bookmarks/bookmarkstore.h
    src/core/downloads/downloadmanager.cpp
    src/core/downloads/downloadmanager.h
    src/core/history/historystore.cpp
    src/core/history/historystore.h
    src/core/omni/omniboxcontroller.cpp
    src/core/omni/omniboxcontroller.h
    src/core/permissions/permissionstore.cpp
    src/core/permissions/permissionstore.h
    src/core/profiles/applicationcontext.cpp
    src/core/profiles/applicationcontext.h
    src/core/profiles/avatarimageprovider.cpp
    src/core/profiles/avatarimageprovider.h
    src/core/profiles/avatarprocessor.cpp
    src/core/profiles/avatarprocessor.h
    src/core/profiles/cookiehandoffcoordinator.cpp
    src/core/profiles/cookiehandoffcoordinator.h
    src/core/profiles/passwordhasher.cpp
    src/core/profiles/passwordhasher.h
    src/core/profiles/profilecolors.cpp
    src/core/profiles/profilecolors.h
    src/core/profiles/profilecontext.cpp
    src/core/profiles/profilecontext.h
    src/core/profiles/profiledatabase.cpp
    src/core/profiles/profiledatabase.h
    src/core/profiles/profileeditorcontroller.cpp
    src/core/profiles/profileeditorcontroller.h
    src/core/profiles/profileerror.cpp
    src/core/profiles/profileerror.h
    src/core/profiles/profileid.cpp
    src/core/profiles/profileid.h
    src/core/profiles/profilelistmodel.cpp
    src/core/profiles/profilelistmodel.h
    src/core/profiles/profilemanager.cpp
    src/core/profiles/profilemanager.h
    src/core/profiles/profilemigration.cpp
    src/core/profiles/profilemigration.h
    src/core/profiles/profilepaths.cpp
    src/core/profiles/profilepaths.h
    src/core/profiles/profileregistry.cpp
    src/core/profiles/profileregistry.h
    src/core/profiles/profilesettings.cpp
    src/core/profiles/profilesettings.h
    src/core/profiles/profiletypes.cpp
    src/core/profiles/profiletypes.h
    src/core/profiles/profilevalidation.cpp
    src/core/profiles/profilevalidation.h
    src/core/profiles/sessionstore.cpp
    src/core/profiles/sessionstore.h
    src/core/profiles/sqlcipherdatabase.cpp
    src/core/profiles/sqlcipherdatabase.h
    src/core/profiles/windowregistry.cpp
    src/core/profiles/windowregistry.h
    src/core/settings/settingsstore.cpp
    src/core/settings/settingsstore.h
    src/core/settings/shortcutregistry.cpp
    src/core/settings/shortcutregistry.h
    src/core/settings/theme/themeeditormodel.cpp
    src/core/settings/theme/themeeditormodel.h
    src/core/settings/theme/thememanager.cpp
    src/core/settings/theme/thememanager.h
    src/core/urlsanitizer.h
    src/core/window/tabmodel.cpp
    src/core/window/tabmodel.h
    src/core/window/thumbnailcache.cpp
    src/core/window/thumbnailcache.h
    src/core/window/tabstripnavigator.cpp
    src/core/window/tabstripnavigator.h
    src/core/window/windowcontroller.cpp
    src/core/window/windowcontroller.h
    src/core/window/windowframe.cpp
    src/core/window/windowframe.h
    src/passwords/credentialvault.cpp
    src/passwords/credentialvault.h
    src/engine/enginefactory.cpp
    src/engine/enginefactory.h
    src/engine/enginebackend.h
    src/engine/engineplugin.h
    src/engine/enginenewviewrequest.cpp
    src/engine/enginenewviewrequest.h
    src/engine/engineprofile.cpp
    src/engine/engineprofile.h
    src/engine/engineprofileparameters.h
    src/engine/portablecookie.cpp
    src/engine/portablecookie.h
    src/engine/engineprofilemap.cpp
    src/engine/engineprofilemap.h
    src/engine/engineregistry.cpp
    src/engine/engineregistry.h
    src/engine/engineview.cpp
    src/engine/engineview.h
)

if(EDEN_ENABLE_AUTOMATION)
    list(APPEND EDEN_CORE_SOURCES
        src/core/automation/automationserver.cpp
        src/core/automation/automationserver.h
        src/core/automation/performancemetrics.cpp
        src/core/automation/performancemetrics.h
    )
endif()

qt_add_library(eden-core SHARED ${EDEN_CORE_SOURCES})
target_compile_definitions(eden-core PUBLIC
    EDEN_ENABLE_AUTOMATION=$<BOOL:${EDEN_ENABLE_AUTOMATION}>
    EDEN_ENGINE_CEF=$<BOOL:${EDEN_ENGINE_CEF}>
    EDEN_ENGINE_QTWEBENGINE=$<BOOL:${EDEN_ENGINE_QTWEBENGINE}>
)
target_include_directories(eden-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eden-core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Sql
    PkgConfig::LIBSECRET
    PkgConfig::SODIUM
    PkgConfig::SQLCIPHER
)

if(EDEN_ENGINE_QTWEBENGINE)
    add_library(eden-engine-qtwebengine SHARED
        src/engine/qtwebengine/qtwebengineplugin.cpp
        src/engine/qtwebengine/qtwebengineprofile.cpp
        src/engine/qtwebengine/qtwebengineprofile.h
        src/engine/qtwebengine/qtwebengineview.cpp
        src/engine/qtwebengine/qtwebengineview.h
    )
    set_target_properties(eden-engine-qtwebengine PROPERTIES PREFIX "" OUTPUT_NAME "eden-engine-qtwebengine")
    target_include_directories(eden-engine-qtwebengine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(eden-engine-qtwebengine PRIVATE eden-core Qt6::WebEngineCore Qt6::WebEngineQuick)
endif()

if(EDEN_ENGINE_CEF)
    find_package(Qt6 6.8 REQUIRED COMPONENTS WebSockets)
    find_package(X11 REQUIRED)
    add_library(eden-cef-process STATIC
        src/engine/cef/cefprocesssettings.cpp
        src/engine/cef/cefprocesssettings.h
    )
    target_include_directories(eden-cef-process PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CEF_INCLUDE_PATH}
    )
    target_link_libraries(eden-cef-process PUBLIC libcef_dll_wrapper "${CEF_LIB_RELEASE}" ${CEF_STANDARD_LIBS})

    add_library(eden-engine-cef SHARED
        src/engine/cef/cefplugin.cpp
        src/engine/cef/cefapplication.cpp
        src/engine/cef/cefapplication.h
        src/engine/cef/cefbrowsersettings.cpp
        src/engine/cef/cefbrowsersettings.h
        src/engine/cef/cefengineview.cpp
        src/engine/cef/cefengineview.h
        src/engine/cef/cefprofile.cpp
        src/engine/cef/cefprofile.h
        src/engine/cef/cefruntime.cpp
        src/engine/cef/cefruntime.h
        src/engine/cef/cefuibridge.cpp
        src/engine/cef/cefuibridge.h
        src/engine/cef/devtoolssocketserver.cpp
        src/engine/cef/devtoolssocketserver.h
    )
    set_target_properties(eden-engine-cef PROPERTIES PREFIX "" OUTPUT_NAME "eden-engine-cef" BUILD_RPATH "$ORIGIN")
    target_include_directories(eden-engine-cef PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CEF_INCLUDE_PATH})
    target_link_libraries(eden-engine-cef PRIVATE eden-core eden-cef-process Qt6::WebSockets X11::X11)
endif()

set_source_files_properties(src/ui/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(eden-ui
    URI Eden.Ui
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Eden/Ui
    QML_FILES
        src/ui/Theme.qml
        src/ui/components/CommandPalette.qml
        src/ui/components/AnchoredSubmenu.qml
        src/ui/components/ChromeBackground.qml
        src/ui/components/ChromeCornerMask.qml
        src/ui/components/EdenButton.qml
        src/ui/components/EdenMenu.qml
        src/ui/components/EdenSlider.qml
        src/ui/components/EdenSwitch.qml
        src/ui/components/EdenTextField.qml
        src/ui/components/FindBar.qml
        src/ui/components/Icon.qml
        src/ui/components/Omnibox.qml
        src/ui/components/OverlaySurface.qml
        src/ui/components/ProfileAvatar.qml
        src/ui/components/ProfileMenu.qml
        src/ui/components/SecurityPopover.qml
        src/ui/components/TabIcon.qml
        src/ui/components/TabPreview.qml
        src/ui/components/TabSidebar.qml
        src/ui/components/TabStrip.qml
        src/ui/components/ThemePreviewCard.qml
        src/ui/panes/BookmarksPane.qml
        src/ui/panes/DownloadsPane.qml
        src/ui/panes/DevToolsPane.qml
        src/ui/panes/HistoryPane.qml
        src/ui/pages/SettingsPage.qml
        src/ui/pages/NewTabPage.qml
        src/ui/pages/ThemeEditorPage.qml
        src/ui/pages/settings/AboutSettings.qml
        src/ui/pages/settings/AppearanceSettings.qml
        src/ui/pages/settings/PlaceholderSettings.qml
        src/ui/pages/settings/SearchSettings.qml
        src/ui/pages/settings/EngineSettings.qml
        src/ui/pages/settings/PasswordSettings.qml
        src/ui/pages/settings/PersonalDataSettings.qml
        src/ui/pages/settings/PrivacySettings.qml
        src/ui/pages/settings/SavedPasswordsSettings.qml
        src/ui/windows/DevToolsWindow.qml
        src/ui/windows/DisplayCapturePrompt.qml
        src/ui/windows/BrowserWindow.qml
        src/ui/windows/FilePickerDialog.qml
        src/ui/windows/JavaScriptDialog.qml
        src/ui/windows/LaunchWindow.qml
        src/ui/windows/PermissionPrompt.qml
        src/ui/windows/ProfileEditorWindow.qml
        src/ui/windows/ProfileWindow.qml
    RESOURCES
        resources/fonts/RobotoFlex.ttf
        resources/icons/edenlogo.png
        resources/eden-logo.png
        resources/licenses/eden-gpl-3.0.txt
        resources/licenses/qt-lgpl-3.0.txt
        resources/licenses/solar-cc-by-4.0.txt
        resources/licenses/chromium-bsd-3-clause.txt
        resources/licenses/cef-bsd-3-clause.txt
        resources/icons/material/close.svg
        resources/icons/material/maximize.svg
        resources/icons/material/minimize.svg
        resources/icons/material/restore.svg
        resources/icons/solar/add-bold-duotone.svg
        resources/icons/solar/add-bold.svg
        resources/icons/solar/add-broken.svg
        resources/icons/solar/add-line-duotone.svg
        resources/icons/solar/add-linear.svg
        resources/icons/solar/add-outline.svg
        resources/icons/solar/arrow-left-bold-duotone.svg
        resources/icons/solar/arrow-left-bold.svg
        resources/icons/solar/arrow-left-broken.svg
        resources/icons/solar/arrow-left-line-duotone.svg
        resources/icons/solar/arrow-left-linear.svg
        resources/icons/solar/arrow-left-outline.svg
        resources/icons/solar/arrow-right-bold-duotone.svg
        resources/icons/solar/arrow-right-bold.svg
        resources/icons/solar/arrow-right-broken.svg
        resources/icons/solar/arrow-right-line-duotone.svg
        resources/icons/solar/arrow-right-linear.svg
        resources/icons/solar/arrow-right-outline.svg
        resources/icons/solar/check-bold-duotone.svg
        resources/icons/solar/check-bold.svg
        resources/icons/solar/check-broken.svg
        resources/icons/solar/check-line-duotone.svg
        resources/icons/solar/check-linear.svg
        resources/icons/solar/check-outline.svg
        resources/icons/solar/chevron-down-bold-duotone.svg
        resources/icons/solar/chevron-down-bold.svg
        resources/icons/solar/chevron-down-broken.svg
        resources/icons/solar/chevron-down-line-duotone.svg
        resources/icons/solar/chevron-down-linear.svg
        resources/icons/solar/chevron-down-outline.svg
        resources/icons/solar/chevron-up-bold-duotone.svg
        resources/icons/solar/chevron-up-bold.svg
        resources/icons/solar/chevron-up-broken.svg
        resources/icons/solar/chevron-up-line-duotone.svg
        resources/icons/solar/chevron-up-linear.svg
        resources/icons/solar/chevron-up-outline.svg
        resources/icons/solar/close-bold-duotone.svg
        resources/icons/solar/close-bold.svg
        resources/icons/solar/close-broken.svg
        resources/icons/solar/close-line-duotone.svg
        resources/icons/solar/close-linear.svg
        resources/icons/solar/close-outline.svg
        resources/icons/solar/code-bold-duotone.svg
        resources/icons/solar/code-bold.svg
        resources/icons/solar/code-broken.svg
        resources/icons/solar/code-line-duotone.svg
        resources/icons/solar/code-linear.svg
        resources/icons/solar/code-outline.svg
        resources/icons/solar/copy-bold-duotone.svg
        resources/icons/solar/copy-bold.svg
        resources/icons/solar/copy-broken.svg
        resources/icons/solar/copy-line-duotone.svg
        resources/icons/solar/copy-linear.svg
        resources/icons/solar/copy-outline.svg
        resources/icons/solar/danger-bold-duotone.svg
        resources/icons/solar/danger-bold.svg
        resources/icons/solar/danger-broken.svg
        resources/icons/solar/danger-line-duotone.svg
        resources/icons/solar/danger-linear.svg
        resources/icons/solar/danger-outline.svg
        resources/icons/solar/download-bold-duotone.svg
        resources/icons/solar/download-bold.svg
        resources/icons/solar/download-broken.svg
        resources/icons/solar/download-line-duotone.svg
        resources/icons/solar/download-linear.svg
        resources/icons/solar/download-outline.svg
        resources/icons/solar/folder-open-bold-duotone.svg
        resources/icons/solar/folder-open-bold.svg
        resources/icons/solar/folder-open-broken.svg
        resources/icons/solar/folder-open-line-duotone.svg
        resources/icons/solar/folder-open-linear.svg
        resources/icons/solar/folder-open-outline.svg
        resources/icons/solar/gallery-edit-bold-duotone.svg
        resources/icons/solar/gallery-edit-bold.svg
        resources/icons/solar/gallery-edit-broken.svg
        resources/icons/solar/gallery-edit-line-duotone.svg
        resources/icons/solar/gallery-edit-linear.svg
        resources/icons/solar/gallery-edit-outline.svg
        resources/icons/solar/globe-bold-duotone.svg
        resources/icons/solar/globe-bold.svg
        resources/icons/solar/globe-broken.svg
        resources/icons/solar/globe-line-duotone.svg
        resources/icons/solar/globe-linear.svg
        resources/icons/solar/globe-outline.svg
        resources/icons/solar/history-bold-duotone.svg
        resources/icons/solar/history-bold.svg
        resources/icons/solar/history-broken.svg
        resources/icons/solar/history-line-duotone.svg
        resources/icons/solar/history-linear.svg
        resources/icons/solar/history-outline.svg
        resources/icons/solar/incognito-bold-duotone.svg
        resources/icons/solar/incognito-bold.svg
        resources/icons/solar/incognito-broken.svg
        resources/icons/solar/incognito-line-duotone.svg
        resources/icons/solar/incognito-linear.svg
        resources/icons/solar/incognito-outline.svg
        resources/icons/solar/lock-bold-duotone.svg
        resources/icons/solar/lock-bold.svg
        resources/icons/solar/lock-broken.svg
        resources/icons/solar/lock-line-duotone.svg
        resources/icons/solar/lock-linear.svg
        resources/icons/solar/lock-outline.svg
        resources/icons/solar/logout-bold-duotone.svg
        resources/icons/solar/logout-bold.svg
        resources/icons/solar/logout-broken.svg
        resources/icons/solar/logout-line-duotone.svg
        resources/icons/solar/logout-linear.svg
        resources/icons/solar/logout-outline.svg
        resources/icons/solar/maximize-bold-duotone.svg
        resources/icons/solar/maximize-bold.svg
        resources/icons/solar/maximize-broken.svg
        resources/icons/solar/maximize-line-duotone.svg
        resources/icons/solar/maximize-linear.svg
        resources/icons/solar/maximize-outline.svg
        resources/icons/solar/menu-dots-bold-duotone.svg
        resources/icons/solar/menu-dots-bold.svg
        resources/icons/solar/menu-dots-broken.svg
        resources/icons/solar/menu-dots-line-duotone.svg
        resources/icons/solar/menu-dots-linear.svg
        resources/icons/solar/menu-dots-outline.svg
        resources/icons/solar/minimize-bold-duotone.svg
        resources/icons/solar/minimize-bold.svg
        resources/icons/solar/minimize-broken.svg
        resources/icons/solar/minimize-line-duotone.svg
        resources/icons/solar/minimize-linear.svg
        resources/icons/solar/minimize-outline.svg
        resources/icons/solar/monitor-bold-duotone.svg
        resources/icons/solar/monitor-bold.svg
        resources/icons/solar/monitor-broken.svg
        resources/icons/solar/monitor-line-duotone.svg
        resources/icons/solar/monitor-linear.svg
        resources/icons/solar/monitor-outline.svg
        resources/icons/solar/muted-bold-duotone.svg
        resources/icons/solar/muted-bold.svg
        resources/icons/solar/muted-broken.svg
        resources/icons/solar/muted-line-duotone.svg
        resources/icons/solar/muted-linear.svg
        resources/icons/solar/muted-outline.svg
        resources/icons/solar/new-window-bold-duotone.svg
        resources/icons/solar/new-window-bold.svg
        resources/icons/solar/new-window-broken.svg
        resources/icons/solar/new-window-line-duotone.svg
        resources/icons/solar/new-window-linear.svg
        resources/icons/solar/new-window-outline.svg
        resources/icons/solar/pin-bold-duotone.svg
        resources/icons/solar/pin-bold.svg
        resources/icons/solar/pin-broken.svg
        resources/icons/solar/pin-line-duotone.svg
        resources/icons/solar/pin-linear.svg
        resources/icons/solar/pin-outline.svg
        resources/icons/solar/refresh-bold-duotone.svg
        resources/icons/solar/refresh-bold.svg
        resources/icons/solar/refresh-broken.svg
        resources/icons/solar/refresh-line-duotone.svg
        resources/icons/solar/refresh-linear.svg
        resources/icons/solar/refresh-outline.svg
        resources/icons/solar/restore-bold-duotone.svg
        resources/icons/solar/restore-bold.svg
        resources/icons/solar/restore-broken.svg
        resources/icons/solar/restore-line-duotone.svg
        resources/icons/solar/restore-linear.svg
        resources/icons/solar/restore-outline.svg
        resources/icons/solar/search-bold-duotone.svg
        resources/icons/solar/search-bold.svg
        resources/icons/solar/search-broken.svg
        resources/icons/solar/search-line-duotone.svg
        resources/icons/solar/search-linear.svg
        resources/icons/solar/search-outline.svg
        resources/icons/solar/settings-bold-duotone.svg
        resources/icons/solar/settings-bold.svg
        resources/icons/solar/settings-broken.svg
        resources/icons/solar/settings-line-duotone.svg
        resources/icons/solar/settings-linear.svg
        resources/icons/solar/settings-outline.svg
        resources/icons/solar/sidebar-bold-duotone.svg
        resources/icons/solar/sidebar-bold.svg
        resources/icons/solar/sidebar-broken.svg
        resources/icons/solar/sidebar-line-duotone.svg
        resources/icons/solar/sidebar-linear.svg
        resources/icons/solar/sidebar-outline.svg
        resources/icons/solar/star-bold-duotone.svg
        resources/icons/solar/star-bold.svg
        resources/icons/solar/star-broken.svg
        resources/icons/solar/star-line-duotone.svg
        resources/icons/solar/star-linear.svg
        resources/icons/solar/star-outline.svg
        resources/icons/solar/trash-bold-duotone.svg
        resources/icons/solar/trash-bold.svg
        resources/icons/solar/trash-broken.svg
        resources/icons/solar/trash-line-duotone.svg
        resources/icons/solar/trash-linear.svg
        resources/icons/solar/trash-outline.svg
        resources/icons/solar/tuning-bold-duotone.svg
        resources/icons/solar/tuning-bold.svg
        resources/icons/solar/tuning-broken.svg
        resources/icons/solar/tuning-line-duotone.svg
        resources/icons/solar/tuning-linear.svg
        resources/icons/solar/tuning-outline.svg
        resources/icons/solar/user-circle-bold-duotone.svg
        resources/icons/solar/user-circle-bold.svg
        resources/icons/solar/user-circle-broken.svg
        resources/icons/solar/user-circle-line-duotone.svg
        resources/icons/solar/user-circle-linear.svg
        resources/icons/solar/user-circle-outline.svg
        resources/icons/solar/volume-bold-duotone.svg
        resources/icons/solar/volume-bold.svg
        resources/icons/solar/volume-broken.svg
        resources/icons/solar/volume-line-duotone.svg
        resources/icons/solar/volume-linear.svg
        resources/icons/solar/volume-outline.svg
        resources/icons/solar/window-bold-duotone.svg
        resources/icons/solar/window-bold.svg
        resources/icons/solar/window-broken.svg
        resources/icons/solar/window-line-duotone.svg
        resources/icons/solar/window-linear.svg
        resources/icons/solar/window-outline.svg
        resources/icons/solar/bookmark-circle-bold-duotone.svg
        resources/icons/solar/bookmark-circle-bold.svg
        resources/icons/solar/bookmark-circle-broken.svg
        resources/icons/solar/bookmark-circle-line-duotone.svg
        resources/icons/solar/bookmark-circle-linear.svg
        resources/icons/solar/bookmark-circle-outline.svg
        resources/icons/solar/cpu-bolt-bold-duotone.svg
        resources/icons/solar/cpu-bolt-bold.svg
        resources/icons/solar/cpu-bolt-broken.svg
        resources/icons/solar/cpu-bolt-line-duotone.svg
        resources/icons/solar/cpu-bolt-linear.svg
        resources/icons/solar/cpu-bolt-outline.svg
        resources/icons/solar/info-circle-bold-duotone.svg
        resources/icons/solar/info-circle-bold.svg
        resources/icons/solar/info-circle-broken.svg
        resources/icons/solar/info-circle-line-duotone.svg
        resources/icons/solar/info-circle-linear.svg
        resources/icons/solar/info-circle-outline.svg
        resources/icons/solar/password-bold-duotone.svg
        resources/icons/solar/password-bold.svg
        resources/icons/solar/password-broken.svg
        resources/icons/solar/password-line-duotone.svg
        resources/icons/solar/password-linear.svg
        resources/icons/solar/password-outline.svg
        resources/icons/solar/eye-bold-duotone.svg
        resources/icons/solar/eye-bold.svg
        resources/icons/solar/eye-broken.svg
        resources/icons/solar/eye-line-duotone.svg
        resources/icons/solar/eye-linear.svg
        resources/icons/solar/eye-outline.svg
        resources/icons/solar/power-bold-duotone.svg
        resources/icons/solar/power-bold.svg
        resources/icons/solar/power-broken.svg
        resources/icons/solar/power-line-duotone.svg
        resources/icons/solar/power-linear.svg
        resources/icons/solar/power-outline.svg
        resources/icons/solar/programming-bold-duotone.svg
        resources/icons/solar/programming-bold.svg
        resources/icons/solar/programming-broken.svg
        resources/icons/solar/programming-line-duotone.svg
        resources/icons/solar/programming-linear.svg
        resources/icons/solar/programming-outline.svg
        resources/icons/solar/round-transfer-vertical-bold-duotone.svg
        resources/icons/solar/round-transfer-vertical-bold.svg
        resources/icons/solar/round-transfer-vertical-broken.svg
        resources/icons/solar/round-transfer-vertical-line-duotone.svg
        resources/icons/solar/round-transfer-vertical-linear.svg
        resources/icons/solar/round-transfer-vertical-outline.svg
        resources/icons/solar/soundwave-bold-duotone.svg
        resources/icons/solar/soundwave-bold.svg
        resources/icons/solar/soundwave-broken.svg
        resources/icons/solar/soundwave-line-duotone.svg
        resources/icons/solar/soundwave-linear.svg
        resources/icons/solar/soundwave-outline.svg
        resources/icons/solar/undo-bold-duotone.svg
        resources/icons/solar/undo-bold.svg
        resources/icons/solar/undo-broken.svg
        resources/icons/solar/undo-line-duotone.svg
        resources/icons/solar/undo-linear.svg
        resources/icons/solar/undo-outline.svg
        resources/icons/solar/redo-bold-duotone.svg
        resources/icons/solar/redo-bold.svg
        resources/icons/solar/redo-broken.svg
        resources/icons/solar/redo-line-duotone.svg
        resources/icons/solar/redo-linear.svg
        resources/icons/solar/redo-outline.svg
        resources/icons/solar/cut-bold-duotone.svg
        resources/icons/solar/cut-bold.svg
        resources/icons/solar/cut-broken.svg
        resources/icons/solar/cut-line-duotone.svg
        resources/icons/solar/cut-linear.svg
        resources/icons/solar/cut-outline.svg
        resources/icons/solar/clipboard-bold-duotone.svg
        resources/icons/solar/clipboard-bold.svg
        resources/icons/solar/clipboard-broken.svg
        resources/icons/solar/clipboard-line-duotone.svg
        resources/icons/solar/clipboard-linear.svg
        resources/icons/solar/clipboard-outline.svg
        resources/icons/solar/clipboard-text-bold-duotone.svg
        resources/icons/solar/clipboard-text-bold.svg
        resources/icons/solar/clipboard-text-broken.svg
        resources/icons/solar/clipboard-text-line-duotone.svg
        resources/icons/solar/clipboard-text-linear.svg
        resources/icons/solar/clipboard-text-outline.svg
        resources/icons/solar/text-selection-bold-duotone.svg
        resources/icons/solar/text-selection-bold.svg
        resources/icons/solar/text-selection-broken.svg
        resources/icons/solar/text-selection-line-duotone.svg
        resources/icons/solar/text-selection-linear.svg
        resources/icons/solar/text-selection-outline.svg
        resources/icons/solar/camera-bold-duotone.svg
        resources/icons/solar/camera-bold.svg
        resources/icons/solar/camera-broken.svg
        resources/icons/solar/camera-line-duotone.svg
        resources/icons/solar/camera-linear.svg
        resources/icons/solar/camera-outline.svg
        resources/icons/solar/text-focus-bold-duotone.svg
        resources/icons/solar/text-focus-bold.svg
        resources/icons/solar/text-focus-broken.svg
        resources/icons/solar/text-focus-line-duotone.svg
        resources/icons/solar/text-focus-linear.svg
        resources/icons/solar/text-focus-outline.svg
        resources/icons/solar/sort-bold-duotone.svg
        resources/icons/solar/sort-bold.svg
        resources/icons/solar/sort-broken.svg
        resources/icons/solar/sort-line-duotone.svg
        resources/icons/solar/sort-linear.svg
        resources/icons/solar/sort-outline.svg
        resources/icons/solar/object-scan-bold-duotone.svg
        resources/icons/solar/object-scan-bold.svg
        resources/icons/solar/object-scan-broken.svg
        resources/icons/solar/object-scan-line-duotone.svg
        resources/icons/solar/object-scan-linear.svg
        resources/icons/solar/object-scan-outline.svg
        resources/icons/solar/expand-bold-duotone.svg
        resources/icons/solar/expand-bold.svg
        resources/icons/solar/expand-broken.svg
        resources/icons/solar/expand-line-duotone.svg
        resources/icons/solar/expand-linear.svg
        resources/icons/solar/expand-outline.svg
        resources/icons/solar/collapse-bold-duotone.svg
        resources/icons/solar/collapse-bold.svg
        resources/icons/solar/collapse-broken.svg
        resources/icons/solar/collapse-line-duotone.svg
        resources/icons/solar/collapse-linear.svg
        resources/icons/solar/collapse-outline.svg
        resources/icons/solar/document-text-bold-duotone.svg
        resources/icons/solar/document-text-bold.svg
        resources/icons/solar/document-text-broken.svg
        resources/icons/solar/document-text-line-duotone.svg
        resources/icons/solar/document-text-linear.svg
        resources/icons/solar/document-text-outline.svg
        resources/icons/solar/filter-bold-duotone.svg
        resources/icons/solar/filter-bold.svg
        resources/icons/solar/filter-broken.svg
        resources/icons/solar/filter-line-duotone.svg
        resources/icons/solar/filter-linear.svg
        resources/icons/solar/filter-outline.svg
        resources/icons/solar/link-bold-duotone.svg
        resources/icons/solar/link-bold.svg
        resources/icons/solar/link-broken.svg
        resources/icons/solar/link-line-duotone.svg
        resources/icons/solar/link-linear.svg
        resources/icons/solar/link-outline.svg
        resources/icons/solar/list-bold-duotone.svg
        resources/icons/solar/list-bold.svg
        resources/icons/solar/list-broken.svg
        resources/icons/solar/list-line-duotone.svg
        resources/icons/solar/list-linear.svg
        resources/icons/solar/list-outline.svg
        resources/icons/solar/square-top-down-bold-duotone.svg
        resources/icons/solar/square-top-down-bold.svg
        resources/icons/solar/square-top-down-broken.svg
        resources/icons/solar/square-top-down-line-duotone.svg
        resources/icons/solar/square-top-down-linear.svg
        resources/icons/solar/square-top-down-outline.svg
        resources/themes/eden-default.json
)
target_link_libraries(eden-ui PUBLIC eden-core Qt6::Quick Qt6::QuickControls2 Qt6::QuickEffects Qt6::QuickShapes)

qt_add_executable(eden-browser
    src/main.cpp
    src/platform/waylandglibeventdispatcher.cpp
    src/platform/waylandglibeventdispatcher.h
)
target_compile_definitions(eden-browser PRIVATE EDEN_VERSION="${PROJECT_VERSION}")
target_link_libraries(eden-browser PRIVATE eden-core eden-ui Qt6::QuickControls2 PkgConfig::GLIB2)
if(EDEN_ENGINE_CEF)
    add_executable(eden-helper src/engine/cef/helpermain.cpp src/engine/cef/cefrendererapp.cpp src/engine/cef/cefrendererapp.h)
    target_include_directories(eden-helper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CEF_INCLUDE_PATH})
    target_link_libraries(eden-helper PRIVATE libcef_dll_wrapper "${CEF_LIB_RELEASE}" ${CEF_STANDARD_LIBS})
    SET_EXECUTABLE_TARGET_PROPERTIES(eden-helper)
    set_target_properties(eden-helper PROPERTIES BUILD_RPATH "$ORIGIN")

    add_dependencies(eden-browser libcef_dll_wrapper)
    add_dependencies(eden-browser eden-helper)
    add_dependencies(eden-browser eden-engine-cef)
    SET_EXECUTABLE_TARGET_PROPERTIES(eden-browser)
    set_target_properties(eden-browser PROPERTIES BUILD_RPATH "$ORIGIN")
    eden_stage_cef_runtime(eden-browser)
endif()
if(EDEN_ENGINE_QTWEBENGINE)
    add_dependencies(eden-browser eden-engine-qtwebengine)
endif()

enable_testing()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME eden-bench-tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench/bench_test.py)

qt_add_executable(eden-tests tests/tabmodel_test.cpp)
target_link_libraries(eden-tests PRIVATE eden-core Qt6::Test)
if(EDEN_ENGINE_QTWEBENGINE)
    target_link_libraries(eden-tests PRIVATE eden-engine-qtwebengine Qt6::WebEngineCore Qt6::WebEngineQuick)
endif()
add_test(NAME eden-tests COMMAND eden-tests)
set_tests_properties(eden-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox")

if(EDEN_ENGINE_QTWEBENGINE)
    qt_add_executable(eden-qtwebengine-user-agent-tests tests/qtwebengineuseragent_test.cpp)
    target_link_libraries(eden-qtwebengine-user-agent-tests PRIVATE eden-core eden-engine-qtwebengine Qt6::Test Qt6::WebEngineQuick)
    add_test(NAME eden-qtwebengine-user-agent-tests COMMAND eden-qtwebengine-user-agent-tests)
    set_tests_properties(eden-qtwebengine-user-agent-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox")
endif()

qt_add_executable(eden-profile-core-tests tests/profilecore_test.cpp)
target_link_libraries(eden-profile-core-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-profile-core-tests COMMAND eden-profile-core-tests)
set_tests_properties(eden-profile-core-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(eden-download-manager-tests tests/downloadmanager_test.cpp)
target_link_libraries(eden-download-manager-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-download-manager-tests COMMAND eden-download-manager-tests)

qt_add_executable(eden-profile-registry-tests tests/profileregistry_test.cpp)
target_link_libraries(eden-profile-registry-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-profile-registry-tests COMMAND eden-profile-registry-tests)
set_tests_properties(eden-profile-registry-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(eden-permission-store-tests tests/permissionstore_test.cpp)
target_link_libraries(eden-permission-store-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-permission-store-tests COMMAND eden-permission-store-tests)
set_tests_properties(eden-permission-store-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(eden-profile-context-tests tests/profilecontext_test.cpp)
target_link_libraries(eden-profile-context-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-profile-context-tests COMMAND eden-profile-context-tests)
set_tests_properties(eden-profile-context-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(eden-password-hasher-tests tests/passwordhasher_test.cpp)
target_link_libraries(eden-password-hasher-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-password-hasher-tests COMMAND eden-password-hasher-tests)
set_tests_properties(eden-password-hasher-tests PROPERTIES TIMEOUT 300 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(eden-cookie-handoff-tests tests/cookiehandoff_test.cpp)
target_link_libraries(eden-cookie-handoff-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-cookie-handoff-tests COMMAND eden-cookie-handoff-tests)
set_tests_properties(eden-cookie-handoff-tests PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(eden-thumbnail-cache-tests tests/thumbnailcache_test.cpp)
target_link_libraries(eden-thumbnail-cache-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-thumbnail-cache-tests COMMAND eden-thumbnail-cache-tests)

qt_add_executable(eden-theme-tests tests/thememanager_test.cpp)
target_link_libraries(eden-theme-tests PRIVATE eden-core eden-ui Qt6::Test)
add_test(NAME eden-theme-tests COMMAND eden-theme-tests)
set_tests_properties(eden-theme-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

qt_add_executable(eden-window-controller-tests tests/windowcontroller_test.cpp)
target_link_libraries(eden-window-controller-tests PRIVATE eden-core Qt6::Test)
target_link_options(eden-window-controller-tests PRIVATE -Wl,--export-dynamic)
add_test(NAME eden-window-controller-tests COMMAND eden-window-controller-tests)
set_tests_properties(eden-window-controller-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

qt_add_executable(eden-window-frame-tests tests/windowframe_test.cpp)
target_link_libraries(eden-window-frame-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-window-frame-tests COMMAND eden-window-frame-tests)
set_tests_properties(eden-window-frame-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(eden-wayland-glib-event-dispatcher-tests
    tests/waylandglibeventdispatcher_test.cpp
    src/platform/waylandglibeventdispatcher.cpp
    src/platform/waylandglibeventdispatcher.h
)
target_include_directories(eden-wayland-glib-event-dispatcher-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eden-wayland-glib-event-dispatcher-tests PRIVATE Qt6::Core Qt6::Test PkgConfig::GLIB2)
add_test(NAME eden-wayland-glib-event-dispatcher-tests COMMAND eden-wayland-glib-event-dispatcher-tests)

qt_add_executable(eden-omnibox-tests tests/omniboxcontroller_test.cpp)
target_link_libraries(eden-omnibox-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-omnibox-tests COMMAND eden-omnibox-tests)
set_tests_properties(eden-omnibox-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/omnibox-test-data;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/omnibox-test-config")

if(EDEN_ENABLE_AUTOMATION)
    qt_add_executable(eden-automation-tests tests/automationserver_test.cpp)
    target_link_libraries(eden-automation-tests PRIVATE eden-core Qt6::Test)
    if(EDEN_ENGINE_QTWEBENGINE)
        add_dependencies(eden-automation-tests eden-engine-qtwebengine)
    endif()
    add_test(NAME eden-automation-tests COMMAND eden-automation-tests)
    set_tests_properties(eden-automation-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox")
endif()

if(EDEN_ENGINE_CEF)
    find_program(EDEN_NODE_EXECUTABLE NAMES node nodejs)
    if(EDEN_NODE_EXECUTABLE)
        add_test(NAME eden-cef-renderer-hook-tests COMMAND ${EDEN_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/cefrendererhooks_test.cjs)
    endif()
    qt_add_executable(eden-cef-process-tests tests/cefprocesssettings_test.cpp)
    target_link_libraries(eden-cef-process-tests PRIVATE eden-cef-process Qt6::Test)
    add_test(NAME eden-cef-process-tests COMMAND eden-cef-process-tests)
    set_tests_properties(eden-cef-process-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(eden-cef-bootstrap-tests tests/cefbootstrap_test.cpp)
    target_include_directories(eden-cef-bootstrap-tests PRIVATE ${CEF_INCLUDE_PATH})
    target_link_libraries(eden-cef-bootstrap-tests PRIVATE eden-core eden-engine-cef eden-cef-process Qt6::Test)
    SET_EXECUTABLE_TARGET_PROPERTIES(eden-cef-bootstrap-tests)
    set_target_properties(eden-cef-bootstrap-tests PROPERTIES BUILD_RPATH "$ORIGIN")
    add_dependencies(eden-cef-bootstrap-tests eden-helper)
    eden_stage_cef_runtime(eden-cef-bootstrap-tests)
    add_test(NAME eden-cef-bootstrap-tests COMMAND eden-cef-bootstrap-tests)
    set_tests_properties(eden-cef-bootstrap-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(eden-cef-handlers-tests tests/cefhandlers_test.cpp)
    target_include_directories(eden-cef-handlers-tests PRIVATE ${CEF_INCLUDE_PATH})
    target_link_libraries(eden-cef-handlers-tests PRIVATE eden-core eden-ui eden-engine-cef eden-cef-process Qt6::Test)
    SET_EXECUTABLE_TARGET_PROPERTIES(eden-cef-handlers-tests)
    set_target_properties(eden-cef-handlers-tests PROPERTIES BUILD_RPATH "$ORIGIN")
    add_dependencies(eden-cef-handlers-tests eden-helper)
    eden_stage_cef_runtime(eden-cef-handlers-tests)
    find_program(EDEN_XVFB_RUN xvfb-run REQUIRED)
    add_test(NAME eden-cef-handlers-tests COMMAND ${EDEN_XVFB_RUN} -a -s "-screen 0 1600x1000x24" $<TARGET_FILE:eden-cef-handlers-tests> --no-sandbox --engine-compositing=osr --ozone-platform=x11)
    set_tests_properties(eden-cef-handlers-tests PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=xcb")

    qt_add_executable(eden-devtools-socket-tests tests/devtoolssocketserver_test.cpp)
    target_link_libraries(eden-devtools-socket-tests PRIVATE eden-core eden-engine-cef Qt6::Test Qt6::WebSockets)
    add_test(NAME eden-devtools-socket-tests COMMAND eden-devtools-socket-tests)
    set_tests_properties(eden-devtools-socket-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(EDEN_ENGINE_SERVO)
    find_program(EDEN_CARGO cargo REQUIRED)
    set(EDEN_SERVO_CRATE_DIR ${CMAKE_SOURCE_DIR}/src/engine/servo/rust)
    set(EDEN_SERVO_STATIC_LIB ${EDEN_SERVO_CRATE_DIR}/target/release/libeden_servo.a)
    add_custom_target(eden-servo-cargo COMMAND ${EDEN_CARGO} build --release WORKING_DIRECTORY ${EDEN_SERVO_CRATE_DIR} BYPRODUCTS ${EDEN_SERVO_STATIC_LIB})
    add_library(eden-servo-ffi STATIC IMPORTED)
    set_target_properties(eden-servo-ffi PROPERTIES IMPORTED_LOCATION ${EDEN_SERVO_STATIC_LIB} INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/src INTERFACE_LINK_LIBRARIES "pthread;dl;m")
    qt_add_executable(eden-servo-ffi-tests tests/servoffi_test.cpp)
    target_link_libraries(eden-servo-ffi-tests PRIVATE eden-servo-ffi Qt6::Test)
    add_dependencies(eden-servo-ffi-tests eden-servo-cargo)
    add_test(NAME eden-servo-ffi-tests COMMAND eden-servo-ffi-tests)
    set_tests_properties(eden-servo-ffi-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
