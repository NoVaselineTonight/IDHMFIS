# Building IDHMFIS

IDHMFIS is a single native executable built with CMake and Ninja. Apart from the Vulkan headers (from vcpkg) and the optional NDI SDK, every dependency is fetched and built by CMake.

---

## Prerequisites (Windows 10/11 x64)

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 or Build Tools | 17.8+ | "Desktop development with C++" workload, Windows 11 SDK |
| CMake | 3.25+ | |
| Ninja | 1.11+ | |
| Git | any | Used by CMake FetchContent |
| vcpkg | current | Supplies the Vulkan headers and VMA (see `vcpkg.json`) |
| NDI 6 SDK | 6.x (5.x also works) | Optional. Only needed for NDI output. |
| WiX Toolset | 4.x | Optional. Only needed for the MSI installer. |

To set up a fresh machine, [`docs/SETUP_BUILD_ENV.ps1`](SETUP_BUILD_ENV.ps1) installs the Build Tools, CMake and Ninja through winget. Run it once from an elevated PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\docs\SETUP_BUILD_ENV.ps1
```

---

## Build

Run these from a **Developer PowerShell for VS 2022** (or any shell where `vcvars64.bat` has been run):

```powershell
git clone https://github.com/NoVaselineTonight/IDHMFIS.git
cd IDHMFIS

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --parallel
```

The binary is written to `build\bin\IDHMFIS.exe`. `launch.bat` in the repo root starts it.

For a debug build, use `-DCMAKE_BUILD_TYPE=Debug` and a separate build directory such as `build-debug`.

> If you see `C1083: Cannot open include file: 'memory'`, the MSVC environment is not initialised in your shell. Your code is fine. Open a Developer PowerShell and configure again.
>
> If you see `LNK1104: cannot open file 'IDHMFIS.exe'`, a previous instance is still running. Close it and rebuild.

---

## Build options

| CMake option | Default | Description |
|---|---|---|
| `CMAKE_BUILD_TYPE` | — | `Debug`, `Release` or `RelWithDebInfo` |
| `ENABLE_NDI` | `OFF` | Build with NDI output. Needs the NDI SDK. |
| `ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy during compilation |
| `IDHMFIS_BUILD_TESTS` | `ON` | Build the benchmark harness and unit tests |
| `IDHMFIS_BUILD_TOOLS` | `ON` | Build development tools such as `artnet_test_sender` |

The code builds with `/W4 /WX /permissive-`, so every warning is an error, in local builds and in CI alike.

---

## NDI (optional)

1. Download the NDI 6 SDK from <https://ndi.video/for-developers/ndi-sdk/> and install it to the default location, or set `NDI_SDK_DIR` to point at it.
2. Configure with `-DENABLE_NDI=ON`.

The NDI runtime is loaded dynamically when the app starts. A build with NDI enabled still runs on machines without the NDI runtime installed: NDI output is simply disabled and the status indicator shows *NDI Unavailable*. DAC output, Art-Net, MIDI, OSC and everything else work either way.

---

## Third-party dependencies

Fetched automatically by CMake FetchContent:

| Library | License | Purpose |
|---|---|---|
| SDL3 | zlib | Window, input, HiDPI |
| Dear ImGui (docking) | MIT | UI |
| nlohmann/json 3.11.3 | MIT | Project serialization |
| RtMidi | MIT-style | MIDI I/O |
| readerwriterqueue | BSD | Lock-free SPSC queue |
| concurrentqueue | BSD | Lock-free MPSC queue |
| KissFFT | BSD | Audio FFT |
| Vulkan Memory Allocator | MIT | GPU memory management |
| hidapi 0.14.0 | BSD | LaserDock USB HID |

Vendored in `src/`: miniaudio (audio playback) and stb_image (image import).

---

## Tests and benchmarks

```powershell
# Project model unit tests
ctest --test-dir build --output-on-failure

# Benchmark harness
.\build\tests\bench\bench_harness.exe --output bench_results.md
```

See [PERFORMANCE.md](PERFORMANCE.md) for the targets and what each benchmark measures.

---

## MSI installer

```powershell
.\installer\build_installer.ps1
```

This builds a Release binary and packages it as `dist\IDHMFIS-Setup-x64.msi`. It needs WiX 4 (`dotnet tool install --global wix`).

---

## Verifying a build

**Art-Net input**

```powershell
.\build\bin\artnet_test_sender.exe --ip 127.0.0.1 --universe 0
```

The DMX monitor should show channel activity, and the preview should follow the animated values.

**DAC output**

1. Connect a Helios (USB), Ether Dream (LAN) or LaserDock (USB).
2. Start IDHMFIS and patch the device under *Outputs*.
3. The connection indicator turns green once the device is streaming.

If you don't have hardware, turn on the emulated Helios DAC under *Settings → DAC*. It runs the complete output pipeline, including the optimizer, safety and output threads, without a device attached.

**NDI output**

1. Install NDI Tools from <https://ndi.video/tools/>.
2. Open NDI Studio Monitor. An `IDHMFIS` source should appear within a couple of seconds of pressing Play.
