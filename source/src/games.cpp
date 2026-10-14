// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
#include "games.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>

namespace uspf {
namespace {

std::mutex g_mu;
std::vector<Game> g_games;
bool g_scanned = false;

fs::path StoreFile() { return AppDataDir() / L"games.json"; }

json LoadStore() {
    if (auto t = ReadFileText(StoreFile())) {
        json j = json::parse(*t, nullptr, false);
        if (j.is_object()) return j;
    }
    return json{{"manual", json::array()}, {"hidden", json::array()}};
}

void SaveStore(const json& j) { WriteFileText(StoreFile(), j.dump(2)); }

// Engine-wide names some exes carry in their version info; never a game's own folder.
const std::set<std::string> kGenericNames = {"unrealengine", "unrealgame", "ue4game", "ue5game", "bootstrappackagedgame",
                                             "game", "epicgames", "launcher"};

std::string IdFor(const fs::path& exe) { return Hex64(Fnv1a(Lower(Utf8(exe.parent_path())))); }

std::optional<Game> MakeGame(const fs::path& root, const std::string& name, const std::string& source, const std::string& appid,
                             const std::string& preferStem = "") {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    auto t = FindUnrealInRoot(root, preferStem);
    if (!t) return std::nullopt;  // only Unreal games are listed automatically
    Game g;
    g.target = *t;
    g.id = IdFor(t->exe);
    g.name = name;
    g.source = source;
    g.appid = appid;
    return g;
}

// ---- Steam ----------------------------------------------------------------------------------

std::optional<fs::path> SteamDir() {
    if (auto p = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath")) return fs::path(*p);
    if (auto p = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath")) return fs::path(*p);
    if (auto p = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath")) return fs::path(*p);
    return std::nullopt;
}

std::set<std::string> g_steamLibsSeen;  // guarded by g_mu (Games() holds it)

// <lib>\steamapps: appmanifest_*.acf give each game's name, install folder and appid.
void ScanSteamLibrary(const fs::path& apps, std::vector<Game>& out) {
    std::string key = Lower(Utf8(apps.lexically_normal()));
    if (!g_steamLibsSeen.insert(key).second) return;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(apps, fs::directory_options::skip_permission_denied, ec)) {
        std::string fn = Lower(Utf8(f.path().filename()));
        if (!fn.starts_with("appmanifest_") || !fn.ends_with(".acf")) continue;
        auto t = ReadFileText(f.path());
        if (!t) continue;
        json m = ParseVdf(*t);
        if (!m.contains("appstate")) continue;
        json& a = m["appstate"];
        std::string name = a.value("name", ""), dir = a.value("installdir", ""), appid = a.value("appid", "");
        if (dir.empty()) continue;
        if (auto g = MakeGame(apps / L"common" / PathU8(dir), name.empty() ? dir : name, "steam", appid)) out.push_back(*g);
    }
}

void ScanSteam(std::vector<Game>& out) {
    auto steam = SteamDir();
    if (!steam) return;
    std::vector<fs::path> libs{*steam};
    if (auto t = ReadFileText(*steam / L"steamapps" / L"libraryfolders.vdf")) {
        json v = ParseVdf(*t);
        if (v.contains("libraryfolders") && v["libraryfolders"].is_object())
            for (auto& [k, lib] : v["libraryfolders"].items()) {
                if (lib.is_object() && lib.contains("path") && lib["path"].is_string()) libs.push_back(PathU8(lib["path"]));
                else if (lib.is_string()) libs.push_back(PathU8(lib.get<std::string>()));  // very old format: "1" "D:\\Steam"
            }
    }
    for (auto& lib : libs) ScanSteamLibrary(lib / L"steamapps", out);
}

// ---- Epic -----------------------------------------------------------------------------------

void ScanEpic(std::vector<Game>& out) {
    fs::path data;
    if (auto p = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Epic Games\\EpicGamesLauncher", L"AppDataPath"))
        data = fs::path(*p);
    else
        data = KnownFolder(0x0023 /*CSIDL_COMMON_APPDATA*/) / L"Epic" / L"EpicGamesLauncher" / L"Data";
    std::error_code ec;
    for (auto& f : fs::directory_iterator(data / L"Manifests", fs::directory_options::skip_permission_denied, ec)) {
        if (Lower(Utf8(f.path().extension())) != ".item") continue;
        auto t = ReadFileText(f.path());
        if (!t) continue;
        json m = json::parse(*t, nullptr, false);
        if (!m.is_object() || m.value("bIsIncompleteInstall", false)) continue;
        std::string loc = m.value("InstallLocation", ""), name = m.value("DisplayName", "");
        std::string launch = m.value("LaunchExecutable", "");
        if (loc.empty()) continue;
        std::string stem = Utf8(PathU8(launch).stem());
        if (auto g = MakeGame(PathU8(loc), name, "epic", "", stem)) out.push_back(*g);
    }
}

// ---- GOG ------------------------------------------------------------------------------------

void ScanGog(std::vector<Game>& out) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = 256;
        if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring key = std::wstring(L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\") + sub;
        auto path = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"path");
        auto name = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"gameName");
        auto exe = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"exe");
        if (!path) continue;
        std::string stem = exe ? Utf8(fs::path(*exe).stem()) : "";
        if (auto g = MakeGame(fs::path(*path), name ? Utf8(*name) : Utf8(fs::path(*path).filename()), "gog", "", stem)) out.push_back(*g);
    }
    RegCloseKey(k);
}

// ---- game folders -------------------------------------------------------------------------

const std::set<std::string> kSkipDirs = {"engine", "_redist", "redist", "__installer", "_installer", "support", "uninstall",
                                         "temp-dodi", "_crack", "_original files", "directx", "commonredist", "mods",
                                         "$recycle.bin", "system volume information", "windows", "windowsapps"};

std::vector<fs::path> DefaultLibraryFolders() {
    std::vector<fs::path> out;
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        fs::path r(root);
        for (const wchar_t* sub : {L"Games", L"Games\\steamapps\\common", L"Games\\SteamLibrary\\steamapps\\common",
                                   L"SteamLibrary\\steamapps\\common", L"steamapps\\common", L"Steam\\steamapps\\common",
                                   L"Program Files (x86)\\Steam\\steamapps\\common", L"Program Files\\Steam\\steamapps\\common",
                                   L"Epic Games", L"Program Files\\Epic Games", L"GOG Games", L"Program Files (x86)\\GOG Galaxy\\Games",
                                   L"Games\\Epic Games", L"Games\\GOG Games"})
            out.push_back(r / sub);
    }
    return out;
}

