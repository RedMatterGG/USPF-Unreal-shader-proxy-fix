// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
#include "runs.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>

namespace uspf {
namespace {

fs::path ArchiveDir(const Game& g) {
    fs::path d = AppDataDir() / L"runs" / Wide(g.id);
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// Copies new or grown run files (a run still in progress keeps growing) into the archive.
void SyncArchive(const Game& g) {
    fs::path src = g.target.exe.parent_path() / L"d3d12async-runs";
    fs::path dst = ArchiveDir(g);
    std::error_code ec;
    for (auto& f : fs::directory_iterator(src, fs::directory_options::skip_permission_denied, ec)) {
        if (!f.is_regular_file(ec)) continue;
        std::string n = Utf8(f.path().filename());
        if (n.find("_dll-") == std::string::npos) continue;  // skip runs.txt
        fs::path to = dst / f.path().filename();
        auto srcSize = f.file_size(ec);
        std::error_code ec2;
        if (fs::exists(to, ec2) && fs::file_size(to, ec2) >= srcSize) continue;
        fs::copy_file(f.path(), to, fs::copy_options::overwrite_existing, ec2);
    }
}

double Num(const std::string& s) {
    if (s.empty()) return NAN;
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    return end == s.c_str() ? NAN : v;
}

json JNum(double v) { return std::isfinite(v) ? json(std::round(v * 100.0) / 100.0) : json(nullptr); }

}  // namespace

json ParseCsv(const std::string& text) {
    json out{{"header", json::array()}, {"rows", json::array()}};
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false, first = true;
    auto endRow = [&] {
        row.push_back(cell);
        cell.clear();
        if (first) { for (auto& h : row) out["header"].push_back(Trim(h)); first = false; }
        else if (!(row.size() == 1 && row[0].empty())) out["rows"].push_back(row);
        row.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
                else quoted = false;
            } else cell += c;
        } else if (c == '"') quoted = true;
        else if (c == ',') { row.push_back(cell); cell.clear(); }
        else if (c == '\n') endRow();
        else if (c != '\r') cell += c;
    }
    if (!cell.empty() || !row.empty()) endRow();
    return out;
}

json ParseRunSummary(const std::string& t) {
    json s = json::object();
    std::smatch m;
    static const std::regex rFrames(
        R"(frames: gameplay\s+([\d.]+) s\s+(\d+) frames\s+avg\s+([\d.]+) fps \(\s*([\d.]+) ms\)\s+1% low\s+([\d.]+) fps\s+0\.1% low\s+([\d.]+) fps\s+worst\s+([\d.]+) ms\s+>50ms (\d+)\s+>100ms (\d+)\s+>250ms (\d+))");
    if (std::regex_search(t, m, rFrames)) {
        s["gameplaySeconds"] = Num(m[1]);
        s["frames"] = Num(m[2]);
        s["avgFps"] = Num(m[3]);
        s["avgMs"] = Num(m[4]);
        s["low1"] = Num(m[5]);
        s["low01"] = Num(m[6]);
        s["worstMs"] = Num(m[7]);
        s["over50"] = Num(m[8]);
        s["over100"] = Num(m[9]);
        s["over250"] = Num(m[10]);
    }
    static const std::regex rMax(R"(fps\s+max ([\d.]+) fps)");
    if (std::regex_search(t, m, rMax)) s["maxFps"] = Num(m[1]);
    static const std::regex rHitch(
        R"(hitches\s+(\d+) gameplay frames >= (\d+) ms: (\d+) pipeline compile, (\d+) forced wait, (\d+) several pipeline calls, (\d+) with no)");
    if (std::regex_search(t, m, rHitch)) {
        s["hitches"] = Num(m[1]);
        s["hitchMs"] = Num(m[2]);
        s["hitchCompile"] = Num(m[3]);
        s["hitchForcedWait"] = Num(m[4]);
        s["hitchSeveral"] = Num(m[5]);
        s["hitchOther"] = Num(m[6]);
        s["shaderHitches"] = Num(m[3]) + Num(m[4]) + Num(m[5]);
    }
    static const std::regex rStartup(R"(precompile finished ([\d.]+) s after launch, (\d+) pipeline)");
    if (std::regex_search(t, m, rStartup)) { s["startupSeconds"] = Num(m[1]); s["startupPipelines"] = Num(m[2]); }
    static const std::regex rLength(R"(logged until ([\d.]+) s after launch)");
    if (std::regex_search(t, m, rLength)) s["lengthSeconds"] = Num(m[1]);
    static const std::regex rGame(R"(game\s+(.+?)\r?\n)");
    if (std::regex_search(t, m, rGame)) s["exe"] = Trim(m[1]);
    static const std::regex rSettings(R"(settings\s+(.+?)\r?\n\s+(.+?)\r?\n)");
    if (std::regex_search(t, m, rSettings)) s["settings"] = Trim(m[1].str() + " " + m[2].str());
    static const std::regex rPso(R"(PSOs\s+(\d+) created, (\d+) compiled in background, (\d+) draws skipped)");
    if (std::regex_search(t, m, rPso)) { s["psos"] = Num(m[1]); s["background"] = Num(m[2]); s["drawsSkipped"] = Num(m[3]); }
    return s;
}

