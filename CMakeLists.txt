cmake_minimum_required(VERSION 3.20)

# ---- Policies ----
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(trading_stack VERSION 0.1.0 LANGUAGES CXX)

# ---- Global defaults ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(POP_ENABLE_WERROR "Treat warnings as errors" OFF)

# ---- External dependencies (resolved once; propagated via target links) ----
include(FetchContent)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)

find_package(Boost CONFIG QUIET COMPONENTS system program_options)
if(NOT Boost_FOUND)
    find_package(Boost REQUIRED COMPONENTS system program_options)
endif()

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- Subprojects ----
add_subdirectory(common)
add_subdirectory(pop)
add_subdirectory(brain)

message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
