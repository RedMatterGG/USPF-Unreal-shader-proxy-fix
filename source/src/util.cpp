// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
#include "util.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>

namespace uspf {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Utf8(const fs::path& p) { return Utf8(p.wstring()); }
fs::path PathU8(const std::string& s) { return fs::path(Wide(s)); }

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string NormalizedName(const std::string& s) {
    std::string o;
    for (unsigned char c : s)
        if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool ReadFileBytes(const fs::path& p, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + r);
    fclose(f);
    return true;
}

std::optional<std::string> ReadFileText(const fs::path& p) {
    std::vector<uint8_t> b;
    if (!ReadFileBytes(p, b)) return std::nullopt;
    std::string s(b.begin(), b.end());
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

bool WriteFileBytes(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".uspf-tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        DeleteFileW(tmp.c_str());
        SetLastError(e);
        return false;
    }
    return true;
}

bool WriteFileText(const fs::path& p, const std::string& s) { return WriteFileBytes(p, s.data(), s.size()); }

std::optional<std::wstring> RegString(HKEY root, const wchar_t* subkey, const wchar_t* value, REGSAM view) {
    HKEY k;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return std::nullopt;
    wchar_t buf[2048];
    DWORD type = 0, size = sizeof buf - sizeof(wchar_t);
    LONG r = RegQueryValueExW(k, value, nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::nullopt;
    buf[size / sizeof(wchar_t)] = 0;
    return std::wstring(buf);
}

fs::path KnownFolder(int csidl) {
    wchar_t p[MAX_PATH];
    if (SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, p) != S_OK) return {};
    return fs::path(p);
}

fs::path LocalAppData() { return KnownFolder(CSIDL_LOCAL_APPDATA); }
fs::path DocumentsDir() { return KnownFolder(CSIDL_PERSONAL); }

fs::path AppDataDir() {
    fs::path d = LocalAppData() / L"USPF";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

bool Resource(const wchar_t* name, const void** data, size_t* size) {
    HRSRC r = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    *data = LockResource(g);
    *size = SizeofResource(nullptr, r);
    return *data != nullptr;
}

std::string Hex64(uint64_t v) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

uint64_t Fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::string ErrorText(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   (LPWSTR)&buf, 0, nullptr);
    std::string s = buf ? Trim(Utf8(std::wstring(buf))) : "error " + std::to_string(code);
    if (buf) LocalFree(buf);
    return s;
}

namespace {
bool IsShippingExe(const fs::path& p) {
    std::string n = Lower(Utf8(p.filename()));
    return n.size() > 13 && n.ends_with("-shipping.exe");
}
const char* kPlatforms[] = {"Win64", "WinGDK", "WinGRTS"};
}  // namespace

// <Root>\<Project>\Binaries\<Platform>\<Name>-<Platform>-Shipping.exe. A launcher stub in the
// root is replaced by the real renderer exe (the one d3d12.dll must sit next to).
namespace {
// Executables that sit in an Unreal Binaries folder but aren't the game.
bool IsHelperExe(const std::string& lowerName) {
    static const char* helpers[] = {"crashreportclient", "crs-", "unrealcefsubprocess", "epicwebhelper", "easyanticheat",
                                    "start_protected", "uninstall", "unins0", "redist", "vc_redist", "dxsetup", "ue4prereq",
                                    "ueprereq", "bootstrap", "cefsharp", "launcher"};
    for (auto* h : helpers)
        if (lowerName.find(h) != std::string::npos) return true;
    return false;
}
}  // namespace

// <Root>\<Project>\Binaries\<Platform>\<exe>. The game exe is the *-Shipping.exe when there is
// one; otherwise (Borderlands 4, Jedi: Survivor ship a renamed exe) the largest non-helper exe,
// accepted only when the folder is clearly an Unreal build: <Root>\Engine or <Project>\Content\Paks.
std::optional<UnrealTarget> FindUnrealInRoot(const fs::path& root, const std::string& preferStem) {
    std::error_code ec;
    bool engine = fs::is_directory(root / L"Engine", ec);
    std::vector<fs::path> shipping, other;
    for (auto& d : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (!d.is_directory(ec)) continue;
        std::string name = Lower(Utf8(d.path().filename()));
        if (name == "engine") continue;
        bool paks = fs::is_directory(d.path() / L"Content" / L"Paks", ec);
        for (const char* plat : kPlatforms) {
            fs::path b = d.path() / L"Binaries" / Wide(plat);
            if (!fs::is_directory(b, ec)) continue;
            for (auto& f : fs::directory_iterator(b, fs::directory_options::skip_permission_denied, ec)) {
                if (!f.is_regular_file(ec)) continue;
                std::string fn = Lower(Utf8(f.path().filename()));
                if (!fn.ends_with(".exe")) continue;
                if (IsShippingExe(f.path())) shipping.push_back(f.path());
                else if ((engine || paks) && !IsHelperExe(fn)) other.push_back(f.path());
            }
        }
    }
    std::vector<fs::path>& found = !shipping.empty() ? shipping : other;
    if (found.empty()) return std::nullopt;
    fs::path pick = found[0];
    if (&found == &other) {  // the largest one is the game
        uintmax_t best = 0;
        for (auto& f : other) {
            auto sz = fs::file_size(f, ec);
            if (!ec && sz > best) { best = sz; pick = f; }
        }
    }
    if (found.size() > 1 && !preferStem.empty())
        for (auto& f : found)
            if (Lower(Utf8(f.filename())).starts_with(Lower(preferStem))) { pick = f; break; }
    UnrealTarget t;
    t.exe = pick;
    t.project = Utf8(pick.parent_path().parent_path().parent_path().filename());
    t.root = pick.parent_path().parent_path().parent_path().parent_path();
    t.isUnreal = true;
    return t;
}

std::optional<UnrealTarget> ResolveTarget(const fs::path& exeIn) {
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(exeIn, ec);
    if (ec) exe = exeIn;
    if (!fs::is_regular_file(exe, ec)) return std::nullopt;
    fs::path dir = exe.parent_path();
    std::string platDir = Utf8(dir.filename());
    std::string bin = Lower(Utf8(dir.parent_path().filename()));
    for (const char* plat : kPlatforms) {
        if (Lower(platDir) == Lower(plat) && bin == "binaries") {
            UnrealTarget t;
            t.exe = exe;
            fs::path projectDir = dir.parent_path().parent_path();
            t.project = Utf8(projectDir.filename());
            t.root = projectDir.parent_path();
            t.isUnreal = fs::is_directory(t.root / L"Engine", ec) || IsShippingExe(exe);
            return t;
        }
    }
    if (fs::is_directory(dir / L"Engine", ec)) {
        if (auto t = FindUnrealInRoot(dir, Utf8(exe.stem()))) return t;
    }
    UnrealTarget t;
    t.exe = exe;
    t.root = dir;
    return t;
}

std::vector<std::string> ExeVersionNames(const fs::path& exe) {
    std::vector<std::string> out;
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (!n) return out;
    std::vector<uint8_t> buf(n);
    if (!GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) return out;
    struct Lang { WORD lang, cp; } *langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &len) || !len) return out;
    for (const wchar_t* key : {L"ProductName", L"FileDescription"}) {
        wchar_t q[128];
        swprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\%ls", langs[0].lang, langs[0].cp, key);
        wchar_t* v = nullptr;
        UINT vl = 0;
        if (VerQueryValueW(buf.data(), q, (void**)&v, &vl) && vl > 1) out.push_back(Trim(Utf8(std::wstring(v))));
    }
    return out;
}

}  // namespace uspf
