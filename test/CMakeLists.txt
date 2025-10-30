add_executable(pop_tests
        feed_handler_test.cpp
        ../src/binance_feed_handler.cpp
)

# pop_core exports include/ as PUBLIC, so no extra include dirs needed
target_link_libraries(pop_tests
        PRIVATE
        pop_core
        GTest::gmock_main   # brings in gmock + gtest + main()
)

# global CMAKE_CXX_STANDARD=23 already set at top-level
target_compile_features(pop_tests PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(pop_tests)
