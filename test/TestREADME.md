### Instruction for running tests:

#### 1. Configure a build folder:
```bash
cd ..
cmake -S . -B build -DPOP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
```
→ generates build/ with Ninja/Makefiles.

**-DPOP_BUILD_TESTS=ON:**
- The -D...=... syntax sets a CMake variable at configure time.
- *POP_BUILD_TESTS* is the option you declared in test/CMakeLists.txt
- If **ON**, CMake will include the test setup (FetchContent + add_subdirectory(test)).
- If **OFF**, no GoogleTest will be fetched, no test binaries are built.

**Arguments:**
- **-S** = source directory (where the top level CMakeLists.txt lives).
- **-B** = build directory (where CMake will put all generated files, Makefiles, Ninja files, object files, binaries).

#### 2. Build:
```bash
cd ..
cmake --build build -j
```
→ compiles pop, pop_core, and pop_tests.

**Arguments:**
- **-j** = number of parallel jobs (cores/threads to use when building). Without a number means “use as many cores as available”. **-j4** would use 4 threads.

#### 3. Run the tests:
```bash
cd ..
cd build
ctest --output-on-failure
```
→ executes pop_tests, shows test results.

**Arguments:**
- **ctest** is CMake’s test runner.
- It looks for tests you registered with *enable_testing() + gtest_discover_tests(...)*.
- **--output-on-failure** makes it print full output only if a test fails, so you see error messages instead of just **“FAILED”**.