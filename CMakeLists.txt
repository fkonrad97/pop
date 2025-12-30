cmake_minimum_required(VERSION 3.28)

# Use Boost CONFIG packages (CMP0167).
if (POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif ()

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
find_package(Boost 1.78 CONFIG REQUIRED COMPONENTS headers system program_options)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# 2) Header-only / external deps via FetchContent
# -----------------------------------------------------------------------------
include(FetchContent)

# nlohmann_json
FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# simdjson
FetchContent_Declare(
        simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG v3.10.1
)
FetchContent_MakeAvailable(simdjson)

# -----------------------------------------------------------------------------
# 3) Core library
# -----------------------------------------------------------------------------
# md
file(GLOB CORE_MD_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/md/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/md/adapters/*.cpp"
)

# Connection handlers
file(GLOB CORE_CLIENT_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/client_connection_handlers/*.cpp"
)

# Orderbook
file(GLOB CORE_ORDERBOOK_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/orderbook/*.cpp"
)

add_library(pop_core STATIC
        ${CORE_CLIENT_SOURCES}
        ${CORE_MD_SOURCES}
        ${CORE_ORDERBOOK_SOURCES}
)

# Public headers live in include/
target_include_directories(pop_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# -----------------------------------------------------------------------------
# 3b) POP PUBLIC HEADERS (FILE_SET)
# Publish all headers during active dev; switch to explicit list when stable.
# -----------------------------------------------------------------------------
file(GLOB_RECURSE POP_PUBLIC_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/*.hpp"
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

# Link deps (PUBLIC so they propagate to consumers of pop_core)
target_link_libraries(pop_core
        PUBLIC
        nlohmann_json::nlohmann_json
        simdjson::simdjson
        Boost::headers
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

# Warnings
if (MSVC)
    target_compile_options(pop_core PRIVATE /W4 /permissive-)
else ()
    target_compile_options(pop_core PRIVATE -Wall -Wextra -Wpedantic)
endif ()

# -----------------------------------------------------------------------------
# 4) App
# -----------------------------------------------------------------------------
add_executable(pop
        app/main.cpp
)

# Link the app against core + explicitly re-list system libs (robust link line)
target_link_libraries(pop
        PRIVATE
        pop_core
        Boost::program_options
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

target_compile_features(pop PRIVATE ${_std_feature})

# -----------------------------------------------------------------------------
# 5) Tests (optional scaffold)
# -----------------------------------------------------------------------------
if (POP_BUILD_TESTS)
    enable_testing()
    # add_subdirectory(tests)  # uncomment when you add tests/
endif ()
