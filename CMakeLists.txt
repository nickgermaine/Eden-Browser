cmake_minimum_required(VERSION 3.21)

project(eden-browser VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets WebEngineCore WebEngineWidgets)

qt_standard_project_setup()

set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

qt_add_executable(eden-browser
    src/main.cpp
    src/components/base/baseapplication.cpp
    src/components/base/baseapplication.h
    src/components/core/core.cpp
    src/components/core/core.h
    src/components/devtools/devtoolscontainer.cpp
    src/components/devtools/devtoolscontainer.h
    src/components/networkmanager/urlintercepter.cpp
    src/components/networkmanager/urlintercepter.h
    src/components/tabs/edenpinnedtabbar.cpp
    src/components/tabs/edenpinnedtabbar.h
    src/components/tabs/tab.cpp
    src/components/tabs/tab.h
    src/components/tabs/tabbar.cpp
    src/components/tabs/tabbar.h
    src/components/toolbar/addressbar.cpp
    src/components/toolbar/addressbar.h
    src/components/json/json.hpp
    resources.qrc
)

target_include_directories(eden-browser PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(eden-browser PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
)