std::vector<fs::path> UserLibraryFolders(const json& store) {
    std::vector<fs::path> out;
    for (auto& f : store.value("folders", json::array()))
        if (f.is_string()) out.push_back(PathU8(f.get<std::string>()));
    return out;
}

std::string NameFromExe(const fs::path& exe) {
    for (auto& n : ExeVersionNames(exe)) {
        std::string k = NormalizedName(n);
        if (k.size() >= 3 && !kGenericNames.count(k)) return n;
    }
    return "";
}

// A folder of game folders: every child that is an Unreal install becomes a game; a child that
// isn't is searched one level deeper (C:\Games\Gears of War E-Day\GoWEDay\...).
void ScanFolder(const fs::path& lib, std::vector<Game>& out) {
    std::error_code ec;
    if (!fs::is_directory(lib, ec)) return;
    // A Steam library folder (...\steamapps\common): its manifests have the real names.
    if (Lower(Utf8(lib.filename())) == "common" && Lower(Utf8(lib.parent_path().filename())) == "steamapps")
        ScanSteamLibrary(lib.parent_path(), out);
    if (fs::is_directory(lib / L"steamapps", ec)) ScanSteamLibrary(lib / L"steamapps", out);
    for (auto& c : fs::directory_iterator(lib, fs::directory_options::skip_permission_denied, ec)) {
        if (!c.is_directory(ec)) continue;
        std::string cname = Utf8(c.path().filename());
        if (kSkipDirs.count(Lower(cname)) || Lower(cname) == "steamapps" || Lower(cname) == "steamlibrary") continue;
        auto add = [&](const fs::path& root) {
            auto t = FindUnrealInRoot(root, Utf8(c.path().filename()));
            if (!t) return false;
            Game g;
            g.target = *t;
            g.id = IdFor(t->exe);
            g.name = CleanFolderName(cname);
            if (NormalizedName(g.name).size() < 2) g.name = NameFromExe(t->exe);
            if (g.name.empty()) g.name = t->project;
            g.source = "local";
            out.push_back(g);
            return true;
        };
        if (add(c.path())) continue;
        int n = 0;
        std::error_code ec2;
        for (auto& gc : fs::directory_iterator(c.path(), fs::directory_options::skip_permission_denied, ec2)) {
            if (++n > 40) break;  // a folder of unrelated stuff: don't crawl it
            if (gc.is_directory(ec2) && !kSkipDirs.count(Lower(Utf8(gc.path().filename())))) add(gc.path());
        }
    }
}

std::optional<Game> ManualGame(const fs::path& exe) {
    auto t = ResolveTarget(exe);
    if (!t) return std::nullopt;
    Game g;
    g.target = *t;
    g.id = IdFor(t->exe);
    auto names = ExeVersionNames(t->exe);
    g.name = !names.empty() && !names[0].empty() ? names[0] : Utf8(t->isUnreal ? t->root.filename() : t->exe.stem());
    g.source = "manual";
    return g;
}


}  // namespace