json ListRuns(const Game& g) {
    SyncArchive(g);
    fs::path dir = ArchiveDir(g);
    std::vector<std::string> names;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(dir, ec)) {
        std::string n = Utf8(f.path().filename());
        if (n.ends_with("_dll-ON.txt") || n.ends_with("_dll-OFF.txt")) names.push_back(n.substr(0, n.size() - 4));
    }
    std::sort(names.rbegin(), names.rend());
    json out = json::array();
    for (auto& n : names) {
        auto txt = ReadFileText(dir / Wide(n + ".txt"));
        json s = txt ? ParseRunSummary(*txt) : json::object();
        // Older DLLs didn't log max fps: take the best full gameplay second from the per-second CSV.
        if (!s.contains("maxFps"))
            if (auto csv = ReadFileText(dir / Wide(n + ".csv"))) {
                json c = ParseCsv(*csv);
                auto& h = c["header"];
                int iPhase = -1, iFps = -1;
                for (int i = 0; i < (int)h.size(); ++i) {
                    if (h[i] == "phase") iPhase = i;
                    if (h[i] == "fps") iFps = i;
                }
                double best = 0;
                if (iPhase >= 0 && iFps >= 0)
                    for (auto& r : c["rows"])
                        if ((int)r.size() > iFps && r[iPhase] == "game") best = std::max(best, Num(r[iFps].get<std::string>()));
                if (best > 0) s["maxFps"] = best;
            }
        std::error_code e2;
        s["hasFrames"] = fs::exists(dir / Wide(n + "_frames.csv"), e2);
        s["id"] = n;
        s["dllOn"] = n.ends_with("_dll-ON");
        s["stamp"] = n.substr(0, 19);
        out.push_back(s);
    }
    return out;
}

