// Per-run logs for comparing runs with the layer on and off.
//
// Each game launch that presents frames gets two files in d3d12async-runs\ next to the DLL:
//   <date>_<time>_dll-ON.csv   one row per second: frame rate, frame times, PSO activity, CPU
//   <date>_<time>_dll-ON.txt   summary of the run, rewritten every 2 s while the game runs
// "dll-OFF" runs are launches with Enabled=0 (the DLL only measures). runs.txt lists every
// kept run's summary, newest first, the newest marked LATEST. Older runs beyond
// RunLogsKeepOn / RunLogsKeepOff are deleted when a new run starts.
#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace d3a {
namespace {
constexpr int kBins = 10001;  // 0.1 ms bins up to 1000 ms; the last bin holds everything slower

struct Hist {
    std::vector<uint32_t> bins = std::vector<uint32_t>(kBins, 0);
    uint64_t frames = 0;
    double totalMs = 0, maxMs = 0, minMs = 1e9;
    uint64_t over50 = 0, over100 = 0, over250 = 0;
    void Add(double ms) {
        int b = (int)(ms * 10.0);
        bins[std::min(std::max(b, 0), kBins - 1)]++;
        frames++;
        totalMs += ms;
        maxMs = std::max(maxMs, ms);
        minMs = std::min(minMs, ms);
        over50 += ms > 50;
        over100 += ms > 100;
        over250 += ms > 250;
    }
    double Percentile(double p) const {  // frame time at percentile p (0..1)
        if (!frames) return 0;
        uint64_t want = (uint64_t)std::ceil(p * frames), acc = 0;
        for (int i = 0; i < kBins; ++i) {
            acc += bins[i];
            if (acc >= want) return std::min((i + 0.5) / 10.0, maxMs);
        }
        return maxMs;
    }
};

std::wstring g_dir;
std::once_flag g_started;
std::mutex g_mu;            // frames pending for the writer
struct Frame { float ms; bool startup; double t; };  // t = seconds after launch, at Present
std::vector<Frame> g_pending;
std::vector<Hitch> g_pendingHitches;  // guarded by g_mu
std::wstring g_hitchPath, g_framesPath;
std::wstring g_csvPath, g_txtPath, g_stamp;
bool g_on = true;
LARGE_INTEGER g_freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
int64_t g_t0 = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }();

double Secs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (t.QuadPart - g_t0) / (double)g_freq.QuadPart; }
uint64_t Ft(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

std::vector<std::wstring> List(const wchar_t* pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.rbegin(), out.rend());  // names start with the timestamp: newest first
    return out;
}

void Prune(const wchar_t* tag, int keep) {  // keep counts the run being started
    auto files = List((std::wstring(L"*_dll-") + tag + L".csv").c_str());
    for (size_t i = keep > 0 ? keep - 1 : 0; i < files.size(); ++i) {
        std::wstring base = files[i].substr(0, files[i].size() - 4);
        DeleteFileW((g_dir + L"\\" + base + L".csv").c_str());
        DeleteFileW((g_dir + L"\\" + base + L".txt").c_str());
        DeleteFileW((g_dir + L"\\" + base + L"_hitches.csv").c_str());
        DeleteFileW((g_dir + L"\\" + base + L"_frames.csv").c_str());
    }
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

void WriteIndex() {
    auto files = List(L"*_dll-*.txt");
    FILE* f = _wfopen((g_dir + L"\\runs.txt").c_str(), L"wb");
    if (!f) return;
    fprintf(f, "d3d12async run logs, newest first. dll-ON = layer active, dll-OFF = Enabled=0 (measuring only).\r\n"
               "Per-second details are in the .csv file with the same name; every gameplay hitch is in <name>_hitches.csv;\r\n"
               "every frame time is in <name>_frames.csv.\r\n\r\n");
    int n = 0;
    for (auto& name : files) {
        FILE* s = _wfopen((g_dir + L"\\" + name).c_str(), L"rb");
        if (!s) continue;
        ++n;
        fprintf(f, "==== #%d%s  %s ====\r\n", n, n == 1 ? "  LATEST" : "", Narrow(name).c_str());
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, s)) > 0) fwrite(buf, 1, r, f);
        fclose(s);
        fprintf(f, "\r\n");
    }
    fclose(f);
}

