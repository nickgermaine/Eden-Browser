cmake_minimum_required(VERSION 3.24)

project(eden-servo-direct LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(EDEN_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${EDEN_SOURCE_ROOT}/cmake/ServoVersion.cmake")
include("${EDEN_SOURCE_ROOT}/cmake/EdenServo.cmake")

find_program(
    EDEN_SERVO_SHELL_EXECUTABLE
    NAMES servoshell servo
    PATHS
        "${EDEN_SERVO_SHELL_DIR}"
        "${EDEN_SERVO_SHELL_DIR}/servo"
    NO_DEFAULT_PATH
    REQUIRED
)

add_executable(eden-servo-shell-direct launcher.cpp)
target_compile_definitions(
    eden-servo-shell-direct
    PRIVATE EDEN_SERVO_SHELL_EXECUTABLE="${EDEN_SERVO_SHELL_EXECUTABLE}"
)

find_program(EDEN_CARGO cargo REQUIRED)
set(EDEN_SERVO_CAPI_TARGET_DIR "${CMAKE_BINARY_DIR}/servo-capi-target")
set(EDEN_SERVO_CAPI_LIBRARY "${EDEN_SERVO_CAPI_TARGET_DIR}/release/libservo_capi.so")

add_custom_command(
    OUTPUT "${EDEN_SERVO_CAPI_LIBRARY}"
    COMMAND
        "${CMAKE_COMMAND}" -E env
        CARGO_NET_OFFLINE=true
        CARGO_TARGET_DIR=${EDEN_SERVO_CAPI_TARGET_DIR}
        RUSTUP_TOOLCHAIN=1.95
        "${EDEN_CARGO}" build --offline --locked --release -p servo-capi
    WORKING_DIRECTORY "${EDEN_SERVO_SOURCE_DIR}"
    USES_TERMINAL
)
add_custom_target(eden-servo-capi-build DEPENDS "${EDEN_SERVO_CAPI_LIBRARY}")

add_library(eden-servo-capi SHARED IMPORTED GLOBAL)
set_target_properties(
    eden-servo-capi
    PROPERTIES IMPORTED_LOCATION "${EDEN_SERVO_CAPI_LIBRARY}"
)
add_dependencies(eden-servo-capi eden-servo-capi-build)

add_executable(eden-servo-direct main.cpp)
target_link_libraries(eden-servo-direct PRIVATE eden-servo-capi dl pthread m)
set_target_properties(
    eden-servo-direct
    PROPERTIES BUILD_RPATH "${EDEN_SERVO_CAPI_TARGET_DIR}/release"
)
add_dependencies(eden-servo-direct eden-servo-capi-build)
