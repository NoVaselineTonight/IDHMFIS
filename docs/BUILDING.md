# Building IDHMFIS from Source

## Quick Start (Windows)

```bat
# 1. Install toolchain (run once as Administrator in PowerShell)
Set-ExecutionPolicy -Scope Process Bypass
.\SETUP_BUILD_ENV.ps1

# 2. Build (any command prompt)
.\build.bat
```

The build script installs VS 2022 Build Tools, CMake, and Ninja via winget,
then configures and compiles everything. All C++ dependencies are fetched
automatically via CMake FetchContent — no vcpkg or manual installs required.

---

## Prerequisites

### Windows (Primary Target)

| Tool | Version | Purpose |
|------|---------|---------|
| Visual Studio 2022 Build Tools | 17.8+ | MSVC C++20 compiler + Windows SDK |
| CMake | 3.25+ | Build system |
| Ninja | 1.11+ | Fast build backend |
| Git | any | Required by CMake FetchContent |
| NDI SDK | 6.x (or 5.x) | NDI streaming (optional — app runs without it) |
| WiX Toolset | 4.x | MSI installer (optional) |

All other dependencies (SDL3, ImGui, nlohmann/json, RtMidi, KissFFT, VMA, etc.)
are downloaded and built automatically by CMake FetchContent — no vcpkg needed.

NDI SDK from https://ndi.video/for-developers/ndi-sdk/ (free, registration required).
Install to the default path (`C:\Program Files\NDI\NDI 6 SDK\`). If not installed,
IDHMFIS builds and runs without NDI output. Enable with `-DENABLE_NDI=ON`.

### macOS (Secondary Target)

| Tool | Version | Purpose |
|------|---------|---------|
| Xcode | 15+ | Apple Clang C++20 |
| CMake | 3.25+ | Build system |
| Homebrew | current | Package manager |
| MoltenVK | latest | Vulkan on Metal |

```bash
brew install cmake ninja molten-vk vulkan-headers
```

NDI SDK for Apple from https://ndi.video/for-developers/ndi-sdk/ — install the macOS package.

---

## Building on Windows

### Development Build (Debug)

```powershell
# Clone the repo
git clone <repo-url> IDHMFIS
cd IDHMFIS

# Configure
cmake -B build-debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build-debug --parallel

# Run
.\build-debug\IDHMFIS.exe
```

### Release Build

```powershell
cmake -B build-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-release --parallel
```

The release binary is at `build-release\IDHMFIS.exe`.

### MSI Installer

```powershell
.\installer\build_installer.ps1
```

Output: `dist\IDHMFIS-Setup-x64.msi`

Requires WiX 4 (`dotnet tool install wix` if not installed — the script handles this).

---

## Building on macOS

```bash
# Configure
cmake -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++

# Build
cmake --build build-release --parallel

# Run
./build-release/IDHMFIS
```

---

## Running Tests

```powershell
# Build with tests
cmake -B build-test -G Ninja `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-test --parallel

# Run performance benchmark (outputs PERF_REPORT.md)
.\build-test\bench_harness.exe --output PERF_REPORT.md

# Run 4-hour soak test (accelerated, ~24 real minutes)
.\build-test\soak_test.exe --duration-minutes 24 --output SOAK_REPORT.md
```

---

## Build Options

| CMake Option | Default | Description |
|-------------|---------|-------------|
| `ENABLE_CLANG_TIDY` | OFF | Run clang-tidy during build |
| `IDHMFIS_WITH_NDI` | AUTO | NDI support (AUTO=detect, ON=require, OFF=disable) |
| `IDHMFIS_WITH_ASIO` | OFF | ASIO audio input support |
| `IDHMFIS_BUILD_TOOLS` | ON | Build artnet_test_sender.exe and bench tools |
| `CMAKE_BUILD_TYPE` | Debug | Debug / Release / RelWithDebInfo |

---

## Third-Party Dependencies (fetched automatically by CMake)

All of the following are downloaded by CMake FetchContent — no manual steps required:

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| Dear ImGui (docking) | git HEAD docking branch | MIT | UI framework |
| nlohmann/json | 3.11.3 | MIT | JSON serialization |
| RtMidi | 6.0.0 | MIT | MIDI input |
| readerwriterqueue | HEAD | BSD | Lock-free SPSC queue |
| concurrentqueue | HEAD | BSD | Lock-free MPSC queue |
| KissFFT | 2.0.0 | BSD | Audio FFT |
| Vulkan Memory Allocator | 3.1.0 | MIT | GPU memory management |

Vulkan headers are sourced from vcpkg (`vulkan` package).
NDI SDK is an external install (not downloaded by CMake).

---

## NDI Not Installed?

If the NDI SDK is not found at configure time, CMake emits:
```
-- NDI SDK not found — building without NDI output. Install from https://ndi.video/
```

The app builds and runs without NDI. The NDI status indicator in the UI shows
"NDI Unavailable". Install the NDI SDK and rebuild to enable NDI streaming.

---

## Clean Build from Scratch (checklist)

1. `git clone` the repo
2. Install prerequisites (VS2022, CMake, vcpkg, Vulkan SDK)
3. `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake`
4. `cmake --build build --parallel`
5. `.\build\IDHMFIS.exe` — should start in < 1.5 seconds

No other steps. If step 3 or 4 fail on a clean clone, that is a bug.

---

## Verifying NDI Output

1. Install NDI Tools (free) from https://ndi.video/tools/
2. Launch NDI Studio Monitor
3. Start IDHMFIS and press Play
4. "IDHMFIS" should appear in NDI Studio Monitor's source list within 2 seconds
5. The feed should show beam/haze simulation at the configured fps

---

## Verifying ArtNet Input

```powershell
# From the build directory, run the test sender:
.\artnet_test_sender.exe --ip 127.0.0.1 --universe 0
```

The test sender sends animated DMX values. IDHMFIS's DMX activity monitor should
show channel activity, and the laser preview should respond within 8 ms.
