#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "App.h"
#include "Audio.h"

// Attach to the parent console (if launched from a terminal) so LogF output
// is visible despite the WINDOWS subsystem.
static void AttachParentConsole()
{
    // Respect redirected handles (e.g. `app > log.txt`); otherwise attach to
    // the parent terminal so printf output is visible.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    bool outRedirected = hOut != nullptr && hOut != INVALID_HANDLE_VALUE;
    bool errRedirected = hErr != nullptr && hErr != INVALID_HANDLE_VALUE;
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* f = nullptr;
        if (!outRedirected)
            freopen_s(&f, "CONOUT$", "w", stdout);
        if (!errRedirected)
            freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

static std::string ToUtf8(const wchar_t* w)
{
    char buf[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return buf;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    AttachParentConsole();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    LaunchOptions opts;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : L""; };
        if (a == L"--selftest") opts.selftest = true;
        else if (a == L"--audiotest") return OceanAudio::OfflineTest();
        else if (a == L"--ui") opts.showUi = true;
        else if (a == L"--novsync") opts.vsync = false;
        else if (a == L"--w") opts.width = std::wcstoul(next().c_str(), nullptr, 10);
        else if (a == L"--h") opts.height = std::wcstoul(next().c_str(), nullptr, 10);
        else if (a == L"--lod") opts.lod = std::wcstol(next().c_str(), nullptr, 10);
        else if (a == L"--sea") opts.seaState = std::wcstol(next().c_str(), nullptr, 10);
        else if (a == L"--time") opts.timeOfDay = std::wcstol(next().c_str(), nullptr, 10);
        else if (a == L"--frames") opts.benchFrames = std::wcstol(next().c_str(), nullptr, 10);
        else if (a == L"--screenshot") opts.screenshotPath = ToUtf8(next().c_str());
        else if (a == L"--campos")
        {
            opts.hasCamera = true;
            opts.camX = std::wcstof(next().c_str(), nullptr);
            opts.camY = std::wcstof(next().c_str(), nullptr);
            opts.camZ = std::wcstof(next().c_str(), nullptr);
        }
        else if (a == L"--yaw") { opts.hasCamera = true; opts.yawDeg = std::wcstof(next().c_str(), nullptr); }
        else if (a == L"--pitch") { opts.hasCamera = true; opts.pitchDeg = std::wcstof(next().c_str(), nullptr); }
        else if (a == L"--help" || a == L"-h" || a == L"/?")
        {
            LogF("RoughOcean options:\n"
                 "  --w N --h N        window size (default 1600x900)\n"
                 "  --lod 0..3         level of detail: Low/Medium/High/Ultra (default 1)\n"
                 "  --sea 0..9         sea state (default 4)\n"
                 "  --time 0..6        early morning/morning/noon/afternoon/evening/late evening/night\n"
                 "  --frames N         benchmark: run N frames, print avg FPS, exit\n"
                 "  --screenshot PATH  save a PNG at the end of a benchmark run\n"
                 "  --ui               keep the UI visible in benchmark screenshots\n"
                 "  --campos X Y Z     camera position override\n"
                 "  --yaw D --pitch D  camera angles override (degrees)\n"
                 "  --novsync          disable vsync\n"
                 "  --selftest         run the GPU FFT correctness test and exit\n"
                 "  --audiotest        print synthesized ocean-sound levels and exit\n");
            return 0;
        }
    }
    LocalFree(argv);

    if (opts.benchFrames > 0)
        opts.vsync = false;

    try
    {
        App app;
        return app.Run(hInst, opts);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Fatal error: %s\n", e.what());
        MessageBoxA(nullptr, e.what(), "Rough Open Ocean - fatal error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