json ParseVdf(const std::string& s) {
    size_t i = 0;
    auto skip = [&] {
        for (;;) {
            while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            break;
        }
    };
    auto token = [&](std::string& out) -> int {  // 0 = end, 1 = string, 2 = '{', 3 = '}'
        skip();
        if (i >= s.size()) return 0;
        if (s[i] == '{') { ++i; return 2; }
        if (s[i] == '}') { ++i; return 3; }
        out.clear();
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char c = s[++i];
                    out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
                    ++i;
                    continue;
                }
                out += s[i++];
            }
            ++i;
        } else {
            while (i < s.size() && !std::isspace((unsigned char)s[i]) && s[i] != '{' && s[i] != '}' && s[i] != '"') out += s[i++];
        }
        return 1;
    };
    std::function<json()> object = [&]() -> json {
        json o = json::object();
        std::string k, v;
        for (;;) {
            int t = token(k);
            if (t == 0 || t == 3) return o;
            if (t != 1) continue;
            size_t save = i;
            int t2 = token(v);
            std::string key = Lower(k);
            if (t2 == 1) o[key] = v;
            else if (t2 == 2) o[key] = object();
            else { i = save; return o; }
        }
    };
    return object();
}

std::vector<Game> Games(bool rescan) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_scanned && !rescan) return g_games;
    std::vector<Game> all;
    g_steamLibsSeen.clear();
    json store = LoadStore();
    ScanSteam(all);
    ScanEpic(all);
    ScanGog(all);
    for (auto& f : UserLibraryFolders(store)) ScanFolder(f, all);
    for (auto& f : DefaultLibraryFolders()) ScanFolder(f, all);
    for (auto& e : store["manual"])
        if (e.is_string())
            if (auto g = ManualGame(PathU8(e.get<std::string>()))) all.push_back(*g);
    std::set<std::string> hidden;
    for (auto& h : store["hidden"]) if (h.is_string()) hidden.insert(h.get<std::string>());
    std::map<std::string, Game> byId;
    for (auto& g : all) {
        if (hidden.count(g.id)) continue;
        auto it = byId.find(g.id);
        if (it == byId.end()) byId[g.id] = g;
        else {
            if (it->second.appid.empty() && !g.appid.empty()) it->second.appid = g.appid;  // keep the store entry's appid
            if (it->second.source == "local" && g.source == "steam") { it->second.name = g.name; it->second.source = g.source; }
        }
    }
    g_games.clear();
    for (auto& [id, g] : byId) g_games.push_back(g);
    std::sort(g_games.begin(), g_games.end(), [](const Game& a, const Game& b) { return Lower(a.name) < Lower(b.name); });
    g_scanned = true;
    return g_games;
}

std::optional<Game> FindGame(const std::string& id) {
    for (auto& g : Games(false))
        if (g.id == id) return g;
    return std::nullopt;
}

json GameJson(const Game& g) {
    return json{{"id", g.id},
                {"name", g.name},
                {"source", g.source},
                {"appid", g.appid},
                {"exe", Utf8(g.target.exe)},
                {"exeDir", Utf8(g.target.exe.parent_path())},
                {"root", Utf8(g.target.root)},
                {"project", g.target.project},
                {"isUnreal", g.target.isUnreal}};
}

std::optional<Game> AddGame(const fs::path& exe, std::string* error) {
    auto g = ManualGame(exe);
    if (!g) { if (error) *error = "That file can't be read."; return std::nullopt; }
    json store = LoadStore();
    std::string p = Utf8(g->target.exe);
    bool have = false;
    for (auto& e : store["manual"]) have |= e.is_string() && Lower(e.get<std::string>()) == Lower(p);
    if (!have) store["manual"].push_back(p);
    json hidden = json::array();
    for (auto& h : store["hidden"]) if (h != g->id) hidden.push_back(h);
    store["hidden"] = hidden;
    SaveStore(store);
    Games(true);
    return g;
}

void HideGame(const std::string& id) {
    json store = LoadStore();
    json manual = json::array();
    for (auto& e : store["manual"])
        if (e.is_string()) {
            auto g = ManualGame(PathU8(e.get<std::string>()));
            if (!g || g->id != id) manual.push_back(e);
        }
    store["manual"] = manual;
    store["hidden"].push_back(id);
    SaveStore(store);
    Games(true);
}

std::string CleanFolderName(const std::string& in) {
    std::string s = in;
    // Repack / scene suffixes: "Palworld.v1.0.1.ZeiGames.com", "Game v1.2.3", "Game (2026)", "Game-Build123".
    static const std::regex version(R"(([ ._-]+v?\d+(\.\d+)+.*$)|([ ._-]+(build|b)\d+.*$)|(\.[A-Za-z0-9-]+\.(com|net|org|ru)$))",
                                    std::regex::icase);
    s = std::regex_replace(s, version, "");
    static const std::regex year(R"(\s*\((19|20)\d\d\)\s*$)");
    s = std::regex_replace(s, year, "");
    for (auto& c : s) if (c == '_' || c == '.') c = ' ';
    s = Trim(s);
    return s.empty() ? in : s;
}

