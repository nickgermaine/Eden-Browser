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
pkg_get_variable(EDEN_WPE_RESOLVED_PREFIX wpe-webkit-2.0 prefix)
if(EDEN_WPE_PREFIX AND NOT EDEN_WPE_RESOLVED_PREFIX STREQUAL EDEN_WPE_PREFIX)
    message(FATAL_ERROR "Requested WebKit prefix ${EDEN_WPE_PREFIX}, but pkg-config selected ${EDEN_WPE_RESOLVED_PREFIX}")
endif()
set(EDEN_WPE_SELECTED_PREFIX "${EDEN_WPE_RESOLVED_PREFIX}" CACHE INTERNAL "Resolved WebKit runtime prefix" FORCE)
message(STATUS "Eden WebKit runtime: ${EDEN_WPE_SELECTED_PREFIX}")
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
target_compile_definitions(eden-engine-wpe PRIVATE EDEN_WPE_BUILD_PREFIX="${EDEN_WPE_SELECTED_PREFIX}")
if(EXISTS "${EDEN_WPE_PREFIX}/share/eden-wpe-build.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EDEN_WPE_PREFIX}/share/eden-wpe-build.json")
    file(READ "${EDEN_WPE_PREFIX}/share/eden-wpe-build.json" EDEN_WPE_BUILD_MANIFEST)
    foreach(EDEN_WPE_FIELD IN ITEMS mode variant profileSHA256 effectiveProfileSHA256 canvasReplayPatch engineExperiments hugePageAlignment)
        string(JSON EDEN_WPE_FIELD_VALUE ERROR_VARIABLE EDEN_WPE_FIELD_ERROR GET "${EDEN_WPE_BUILD_MANIFEST}" ${EDEN_WPE_FIELD})
        if(NOT EDEN_WPE_FIELD_ERROR)
            target_compile_definitions(eden-engine-wpe PRIVATE "EDEN_WPE_BUILD_${EDEN_WPE_FIELD}=\"${EDEN_WPE_FIELD_VALUE}\"")
        endif()
    endforeach()
    string(JSON EDEN_WPE_HAS_CANVAS_REPLAY ERROR_VARIABLE EDEN_WPE_MANIFEST_ERROR GET "${EDEN_WPE_BUILD_MANIFEST}" canvasReplayPatch)
    if(EDEN_WPE_TUNED_DEFAULTS AND NOT EDEN_WPE_MANIFEST_ERROR AND EDEN_WPE_HAS_CANVAS_REPLAY)
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
