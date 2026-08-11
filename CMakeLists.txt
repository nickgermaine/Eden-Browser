cmake_minimum_required(VERSION 3.21)

project(eden-browser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 QuickEffects QuickShapes Sql Test WebEngineCore WebEngineQuick)

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
    src/core/window/tabstripnavigator.cpp
    src/core/window/tabstripnavigator.h
    src/core/window/windowcontroller.cpp
    src/core/window/windowcontroller.h
    src/core/window/windowframe.cpp
    src/core/window/windowframe.h
    src/engine/enginefactory.cpp
    src/engine/enginefactory.h
    src/engine/enginenewviewrequest.cpp
    src/engine/enginenewviewrequest.h
    src/engine/engineprofile.cpp
    src/engine/engineprofile.h
    src/engine/engineview.cpp
    src/engine/engineview.h
    src/engine/qtwebengine/qtwebengineprofile.cpp
    src/engine/qtwebengine/qtwebengineprofile.h
    src/engine/qtwebengine/qtwebengineview.cpp
    src/engine/qtwebengine/qtwebengineview.h
)

qt_add_library(eden-core STATIC ${EDEN_CORE_SOURCES})
target_include_directories(eden-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eden-core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Sql
    Qt6::WebEngineCore
    Qt6::WebEngineQuick
)

set_source_files_properties(src/ui/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(eden-ui
    URI Eden.Ui
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Eden/Ui
    QML_FILES
        src/ui/Theme.qml
        src/ui/components/CommandPalette.qml
        src/ui/components/ChromeBackground.qml
        src/ui/components/ChromeCornerMask.qml
        src/ui/components/EdenButton.qml
        src/ui/components/EdenMenu.qml
        src/ui/components/EdenSlider.qml
        src/ui/components/EdenTextField.qml
        src/ui/components/FindBar.qml
        src/ui/components/Icon.qml
        src/ui/components/Omnibox.qml
        src/ui/components/OverlaySurface.qml
        src/ui/components/SecurityPopover.qml
        src/ui/components/TabIcon.qml
        src/ui/components/TabSidebar.qml
        src/ui/components/TabStrip.qml
        src/ui/components/ThemePreviewCard.qml
        src/ui/panes/BookmarksPane.qml
        src/ui/panes/DownloadsPane.qml
        src/ui/panes/HistoryPane.qml
        src/ui/pages/SettingsPage.qml
        src/ui/pages/ThemeEditorPage.qml
        src/ui/pages/settings/AboutSettings.qml
        src/ui/pages/settings/AppearanceSettings.qml
        src/ui/pages/settings/PlaceholderSettings.qml
        src/ui/pages/settings/SearchSettings.qml
        src/ui/windows/BrowserWindow.qml
    RESOURCES
        resources/fonts/RobotoFlex.ttf
        resources/icons/edenlogo.png
        resources/eden-logo.png
        resources/licenses/qt-lgpl-3.0.txt
        resources/licenses/solar-cc-by-4.0.txt
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
        resources/themes/eden-default.json
)
target_link_libraries(eden-ui PUBLIC eden-core Qt6::Quick Qt6::QuickControls2 Qt6::QuickEffects Qt6::QuickShapes)

qt_add_executable(eden-browser src/main.cpp)
target_compile_definitions(eden-browser PRIVATE EDEN_VERSION="${PROJECT_VERSION}")
target_link_libraries(eden-browser PRIVATE eden-core eden-ui Qt6::QuickControls2)

enable_testing()

qt_add_executable(eden-tests tests/tabmodel_test.cpp)
target_link_libraries(eden-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-tests COMMAND eden-tests)
set_tests_properties(eden-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox")

qt_add_executable(eden-theme-tests tests/thememanager_test.cpp)
target_link_libraries(eden-theme-tests PRIVATE eden-core eden-ui Qt6::Test)
add_test(NAME eden-theme-tests COMMAND eden-theme-tests)
set_tests_properties(eden-theme-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

qt_add_executable(eden-window-frame-tests tests/windowframe_test.cpp)
target_link_libraries(eden-window-frame-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-window-frame-tests COMMAND eden-window-frame-tests)
set_tests_properties(eden-window-frame-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(eden-omnibox-tests tests/omniboxcontroller_test.cpp)
target_link_libraries(eden-omnibox-tests PRIVATE eden-core Qt6::Test)
add_test(NAME eden-omnibox-tests COMMAND eden-omnibox-tests)
set_tests_properties(eden-omnibox-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/omnibox-test-data;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/omnibox-test-config")
