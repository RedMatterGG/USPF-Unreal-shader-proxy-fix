// USPF backend tests. Runs on Windows or under Wine: builds a fake Steam library, an Epic
// manifest, a game install with Saved folders, then drives the same functions the UI calls.
#include <cstdio>

#include "games.h"
#include "ini.h"
#include "ops.h"
#include "runs.h"
#include "streaming.h"
#include "util.h"

using namespace uspf;

static int g_pass, g_fail;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        bool ok_ = (cond);                                   \
        printf("  %s  ", ok_ ? "PASS" : "FAIL");             \
        printf(__VA_ARGS__);                                 \
        printf("\n");                                        \
        ok_ ? ++g_pass : ++g_fail;                           \
    } while (0)

static fs::path T;  // test root
static std::optional<std::wstring> origSteam, origEpic;

static void Touch(const fs::path& p, const std::string& text = "x") { WriteFileText(p, text); }

static void CopySelfTo(const fs::path& p) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::copy_file(self, p, fs::copy_options::overwrite_existing, ec);
}

static void SetReg(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& v) {
    HKEY k;
    RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

static std::optional<Game> ByName(const std::string& n) {
    for (auto& g : Games(false)) if (g.name == n) return g;
    return std::nullopt;
}

int main(int argc, char** argv) {
    std::error_code ec;
    T = fs::temp_directory_path() / L"uspf_test";
    fs::remove_all(T, ec);
    fs::create_directories(T, ec);
    fs::path data = argc > 1 ? fs::path(argv[1]) : fs::path(L"data");
    fs::remove_all(AppDataDir() / L"runs", ec);
    fs::remove(AppDataDir() / L"games.json", ec);
    // Saved folders are looked up in the real %LOCALAPPDATA% and Documents\My Games: use unique names.
    const std::string kName = "Uspf Test Game";

    printf("[1] VDF parser\n");
    {
        json v = ParseVdf("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Steam\"\n\t\t\"apps\" { \"10\" \"1\" }\n\t}\n"
                          "// comment\n\t\"1\" { \"Path\" \"D:\\\\Lib \\\"x\\\"\" }\n}\n");
        CHECK(v["libraryfolders"]["0"]["path"] == "C:\\Steam" && v["libraryfolders"]["0"]["apps"]["10"] == "1",
              "nested objects, escaped backslashes");
        CHECK(v["libraryfolders"]["1"]["path"] == "D:\\Lib \"x\"", "keys lower-cased, escaped quotes, comments skipped");
    }

    printf("[2] game discovery: Steam, Epic, added by hand\n");
    fs::path steam = T / L"Steam", lib2 = T / L"SteamLib2";
    fs::path gameRoot = lib2 / L"steamapps" / L"common" / Wide(kName);
    fs::path shipping = gameRoot / L"UspfGame" / L"Binaries" / L"Win64" / L"UspfGame-Win64-Shipping.exe";
    {
        std::string esc;
        for (char c : Utf8(lib2)) { if (c == '\\') esc += "\\\\"; else esc += c; }
        Touch(steam / L"steamapps" / L"libraryfolders.vdf",
              "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + esc + "\"\n\t}\n}\n");
        Touch(lib2 / L"steamapps" / L"appmanifest_123450.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"123450\"\n\t\"name\"\t\t\"" + kName + "\"\n\t\"installdir\"\t\t\"" + kName + "\"\n}\n");
        CopySelfTo(shipping);
        CopySelfTo(gameRoot / L"UspfGame.exe");  // launcher stub
        fs::create_directories(gameRoot / L"Engine" / L"Binaries", ec);
        // a non-Unreal Steam game: not listed
        Touch(lib2 / L"steamapps" / L"appmanifest_999.acf", "\"AppState\" { \"appid\" \"999\" \"name\" \"Not Unreal\" \"installdir\" \"NotUnreal\" }");
        CopySelfTo(lib2 / L"steamapps" / L"common" / L"NotUnreal" / L"game.exe");
        // A real Steam/Epic install's values are put back at the end.
        origSteam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
        origEpic = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Epic Games\\EpicGamesLauncher", L"AppDataPath");
        SetReg(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", steam.wstring());

        fs::path epicData = T / L"EpicData";
        fs::path epicRoot = T / L"EpicGames" / L"Uspf Epic Game";
        CopySelfTo(epicRoot / L"EpicProj" / L"Binaries" / L"Win64" / L"EpicProj-Win64-Shipping.exe");
        json item{{"DisplayName", "Uspf Epic Game"}, {"InstallLocation", Utf8(epicRoot)}, {"LaunchExecutable", "EpicProj.exe"},
                  {"bIsIncompleteInstall", false}};
        Touch(epicData / L"Manifests" / L"ABC.item", item.dump());
        SetReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Epic Games\\EpicGamesLauncher", L"AppDataPath", epicData.wstring());

        Games(true);
        auto g = ByName(kName);
        CHECK(g && g->source == "steam" && g->appid == "123450", "Steam game found through libraryfolders.vdf (appid %s)",
              g ? g->appid.c_str() : "-");
        CHECK(g && g->target.exe == shipping && g->target.project == "UspfGame",
              "its exe is the Unreal shipping exe, not the launcher stub (%s)", g ? Utf8(g->target.exe.filename()).c_str() : "-");
        CHECK(!ByName("Not Unreal"), "non-Unreal Steam games are not listed");
        auto e = ByName("Uspf Epic Game");
        CHECK(e && e->source == "epic", "Epic game found through its manifest");

        fs::path manualRoot = T / L"Manual Game";
        CopySelfTo(manualRoot / L"ManualProj" / L"Binaries" / L"Win64" / L"ManualProj-Win64-Shipping.exe");
        CopySelfTo(manualRoot / L"ManualProj.exe");
        fs::create_directories(manualRoot / L"Engine", ec);
        std::string err;
        auto m = AddGame(manualRoot / L"ManualProj.exe", &err);
        CHECK(m && m->source == "manual" && Utf8(m->target.exe.filename()) == "ManualProj-Win64-Shipping.exe",
              "a game added by its launcher stub resolves to the shipping exe");
        bool listed = false;
        for (auto& x : Games(false)) listed |= m && x.id == m->id;
        CHECK(listed, "the added game is in the list (and kept in games.json)");
        if (m) HideGame(m->id);
        listed = false;
        for (auto& x : Games(false)) listed |= m && x.id == m->id;
        CHECK(!listed, "removing it hides it");
    }

    auto game = ByName(kName);
    if (!game) { printf("cannot continue without the Steam game\n"); return 1; }
    Game g = *game;

    printf("[3] Saved folders split between LOCALAPPDATA and My Games\n");
    fs::path savedLocal = LocalAppData() / Wide(kName) / L"Saved";
    fs::path savedDocs = DocumentsDir() / L"My Games" / Wide(kName) / L"Saved";
    fs::remove_all(savedLocal.parent_path(), ec);
    fs::remove_all(savedDocs.parent_path(), ec);
    Touch(savedLocal / L"D3DGraphics_1.ushaderprecache", std::string(5000, 'g'));
    Touch(savedDocs / L"Config" / L"Windows" / L"Engine.ini", "[Core.Log]\r\nLogX=Verbose\r\n\r\n[SystemSettings]\r\nr.Foo=1\r\n");
    Touch(savedDocs / L"PipelineCaches" / L"a.upipelinecache", std::string(3000, 'p'));
    Touch(savedDocs / L"SaveGames" / L"slot.ushaderprecache", "keep");
    {
        auto saved = SavedDirs(g);
        bool l = false, d = false;
        for (auto& s : saved) { l |= s == savedLocal; d |= s == savedDocs; }
        CHECK(l && d, "both Saved folders found (%zu)", saved.size());
        auto cfg = ConfigDirs(g);
        CHECK(cfg.size() == 1 && cfg[0] == savedDocs / L"Config" / L"Windows", "Engine.ini folder found under My Games");
    }

    printf("[4] install / uninstall\n");
    fs::path dir = g.target.exe.parent_path();
    {
        json s = GameStatus(g);
        CHECK(!s["installed"].get<bool>() && !s["dllPresent"].get<bool>(), "not installed at first");
        json r = Install(g, false, true, "full");
        CHECK(r.value("ok", false), "install ok (%s)", r.dump().c_str());
        s = GameStatus(g);
        CHECK(s["installed"].get<bool>() && s["upToDate"].get<bool>() && s["iniPresent"].get<bool>(),
              "d3d12.dll identical to the bundled one, d3d12async.ini written");
        CHECK(s["engineIni"]["enabled"].get<bool>(), "Engine.ini precache block present");
        auto eng = ReadFileText(savedDocs / L"Config" / L"Windows" / L"Engine.ini").value_or("");
        CHECK(eng.find("[SystemSettings]\r\n; >>> d3d12async") != std::string::npos && eng.find("r.Foo=1") != std::string::npos &&
                  eng.find("LogX=Verbose") != std::string::npos && eng.find("r.PSOPrecache.Resources=1") != std::string::npos,
              "block inserted right under [SystemSettings], other lines kept, CRLF kept");
        CHECK(fs::exists(savedDocs / L"Config" / L"Windows" / L"Engine.ini.d3d12async.bak", ec), "Engine.ini backed up once");
        json again = Install(g, false, true, "full");
        auto eng2 = ReadFileText(savedDocs / L"Config" / L"Windows" / L"Engine.ini").value_or("");
        size_t n = 0;
        for (size_t p = eng2.find("; >>> d3d12async"); p != std::string::npos; p = eng2.find("; >>> d3d12async", p + 1)) ++n;
        CHECK(again.value("ok", false) && n == 1, "installing again keeps a single block");

        Uninstall(g, true);
        Touch(dir / L"d3d12.dll", "some other mod");
        r = Install(g, false, false, "full");
        CHECK(!r.value("ok", true) && r.value("code", "") == "foreign-dll", "another d3d12.dll: refused without force");
        r = Install(g, true, false, "full");
        CHECK(r.value("ok", false) && ReadFileText(dir / L"d3d12.dll.d3d12async.bak").value_or("") == "some other mod",
              "with force: the other DLL is backed up");
        r = Uninstall(g, true);
        CHECK(r.value("ok", false) && ReadFileText(dir / L"d3d12.dll").value_or("") == "some other mod" &&
                  !fs::exists(dir / L"d3d12async.ini", ec),
              "uninstall restores it and removes d3d12async.ini");
        auto eng3 = ReadFileText(savedDocs / L"Config" / L"Windows" / L"Engine.ini").value_or("");
        CHECK(eng3.find("d3d12async") == std::string::npos && eng3.find("r.Foo=1") != std::string::npos, "Engine.ini block removed, rest kept");
        DeleteFileW((dir / L"d3d12.dll").c_str());

        fs::create_directories(g.target.root / L"EasyAntiCheat", ec);
        r = Install(g, false, false, "full");
        CHECK(!r.value("ok", true) && r.value("code", "") == "anticheat" && r["files"].size() == 1, "anti-cheat found: refused");
        fs::remove_all(g.target.root / L"EasyAntiCheat", ec);
        Install(g, false, false, "full");
    }

    printf("[5] settings editor\n");
    {
        json ini = IniGet(g);
        bool help = false;
        for (auto& k : ini["schema"])
            if (k["key"] == "QueueFullMode") help = k["help"].get<std::string>().find("placeholder") != std::string::npos;
        CHECK(ini["schema"].size() > 40 && help, "schema from the bundled ini with help text (%zu keys)", ini["schema"].size());
        CHECK(ini["values"]["QueueFullMode"] == "placeholder", "current values read");
        json r = IniSet(g, json{{"QueueFullMode", "compile"}, {"UiWaitMaxMs", "4"}});
        std::string text = ReadFileText(dir / L"d3d12async.ini").value_or("");
        auto keys = ParseIni(text);
        std::string q, u;
        for (auto& k : keys) { if (k.key == "QueueFullMode") q = k.value; if (k.key == "UiWaitMaxMs") u = k.value; }
        CHECK(r.value("ok", false) && q == "compile" && u == "4" && text.find("; What happens once MaxQueuedCompiles") != std::string::npos,
              "values changed in place, comments kept");
        IniReset(g);
        CHECK(IniGet(g)["values"]["QueueFullMode"] == "placeholder", "reset to defaults");
        std::string up = UpdateIni("[d3d12async]\nA=1\n\n[other]\nB=2\n", {{"a", "5"}, {"New", "x"}});
        CHECK(up == "[d3d12async]\nA=5\nNew=x\n\n[other]\nB=2\n", "missing keys go to the [d3d12async] section");
    }

    printf("[6] run logs\n");
    {
        fs::path runs = dir / L"d3d12async-runs";
        fs::create_directories(runs, ec);
        for (auto& f : fs::directory_iterator(data / L"runs", ec)) fs::copy_file(f.path(), runs / f.path().filename(), ec);
        json list = ListRuns(g);
        CHECK(list.size() == 2 && list[0]["id"] == "2026-10-14_10-21-30_dll-ON", "two runs, newest first");
        json old = list.size() > 1 ? list[1] : json::object();
        CHECK(old.value("avgFps", 0.0) == 110.3 && old.value("low1", 0.0) == 72.7 && old.value("low01", 0.0) == 52.5 &&
                  old.value("hitches", 0.0) == 3 && old.value("shaderHitches", -1.0) == 0,
              "summary parsed: 110.3 avg, 72.7 / 52.5 lows, 3 hitches, 0 from shaders");
        CHECK(old.value("maxFps", 0.0) > 110 && !old.value("hasFrames", true),
              "old run without a max-fps line: max fps from the per-second CSV (%.1f)", old.value("maxFps", 0.0));
        CHECK(list[0].value("maxFps", 0.0) == 10.0 && list[0].value("hasFrames", false), "new run: max fps from the summary, frame file present");
        json d = RunDetail(g, "2026-10-10_06-30-21_dll-ON");
        CHECK(d["seconds"]["t"].size() > 700 && d["seconds"]["fps"].size() == d["seconds"]["t"].size() && d["hitches"].size() == 3,
              "detail: %zu gameplay seconds, 3 hitches", d["seconds"]["t"].size());
        CHECK(d["seconds"]["low1"][0].is_number(), "older logs: 1%% low per second from the P99 frame time");
        json d2 = RunDetail(g, "2026-10-14_10-21-30_dll-ON");
        CHECK(d2["frames"]["ms"].size() >= 8 && d2["seconds"]["low01"].size() > 0, "new run: every gameplay frame time (%zu)",
              d2["frames"]["ms"].size());
        CHECK(RunDetail(g, "..\\..\\x").contains("error"), "run ids can't escape the folder");
        fs::remove_all(runs, ec);
        CHECK(ListRuns(g).size() == 2, "runs stay in USPF's archive after the game folder's copies are gone");
    }

    printf("[7] shader caches\n");
    {
        json list = CacheList(&g);
        size_t gameFiles = 0;
        for (auto& t : list) if (t["kind"] == "game") gameFiles += t["files"].get<size_t>();
        CHECK(gameFiles == 2, "game caches: the .ushaderprecache and the .upipelinecache, not the SaveGames file (%zu)", gameFiles);
        json r = CacheClear(&g, json::array({"game"}));
        CHECK(r["deleted"] == 2 && !fs::exists(savedLocal / L"D3DGraphics_1.ushaderprecache", ec) &&
                  fs::exists(savedDocs / L"SaveGames" / L"slot.ushaderprecache", ec) &&
                  fs::exists(savedDocs / L"Config" / L"Windows" / L"Engine.ini", ec),
              "cleared; saves and config untouched");
    }

    printf("[8] Engine.ini block\n");
    {
        std::string a = ApplyEngineIniBlock("", true, "fast");
        CHECK(a.find("[SystemSettings]") == 0 && a.find("WaitForHighPriorityRequestsOnly=1") != std::string::npos &&
                  a.find("Resources=1") == std::string::npos,
              "fast load mode: high-priority waits only, no per-resource precache");
        CHECK(ApplyEngineIniBlock(a, false, "fast") == "[SystemSettings]\r\n", "removing it leaves the rest");
    }

    printf("[9] Engine.ini rewritten by the game; leftovers of the old streaming settings\n");
    {
        auto stripComments = [](const std::string& t) {
            std::string out;
            for (size_t i = 0; i < t.size();) {
                size_t j = t.find('\n', i);
                std::string l = t.substr(i, (j == std::string::npos ? t.size() : j + 1) - i);
                if (l.empty() || l[0] != ';') out += l;
                if (j == std::string::npos) break;
                i = j + 1;
            }
            return out;
        };
        // What USPF 0.2.x wrote: a marked block in [SystemSettings].
        std::string base = "[SystemSettings]\r\nr.Foo=1\r\n\r\n[/Script/Engine.RendererSettings]\r\ns.AsyncLoadingTimeLimit=9\r\n";
        std::string old = "[SystemSettings]\r\n; >>> USPF: streaming settings (remove this block to undo)\r\n; textures\r\n"
                          "r.Streaming.FramesForFullUpdate=8\r\n; level streaming\r\ns.AsyncLoadingTimeLimit=3\r\n; <<< USPF streaming\r\n"
                          "r.Foo=1\r\n\r\n[/Script/Engine.RendererSettings]\r\ns.AsyncLoadingTimeLimit=9\r\n";
        CHECK(ReadStreamingBlock(old).size() == 2, "old streaming lines found");
        CHECK(RemoveStreamingBlock(old) == base, "removed with their markers; the rest and other sections untouched");
        CHECK(RemoveStreamingBlock(stripComments(old)) == base, "also removed after the game dropped the comment markers");
        CHECK(RemoveStreamingBlock(base) == base, "nothing to remove: unchanged");

        fs::path eng = savedDocs / L"Config" / L"Windows" / L"Engine.ini";
        Touch(eng, "[SystemSettings]\r\nr.Foo=1\r\n");
        // The precache block, then the game rewrites Engine.ini on exit without any comment lines.
        SetEngineIni(g, true, "full");
        WriteFileText(eng, stripComments(ReadFileText(eng).value_or("")));
        CHECK(GameStatus(g)["engineIni"]["enabled"] == true, "precache settings still recognised after the game strips the markers");
        SetEngineIni(g, true, "full");
        std::string t4 = ReadFileText(eng).value_or("");
        size_t n = 0;
        for (size_t p = 0; (p = t4.find("r.PSOPrecaching=1", p)) != std::string::npos; p += 5) ++n;
        CHECK(n == 1, "re-applying replaces the old lines instead of adding duplicates");
        SetEngineIni(g, false, "full");
        CHECK(ReadFileText(eng).value_or("").find("r.PSOPrecaching") == std::string::npos, "precache settings removed without markers");

        WriteFileText(eng, stripComments(old));
        Uninstall(g, true);
        std::string after = ReadFileText(eng).value_or("");
        CHECK(ReadStreamingBlock(after).empty() && after.find("s.AsyncLoadingTimeLimit=9") != std::string::npos &&
                  after.find("r.Foo=1") != std::string::npos,
              "uninstall removes leftover streaming lines from older versions; the game's own lines stay");
    }

    printf("[10] game folders: installs outside the stores\n");
    {
        fs::path lib = T / L"Games";
        auto mk = [&](const fs::path& exe, size_t size) { WriteFileText(exe, std::string(size, 'e')); };
        // renamed exe (no -Shipping), Engine folder at the root, helper exes beside it
        fs::path bl = lib / L"Borderlands 9";
        fs::create_directories(bl / L"Engine", ec);
        mk(bl / L"Borderlands9.exe", 1000);
        mk(bl / L"OakGame" / L"Binaries" / L"Win64" / L"Borderlands9.exe", 90000);
        mk(bl / L"OakGame" / L"Binaries" / L"Win64" / L"CrashReportClient.exe", 120000);
        // repack folder name, shipping exe
        fs::path pal = lib / L"Palworld.v1.0.1.ZeiGames.com";
        fs::create_directories(pal / L"Engine", ec);
        mk(pal / L"Pal" / L"Binaries" / L"Win64" / L"Palworld-Win64-Shipping.exe", 5000);
        // one level deeper
        fs::path gow = lib / L"Gears of Test (2026)" / L"GoWTest";
        fs::create_directories(gow / L"Engine", ec);
        mk(gow / L"Fairlight" / L"Binaries" / L"Win64" / L"GoWTest-Win64-Shipping.exe", 5000);
        // a Steam library Steam doesn't know about
        fs::path sa = lib / L"steamapps";
        Touch(sa / L"appmanifest_999001.acf", "\"AppState\"\n{\n\t\"appid\"\t\t\"999001\"\n\t\"name\"\t\t\"The Blood of Testwalker\"\n\t\"installdir\"\t\t\"Testwalker\"\n}\n");
        fs::create_directories(sa / L"common" / L"Testwalker" / L"Engine", ec);
        mk(sa / L"common" / L"Testwalker" / L"Dawn" / L"Binaries" / L"Win64" / L"Dawn-Win64-Shipping.exe", 5000);
        // not Unreal: no Engine, no Paks, no -Shipping
        mk(lib / L"Some Tool" / L"Tool" / L"Binaries" / L"Win64" / L"tool.exe", 5000);
        // short project name, Saved under LOCALAPPDATA\<project>
        fs::path xq = lib / L"Xq Blade";
        fs::create_directories(xq / L"Engine", ec);
        mk(xq / L"Xq" / L"Binaries" / L"Win64" / L"Xq-Win64-Shipping.exe", 5000);
        fs::path xqSaved = LocalAppData() / L"Xq" / L"Saved";
        fs::remove_all(xqSaved.parent_path(), ec);
        fs::create_directories(xqSaved / L"Config" / L"WindowsNoEditor", ec);

        CHECK(AddLibraryFolder(lib), "folder added");
        Games(true);
        auto bl9 = ByName("Borderlands 9");
        CHECK(bl9 && Utf8(bl9->target.exe.filename()) == "Borderlands9.exe" && bl9->target.project == "OakGame" && bl9->source == "local",
              "renamed game exe found (largest non-helper exe in Binaries, Engine folder at the root)");
        auto p = ByName("Palworld");
        CHECK(p.has_value(), "repack folder name cleaned (\"Palworld.v1.0.1.ZeiGames.com\" -> \"Palworld\")");
        auto gw = ByName("Gears of Test");
        CHECK(gw && gw->target.project == "Fairlight", "a game one folder deeper is found, named after its top folder");
        auto tw = ByName("The Blood of Testwalker");
        CHECK(tw && tw->source == "steam" && tw->appid == "999001", "a Steam library inside the folder: name and appid from its manifest");
        bool tool = false;
        for (auto& x : Games(false)) tool |= x.target.project == "Tool";
        CHECK(!tool, "folders that aren't Unreal games are skipped");
        auto x = ByName("Xq Blade");
        bool cfg = false;
        if (x) for (auto& d : ConfigDirs(*x)) cfg |= d == xqSaved / L"Config" / L"WindowsNoEditor";
        CHECK(cfg, "config folder found for a 2-letter project name (LOCALAPPDATA\\Xq\\Saved\\Config\\WindowsNoEditor)");
        json lf = LibraryFoldersJson();
        bool listed = false;
        for (auto& f : lf) listed |= f["added"] == true && f["path"] == Utf8(lib);
        CHECK(listed, "the added folder is listed");
        RemoveLibraryFolder(lib);
        Games(true);
        CHECK(!ByName("Palworld"), "removing the folder drops its games");
        CHECK(CleanFolderName("Game Name v1.2.3") == "Game Name" && CleanFolderName("Gears of War E-Day (2026)") == "Gears of War E-Day" &&
                  CleanFolderName("Silent Hill f") == "Silent Hill f" && CleanFolderName("STAR WARS Jedi - Survivor") == "STAR WARS Jedi - Survivor",
              "folder names cleaned without damaging normal ones");
        fs::remove_all(xqSaved.parent_path(), ec);
    }

    // cleanup
    Uninstall(g, true);
    fs::remove_all(savedLocal.parent_path(), ec);
    fs::remove_all(savedDocs.parent_path(), ec);
    fs::remove_all(AppDataDir() / L"runs" / Wide(g.id), ec);
    if (origSteam) SetReg(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", *origSteam);
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (origEpic) SetReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Epic Games\\EpicGamesLauncher", L"AppDataPath", *origEpic);
    else RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Epic Games\\EpicGamesLauncher", L"AppDataPath");
    fs::remove_all(T, ec);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
