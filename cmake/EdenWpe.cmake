include(cmake/WpeVersion.cmake)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The WebKit backend requires Linux")
endif()
set(EDEN_WPE_PREFIX "" CACHE PATH "Prefix containing the WPE WebKit installation")
if(NOT EDEN_WPE_PREFIX)
    foreach(EDEN_WPE_LOCAL_PREFIX IN ITEMS local-performance-engine-experiments local-performance local)
        if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/wpe/${EDEN_WPE_LOCAL_PREFIX}/lib/pkgconfig/wpe-webkit-2.0.pc")
            set(EDEN_WPE_PREFIX "${CMAKE_SOURCE_DIR}/third_party/wpe/${EDEN_WPE_LOCAL_PREFIX}")
            break()
        endif()
    endforeach()
endif()
set(EDEN_WPE_PKG_CONFIG_PATH "$ENV{PKG_CONFIG_PATH}")
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/wpe/usr/lib/pkgconfig/wpe-1.0.pc")
    set(EDEN_WPE_PKG_CONFIG_PATH "${CMAKE_SOURCE_DIR}/third_party/wpe/usr/lib/pkgconfig:${EDEN_WPE_PKG_CONFIG_PATH}")
endif()
if(EDEN_WPE_PREFIX)
    set(EDEN_WPE_PKG_CONFIG_PATH "${EDEN_WPE_PREFIX}/lib/pkgconfig:${EDEN_WPE_PKG_CONFIG_PATH}")
endif()
set(EDEN_WPE_ORIGINAL_PKG_CONFIG_PATH "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "${EDEN_WPE_PKG_CONFIG_PATH}")
unset(__pkg_config_checked_WPE CACHE)
get_cmake_property(EDEN_WPE_CACHE_VARIABLES CACHE_VARIABLES)
foreach(EDEN_WPE_CACHE_VARIABLE IN LISTS EDEN_WPE_CACHE_VARIABLES)
    if(EDEN_WPE_CACHE_VARIABLE MATCHES "^pkgcfg_lib_WPE_")
        unset(${EDEN_WPE_CACHE_VARIABLE} CACHE)
    endif()
endforeach()
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET "wpe-webkit-2.0>=${EDEN_WPE_MINIMUM_VERSION}" wpe-platform-headless-2.0)
pkg_check_modules(WPE_GRAPHICS REQUIRED IMPORTED_TARGET epoxy egl libdrm xkbcommon)
set(ENV{PKG_CONFIG_PATH} "${EDEN_WPE_ORIGINAL_PKG_CONFIG_PATH}")
add_library(eden-engine-wpe SHARED
    src/engine/wpe/wpeplugin.cpp
    src/engine/wpe/wpedisplaysync.cpp
    src/engine/wpe/wpedisplaysync.h
    src/engine/wpe/wperuntime.cpp
    src/engine/wpe/wperuntime.h
    src/engine/wpe/wpesurface.cpp
    src/engine/wpe/wpesurface.h
    src/engine/wpe/wpeprofile.cpp
    src/engine/wpe/wpeprofile.h
    src/engine/wpe/wpeengineview.cpp
    src/engine/wpe/wpeengineview.h
)
set_target_properties(eden-engine-wpe PROPERTIES PREFIX "" OUTPUT_NAME "eden-engine-wpe")
option(EDEN_WPE_TUNED_DEFAULTS "Enable measured WebKit performance defaults" ON)
if(EDEN_WPE_TUNED_DEFAULTS AND EXISTS "${EDEN_WPE_PREFIX}/share/eden-wpe-build.json")
    file(READ "${EDEN_WPE_PREFIX}/share/eden-wpe-build.json" EDEN_WPE_BUILD_MANIFEST)
    string(JSON EDEN_WPE_HAS_CANVAS_REPLAY ERROR_VARIABLE EDEN_WPE_MANIFEST_ERROR GET "${EDEN_WPE_BUILD_MANIFEST}" canvasReplayPatch)
    if(NOT EDEN_WPE_MANIFEST_ERROR AND EDEN_WPE_HAS_CANVAS_REPLAY)
        target_compile_definitions(eden-engine-wpe PRIVATE EDEN_WPE_CANVAS_REPLAY_DEFAULT=2)
    endif()
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS OpenGL)
target_link_libraries(eden-engine-wpe PRIVATE eden-core PkgConfig::WPE PkgConfig::WPE_GRAPHICS Qt6::OpenGL)
target_compile_options(eden-engine-wpe PRIVATE -Wall -Wextra -Wpedantic)
qt_add_resources(eden-engine-wpe wpe_forms
    PREFIX "/eden/wpe"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/wpe"
    FILES src/engine/wpe/wpeforms.js
)
add_executable(eden-wpe-tests
    tests/wpebackend_test.cpp
    src/platform/waylandglibeventdispatcher.cpp
    src/platform/waylandglibeventdispatcher.h
)
target_link_libraries(eden-wpe-tests PRIVATE eden-core eden-engine-wpe PkgConfig::WPE Qt6::Test)
if(EDEN_ENGINE_CEF)
    add_dependencies(eden-wpe-tests eden-engine-cef eden-helper)
    eden_stage_cef_runtime(eden-wpe-tests)
endif()
find_program(EDEN_WPE_TEST_PYTHON NAMES python3 REQUIRED)
add_test(NAME eden-wpe-native COMMAND "${EDEN_WPE_TEST_PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/test_wpe.py" --build "${CMAKE_BINARY_DIR}")
set_tests_properties(eden-wpe-native PROPERTIES TIMEOUT 340 LABELS "native;webkit")
add_executable(eden-wpe-bench EXCLUDE_FROM_ALL
    scripts/bench/wpe/main.cpp
    src/platform/waylandglibeventdispatcher.cpp
    src/platform/waylandglibeventdispatcher.h
)
target_link_libraries(eden-wpe-bench PRIVATE eden-core eden-engine-wpe PkgConfig::WPE)
