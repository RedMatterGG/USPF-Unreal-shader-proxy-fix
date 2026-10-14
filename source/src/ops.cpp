// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
#include "ops.h"
#include "streaming.h"
#include "engineini.h"

#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "icon.h"
#include "ini.h"
#include "net.h"

namespace uspf {
namespace {

const char* kMarkerBegin = "; >>> d3d12async: Unreal PSO precache settings (remove this block to undo)";
const char* kMarkerEnd = "; <<< d3d12async";

std::vector<uint8_t> BundledDll() {
    const void* d;
    size_t n;
    if (!Resource(L"D3D12_DLL", &d, &n)) return {};
    return std::vector<uint8_t>((const uint8_t*)d, (const uint8_t*)d + n);
}

std::string BundledIni() {
    const void* d;
    size_t n;
    if (!Resource(L"D3D12ASYNC_INI", &d, &n)) return {};
    return std::string((const char*)d, n);
}

fs::path DllPath(const Game& g) { return g.target.exe.parent_path() / L"d3d12.dll"; }
fs::path IniPath(const Game& g) { return g.target.exe.parent_path() / L"d3d12async.ini"; }
fs::path BackupPath(const Game& g) { return g.target.exe.parent_path() / L"d3d12.dll.d3d12async.bak"; }

bool Contains(const std::vector<uint8_t>& hay, const char* needle) {
    size_t n = strlen(needle);
    return std::search(hay.begin(), hay.end(), needle, needle + n) != hay.end();
}

// Anti-cheat files in the install root or next to the exe.
std::vector<std::string> FindAntiCheat(const Game& g) {
    static const char* markers[] = {"easyanticheat", "battleye", "beservice", "vgk", "nprotect", "xigncode",
                                    "punkbuster", "eaanticheat", "anticheatexpert", "mhyprot"};
    std::vector<std::string> found;
    std::error_code ec;
    for (auto& dir : {g.target.root, g.target.exe.parent_path()})
        for (auto& f : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            std::string n = Lower(Utf8(f.path().filename()));
            for (auto* m : markers)
                if (n.find(m) != std::string::npos) {
                    std::string p = Utf8(f.path());
                    if (std::find(found.begin(), found.end(), p) == found.end()) found.push_back(p);
                }
        }
    return found;
}

std::string LastError() { return ErrorText(GetLastError()); }

json Fail(const std::string& msg, const std::string& code = "") {
    json j{{"ok", false}, {"error", msg}};
    if (!code.empty()) j["code"] = code;
    return j;
}

bool AccessDenied(DWORD e) { return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_PRIVILEGE_NOT_HELD; }

json WriteFail(const fs::path& p) {
    DWORD e = GetLastError();
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION)
        return Fail("Can't write " + Utf8(p) + ": the file is in use. Close the game first.", "in-use");
    if (AccessDenied(e))
        return Fail("Can't write " + Utf8(p) + ": access denied. Restart USPF as administrator for games in protected folders.",
                    "access-denied");
    return Fail("Can't write " + Utf8(p) + ": " + ErrorText(e));
}

std::vector<std::string> SplitLines(const std::string& t) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string::npos) { if (i < t.size()) out.push_back(t.substr(i)); break; }
        std::string l = t.substr(i, j - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        i = j + 1;
    }
    return out;
}

std::set<std::string> PsoKeys() {
    std::set<std::string> k;
    for (auto& l : EngineIniBlock("full")) {
        size_t eq = l.find('=');
        if (eq != std::string::npos && l[0] != ';') k.insert(Lower(l.substr(0, eq)));
    }
    return k;
}

// The game rewrites Engine.ini when it exits and drops comment lines (our markers), so the block
// is recognised by its keys in [SystemSettings].
bool HasEngineBlock(const std::string& t) {
    return t.find(kMarkerBegin) != std::string::npos || !ReadSystemSettings(t, PsoKeys()).empty();
}

json EngineIniStatus(const Game& g) {
    json dirs = json::array();
    bool any = false;
    for (auto& d : ConfigDirs(g)) {
        auto t = ReadFileText(d / L"Engine.ini");
        bool has = t && HasEngineBlock(*t);
        any |= has;
        dirs.push_back(json{{"dir", Utf8(d)}, {"hasBlock", has}});
    }
    return json{{"configDirs", dirs}, {"enabled", any}};
}

