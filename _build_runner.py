"""
Build runner — invokes the MSVC environment and CMake/Ninja without
going through the MSYS2 bash hook that interferes with cmd.exe stdin.
Auto-increments build number by 0.01 each run.
"""
import subprocess
import sys
import os

VCVARS   = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
CL       = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe"
MT       = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\mt.exe"
RC_EXE   = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\rc.exe"
WINSDK   = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
CMAKE    = r"C:\Program Files\CMake\bin\cmake.exe"
NINJA    = r"C:\Users\onnik\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe"
SRC      = r"C:\Users\onnik\Desktop\IDHMFIS"
BUILD    = r"C:\Users\onnik\Desktop\IDHMFIS\build"
LOG      = r"C:\Users\onnik\Desktop\IDHMFIS\build_log.txt"
BUILD_NUM_FILE = os.path.join(SRC, "build_number.txt")
VERSION_H      = os.path.join(SRC, "src", "version.h")

# ── Increment build number ────────────────────────────────────────────────────
# If build_number.txt contains a non-numeric string, use it as a literal version
# (no increment). This allows special version names like "5.20.FUCKTHIS".
# Numeric strings are incremented by 0.01 as usual.
try:
    with open(BUILD_NUM_FILE, "r") as f:
        raw = f.read().strip()
    build_num = round(float(raw) + 0.01, 2)
    version_str = f"{build_num:.2f}"
    new_num_content = f"{build_num:.2f}\n"
except Exception:
    version_str = raw if raw else "3.10"
    new_num_content = f"{version_str}\n"

with open(BUILD_NUM_FILE, "w") as f:
    f.write(new_num_content)

with open(VERSION_H, "w") as f:
    f.write(f'#pragma once\n#define IDHMFIS_VERSION "{version_str}"\n')

print(f"Build number: {version_str}")
sys.stdout.flush()

# ── CMake / Ninja ─────────────────────────────────────────────────────────────
cmd = (
    f'"{VCVARS}" && '
    f'set "PATH=%PATH%;C:\\Program Files\\CMake\\bin;'
    f'C:\\Users\\onnik\\AppData\\Local\\Microsoft\\WinGet\\Packages\\'
    f'Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;'
    f'{WINSDK}" && '
    f'cmake --version && '
    f'ninja --version && '
    f'echo. && '
    f'echo === CONFIGURE === && '
    # Wipe stale cache so cmake re-detects the compiler cleanly
    f'if exist "{BUILD}\\CMakeCache.txt" del /f /q "{BUILD}\\CMakeCache.txt" && '
    f'if exist "{BUILD}\\CMakeFiles" rmdir /s /q "{BUILD}\\CMakeFiles" && '
    f'cmake -B "{BUILD}" -G Ninja -DCMAKE_BUILD_TYPE=Release '
    f'-DCMAKE_CXX_COMPILER="{CL}" '
    f'-DCMAKE_MT="{MT}" '
    f'-DCMAKE_CXX_STANDARD=20 -DIDHMFIS_BUILD_TESTS=ON -DIDHMFIS_BUILD_TOOLS=ON '
    f'-S "{SRC}" && '
    f'echo. && '
    f'echo === BUILD === && '
    f'cmake --build "{BUILD}" --config Release --parallel'
)

print(f"Writing build log to: {LOG}")
print(f"Running build...")
sys.stdout.flush()

with open(LOG, "w", encoding="utf-8") as log_file:
    result = subprocess.run(
        f'cmd.exe /c "{cmd}"',
        stdout=log_file,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,  # prevent any stdin interference
        shell=False
    )

print(f"Build exited with code: {result.returncode}")

# Print last 80 lines of log
try:
    with open(LOG, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    tail = lines[-80:] if len(lines) > 80 else lines
    for line in tail:
        print(line, end="")
except Exception as e:
    print(f"Could not read log: {e}")

sys.exit(result.returncode)
