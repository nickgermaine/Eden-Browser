cmake_minimum_required(VERSION 3.21)

project(eden-cef-direct LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "eden-cef-direct supports Linux only")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "eden-cef-direct must be built in Release mode")
endif()

get_filename_component(EDEN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${EDEN_ROOT}/cmake/CefVersion.cmake")

set(EDEN_CEF_ROOT "${EDEN_ROOT}/third_party/cef/${EDEN_CEF_LINUX_X86_64_ARCHIVE}")
string(REGEX REPLACE "\\.tar\\.bz2$" "" EDEN_CEF_ROOT "${EDEN_CEF_ROOT}")

if(NOT EXISTS "${EDEN_CEF_ROOT}/cmake/FindCEF.cmake")
    message(FATAL_ERROR "Pinned CEF distribution not found at ${EDEN_CEF_ROOT}")
endif()

set(CEF_ROOT "${EDEN_CEF_ROOT}")
list(PREPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/libcef_dll_wrapper")

add_executable(eden-cef-direct main.cpp)
target_include_directories(eden-cef-direct PRIVATE "${CEF_INCLUDE_PATH}")
target_link_libraries(eden-cef-direct PRIVATE libcef_dll_wrapper "${CEF_LIB_RELEASE}" ${CEF_STANDARD_LIBS})
SET_EXECUTABLE_TARGET_PROPERTIES(eden-cef-direct)
set_target_properties(eden-cef-direct PROPERTIES BUILD_RPATH "$ORIGIN")

set(EDEN_CEF_RUNTIME_FILES
    "${EDEN_CEF_ROOT}/Release/chrome-sandbox"
    "${EDEN_CEF_ROOT}/Release/libcef.so"
    "${EDEN_CEF_ROOT}/Release/libEGL.so"
    "${EDEN_CEF_ROOT}/Release/libGLESv2.so"
    "${EDEN_CEF_ROOT}/Release/libvk_swiftshader.so"
    "${EDEN_CEF_ROOT}/Release/libvulkan.so.1"
    "${EDEN_CEF_ROOT}/Release/v8_context_snapshot.bin"
    "${EDEN_CEF_ROOT}/Release/vk_swiftshader_icd.json"
    "${EDEN_CEF_ROOT}/Resources/chrome_100_percent.pak"
    "${EDEN_CEF_ROOT}/Resources/chrome_200_percent.pak"
    "${EDEN_CEF_ROOT}/Resources/icudtl.dat"
    "${EDEN_CEF_ROOT}/Resources/resources.pak"
)

add_custom_command(
    TARGET eden-cef-direct
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${EDEN_CEF_RUNTIME_FILES}
        "$<TARGET_FILE_DIR:eden-cef-direct>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${EDEN_CEF_ROOT}/Resources/locales"
        "$<TARGET_FILE_DIR:eden-cef-direct>/locales"
    VERBATIM
)
