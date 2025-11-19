cmake_minimum_required(VERSION 3.28)

# Use Boost CONFIG packages (CMP0167).
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

project(pop LANGUAGES CXX)

# -----------------------------------------------------------------------------
# 0) Language + options
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(_std_feature cxx_std_23)

option(POP_BUILD_TESTS "Build unit tests" ON)

# -----------------------------------------------------------------------------
# 1) System packages
# -----------------------------------------------------------------------------
find_package(Boost 1.78 CONFIG REQUIRED COMPONENTS headers system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# 2) Header-only deps via FetchContent
# -----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# -----------------------------------------------------------------------------
# 3) Core library
# -----------------------------------------------------------------------------
add_library(pop_core STATIC
        src/binance_feed_handler.cpp
        src/okx_feed_handler.cpp
        src/bybit_feed_handler.cpp
        src/bitget_feed_handler.cpp
        src/rest_client.cpp
        src/ws_client.cpp
)

# public headers live in include/
target_include_directories(pop_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Publish all headers during active dev; switch to explicit list when stable.
file(GLOB_RECURSE POP_PUBLIC_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_SOURCE_DIR}/include/*.h"
)
target_sources(pop_core
        PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS ${CMAKE_SOURCE_DIR}/include
        FILES ${POP_PUBLIC_HEADERS}
)

# Modern features
target_compile_features(pop_core PUBLIC ${_std_feature})

# Link deps
target_link_libraries(pop_core
        PUBLIC
        nlohmann_json::nlohmann_json
        Boost::headers
        PRIVATE
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

# Warnings
if (MSVC)
    target_compile_options(pop_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(pop_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# -----------------------------------------------------------------------------
# 4) App
# -----------------------------------------------------------------------------
add_executable(pop
        app/main.cpp
)
target_link_libraries(pop PRIVATE pop_core)
target_compile_features(pop PRIVATE ${_std_feature})