// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// Cleanup of the "streaming smoothing" Engine.ini settings that USPF 0.2.0 - 0.2.2 could write.
// The feature was removed in 0.3; uninstalling (with the Engine.ini option) still takes those
// lines out of the game's Engine.ini, so nothing is left behind.
#include "streaming.h"

#include "engineini.h"

#include <set>

namespace uspf {
namespace {
const char* kBegin = "; >>> USPF: streaming settings (remove this block to undo)";
const char* kEnd = "; <<< USPF streaming";

const std::vector<std::string>& OldKeys() {
    static const std::vector<std::string> k = {
        "r.Streaming.FramesForFullUpdate",
        "r.Streaming.MaxTempMemoryAllowed",
        "r.Streaming.NumStaticComponentsProcessedPerFrame",
        "r.Streaming.PoolSize",
        "r.Streaming.LimitPoolSizeToVRAM",
        "r.Nanite.Streaming.StreamingPoolSize",
        "s.AsyncLoadingTimeLimit",
        "s.LevelStreamingActorsUpdateTimeLimit",
        "s.LevelStreamingComponentsRegistrationGranularity",
        "s.LevelStreamingComponentsUnregistrationGranularity",
        "s.UnregisterComponentsTimeLimit",
        "wp.Runtime.BlockOnSlowStreaming",
        "s.ForceGCAfterLevelStreamedOut",
        "gc.TimeBetweenPurgingPendingKillObjects",
        "s.IoDispatcherCacheSizeMB",
        "r.Streaming.AmortizeCPUToGPUCopy",
        "r.Streaming.MaxNumTexturesToStreamPerFrame",
        "r.Nanite.Streaming.MaxPendingPages",
        "r.Nanite.Streaming.MaxPageInstallsPerFrame",
        "nanite",
    };
    return k;
}

std::set<std::string> ManagedKeys() {
    std::set<std::string> k;
    for (auto& n : OldKeys()) k.insert(Lower(n));
    return k;
}
}  // namespace

std::vector<std::pair<std::string, std::string>> ReadStreamingBlock(const std::string& text) {
    auto vals = ReadSystemSettings(text, ManagedKeys());
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& n : OldKeys()) {
        auto it = vals.find(Lower(n));
        if (it != vals.end()) out.push_back({n, it->second});
    }
    return out;
}

std::string RemoveStreamingBlock(const std::string& text) { return RewriteSystemSettings(text, kBegin, kEnd, ManagedKeys(), {}); }

json RemoveStreaming(const Game& g) {
    json written = json::array();
    std::error_code ec;
    for (auto& d : ConfigDirs(g)) {
        fs::path p = d / L"Engine.ini";
        if (!fs::exists(p, ec)) continue;
        std::string text = ReadFileText(p).value_or("");
        std::string next = RemoveStreamingBlock(text);
        if (next == text) continue;
        DWORD attr = GetFileAttributesW(p.c_str());
        bool ro = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY);
        if (ro) SetFileAttributesW(p.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
        bool ok = WriteFileText(p, next);
        DWORD err = GetLastError();
        if (ro) SetFileAttributesW(p.c_str(), attr);  // games set it read-only on purpose to keep tweaks
        if (!ok) return json{{"error", "Can't write " + Utf8(p) + ": " + ErrorText(err)}};
        written.push_back(Utf8(p));
    }
    return json{{"ok", true}, {"written", written}};
}

}  // namespace uspf
