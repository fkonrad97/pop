# Running a CMake C++ Project from the Unix Command Line

This guide shows how to configure, build, run, test, and clean a CMake-based C++ project from a Unix terminal (Linux/macOS). It assumes your project has a top-level `CMakeLists.txt`.

> If your project name/target differs, replace `myapp` with your executable target name.

---

## 0) Prerequisites

- CMake (≥ 3.20 recommended)
- A C++ compiler (GCC/Clang)  
- Optional but recommended: **Ninja** (faster builds than Make)

Check versions:
```bash
cmake --version
g++ --version   # or clang++ --version
ninja --version # optional
```

---

## 1) Configure (Generate Build Files)

Create an out-of-source build directory so your repo stays clean:

```bash
# From the project root (where CMakeLists.txt is)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
# Alternatives:
#   -DCMAKE_BUILD_TYPE=Release
#   -G "Unix Makefiles"   # if you don’t use Ninja
```

What this does:
- Reads `CMakeLists.txt`
- Detects compilers & deps
- Writes build files into `./build/` (e.g., `build.ninja`, `CMakeCache.txt`)

> Tip: For realistic performance with symbols, use `-DCMAKE_BUILD_TYPE=RelWithDebInfo`.

---

## 2) Build

```bash
cmake --build build -j
# Or:
# ninja -C build
# make  -C build -j        # if you used "Unix Makefiles"
```

If the build finishes with “no work to do”, it means everything is already compiled or you don’t have any targets defined yet (see **Troubleshooting**).

---

## 3) Run the Executable

Typical locations (depending on your CMake setup):

```bash
# Most common with Ninja
./build/myapp

# If your CMake sets a bin/ dir
./build/bin/myapp

# Unsure where it landed? List executables:
find build -type f -perm -u=x -maxdepth 4
```

Pass args if needed:
```bash
./build/myapp --help
```

---

## 4) Run Tests (if defined)

If your project uses `add_test(...)`:

```bash
ctest --test-dir build --output-on-failure
```

You can also filter tests:
```bash
ctest --test-dir build -R my_test_name
```

---

## 5) Clean / Reconfigure

**Clean the build tree:**
```bash
cmake --build build --target clean
# or just delete the build directory:
rm -rf build
```

**Reconfigure with different options:**
```bash
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

---

## 6) Switch Compilers / Flags

Choose compiler at configure time:
```bash
CC=clang CXX=clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Export compile commands (for IDEs & linters):
```bash
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Enable sanitizers (example for Debug):
```cmake
# In CMakeLists.txt
target_compile_options(myapp PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
target_link_options(myapp    PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
```

Enable LTO for Release:
```cmake
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
```

---

## 7) Useful Commands at a Glance

```bash
# Configure
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build
cmake --build build -j

# Run
./build/myapp

# Tests
ctest --test-dir build --output-on-failure

# Clean
cmake --build build --target clean
# or
rm -rf build
```

---

## 8) Troubleshooting

**“ninja: no work to do”**  
- The build is already up to date, or
- No targets are defined. Check available targets:
  ```bash
  cmake --build build --target help
  ninja -C build -t targets all
  ```
  Ensure your `CMakeLists.txt` contains something like:
  ```cmake
  add_executable(myapp
      src/main.cpp
      # other .cpp files
  )
  ```

**Can’t find the executable**  
- Your project may set a custom output dir (e.g., `bin/`).
- Use:
  ```bash
  find build -type f -perm -u=x -maxdepth 4
  ```

**Wrong build type / flags**  
- Reconfigure with a fresh build directory:
  ```bash
  rm -rf build
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
  ```

**Linker/dep errors**  
- Make sure required libraries are linked in CMake:
  ```cmake
  target_link_libraries(myapp PRIVATE nlohmann_json::nlohmann_json)
  ```
- And that `find_package(...)` or FetchContent is set up properly.

---

## 9) FAQ

- **Do I always need `-G Ninja`?**  
  No, but it’s faster. Without Ninja, CMake will pick a default (often Makefiles on Linux/macOS).

- **Do I need to re-run `cmake -S -B` after code changes?**  
  For added source files that match glob patterns or when you change CMake options, yes. For normal edits to existing `.cpp` files, just `cmake --build build`.

- **How do I pass runtime environment variables?**  
  ```bash
  MY_ENV=1 ./build/myapp
  ```