json WriteEngineIni(const Game& g, bool enable, const std::string& loadMode) {
    auto dirs = ConfigDirs(g);
    if (dirs.empty())
        return Fail(enable ? "No Saved\\Config folder found for this game. Start the game once (it creates the folder), then try again."
                           : "No Engine.ini found; nothing to remove.",
                    "no-config");
    json written = json::array();
    for (auto& d : dirs) {
        fs::path p = d / L"Engine.ini";
        std::error_code ec;
        bool exists = fs::exists(p, ec);
        std::string text = exists ? ReadFileText(p).value_or("") : "";
        if (!enable && !HasEngineBlock(text)) continue;
        std::string next = ApplyEngineIniBlock(text, enable, loadMode);
        if (next == text) continue;
        fs::path bak = p;
        bak += L".d3d12async.bak";
        if (exists && !fs::exists(bak, ec)) fs::copy_file(p, bak, ec);
        DWORD attr = GetFileAttributesW(p.c_str());
        bool ro = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY);
        if (ro) SetFileAttributesW(p.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
        bool ok = WriteFileText(p, next);
        if (ro) SetFileAttributesW(p.c_str(), attr);  // games set it read-only on purpose to keep tweaks
        if (!ok) return WriteFail(p);
        written.push_back(Utf8(p));
    }
    return json{{"ok", true}, {"written", written}};
}

uint64_t DirSize(const fs::path& d, size_t* count, const std::vector<std::string>& exts = {}) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::string p = Lower(Utf8(it->path()));
        if (!exts.empty()) {
            if (p.find("\\savegames\\") != std::string::npos || p.find("\\config\\") != std::string::npos) continue;
            bool match = false;
            for (auto& e : exts) match |= p.ends_with(e);
            if (!match) continue;
        }
        if (it->is_regular_file(ec)) { total += it->file_size(ec); ++*count; }
    }
    return total;
}

struct CacheTarget { std::string kind, what; fs::path root; std::vector<std::string> exts; };

std::vector<CacheTarget> CacheTargets(const Game* g) {
    fs::path lad = LocalAppData();
    std::vector<CacheTarget> t = {
        {"driver", "NVIDIA D3D shader cache (all games)", lad / L"NVIDIA" / L"DXCache", {}},
        {"driver", "AMD D3D12 shader cache (all games)", lad / L"AMD" / L"DxcCache", {}},
        {"driver", "AMD D3D11 shader cache (all games)", lad / L"AMD" / L"DxCache", {}},
        {"driver", "Windows D3D shader cache (all games)", lad / L"D3DSCache", {}},
    };
    if (g)
        for (auto& s : SavedDirs(*g)) {
            t.push_back({"game", "Unreal D3D12 pipeline cache (*.ushaderprecache)", s, {".ushaderprecache"}});
            t.push_back({"game", "Recorded PSO list (*.upipelinecache)", s, {".upipelinecache"}});
        }
    return t;
}

int CompareVersions(const std::string& a, const std::string& b) {
    auto parts = [](std::string s) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        std::vector<int> v;
        size_t i = 0;
        while (i < s.size() && v.size() < 4) {
            v.push_back(atoi(s.c_str() + i));
            size_t dot = s.find('.', i);
            if (dot == std::string::npos) break;
            i = dot + 1;
        }
        while (v.size() < 4) v.push_back(0);
        return v;
    };
    auto x = parts(a), y = parts(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

std::mutex g_artMu;
std::set<std::string> g_artTried;  // games whose artwork download was attempted this session

std::string SteamAppIdByName(const std::string& name) {
    std::string q;
    for (unsigned char c : name) {
        if (std::isalnum(c)) q += (char)c;
        else { char b[4]; snprintf(b, 4, "%%%02X", c); q += b; }
    }
    auto r = HttpGet("https://store.steampowered.com/api/storesearch/?term=" + q + "&l=english&cc=US", {}, 1 << 20);
    if (r.status != 200) return "";
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("items")) return "";
    std::string want = NormalizedName(name);
    for (auto& it : j["items"])
        if (it.is_object() && NormalizedName(it.value("name", "")) == want && it.contains("id")) return std::to_string(it["id"].get<long long>());
    return "";
}

}  // namespace

