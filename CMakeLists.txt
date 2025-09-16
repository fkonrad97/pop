cmake_minimum_required(VERSION 3.20)

# -----------------------------------------------------------------------------
# CMake policy & Boost discovery
# -----------------------------------------------------------------------------
# CMP0167 NEW => CMake no longer uses the deprecated FindBoost module.
# It will use Boost's official CMake package config (CONFIG mode) instead.
# This silences the 3.28+ warning and ensures modern Boost:: targets.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(pop LANGUAGES CXX)

# -----------------------------------------------------------------------------
# 0) Global language settings
# -----------------------------------------------------------------------------
# Prefer modern, portable C++ without compiler extensions.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Also declare per-target features (good for IDEs/generators).
set(_std_feature cxx_std_23)

# Toggle tests on/off from the configure line: -DPOP_BUILD_TESTS=OFF
option(POP_BUILD_TESTS "Build unit tests" ON)

# -----------------------------------------------------------------------------
# 1) System packages (runtime/link-time dependencies)
# -----------------------------------------------------------------------------
# Use CONFIG mode explicitly to match CMP0167 NEW behavior.
find_package(Boost 1.78 CONFIG REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# 2) Header-only dependencies via FetchContent (downloaded at configure time)
# -----------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# -----------------------------------------------------------------------------
# 3) Core library with actual code
# -----------------------------------------------------------------------------
add_library(pop_core STATIC
        src/feed_handler.cpp
        # Later you'll add:
        # src/net/http_beast.cpp
        # src/net/ws_beast.cpp
        # src/handlers/binance_feed_handler.cpp
)

# Public headers for consumers (app/tests) live in include/
target_include_directories(pop_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# -----------------------------------------------------------------------------
# Public header publication (FILE_SET) using GLOB during active development
#
# We GLOB all headers under include/ to avoid hand-maintaining the list while
# the API is rapidly evolving. CONFIGURE_DEPENDS tells CMake to reconfigure
# when headers are added/removed.
#
# NOTE: Once the API stabilizes (packaging/exports), consider switching to an
# explicit list (no GLOB) for reproducibility and clearer control over the
# published surface. That would look like:
#   set(POP_PUBLIC_HEADERS
#     ${CMAKE_SOURCE_DIR}/include/md/feed_handler.hpp
#     ${CMAKE_SOURCE_DIR}/include/md/order_book.hpp
#     ...
#   )
#   target_sources(pop_core PUBLIC FILE_SET public_headers TYPE HEADERS
#                  BASE_DIRS ${CMAKE_SOURCE_DIR}/include FILES ${POP_PUBLIC_HEADERS})
# -----------------------------------------------------------------------------
file(GLOB_RECURSE POP_PUBLIC_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_SOURCE_DIR}/include/*.h")

target_sources(pop_core
        PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS ${CMAKE_SOURCE_DIR}/include
        FILES ${POP_PUBLIC_HEADERS}
)

# Modern feature declaration (optional if relying on global C++23 settings)
target_compile_features(pop_core PUBLIC ${_std_feature})

# Link dependencies for pop_core
target_link_libraries(pop_core
        PUBLIC
        nlohmann_json::nlohmann_json
        PRIVATE
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

# Stricter warnings: handy while building out connectors
if (MSVC)
    target_compile_options(pop_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(pop_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# -----------------------------------------------------------------------------
# 4) App: links to pop_core
# -----------------------------------------------------------------------------
add_executable(pop app/main.cpp)
target_link_libraries(pop PRIVATE pop_core)
target_compile_features(pop PRIVATE ${_std_feature})

# -----------------------------------------------------------------------------
# 5) Tests (GoogleTest + GoogleMock)
# -----------------------------------------------------------------------------
if (POP_BUILD_TESTS)
    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(test)
endif()