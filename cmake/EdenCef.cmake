include_guard(GLOBAL)
include(FetchContent)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The pinned CEF distribution supports Linux only")
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" EDEN_CEF_PROCESSOR)
if(NOT EDEN_CEF_PROCESSOR MATCHES "^(x86_64|amd64)$")
    message(FATAL_ERROR "The pinned CEF distribution supports x86_64 only")
endif()

set(EDEN_CEF_ARCHIVE_NAME "${EDEN_CEF_LINUX_X86_64_ARCHIVE}")
set(EDEN_CEF_ARCHIVE_SHA1 "${EDEN_CEF_LINUX_X86_64_SHA1}")
set(EDEN_CEF_ROOT "${CMAKE_SOURCE_DIR}/third_party/cef/${EDEN_CEF_ARCHIVE_NAME}")
string(REGEX REPLACE "\\.tar\\.bz2$" "" EDEN_CEF_ROOT "${EDEN_CEF_ROOT}")

if(NOT EXISTS "${EDEN_CEF_ROOT}/cmake/FindCEF.cmake")
    if(EDEN_CEF_LOCAL_ARCHIVE)
        get_filename_component(EDEN_CEF_ARCHIVE_URL "${EDEN_CEF_LOCAL_ARCHIVE}" ABSOLUTE)
        if(NOT EXISTS "${EDEN_CEF_ARCHIVE_URL}")
            message(FATAL_ERROR "EDEN_CEF_LOCAL_ARCHIVE does not exist: ${EDEN_CEF_ARCHIVE_URL}")
        endif()
        file(SHA1 "${EDEN_CEF_ARCHIVE_URL}" EDEN_CEF_LOCAL_ARCHIVE_SHA1)
        if(NOT EDEN_CEF_LOCAL_ARCHIVE_SHA1 STREQUAL EDEN_CEF_ARCHIVE_SHA1)
            message(FATAL_ERROR "EDEN_CEF_LOCAL_ARCHIVE SHA-1 mismatch, expected ${EDEN_CEF_ARCHIVE_SHA1}, got ${EDEN_CEF_LOCAL_ARCHIVE_SHA1}")
        endif()
    else()
        string(REPLACE "+" "%2B" EDEN_CEF_URL_VERSION "${EDEN_CEF_VERSION}")
        set(EDEN_CEF_ARCHIVE_URL "https://cef-builds.spotifycdn.com/cef_binary_${EDEN_CEF_URL_VERSION}_linux64_minimal.tar.bz2")
    endif()

    message(STATUS "Fetching CEF ${EDEN_CEF_VERSION} into ${EDEN_CEF_ROOT}")
    FetchContent_Declare(eden_cef
        URL "${EDEN_CEF_ARCHIVE_URL}"
        URL_HASH "SHA1=${EDEN_CEF_ARCHIVE_SHA1}"
        SOURCE_DIR "${EDEN_CEF_ROOT}"
        SOURCE_SUBDIR eden-no-cmake-project
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(eden_cef)
endif()

set(CEF_ROOT "${EDEN_CEF_ROOT}")
list(PREPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/third_party/cef/libcef_dll_wrapper" EXCLUDE_FROM_ALL)
set_target_properties(libcef_dll_wrapper PROPERTIES AUTOMOC OFF AUTORCC OFF)

function(eden_stage_cef_runtime target_name)
    file(GLOB EDEN_CEF_RELEASE_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE "${EDEN_CEF_ROOT}/Release/*")
    file(GLOB EDEN_CEF_RESOURCE_PACKS CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE "${EDEN_CEF_ROOT}/Resources/*.pak")

    if(NOT EDEN_CEF_RELEASE_FILES)
        message(FATAL_ERROR "The CEF distribution contains no runtime files")
    endif()
    if(NOT EDEN_CEF_RESOURCE_PACKS)
        message(FATAL_ERROR "The CEF distribution contains no resource packs")
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target_name}>/locales"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${EDEN_CEF_RELEASE_FILES} "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${EDEN_CEF_ROOT}/Resources/icudtl.dat" ${EDEN_CEF_RESOURCE_PACKS} "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${EDEN_CEF_ROOT}/Resources/locales" "$<TARGET_FILE_DIR:${target_name}>/locales"
        VERBATIM
    )
endfunction()