std::vector<std::string> EngineIniBlock(const std::string& loadMode) {
    bool full = loadMode != "fast";
    std::vector<std::string> l = {kMarkerBegin,
                                  "r.PSOPrecaching=1",
                                  "r.PSOPrecache.GlobalShaders=1",
                                  "r.PSOPrecache.Components=1",
                                  std::string("r.PSOPrecaching.WaitForHighPriorityRequestsOnly=") + (full ? "0" : "1"),
                                  "r.PSOPrecache.ProxyCreationWhenPSOReady=1",
                                  "r.PSOPrecache.ProxyCreationDelayStrategy=0",
                                  "r.PSOPrecache.KeepInMemoryUntilUsed=1",
                                  std::string("r.ShaderPipelineCache.ExcludePrecachePSO=") + (full ? "0" : "1")};
    if (full) l.push_back("r.PSOPrecache.Resources=1");
    l.push_back(kMarkerEnd);
    return l;
}

std::string ApplyEngineIniBlock(const std::string& text, bool enable, const std::string& loadMode) {
    std::vector<std::string> block;
    if (enable) {
        auto all = EngineIniBlock(loadMode);
        block.assign(all.begin() + 1, all.end() - 1);  // without the markers: RewriteSystemSettings adds them
    }
    return RewriteSystemSettings(text, kMarkerBegin, kMarkerEnd, PsoKeys(), block);
}

json GameStatus(const Game& g) {
    json s = GameJson(g);
    std::error_code ec;
    std::vector<uint8_t> cur;
    bool hasDll = ReadFileBytes(DllPath(g), cur);
    bool ours = hasDll && Contains(cur, "D3D12Async_GetStats");
    s["dllPresent"] = hasDll;
    s["installed"] = ours;
    s["foreignDll"] = hasDll && !ours;
    s["upToDate"] = ours && cur == BundledDll();
    s["iniPresent"] = fs::exists(IniPath(g), ec);
    s["backupPresent"] = fs::exists(BackupPath(g), ec);
    s["antiCheat"] = FindAntiCheat(g);
    s["engineIni"] = EngineIniStatus(g);
    json saved = json::array();
    for (auto& d : SavedDirs(g)) saved.push_back(Utf8(d));
    s["savedDirs"] = saved;
    s["runsDir"] = Utf8(g.target.exe.parent_path() / L"d3d12async-runs");
    if (s["iniPresent"]) {
        auto t = ReadFileText(IniPath(g));
        bool enabled = true;
        if (t)
            for (auto& k : ParseIni(*t))
                if (Lower(k.key) == "enabled") enabled = k.value != "0";
        s["enabledInIni"] = enabled;
    }
    return s;
}

json Install(const Game& g, bool force, bool engineIni, const std::string& loadMode) {
    if (Lower(Utf8(g.target.exe)).find("\\windowsapps\\") != std::string::npos)
        return Fail("This is a Microsoft Store / Xbox app folder, which is write-protected. Not supported.", "store");
    auto ac = FindAntiCheat(g);
    if (!ac.empty() && !force) {
        json f = Fail("Anti-cheat found. An injected DLL can get your account banned in online play. "
                      "Install only for offline / single-player use, at your own risk.",
                      "anticheat");
        f["files"] = ac;
        return f;
    }
    auto dll = BundledDll();
    if (dll.empty()) return Fail("The bundled d3d12.dll is missing from USPF.exe.");
    std::vector<uint8_t> cur;
    if (ReadFileBytes(DllPath(g), cur) && !Contains(cur, "D3D12Async_GetStats")) {
        if (!force) return Fail("Another d3d12.dll is already in the game folder (another mod or overlay).", "foreign-dll");
        if (!MoveFileExW(DllPath(g).c_str(), BackupPath(g).c_str(), MOVEFILE_REPLACE_EXISTING)) return WriteFail(DllPath(g));
    }
    if (!WriteFileBytes(DllPath(g), dll.data(), dll.size())) return WriteFail(DllPath(g));
    std::error_code ec;
    if (!fs::exists(IniPath(g), ec)) {
        std::string ini = BundledIni();
        if (!WriteFileText(IniPath(g), ini)) return WriteFail(IniPath(g));
    }
    json r{{"ok", true}, {"dll", Utf8(DllPath(g))}};
    if (engineIni && g.target.isUnreal) r["engineIni"] = WriteEngineIni(g, true, loadMode);
    return r;
}