void Line(FILE* f, const char* label, const Hist& h, double secs) {
    if (!h.frames) { fprintf(f, "%-22s no frames\r\n", label); return; }
    double avg = h.totalMs / h.frames;
    fprintf(f, "%-22s %7.1f s  %8llu frames  avg %6.1f fps (%5.2f ms)  1%% low %6.1f fps  0.1%% low %6.1f fps  "
               "worst %7.1f ms  >50ms %llu  >100ms %llu  >250ms %llu\r\n",
            label, secs, (unsigned long long)h.frames, 1000.0 / avg, avg, 1000.0 / h.Percentile(0.99),
            1000.0 / h.Percentile(0.999), h.maxMs, (unsigned long long)h.over50, (unsigned long long)h.over100,
            (unsigned long long)h.over250);
}

// GPU 3D-engine use of this process, from Windows' own performance counters ("GPU Engine",
// the same data Task Manager shows). Read once a second on the run-log thread; nothing is
// injected into the game's GPU work. -1 when the counters aren't available (Wine, old Windows).
class GpuUsage {
public:
    GpuUsage() {
        HMODULE pdh = LoadLibraryW(L"pdh.dll");
        if (!pdh) return;
        open_ = (Open)(void*)GetProcAddress(pdh, "PdhOpenQueryW");
        add_ = (Add)(void*)GetProcAddress(pdh, "PdhAddEnglishCounterW");
        collect_ = (Collect)(void*)GetProcAddress(pdh, "PdhCollectQueryData");
        array_ = (Array)(void*)GetProcAddress(pdh, "PdhGetFormattedCounterArrayW");
        if (!open_ || !add_ || !collect_ || !array_) return;
        if (open_(nullptr, 0, &query_) != 0) return;
        if (add_(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter_) != 0) return;
        collect_(query_);
        swprintf(pidTag_, 32, L"pid_%lu_", GetCurrentProcessId());
        ok_ = true;
    }
    double Sample() {
        if (!ok_ || collect_(query_) != 0) return -1;
        DWORD size = 0, count = 0;
        if (array_(counter_, 0x00000200 /*PDH_FMT_DOUBLE*/, &size, &count, nullptr) != (LONG)0x800007D2 /*PDH_MORE_DATA*/ || !size)
            return -1;
        buf_.resize(size);
        auto* items = reinterpret_cast<Item*>(buf_.data());
        if (array_(counter_, 0x00000200, &size, &count, items) != 0) return -1;
        double sum = 0;
        for (DWORD i = 0; i < count; ++i)
            if (items[i].status == 0 && wcsstr(items[i].name, pidTag_) && wcsstr(items[i].name, L"engtype_3D")) sum += items[i].value;
        return sum > 100 ? 100 : sum;
    }
private:
    struct Item { LPWSTR name; DWORD status; double value; };  // PDH_FMT_COUNTERVALUE_ITEM_W with a double
    using Open = LONG(WINAPI*)(LPCWSTR, DWORD_PTR, HANDLE*);
    using Add = LONG(WINAPI*)(HANDLE, LPCWSTR, DWORD_PTR, HANDLE*);
    using Collect = LONG(WINAPI*)(HANDLE);
    using Array = LONG(WINAPI*)(HANDLE, DWORD, DWORD*, DWORD*, void*);
    Open open_ = nullptr; Add add_ = nullptr; Collect collect_ = nullptr; Array array_ = nullptr;
    HANDLE query_ = nullptr, counter_ = nullptr;
    wchar_t pidTag_[32] = {};
    std::vector<uint8_t> buf_;
    bool ok_ = false;
};

uint64_t ReadBytes() {
    IO_COUNTERS io{};
    return GetProcessIoCounters(GetCurrentProcess(), &io) ? io.ReadTransferCount : 0;
}

