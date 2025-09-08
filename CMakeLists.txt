# Top-level CMakeLists

cmake_minimum_required(VERSION 3.28)
project(pop LANGUAGES CXX)

# enforce C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# main executable
add_executable(pop
        app/main.cpp
        src/utils.cpp
)

# include headers from include/
target_include_directories(pop PRIVATE include)