cmake_minimum_required(VERSION 3.20)

# ---- Policies (silence FetchContent timestamp warning; improve robustness) ----
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(pop VERSION 0.1.0 LANGUAGES CXX)

# ---- Global project defaults ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(POP_ENABLE_WERROR "Treat warnings as errors" OFF)

# ---- Dependencies ----
include(FetchContent)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Boost (Beast/Asio + program_options for your CmdLine)
set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)

# Prefer config-mode if available
find_package(Boost CONFIG QUIET COMPONENTS system program_options)
if(NOT Boost_FOUND)
    find_package(Boost REQUIRED COMPONENTS system program_options)
endif()

# nlohmann/json (FetchContent)
FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- Targets ----

# Library from your src/ tree
file(GLOB_RECURSE POP_SRC CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

add_library(pop_core ${POP_SRC})
target_include_directories(pop_core
        PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(pop_core PUBLIC cxx_std_23)

# Warnings + debug flags
if(MSVC)
    target_compile_options(pop_core PRIVATE /W4)
    if(POP_ENABLE_WERROR)
        target_compile_options(pop_core PRIVATE /WX)
    endif()
else()
    target_compile_options(pop_core PRIVATE
            -Wall -Wextra -Wpedantic
            $<$<BOOL:${POP_ENABLE_WERROR}>:-Werror>
            $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-Og>
    )
endif()

# Link deps for core (keep central so final exe inherits)
target_link_libraries(pop_core
        PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        nlohmann_json::nlohmann_json
)

# Boost imported targets differ depending on package mode; handle both.
if(TARGET Boost::system)
    target_link_libraries(pop_core PUBLIC Boost::system)
elseif(TARGET Boost::boost)
    target_link_libraries(pop_core PUBLIC Boost::boost)
endif()

if(TARGET Boost::program_options)
    target_link_libraries(pop_core PUBLIC Boost::program_options)
else()
    # FindBoost fallback on some setups
    if(DEFINED Boost_LIBRARIES)
        target_link_libraries(pop_core PUBLIC ${Boost_LIBRARIES})
    endif()
endif()

# Main executable
add_executable(pop app/main.cpp)
target_link_libraries(pop PRIVATE pop_core)

set_target_properties(pop PROPERTIES OUTPUT_NAME "pop")

message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")