include_guard(GLOBAL)
include(FetchContent)

set(EDEN_SOLAR_ICONS_ROOT "${CMAKE_SOURCE_DIR}/third_party/solar-icons")

if(NOT EXISTS "${EDEN_SOLAR_ICONS_ROOT}/icons/SVG")
    message(STATUS "Fetching Solar icon set ${EDEN_SOLAR_ICONS_COMMIT} into ${EDEN_SOLAR_ICONS_ROOT}")
    FetchContent_Declare(eden_solar_icons
        URL "https://github.com/480-Design/Solar-Icon-Set/archive/${EDEN_SOLAR_ICONS_COMMIT}.tar.gz"
        URL_HASH "SHA256=${EDEN_SOLAR_ICONS_SHA256}"
        SOURCE_DIR "${EDEN_SOLAR_ICONS_ROOT}"
        SOURCE_SUBDIR eden-no-cmake-project
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(eden_solar_icons)
endif()