json Uninstall(const Game& g, bool engineIni) {
    std::vector<uint8_t> cur;
    json r{{"ok", true}};
    if (ReadFileBytes(DllPath(g), cur)) {
        if (!Contains(cur, "D3D12Async_GetStats")) return Fail("The d3d12.dll there is not d3d12async; left alone.", "foreign-dll");
        if (!DeleteFileW(DllPath(g).c_str())) return WriteFail(DllPath(g));
        std::error_code ec;
        if (fs::exists(BackupPath(g), ec)) {
            MoveFileExW(BackupPath(g).c_str(), DllPath(g).c_str(), 0);
            r["restoredBackup"] = true;
        }
    }
    DeleteFileW(IniPath(g).c_str());
    DeleteFileW((g.target.exe.parent_path() / L"d3d12async.log").c_str());
    if (engineIni) r["engineIni"] = WriteEngineIni(g, false, "full");
    if (engineIni) r["streaming"] = RemoveStreaming(g);
    return r;
}

json SetEngineIni(const Game& g, bool enable, const std::string& loadMode) { return WriteEngineIni(g, enable, loadMode); }

json IniGet(const Game& g) {
    json schema = json::array();
    for (auto& k : ParseIni(BundledIni())) schema.push_back(json{{"key", k.key}, {"default", k.value}, {"help", k.help}});
    json values = json::object();
    auto t = ReadFileText(IniPath(g));
    if (t)
        for (auto& k : ParseIni(*t)) values[k.key] = k.value;
    return json{{"schema", schema}, {"values", values}, {"present", (bool)t}, {"path", Utf8(IniPath(g))}};
}

json IniSet(const Game& g, const json& values) {
    std::string text = ReadFileText(IniPath(g)).value_or(BundledIni());
    std::vector<std::pair<std::string, std::string>> kv;
    for (auto& [k, v] : values.items()) {
        std::string s = v.is_string() ? v.get<std::string>() : v.dump();
        if (s.find_first_of("\r\n") != std::string::npos) return Fail("Invalid value for " + k);
        kv.push_back({k, s});
    }
    if (!WriteFileText(IniPath(g), UpdateIni(text, kv))) return WriteFail(IniPath(g));
    return json{{"ok", true}};
}

json IniReset(const Game& g) {
    if (!WriteFileText(IniPath(g), BundledIni())) return WriteFail(IniPath(g));
    return json{{"ok", true}};
}

json CacheList(const Game* g) {
    json out = json::array();
    std::error_code ec;
    for (auto& t : CacheTargets(g)) {
        if (!fs::is_directory(t.root, ec)) continue;
        size_t n = 0;
        uint64_t bytes = DirSize(t.root, &n, t.exts);
        if (!n) continue;
        out.push_back(json{{"kind", t.kind}, {"what", t.what}, {"path", Utf8(t.root)}, {"files", n}, {"bytes", bytes}});
    }
    return out;
}

json CacheClear(const Game* g, const json& kinds) {
    std::set<std::string> want;
    for (auto& k : kinds) if (k.is_string()) want.insert(k.get<std::string>());
    size_t ok = 0, locked = 0;
    std::error_code ec;
    for (auto& t : CacheTargets(g)) {
        if (!want.count(t.kind) || !fs::is_directory(t.root, ec)) continue;
        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(t.root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string p = Lower(Utf8(it->path()));
            if (!t.exts.empty()) {  // game Saved folder: only the cache files, never saves or settings
                if (p.find("\\savegames\\") != std::string::npos || p.find("\\config\\") != std::string::npos) continue;
                bool match = false;
                for (auto& e : t.exts) match |= p.ends_with(e);
                if (!match) continue;
            }
            files.push_back(it->path());
        }
        for (auto& f : files) (DeleteFileW(f.c_str()) ? ok : locked)++;
    }
    return json{{"ok", true}, {"deleted", ok}, {"locked", locked}};
}

