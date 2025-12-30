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
# NOTE: added 'program_options' here
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
file(GLOB CORE_GENERIC_FEED_HANDLER
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/md/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/md/adapters/*.cpp"
)

# Connection handlers
file(GLOB CORE_CLIENT_HANDLERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/client_connection_handlers/*.cpp"
)

# Orderbook
file(GLOB CORE_ORDERBOOK
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/orderbook/*.cpp"
)

add_library(pop_core STATIC
        ${CORE_CLIENT_HANDLERS}
        ${CORE_GENERIC_FEED_HANDLER}
        ${CORE_ORDERBOOK}
)

# public headers live in include/
target_include_directories(pop_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# -----------------------------------------------------------------------------
# 3) POP PUBLIC HEADERS
# Publish all headers during active dev; switch to explicit list when stable.
# -----------------------------------------------------------------------------
# md
file(GLOB CORE_GENERIC_FEED_HANDLER_HEADER
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/md/*.hpp"
)

# Feed handlers
file(GLOB CORE_ABSTRACT_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/abstract/*.hpp"
)

# Connection handlers
file(GLOB CORE_CLIENT_CONNECTION_HANDLER_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/client_connection_handlers/*.hpp"
)

# Orderbook
file(GLOB CORE_ORDERBOOK_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/orderbook/*.hpp"
)

# Utilities
file(GLOB CORE_UTIL_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/utils/*.hpp"
)

# Orderbook
file(GLOB CORE_INCLUDE_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/*.hpp"
)

# All includes
file(GLOB_RECURSE POP_PUBLIC_HEADERS
        CONFIGURE_DEPENDS
        ${CORE_ABSTRACT_HEADERS}
        ${CORE_CLIENT_CONNECTION_HANDLER_HEADERS}
        ${CORE_GENERIC_FEED_HANDLER_HEADER}
        ${CORE_ORDERBOOK_HEADERS}
        ${CORE_INCLUDE_HEADERS}
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
        simdjson::simdjson
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
else ()
    target_compile_options(pop_core PRIVATE -Wall -Wextra -Wpedantic)
endif ()

# -----------------------------------------------------------------------------
# 4) App
# -----------------------------------------------------------------------------
add_executable(pop
        app/main.cpp
)

# Link the app against core + Boost::program_options
target_link_libraries(pop
        PRIVATE
        pop_core
        Boost::program_options
)

target_compile_features(pop PRIVATE ${_std_feature})