json LibraryFoldersJson() {
    json store = LoadStore();
    json out = json::array();
    std::error_code ec;
    for (auto& f : UserLibraryFolders(store)) out.push_back(json{{"path", Utf8(f)}, {"added", true}, {"exists", fs::is_directory(f, ec)}});
    for (auto& f : DefaultLibraryFolders())
        if (fs::is_directory(f, ec)) out.push_back(json{{"path", Utf8(f)}, {"added", false}, {"exists", true}});
    return out;
}

bool AddLibraryFolder(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    json store = LoadStore();
    if (!store.contains("folders")) store["folders"] = json::array();
    std::string p = Utf8(dir);
    for (auto& f : store["folders"]) if (f.is_string() && Lower(f.get<std::string>()) == Lower(p)) return true;
    store["folders"].push_back(p);
    SaveStore(store);
    return true;
}

void RemoveLibraryFolder(const fs::path& dir) {
    json store = LoadStore();
    json keep = json::array();
    for (auto& f : store.value("folders", json::array()))
        if (f.is_string() && Lower(f.get<std::string>()) != Lower(Utf8(dir))) keep.push_back(f);
    store["folders"] = keep;
    SaveStore(store);
}

std::vector<std::string> GameNames(const Game& g) {
    std::vector<std::string> raw = {g.target.project, Utf8(g.target.root.filename()), g.name};
    std::string stem = Utf8(g.target.exe.stem());
    for (const char* suf : {"-Win64-Shipping", "-WinGDK-Shipping", "-WinGRTS-Shipping"})
        if (Lower(stem).ends_with(Lower(suf))) stem = stem.substr(0, stem.size() - strlen(suf));
    raw.push_back(stem);
    for (auto& n : ExeVersionNames(g.target.exe)) raw.push_back(n);
    std::vector<std::string> out;
    // The Unreal project name is what the engine names the Saved folder after, and it can be
    // short (Stellar Blade: SB, Black Myth: Wukong: b1). Other names need 3+ characters.
    std::string proj = NormalizedName(g.target.project);
    if (proj.size() >= 2 && !kGenericNames.count(proj)) out.push_back(proj);
    for (auto& r : raw) {
        std::string n = NormalizedName(r);
        if (n.size() >= 3 && !kGenericNames.count(n) && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    return out;
}

// A game's Saved folder can be split: Borderlands 4 / Tiny Tina's Wonderlands keep Saved\Config
// under Documents\My Games\<Game>, but their D3D12 pipeline caches directly in
// %LOCALAPPDATA%\<Game>\Saved, which has no Config folder. Every matching Saved folder counts.
std::vector<fs::path> SavedDirs(const Game& g) {
    auto wanted = GameNames(g);
    std::vector<fs::path> bases = {LocalAppData(), DocumentsDir() / L"My Games", DocumentsDir(), g.target.root};
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& b : bases) {
        if (b.empty() || !fs::is_directory(b, ec)) continue;
        for (auto& d : fs::directory_iterator(b, fs::directory_options::skip_permission_denied, ec)) {
            if (!d.is_directory(ec)) continue;
            std::string n = NormalizedName(Utf8(d.path().filename()));
            if (std::find(wanted.begin(), wanted.end(), n) == wanted.end()) continue;
            fs::path s = d.path() / L"Saved";
            if (fs::is_directory(s, ec) && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
    }
    return out;
}

std::vector<fs::path> ConfigDirs(const Game& g) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& s : SavedDirs(g))
        for (const wchar_t* plat : {L"Windows", L"WinGDK", L"WindowsNoEditor"}) {
            fs::path p = s / L"Config" / plat;
            if (fs::is_directory(p, ec)) out.push_back(p);
        }
    return out;
}

std::optional<fs::path> SteamLocalArt(const std::string& appid) {
    auto steam = SteamDir();
    if (!steam || appid.empty()) return std::nullopt;
    fs::path cache = *steam / L"appcache" / L"librarycache";
    std::error_code ec;
    // Newer clients: librarycache\<appid>\...\library_hero.jpg; older: <appid>_library_hero.jpg.
    for (const char* name : {"library_hero.jpg", "header.jpg"}) {
        fs::path dir = cache / Wide(appid);
        if (fs::is_directory(dir, ec))
            for (auto& f : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
                if (f.is_regular_file(ec) && Lower(Utf8(f.path().filename())) == name) return f.path();
        fs::path flat = cache / Wide(appid + "_" + name);
        if (fs::is_regular_file(flat, ec)) return flat;
    }
    return std::nullopt;
}

}  // namespace uspf