void Writer() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    FILE* csv = _wfopen(g_csvPath.c_str(), L"wb");
    if (!csv) { Log(LogLevel::Warn, "run log: cannot write %ls", g_csvPath.c_str()); return; }
    fprintf(csv, "time_s,phase,fps_cap,frames,fps,avg_ms,p99_ms,max_ms,frames_over_50ms,psos_created,compute_created,library_loads,"
                 "ms_in_pso_calls,placeholders,draws_skipped,compile_queue,forced_waits,game_cpu_pct,system_cpu_pct,"
                 "disk_read_mb_s,gpu_3d_pct,min_ms,p999_ms,low1_fps,low01_fps\r\n");
    fflush(csv);
    FILE* hcsv = _wfopen(g_hitchPath.c_str(), L"wb");
    if (hcsv) {
        fprintf(hcsv, "time_s,frame_ms,cause,detail,longest_call_ms,longest_call_thread,pipeline_calls,ms_in_pipeline_calls,"
                      "new_placeholders,draws_skipped,forced_waits,compile_queue\r\n");
        fflush(hcsv);
    }
    // Every frame: seconds after launch, frame time, and whether it counts as gameplay (0 = startup
    // precompile or loading screen, excluded from the stats). Feeds the USPF app's frame-time chart.
    FILE* fcsv = _wfopen(g_framesPath.c_str(), L"wb");
    if (fcsv) { fprintf(fcsv, "time_s,frame_ms,gameplay\r\n"); fflush(fcsv); }
    double bestSecFps = 0, bestSecAt = 0;  // highest one-second fps in gameplay
    std::vector<Hitch> allHitches;  // for the summary
    Hist after;  // gameplay only: frames during the startup precompile are not counted (capped / loading screen)
    double afterSecs = 0, firstFrame = -1;
    uint64_t last[7] = {g_stats.psoCreates, g_stats.psoPlaceholders, g_stats.drawsSkipped, g_stats.forcedWaits,
                        g_stats.computeCreates, g_stats.libraryLoads, g_stats.callUs};
    FILETIME a, b, k, u, si, sk, su;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    GetSystemTimes(&si, &sk, &su);
    uint64_t pPrev = Ft(k) + Ft(u), sIdle = Ft(si), sBusy = Ft(sk) + Ft(su);
    SYSTEM_INFO sysi;
    GetSystemInfo(&sysi);
    double prevT = Secs();
    GpuUsage gpu;
    uint64_t readPrev = ReadBytes();
    for (int tick = 1;; ++tick) {
        Sleep(1000);
        std::vector<Frame> frames;
        std::vector<Hitch> hitches;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            frames.swap(g_pending);
            hitches.swap(g_pendingHitches);
        }
        static const char* kKind[] = {"not-pipeline", "compile", "forced-wait", "several-calls"};
        for (auto& h : hitches) {
            if (hcsv)
                fprintf(hcsv, "%.2f,%.1f,%s,\"%s\",%.1f,%s,%u,%.1f,%llu,%llu,%llu,%llu\r\n", h.atSeconds, h.frameMs,
                        kKind[(int)h.kind], DescribeHitch(h).c_str(), h.longestCallMs, h.calls ? (h.longestOnRenderThread ? "command-recording" : "game-task") : "",
                        h.calls, h.callMs, (unsigned long long)h.placeholders, (unsigned long long)h.skipped,
                        (unsigned long long)h.forcedWaits, (unsigned long long)h.queue);
            if (allHitches.size() < 100000) allHitches.push_back(h);
        }
        if (hcsv && !hitches.empty()) fflush(hcsv);
        double t = Secs(), dt = t - prevT;
        prevT = t;
        bool inStartup = StartupPhase();
        Hist sec;
        for (auto& fr : frames)
            if (!fr.startup) { sec.Add(fr.ms); after.Add(fr.ms); }
        if (fcsv && !frames.empty()) {
            for (auto& fr : frames) fprintf(fcsv, "%.3f,%.2f,%d\r\n", fr.t, fr.ms, fr.startup ? 0 : 1);
            fflush(fcsv);
        }
        if (!inStartup) afterSecs += dt;
        if (firstFrame < 0 && !frames.empty()) firstFrame = t;
        uint64_t now[7] = {g_stats.psoCreates, g_stats.psoPlaceholders, g_stats.drawsSkipped, g_stats.forcedWaits,
                           g_stats.computeCreates, g_stats.libraryLoads, g_stats.callUs};
        GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
        GetSystemTimes(&si, &sk, &su);
        uint64_t p = Ft(k) + Ft(u), idle = Ft(si), busy = Ft(sk) + Ft(su);  // system kernel time includes idle
        double gameCpu = (p - pPrev) / (dt * 1e7 * sysi.dwNumberOfProcessors) * 100.0;
        double sysCpu = busy - sBusy ? (1.0 - (double)(idle - sIdle) / (busy - sBusy)) * 100.0 : 0;
        pPrev = p; sIdle = idle; sBusy = busy;
        uint64_t readNow = ReadBytes();
        double readMb = (readNow - readPrev) / (dt * 1048576.0);
        readPrev = readNow;
        double gpuPct = gpu.Sample();
        char gpuCol[16] = "";
        if (gpuPct >= 0) snprintf(gpuCol, sizeof gpuCol, "%.0f", gpuPct);
        char fr[160] = ",,,,,";  // startup seconds: frame columns left empty
        if (!(inStartup && sec.frames == 0))
            snprintf(fr, sizeof fr, "%llu,%.1f,%.2f,%.2f,%.2f,%llu", (unsigned long long)sec.frames, sec.frames / dt,
                     sec.frames ? sec.totalMs / sec.frames : 0.0, sec.Percentile(0.99), sec.maxMs, (unsigned long long)sec.over50);
        char ext[96] = ",,,";  // min_ms, p999_ms, low1_fps, low01_fps (empty without gameplay frames)
        if (sec.frames) {
            snprintf(ext, sizeof ext, "%.2f,%.2f,%.1f,%.1f", sec.minMs, sec.Percentile(0.999), 1000.0 / sec.Percentile(0.99),
                     1000.0 / sec.Percentile(0.999));
            // a full second of gameplay only: a partial one (startup ending mid-second) would overstate it
            if (!inStartup && dt > 0.5 && sec.frames / dt > bestSecFps) { bestSecFps = sec.frames / dt; bestSecAt = t; }
        }
        fprintf(csv, "%.1f,%s,%d,%s,%llu,%llu,%llu,%.1f,%llu,%llu,%llu,%llu,%.0f,%.0f,%.1f,%s,%s\r\n", t, inStartup ? "startup" : "game",
                StartupCapping() ? g_cfg.startupFpsCap : 0, fr, (unsigned long long)(now[0] - last[0]),
                (unsigned long long)(now[4] - last[4]), (unsigned long long)(now[5] - last[5]), (now[6] - last[6]) / 1000.0,
                (unsigned long long)(now[1] - last[1]), (unsigned long long)(now[2] - last[2]), (unsigned long long)PoolPending(),
                (unsigned long long)(now[3] - last[3]), gameCpu, sysCpu, readMb, gpuCol, ext);
        fflush(csv);
        for (int i = 0; i < 7; ++i) last[i] = now[i];
        if (tick % 2) continue;

        FILE* f = _wfopen(g_txtPath.c_str(), L"wb");
        if (!f) continue;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fprintf(f, "run       %s   dll-%s%s\r\n", Narrow(g_stamp).c_str(), g_on ? "ON" : "OFF",
                g_on ? "" : " (Enabled=0: pass-through, measuring only)");
        fprintf(f, "game      %s\r\n", Narrow(exe).c_str());
        fprintf(f, "settings  BudgetMs=%g Workers=%d WorkerCpus=%s AsyncThreads=%s StartupCompileNormally=%d "
                   "StartupQuietSeconds=%d StartupCpuBusyPercent=%d StartupFpsCap=%d StartupBoostCompileThreads=%d "
                   "UiPipelinesCompileNormally=%d\r\n",
                g_cfg.budgetMs, g_cfg.workers, Narrow(g_cfg.workerCpus).c_str(), g_cfg.allThreads ? "all" : "recording",
                (int)g_cfg.startupSync, g_cfg.startupQuietSeconds, g_cfg.startupCpuBusy, g_cfg.startupFpsCap,
                (int)g_cfg.startupBoost, (int)g_cfg.syncUi);
        fprintf(f, "          ComputePipelineMode=%s MaxQueuedCompiles=%d QueueFullMode=%s MaxPlaceholders=%d WorkerEfficiencyMode=%d "
                   "MaxCompilesPerSecond=%g\r\n",
                g_cfg.computeMode == 2 ? "skip" : g_cfg.computeMode == 1 ? "wait" : "sync", g_cfg.maxQueue,
                g_cfg.queueFullPlaceholder ? "placeholder" : "compile", g_cfg.maxPlaceholders, (int)g_cfg.workerEfficiency,
                g_cfg.maxCompilesPerSec);
        fprintf(f, "length    logged until %.0f s after launch; first frame at %.1f s\r\n", t, firstFrame);
        double end = StartupEndedAt();
        if (end >= 0)
            fprintf(f, "startup   precompile finished %.1f s after launch, %llu pipeline creates/loads; "
                       "frame stats not recorded during it%s\r\n",
                    end, (unsigned long long)StartupPsos(),
                    StartupWarm() ? " (warm launch: the driver cache already had this game's pipelines)" : "");
        else
            fprintf(f, "startup   precompile still running (%llu pipeline creates/loads so far); frame stats start after it\r\n",
                    (unsigned long long)StartupPsos());
        Line(f, "frames: gameplay", after, afterSecs);
        if (after.frames)
            fprintf(f, "fps       max %.1f fps (best one-second average, at %.0f s); fastest single frame %.2f ms\r\n", bestSecFps,
                    bestSecAt, after.minMs);
        {
            uint64_t byKind[4] = {}, compileByCause[kCallCauses] = {};
            for (auto& h : allHitches) {
                byKind[(int)h.kind]++;
                if (h.kind == HitchKind::Compile || h.kind == HitchKind::ForcedWait) compileByCause[(int)h.longestCause]++;
            }
            fprintf(f, "hitches   %llu gameplay frames >= %d ms: %llu pipeline compile, %llu forced wait, %llu several pipeline calls, "
                       "%llu with no pipeline compile (streaming / CPU / GPU)\r\n",
                    (unsigned long long)allHitches.size(), g_cfg.hitchMs, (unsigned long long)byKind[1], (unsigned long long)byKind[2],
                    (unsigned long long)byKind[3], (unsigned long long)byKind[0]);
            for (int c = 0; c < kCallCauses; ++c)
                if (compileByCause[c])
                    fprintf(f, "            compile hitches from: %s: %llu\r\n", CauseName((CallCause)c), (unsigned long long)compileByCause[c]);
            std::vector<const Hitch*> worst;
            for (auto& h : allHitches) worst.push_back(&h);
            std::sort(worst.begin(), worst.end(), [](const Hitch* a, const Hitch* b) { return a->frameMs > b->frameMs; });
            for (size_t i = 0; i < worst.size() && i < 5; ++i)
                fprintf(f, "            worst #%zu: %.0f ms at %.1f s - %s\r\n", i + 1, worst[i]->frameMs, worst[i]->atSeconds,
                        DescribeHitch(*worst[i]).c_str());
        }
        fprintf(f, "game-thread time in pipeline calls during gameplay, by cause:");
        bool any = false;
        for (int c = 0; c < kCallCauses; ++c) {
            uint64_t us = g_stats.callUsByCause[c].load();
            if (c == (int)CallCause::Startup || c == (int)CallCause::Loading || us < 1000) continue;
            fprintf(f, "%s %s %.1f s", any ? "," : "", CauseName((CallCause)c), us / 1e6);
            any = true;
        }
        fprintf(f, "%s\r\n", any ? "" : " none");
        fprintf(f, "pipelines %llu graphics created, %llu compute created, %llu library loads, %.1f s inside those calls\r\n",
                (unsigned long long)g_stats.psoCreates.load(), (unsigned long long)g_stats.computeCreates.load(),
                (unsigned long long)g_stats.libraryLoads.load(), g_stats.callUs.load() / 1e6);
        if (g_cfg.computeMode == 2)
            fprintf(f, "EXPERIMENTAL ComputePipelineMode=skip: %llu compute dispatches skipped\r\n",
                    (unsigned long long)g_stats.dispatchesSkipped.load());
        if (g_cfg.loadingSync) {
            LoadingStats ls = GetLoadingStats();
            fprintf(f, "loading   %llu loading screens detected (%.1f s without frames): %llu pipelines compiled normally in them; "
                       "their frames are not in the gameplay stats\r\n",
                    (unsigned long long)ls.periods, ls.seconds, (unsigned long long)g_stats.psoLoadingSync.load());
        }
        if (g_stats.uiWaits.load())
            fprintf(f, "UI        %llu draws waited for a UI pipeline (%llu cut short by UiWaitMaxMs=%g and skipped)\r\n",
                    (unsigned long long)g_stats.uiWaits.load(), (unsigned long long)g_stats.uiWaitTimeouts.load(), g_cfg.uiWaitMaxMs);
        if (g_cfg.queueFullPlaceholder || g_stats.psoOverflow.load() || g_cfg.maxCompilesPerSec > 0 || g_cfg.workerEfficiency)
            fprintf(f, "queue     longest compile queue in gameplay %llu; %llu placeholders past MaxQueuedCompiles "
                       "(QueueFullMode=placeholder, MaxPlaceholders=%d); workers waited %.1f s for MaxCompilesPerSecond\r\n",
                    (unsigned long long)g_stats.queuePeak.load(), (unsigned long long)g_stats.psoOverflow.load(), g_cfg.maxPlaceholders,
                    g_stats.rateLimitedUs.load() / 1e6);
        fprintf(f, "PSOs      %llu created, %llu compiled in background, %llu draws skipped, %llu forced waits, %llu failed\r\n",
                (unsigned long long)g_stats.psoCreates.load(), (unsigned long long)g_stats.psoPlaceholders.load(),
                (unsigned long long)g_stats.drawsSkipped.load(), (unsigned long long)g_stats.forcedWaits.load(),
                (unsigned long long)g_stats.psoFailed.load());
        fclose(f);
        WriteIndex();
    }
}