json RunDetail(const Game& g, const std::string& run) {
    if (run.find_first_of("\\/:") != std::string::npos || run.find("..") != std::string::npos) return json{{"error", "bad run id"}};
    SyncArchive(g);
    fs::path dir = ArchiveDir(g);
    json out{{"id", run}};
    if (auto txt = ReadFileText(dir / Wide(run + ".txt"))) out["summary"] = ParseRunSummary(*txt), out["text"] = *txt;

    // Per-second CSV: gameplay seconds only (startup rows have no frame stats).
    json sec{{"t", json::array()}, {"fps", json::array()}, {"avgMs", json::array()}, {"p99Ms", json::array()},
             {"maxMs", json::array()}, {"low1", json::array()}, {"low01", json::array()}, {"cpu", json::array()},
             {"gpu", json::array()}, {"queue", json::array()}, {"placeholders", json::array()}};
    json startupSec{{"t", json::array()}, {"queue", json::array()}};
    if (auto csv = ReadFileText(dir / Wide(run + ".csv"))) {
        json c = ParseCsv(*csv);
        std::map<std::string, int> col;
        for (int i = 0; i < (int)c["header"].size(); ++i) col[c["header"][i]] = i;
        auto get = [&](const json& r, const char* name) -> double {
            auto it = col.find(name);
            if (it == col.end() || it->second >= (int)r.size()) return NAN;
            return Num(r[it->second].get<std::string>());
        };
        for (auto& r : c["rows"]) {
            bool game = col.count("phase") && col["phase"] < (int)r.size() && r[col["phase"]] == "game";
            double t = get(r, "time_s");
            if (!game) { startupSec["t"].push_back(JNum(t)); startupSec["queue"].push_back(JNum(get(r, "compile_queue"))); continue; }
            double fps = get(r, "fps"), p99 = get(r, "p99_ms");
            if (!std::isfinite(fps)) continue;
            double low1 = get(r, "low1_fps"), low01 = get(r, "low01_fps");
            if (!std::isfinite(low1) && p99 > 0) low1 = 1000.0 / p99;  // older logs: P99 frame time only
            sec["t"].push_back(JNum(t));
            sec["fps"].push_back(JNum(fps));
            sec["avgMs"].push_back(JNum(get(r, "avg_ms")));
            sec["p99Ms"].push_back(JNum(p99));
            sec["maxMs"].push_back(JNum(get(r, "max_ms")));
            sec["low1"].push_back(JNum(low1));
            sec["low01"].push_back(JNum(low01));
            sec["cpu"].push_back(JNum(get(r, "game_cpu_pct")));
            sec["gpu"].push_back(JNum(get(r, "gpu_3d_pct")));
            sec["queue"].push_back(JNum(get(r, "compile_queue")));
            sec["placeholders"].push_back(JNum(get(r, "placeholders")));
        }
    }
    out["seconds"] = sec;
    out["startup"] = startupSec;

    // Every gameplay frame. Very long runs are thinned by keeping the slower frame of each pair,
    // so spikes survive.
    json frames{{"t", json::array()}, {"ms", json::array()}};
    if (auto fcsv = ReadFileText(dir / Wide(run + "_frames.csv"))) {
        std::vector<float> ts, ms;
        size_t pos = fcsv->find('\n');
        while (pos != std::string::npos && pos + 1 < fcsv->size()) {
            size_t eol = fcsv->find('\n', pos + 1);
            std::string line = fcsv->substr(pos + 1, (eol == std::string::npos ? fcsv->size() : eol) - pos - 1);
            pos = eol;
            double t, m;
            int gp;
            if (sscanf(line.c_str(), "%lf,%lf,%d", &t, &m, &gp) == 3 && gp == 1) { ts.push_back((float)t); ms.push_back((float)m); }
        }
        size_t step = 1;
        while (ts.size() / step > 150000) step *= 2;
        for (size_t i = 0; i < ts.size(); i += step) {
            size_t worst = i;
            for (size_t j = i + 1; j < std::min(i + step, ts.size()); ++j) if (ms[j] > ms[worst]) worst = j;
            frames["t"].push_back(std::round(ts[worst] * 1000.0) / 1000.0);
            frames["ms"].push_back(std::round(ms[worst] * 100.0) / 100.0);
        }
        out["framesThinned"] = step > 1;
    }
    out["frames"] = frames;

    json hitches = json::array();
    if (auto h = ReadFileText(dir / Wide(run + "_hitches.csv"))) {
        json c = ParseCsv(*h);
        std::map<std::string, int> col;
        for (int i = 0; i < (int)c["header"].size(); ++i) col[c["header"][i]] = i;
        auto str = [&](const json& r, const char* n) -> std::string {
            auto it = col.find(n);
            return it != col.end() && it->second < (int)r.size() ? r[it->second].get<std::string>() : std::string();
        };
        for (auto& r : c["rows"]) {
            hitches.push_back(json{{"t", JNum(Num(str(r, "time_s")))},
                                   {"ms", JNum(Num(str(r, "frame_ms")))},
                                   {"cause", str(r, "cause")},
                                   {"detail", str(r, "detail")},
                                   {"calls", JNum(Num(str(r, "pipeline_calls")))},
                                   {"queue", JNum(Num(str(r, "compile_queue")))}});
        }
    }
    out["hitches"] = hitches;
    return out;
}

}  // namespace uspf