std::optional<fs::path> ArtFile(const Game& g) {
    fs::path dir = AppDataDir() / L"art";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path jpg = dir / Wide(g.id + ".jpg"), png = dir / Wide(g.id + ".png");
    if (fs::exists(jpg, ec)) return jpg;
    bool tryNet;
    {
        std::lock_guard<std::mutex> lk(g_artMu);
        tryNet = g_artTried.insert(g.id).second;
    }
    std::string appid = g.appid;
    if (!appid.empty())
        if (auto local = SteamLocalArt(appid)) {
            fs::copy_file(*local, jpg, fs::copy_options::overwrite_existing, ec);
            if (!ec) return jpg;
        }
    if (tryNet) {
        if (appid.empty()) appid = SteamAppIdByName(g.name);
        if (!appid.empty())
            for (const char* name : {"library_hero.jpg", "header.jpg"}) {
                auto r = HttpGet("https://cdn.cloudflare.steamstatic.com/steam/apps/" + appid + "/" + name, {}, 16 << 20);
                if (r.status == 200 && r.body.size() > 1000 && (uint8_t)r.body[0] == 0xFF) {
                    WriteFileBytes(jpg, r.body.data(), r.body.size());
                    return jpg;
                }
            }
    }
    if (fs::exists(png, ec)) return png;
    auto icon = ExeIconPng(g.target.exe.wstring());
    if (!icon.empty() && WriteFileBytes(png, icon.data(), icon.size())) return png;
    return std::nullopt;
}

json CheckUpdate() {
    std::string url = std::string("https://api.github.com/repos/") + kGithubOwner + "/" + kGithubRepo + "/releases/latest";
    auto r = HttpGet(url, {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"}, 4 << 20);
    json out{{"current", kAppVersion}, {"releasesUrl", std::string("https://github.com/") + kGithubOwner + "/" + kGithubRepo + "/releases"}};
    if (r.status == 0) { out["status"] = "error"; out["error"] = r.error; return out; }
    if (r.status == 404) { out["status"] = "none"; return out; }  // no repository or no release yet
    if (r.status == 403 || r.status == 429) { out["status"] = "error"; out["error"] = "GitHub rate limit reached; try again later."; return out; }
    if (r.status != 200) { out["status"] = "error"; out["error"] = "GitHub answered HTTP " + std::to_string(r.status); return out; }
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) { out["status"] = "error"; out["error"] = "Unreadable answer from GitHub"; return out; }
    std::string tag = j.value("tag_name", "");
    out["latest"] = tag;
    out["notes"] = j.value("body", "");
    out["pageUrl"] = j.value("html_url", "");
    for (auto& a : j.value("assets", json::array())) {
        std::string n = Lower(a.value("name", ""));
        if (n.ends_with(".exe") && n.find("uspf") != std::string::npos) { out["assetUrl"] = a.value("browser_download_url", ""); break; }
    }
    out["status"] = CompareVersions(tag, kAppVersion) > 0 ? "available" : "current";
    return out;
}

json ApplyUpdate(const std::string& assetUrl) {
    if (!assetUrl.starts_with("https://github.com/") && !assetUrl.starts_with("https://objects.githubusercontent.com/"))
        return Fail("Refusing to download from outside GitHub.");
    auto r = HttpGet(assetUrl, {"Accept: application/octet-stream"}, 64 << 20);
    if (r.status != 200 || r.body.size() < 2 || r.body[0] != 'M' || r.body[1] != 'Z')
        return Fail(r.status ? "Download failed (HTTP " + std::to_string(r.status) + ")" : "Download failed: " + r.error);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::path exe(self), fresh = exe, old = exe;
    fresh += L".new";
    old += L".old";
    if (!WriteFileBytes(fresh, r.body.data(), r.body.size())) return WriteFail(fresh);
    // A running exe can be renamed but not overwritten.
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return WriteFail(exe);
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        return WriteFail(exe);
    }
    ShellExecuteW(nullptr, L"open", exe.c_str(), L"--updated", nullptr, SW_SHOWNORMAL);
    return json{{"ok", true}, {"restart", true}};
}

bool IsElevated() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

}  // namespace uspf