void Start() {
    if (g_dir.empty()) return;
    g_on = g_cfg.enabled;
    int keep = g_on ? g_cfg.runLogsKeepOn : g_cfg.runLogsKeepOff;
    if (keep <= 0) return;
    CreateDirectoryW(g_dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04u-%02u-%02u_%02u-%02u-%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_stamp = stamp;
    Prune(g_on ? L"ON" : L"OFF", keep);
    std::wstring base = g_dir + L"\\" + g_stamp + (g_on ? L"_dll-ON" : L"_dll-OFF");
    g_csvPath = base + L".csv";
    g_hitchPath = base + L"_hitches.csv";
    g_framesPath = base + L"_frames.csv";
    g_txtPath = base + L".txt";
    Log(LogLevel::Info, "run log: %ls.csv / .txt", base.c_str());
    std::thread(Writer).detach();
}
}  // namespace

void RunLogSetDir(const std::wstring& dllDir) {
    std::wstring d = dllDir + L"\\d3d12async-runs";
    CreateDirectoryW(d.c_str(), nullptr);
    std::wstring probe = d + L"\\.write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {  // game folder not writable: use %TEMP%
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        d = std::wstring(tmp) + L"d3d12async-runs";
    } else {
        CloseHandle(h);
    }
    g_dir = d;
}

double RunSeconds() { return Secs(); }

void RunLogHitch(const Hitch& h) {
    if (!g_cfg.runLogs || g_hitchPath.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pendingHitches.size() < 10000) g_pendingHitches.push_back(h);
}

void RunLogFrame(double frameMs, bool loading) {
    if (!g_cfg.runLogs) return;
    std::call_once(g_started, Start);  // first frame starts the run (probe processes never get a file)
    if (g_csvPath.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pending.size() < 100000) g_pending.push_back({(float)frameMs, StartupPhase() || loading, Secs()});
}

}  // namespace d3a
