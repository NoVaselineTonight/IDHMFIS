// src/main.cpp
// IDHMFIS — application entry point.
//
// On Windows the executable is built with the WINDOWS subsystem (WIN32 flag in
// CMakeLists.txt) so the entry point is WinMain.  In Debug builds we re-attach
// a console window so that log output is visible without a debugger.
//
// On macOS/Linux the standard main() entry point is used.

#include "ui/app.h"

#include <exception>
#include <cstdio>
#include <cstring>

#ifdef IDHMFIS_WINDOWS
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

#ifdef IDHMFIS_WINDOWS
/// In Debug builds attach a console so printf / std::cerr are visible.
/// In Release builds this is a no-op — the WINDOWS subsystem hides the console.
static void attach_debug_console()
{
#ifdef _DEBUG
    if (AllocConsole()) {
        FILE* unused_fp = nullptr;
        (void)freopen_s(&unused_fp, "CONOUT$", "w", stdout);
        (void)freopen_s(&unused_fp, "CONOUT$", "w", stderr);
        (void)freopen_s(&unused_fp, "CONIN$",  "r", stdin);
        SetConsoleOutputCP(CP_UTF8);
    }
#endif
}
#endif // IDHMFIS_WINDOWS

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────

#ifdef IDHMFIS_WINDOWS

int WINAPI WinMain(
    _In_     HINSTANCE /*hInstance*/,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     LPSTR     /*lpCmdLine*/,
    _In_     int       /*nShowCmd*/)
{
    attach_debug_console();

    // Startup log — written even if init() throws, so we can diagnose crashes.
    // Path: %APPDATA%\IDHMFIS\logs\startup.log  (never writes to CWD or desktop).
    // NOTE: no std::string here — any C++ object with a destructor in WinMain's
    // scope triggers MSVC C2712 ("cannot use __try in functions that require
    // object unwinding"), even when the object lives in a nested block.
    FILE* log_fp = nullptr;
    {
        char appdata[MAX_PATH]{};
        char idhmfis_dir[MAX_PATH + 16]{};
        char log_dir[MAX_PATH + 32]{};
        char log_path[MAX_PATH + 64]{};
        if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
            std::snprintf(idhmfis_dir, sizeof(idhmfis_dir), "%s\\IDHMFIS",      appdata);
            std::snprintf(log_dir,     sizeof(log_dir),     "%s\\IDHMFIS\\logs", appdata);
            std::snprintf(log_path,    sizeof(log_path),    "%s\\startup.log",  log_dir);
            CreateDirectoryA(idhmfis_dir, nullptr);
            CreateDirectoryA(log_dir,     nullptr);
            fopen_s(&log_fp, log_path, "w");
        }
    }

    // run_app() is a separate function so __try/__except can wrap it without
    // triggering C2712 (cannot use __try in functions that require object unwinding).
    struct Runner {
        FILE* log_fp;
        void log(const char* msg) const {
            if (log_fp) { fputs(msg, log_fp); fputs("\n", log_fp); fflush(log_fp); }
        }
        int run() {
            try {
                log("Constructing Application...");
                idhmfis::Application app;
                log("Application constructed, calling run()...");
                int ret = app.run();
                log("run() returned");
                return ret;
            }
            catch (const std::exception& ex) {
                log(ex.what());
                MessageBoxA(nullptr, ex.what(), "IDHMFIS — Fatal Error", MB_OK | MB_ICONERROR);
                return 1;
            }
            catch (...) {
                log("Unknown C++ exception");
                MessageBoxA(nullptr, "An unknown fatal error occurred.",
                            "IDHMFIS — Fatal Error", MB_OK | MB_ICONERROR);
                return 1;
            }
        }
    } runner{ log_fp };

    runner.log("WinMain entered");

    // Install terminate handler so background-thread crashes write to startup.log.
    static FILE* g_term_fp = log_fp;
    std::set_terminate([]{
        if (g_term_fp) {
            fputs("FATAL: std::terminate() called (unhandled exception in background thread)\n",
                  g_term_fp);
            fflush(g_term_fp);
        }
        // Write to a fallback file in case log_fp was already closed
        char appdata[MAX_PATH]{};
        if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
            char path[MAX_PATH + 64]{};
            std::snprintf(path, sizeof(path), "%s\\IDHMFIS\\logs\\startup.log", appdata);
            FILE* f = nullptr;
            if (fopen_s(&f, path, "a") == 0 && f) {
                fputs("FATAL: std::terminate() called\n", f);
                fflush(f);
                fclose(f);
            }
        }
    });

    int result = 1;
    __try {
        result = runner.run();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        char seh_msg[256];
        std::snprintf(seh_msg, sizeof(seh_msg),
            "SEH crash: code=0x%08X",
            (unsigned)GetExceptionCode());
        runner.log(seh_msg);
        if (log_fp) fclose(log_fp);
        MessageBoxA(nullptr, seh_msg, "IDHMFIS — Crash", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (log_fp) fclose(log_fp);
    return result;
}

#else // macOS / Linux

int main(int /*argc*/, char* /*argv*/[])
{
    try {
        idhmfis::Application app;
        return app.run();
    }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "IDHMFIS fatal error: %s\n", ex.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "IDHMFIS fatal error: unknown exception\n");
        return 1;
    }
}

#endif // IDHMFIS_WINDOWS
