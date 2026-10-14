// Config, logging and statistics.
#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>

namespace d3a {

Config g_cfg;
Stats g_stats;

namespace {
std::mutex g_logMutex;
FILE* g_log = nullptr;
LARGE_INTEGER g_qpcFreq, g_qpcStart;

std::wstring ReadIni(const std::wstring& ini, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[256];
    GetPrivateProfileStringW(L"d3d12async", key, def, buf, 256, ini.c_str());
    return buf;
}
}  // namespace

std::wstring ModuleDir(HMODULE m) {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(m, p, MAX_PATH * 2);
    std::wstring s(p, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

void LoadConfig(const std::wstring& dir) {
    std::wstring ini = dir + L"\\d3d12async.ini";
    g_cfg.enabled = _wtoi(ReadIni(ini, L"Enabled", L"1").c_str()) != 0;
    g_cfg.budgetMs = _wtof(ReadIni(ini, L"BudgetMs", L"1").c_str());
    if (g_cfg.budgetMs < 0) g_cfg.budgetMs = 0;
    g_cfg.workers = _wtoi(ReadIni(ini, L"Workers", L"2").c_str());
    std::wstring threads = ReadIni(ini, L"AsyncThreads", L"all");
    g_cfg.allThreads = _wcsicmp(threads.c_str(), L"all") == 0;
    g_cfg.maxQueue = _wtoi(ReadIni(ini, L"MaxQueuedCompiles", L"256").c_str());
    {
        std::wstring b = ReadIni(ini, L"BurstWorkers", L"auto");
        g_cfg.burstWorkers = _wcsicmp(b.c_str(), L"auto") == 0 ? -1 : _wtoi(b.c_str());
        g_cfg.burstQueue = _wtoi(ReadIni(ini, L"BurstQueue", L"16").c_str());
        std::wstring c = ReadIni(ini, L"ComputePipelineMode", L"skip");
        g_cfg.computeMode = _wcsicmp(c.c_str(), L"wait") == 0 ? 1 : _wcsicmp(c.c_str(), L"skip") == 0 ? 2 : 0;
    }
    {
        std::wstring q = ReadIni(ini, L"QueueFullMode", L"placeholder");
        g_cfg.queueFullPlaceholder = _wcsicmp(q.c_str(), L"placeholder") == 0;
        g_cfg.maxPlaceholders = _wtoi(ReadIni(ini, L"MaxPlaceholders", L"512").c_str());
        if (g_cfg.maxPlaceholders < 0) g_cfg.maxPlaceholders = 0;
        if (g_cfg.maxPlaceholders > 8192) g_cfg.maxPlaceholders = 8192;
        g_cfg.workerEfficiency = _wtoi(ReadIni(ini, L"WorkerEfficiencyMode", L"1").c_str()) != 0;
        g_cfg.maxCompilesPerSec = _wtof(ReadIni(ini, L"MaxCompilesPerSecond", L"0").c_str());
        if (g_cfg.maxCompilesPerSec < 0) g_cfg.maxCompilesPerSec = 0;
    }
    g_cfg.workerCpus = ReadIni(ini, L"WorkerCpus", L"upper-half");
    g_cfg.lowPriority = _wtoi(ReadIni(ini, L"LowPriorityWorkers", L"1").c_str()) != 0;
    g_cfg.slowLogMs = _wtof(ReadIni(ini, L"SlowCompileLogMs", L"50").c_str());
    g_cfg.debugDelayMs = _wtof(ReadIni(ini, L"DebugCompileDelayMs", L"0").c_str());
    g_cfg.overlay = _wtoi(ReadIni(ini, L"Overlay", L"1").c_str()) != 0;
    g_cfg.overlayStats = _wtoi(ReadIni(ini, L"OverlayStats", L"1").c_str()) != 0;
    g_cfg.overlayFollowFocus = _wtoi(ReadIni(ini, L"OverlayOnlyWhenFocused", L"0").c_str()) != 0;
    g_cfg.overlayOpacity = _wtoi(ReadIni(ini, L"OverlayOpacity", L"200").c_str());
    if (g_cfg.overlayOpacity < 30) g_cfg.overlayOpacity = 30;
    if (g_cfg.overlayOpacity > 255) g_cfg.overlayOpacity = 255;
    g_cfg.startupSync = _wtoi(ReadIni(ini, L"StartupCompileNormally", L"0").c_str()) != 0;
    g_cfg.startupMinPsos = _wtoi(ReadIni(ini, L"StartupMinPsos", L"64").c_str());
    g_cfg.startupQuietSeconds = _wtoi(ReadIni(ini, L"StartupQuietSeconds", L"10").c_str());
    if (g_cfg.startupQuietSeconds < 1) g_cfg.startupQuietSeconds = 1;
    if (g_cfg.startupQuietSeconds > 60) g_cfg.startupQuietSeconds = 60;
    g_cfg.startupQuietRate = _wtoi(ReadIni(ini, L"StartupQuietRate", L"10").c_str());
    g_cfg.startupBoost = _wtoi(ReadIni(ini, L"StartupBoostCompileThreads", L"1").c_str()) != 0;
    g_cfg.startupCpuBusy = _wtoi(ReadIni(ini, L"StartupCpuBusyPercent", L"80").c_str());
    g_cfg.startupFloodSeconds = _wtoi(ReadIni(ini, L"StartupFloodSeconds", L"10").c_str());
    g_cfg.startupFloodWait = _wtoi(ReadIni(ini, L"StartupFloodWaitSeconds", L"120").c_str());
    g_cfg.startupMinSeconds = _wtoi(ReadIni(ini, L"StartupMinSeconds", L"150").c_str());
    if (g_cfg.startupMinSeconds < 0) g_cfg.startupMinSeconds = 0;
    g_cfg.startupHotkey = _wtoi(ReadIni(ini, L"StartupHotkey", L"1").c_str()) != 0;
    g_cfg.startupMaxSeconds = _wtoi(ReadIni(ini, L"StartupMaxSeconds", L"0").c_str());
    g_cfg.startupFpsCap = _wtoi(ReadIni(ini, L"StartupFpsCap", L"30").c_str());
    if (g_cfg.startupFpsCap < 0) g_cfg.startupFpsCap = 0;
    g_cfg.measureWhenDisabled = _wtoi(ReadIni(ini, L"MeasureWhenDisabled", L"1").c_str()) != 0;
    g_cfg.runLogs = _wtoi(ReadIni(ini, L"RunLogs", L"1").c_str()) != 0;
    g_cfg.runLogsKeepOn = _wtoi(ReadIni(ini, L"RunLogsKeepOn", L"1").c_str());
    g_cfg.runLogsKeepOff = _wtoi(ReadIni(ini, L"RunLogsKeepOff", L"1").c_str());
    g_cfg.hitchMs = _wtoi(ReadIni(ini, L"HitchMs", L"50").c_str());
    g_cfg.settingsPanel = _wtoi(ReadIni(ini, L"SettingsPanel", L"0").c_str()) != 0;
    g_cfg.overlayGraph = _wtoi(ReadIni(ini, L"OverlayGraph", L"1").c_str()) != 0;
    g_cfg.syncUi = _wtoi(ReadIni(ini, L"UiPipelinesCompileNormally", L"1").c_str()) != 0;
    {
        std::wstring m = ReadIni(ini, L"UiPipelineMode", L"wait");
        g_cfg.uiMode = _wcsicmp(m.c_str(), L"sync") == 0 ? 2 : _wcsicmp(m.c_str(), L"async") == 0 ? 0 : 1;
        if (!g_cfg.syncUi) g_cfg.uiMode = 0;  // old switch: UiPipelinesCompileNormally=0
        g_cfg.uiWaitMaxMs = _wtof(ReadIni(ini, L"UiWaitMaxMs", L"2").c_str());
        g_cfg.loadingSync = _wtoi(ReadIni(ini, L"LoadingCompileNormally", L"0").c_str()) != 0;
        g_cfg.loadingGapMs = _wtoi(ReadIni(ini, L"LoadingGapMs", L"500").c_str());
        g_cfg.loadingHoldMs = _wtoi(ReadIni(ini, L"LoadingHoldMs", L"1500").c_str());
        if (g_cfg.loadingHoldMs < 0) g_cfg.loadingHoldMs = 0;
        if (g_cfg.uiWaitMaxMs < 0) g_cfg.uiWaitMaxMs = 0;
    }
    std::wstring lvl = ReadIni(ini, L"LogLevel", L"info");
    const wchar_t* names[] = {L"error", L"warn", L"info", L"perf", L"debug"};
    for (int i = 0; i < 5; ++i)
        if (_wcsicmp(lvl.c_str(), names[i]) == 0) g_cfg.logLevel = (LogLevel)i;
}

void LogInit(const std::wstring& dir) {
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    g_log = _wfopen((dir + L"\\d3d12async.log").c_str(), L"w");
    if (!g_log) {  // game folder not writable (e.g. Program Files): fall back to %TEMP%
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        g_log = _wfopen((std::wstring(tmp) + L"d3d12async.log").c_str(), L"w");
    }
}

void Log(LogLevel lvl, const char* fmt, ...) {
    if ((int)lvl > (int)g_cfg.logLevel || !g_log) return;
    static const char* tag[] = {"ERROR", "WARN", "INFO", "PERF", "DEBUG"};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_qpcStart.QuadPart) / double(g_qpcFreq.QuadPart);
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMutex);
    fprintf(g_log, "[%9.3f] [%s] [tid %lu] %s\n", t, tag[(int)lvl], GetCurrentThreadId(), msg);
    fflush(g_log);
}

namespace {
std::atomic<uint64_t> g_waits[WaitReasonCount];
const char* kWaitNames[WaitReasonCount] = {"QueryInterface", "GetCachedBlob", "bundle", "residency", "pipeline library", "other"};
}

void NoteForcedWait(WaitReason r, void* caller, const char* detail) {
    uint64_t n = ++g_waits[r];
    if (n <= 5 || (n & (n - 1)) == 0) {  // first few, then at powers of two
        char where[300] = "?";
        if (caller) DescribeAddress(caller, where, sizeof where);
        Log(LogLevel::Info, "forced wait #%llu (%s%s%s) from %s", (unsigned long long)n, kWaitNames[r], detail ? ": " : "",
            detail ? detail : "", where);
    }
}

void LogStats(const char* why) {
    Log(LogLevel::Info,
        "stats (%s): graphics PSOs=%llu passthrough=%llu in-budget=%llu placeholders=%llu failed=%llu "
        "draws-skipped=%llu forced-waits=%llu bg-compile-total=%.1f ms startup-normal=%llu ui-normal=%llu",
        why, (unsigned long long)g_stats.psoRequests.load(), (unsigned long long)g_stats.psoSync.load(),
        (unsigned long long)g_stats.psoInBudget.load(), (unsigned long long)g_stats.psoPlaceholders.load(),
        (unsigned long long)g_stats.psoFailed.load(), (unsigned long long)g_stats.drawsSkipped.load(),
        (unsigned long long)g_stats.forcedWaits.load(), g_stats.bgCompileUs.load() / 1000.0,
        (unsigned long long)g_stats.psoStartupSync.load(), (unsigned long long)g_stats.psoUiSync.load());
    if (g_stats.forcedWaits.load())
        Log(LogLevel::Info, "forced waits by reason: QueryInterface=%llu GetCachedBlob=%llu bundle=%llu residency=%llu library=%llu other=%llu",
            (unsigned long long)g_waits[0].load(), (unsigned long long)g_waits[1].load(), (unsigned long long)g_waits[2].load(),
            (unsigned long long)g_waits[3].load(), (unsigned long long)g_waits[4].load(), (unsigned long long)g_waits[5].load());
}

}  // namespace d3a
