file(GLOB_RECURSE POP_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

add_library(pop_core STATIC ${POP_SRC})
target_include_directories(pop_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(pop_core PUBLIC cxx_std_23)

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

target_link_libraries(pop_core PUBLIC common_core)

if(TARGET Boost::program_options)
    target_link_libraries(pop_core PUBLIC Boost::program_options)
elseif(DEFINED Boost_LIBRARIES)
    target_link_libraries(pop_core PUBLIC ${Boost_LIBRARIES})
endif()

# Main executable
add_executable(pop app/main.cpp)
target_link_libraries(pop PRIVATE pop_core)
set_target_properties(pop PROPERTIES OUTPUT_NAME "pop")
