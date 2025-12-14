cmake_minimum_required(VERSION 3.16)
project(bacnet-event-server VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(BUILD_TESTS "Build unit tests" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# Debug/Release
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(DEBUG)
    add_compile_options(-g -O0)
else()
    add_compile_options(-O2)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# External Dependencies
# ============================================================================

# hiredis (Redis C Client)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED hiredis)
include_directories(${HIREDIS_INCLUDE_DIRS})

# cJSON
pkg_check_modules(CJSON REQUIRED libcjson)
include_directories(${CJSON_INCLUDE_DIRS})

# libcurl (for KurrentDB HTTP API)
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIRS})

# pthread
find_package(Threads REQUIRED)

# math library
find_library(MATH_LIBRARY m)

# BACnet-Stack (assumed as submodule or externally installed)
# Option 1: As submodule
if(EXISTS "${CMAKE_SOURCE_DIR}/external/bacnet-stack/CMakeLists.txt")
    add_subdirectory(external/bacnet-stack)
    set(BACNET_STACK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/bacnet-stack/src")
    set(BACNET_STACK_LIBRARIES bacnet-stack)
else()
    # Option 2: System installation
    find_path(BACNET_STACK_INCLUDE_DIR bacnet/basic/object/device.h
        PATHS /usr/local/include /usr/include
    )
    find_library(BACNET_STACK_LIBRARIES bacnet-stack
        PATHS /usr/local/lib /usr/lib
    )
endif()

if(BACNET_STACK_INCLUDE_DIR)
    include_directories(${BACNET_STACK_INCLUDE_DIR})
endif()

# ============================================================================
# Sources
# ============================================================================

set(SOURCES
    src/main.c
    src/redis_cache.c
    src/message_handler.c
    src/kurrentdb_client.c
    src/bacnet_server.c
    src/logging.c
    src/health_api.c
)

set(HEADERS
    include/redis_cache.h
    include/kurrentdb_client.h
    include/bacnet_server.h
    include/message_handler.h
    include/logging.h
    include/health_api.h
)

# ============================================================================
# Executable
# ============================================================================

add_executable(bacnet-event-server ${SOURCES} ${HEADERS})

target_link_libraries(bacnet-event-server
    ${HIREDIS_LIBRARIES}
    ${CJSON_LIBRARIES}
    ${CURL_LIBRARIES}
    Threads::Threads
    ${MATH_LIBRARY}
    # ${BACNET_STACK_LIBRARIES}  # If available
)

# Compile definition when BACnet Stack is available
if(BACNET_STACK_LIBRARIES)
    target_compile_definitions(bacnet-event-server PRIVATE HAVE_BACNET_STACK)
    target_link_libraries(bacnet-event-server ${BACNET_STACK_LIBRARIES})
endif()

# ============================================================================
# Installation
# ============================================================================

install(TARGETS bacnet-event-server
    RUNTIME DESTINATION bin
)

install(FILES config/server-config.json
    DESTINATION /etc/bacnet-gateway
    RENAME server-config.json.example
)

install(DIRECTORY docs/
    DESTINATION share/bacnet-gateway/docs
)

# ============================================================================
# Packaging
# ============================================================================

set(CPACK_PACKAGE_NAME "bacnet-event-server")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "BACnet to KurrentDB Gateway Server")
set(CPACK_PACKAGE_VENDOR "Unlock Europe - Free and Open Source Software - Energy")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Unlock Europe - Free and Open Source Software - Energy")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libhiredis-dev, libcjson-dev, libcurl4-openssl-dev")

include(CPack)

# ============================================================================
# Tests
# ============================================================================

if(BUILD_TESTS)
    enable_testing()
    
    add_executable(test_message_handler
        tests/test_message_handler.c
        src/message_handler.c
    )
    target_link_libraries(test_message_handler ${CJSON_LIBRARIES})
    add_test(NAME test_message_handler COMMAND test_message_handler)
endif()
