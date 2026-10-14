// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// Win32 host: one window with a WebView2 control. The UI (web/) is served from resources in this
// exe at https://uspf.app/; it talks to the backend with chrome.webview.postMessage
// ({id, method, params} -> {id, result} or {id, error}). Slow calls run on worker threads.
#include "util.h"  // windows.h first

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <functional>
#include <map>
#include <thread>

#include "WebView2.h"
#include "games.h"
#include <shobjidl.h>

#include "ops.h"
#include "runs.h"
#include "util.h"

using namespace uspf;

namespace {

constexpr UINT WM_REPLY = WM_APP + 1;
const wchar_t* kClass = L"USPF_MainWindow";
const wchar_t* kOrigin = L"https://uspf.app/";

HWND g_hwnd;
ICoreWebView2Environment* g_env;
ICoreWebView2Controller* g_controller;
ICoreWebView2* g_view;
bool g_devtools;

// A COM callback object around a lambda: the WebView2 completion/event handler interfaces all have
// one Invoke method. No WRL with MinGW, so IUnknown is implemented by hand.
template <class I, class... A>
struct Callback : I {
    std::function<HRESULT(A...)> fn;
    const IID& iid;
    LONG ref = 1;
    Callback(const IID& id, std::function<HRESULT(A...)> f) : fn(std::move(f)), iid(id) {}
    virtual ~Callback() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return fn(a...); }
};

struct WebFile { const wchar_t* path; const wchar_t* res; const wchar_t* mime; };
const WebFile kFiles[] = {
    {L"/", L"WEB_INDEX", L"text/html; charset=utf-8"},
    {L"/index.html", L"WEB_INDEX", L"text/html; charset=utf-8"},
    {L"/app.js", L"WEB_APP_JS", L"text/javascript; charset=utf-8"},
    {L"/app.css", L"WEB_APP_CSS", L"text/css; charset=utf-8"},
    {L"/uPlot.iife.min.js", L"WEB_UPLOT_JS", L"text/javascript; charset=utf-8"},
    {L"/uPlot.min.css", L"WEB_UPLOT_CSS", L"text/css; charset=utf-8"},
    {L"/logo.svg", L"WEB_LOGO", L"image/svg+xml"},
};

void Reply(const json& msg) {
    auto* s = new std::wstring(Wide(msg.dump(-1, ' ', false, json::error_handler_t::replace)));
    if (!PostMessageW(g_hwnd, WM_REPLY, 0, (LPARAM)s)) delete s;
}

Game NeedGame(const json& p) {
    auto g = FindGame(p.value("id", ""));
    if (!g) throw std::runtime_error("That game is no longer in the list. Rescan the games.");
    return *g;
}

std::optional<fs::path> PickExe() {
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = g_hwnd;
    o.lpstrFilter = L"Game executable (*.exe)\0*.exe\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH * 4;
    o.lpstrTitle = L"Select the game's executable";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&o)) return std::nullopt;
    return fs::path(file);
}

// Folder picker (IFileOpenDialog in folder mode).
std::optional<fs::path> PickFolder() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return std::nullopt;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(L"Select a folder that contains games (for example D:\\Games)");
    std::optional<fs::path> out;
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) { out = fs::path(path); CoTaskMemFree(path); }
            item->Release();
        }
    }
    d->Release();
    return out;
}

json GamesJson(bool rescan) {
    json list = json::array();
    for (auto& g : Games(rescan)) list.push_back(GameJson(g));
    return list;
}

// Calls that must run on the UI thread (dialogs, process exit).
bool HandleOnUi(const std::string& method, const json& p, json& result) {
    if (method == "addGame") {
        auto exe = PickExe();
        if (!exe) { result = json{{"cancelled", true}}; return true; }
        std::string err;
        auto g = AddGame(*exe, &err);
        result = g ? json{{"game", GameJson(*g)}, {"games", GamesJson(false)}} : json{{"error", err}};
        return true;
    }
    if (method == "addLibraryFolder") {
        auto dir = PickFolder();
        if (!dir) { result = json{{"cancelled", true}}; return true; }
        AddLibraryFolder(*dir);
        result = json{{"folders", LibraryFoldersJson()}, {"games", GamesJson(true)}};
        return true;
    }
    if (method == "restartElevated") {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        if ((INT_PTR)ShellExecuteW(g_hwnd, L"runas", self, L"--elevated", nullptr, SW_SHOWNORMAL) > 32) PostQuitMessage(0);
        result = json{{"ok", true}};
        return true;
    }
    if (method == "openPath") {
        fs::path path = PathU8(p.value("path", ""));
        std::error_code ec;
        if (!fs::is_directory(path, ec)) path = path.parent_path();  // never launch files, only show folders
        if (!fs::is_directory(path, ec)) { result = json{{"error", "Folder not found"}}; return true; }
        ShellExecuteW(g_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        result = json{{"ok", true}};
        return true;
    }
    if (method == "openUrl") {
        std::string url = p.value("url", "");
        if (!url.starts_with("https://")) { result = json{{"error", "Only https links open"}}; return true; }
        ShellExecuteW(g_hwnd, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        result = json{{"ok", true}};
        return true;
    }
    return false;
}

json HandleOnWorker(const std::string& m, const json& p) {
    if (m == "init") return json{{"version", kAppVersion}, {"elevated", IsElevated()}, {"games", GamesJson(false)},
                                 {"github", std::string("https://github.com/") + kGithubOwner + "/" + kGithubRepo}};
    if (m == "games") return GamesJson(p.value("rescan", false));
    if (m == "libraryFolders") return LibraryFoldersJson();
    if (m == "removeLibraryFolder") {
        RemoveLibraryFolder(PathU8(p.value("path", "")));
        return json{{"folders", LibraryFoldersJson()}, {"games", GamesJson(true)}};
    }
    if (m == "removeGame") { HideGame(p.value("id", "")); return GamesJson(false); }
    if (m == "status") return GameStatus(NeedGame(p));
    if (m == "install") return Install(NeedGame(p), p.value("force", false), p.value("engineIni", true), p.value("loadMode", "full"));
    if (m == "uninstall") return Uninstall(NeedGame(p), p.value("engineIni", true));
    if (m == "engineIni") return SetEngineIni(NeedGame(p), p.value("enable", true), p.value("loadMode", "full"));
    if (m == "iniGet") return IniGet(NeedGame(p));
    if (m == "iniSet") return IniSet(NeedGame(p), p.value("values", json::object()));
    if (m == "iniReset") return IniReset(NeedGame(p));
    if (m == "runs") return ListRuns(NeedGame(p));
    if (m == "run") return RunDetail(NeedGame(p), p.value("run", ""));
    if (m == "cacheList" || m == "cacheClear") {
        std::optional<Game> g;
        if (!p.value("id", "").empty()) g = FindGame(p["id"]);
        return m == "cacheList" ? CacheList(g ? &*g : nullptr) : CacheClear(g ? &*g : nullptr, p.value("kinds", json::array()));
    }
    if (m == "art") {
        auto g = NeedGame(p);
        auto f = ArtFile(g);
        if (!f) return json{{"url", nullptr}};
        std::error_code ec;
        auto stamp = fs::last_write_time(*f, ec).time_since_epoch().count();
        return json{{"url", "https://uspf.app/art/" + g.id + "?v=" + std::to_string(stamp)},
                    {"kind", Lower(Utf8(f->extension())) == ".png" ? "icon" : "banner"}};
    }
    if (m == "checkUpdate") return CheckUpdate();
    if (m == "applyUpdate") {
        json r = ApplyUpdate(p.value("url", ""));
        if (r.value("restart", false)) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return r;
    }
    throw std::runtime_error("unknown method " + m);
}

void OnMessage(const std::wstring& raw) {
    json msg = json::parse(Utf8(raw), nullptr, false);
    if (!msg.is_object()) return;
    json id = msg.value("id", json(nullptr));
    std::string method = msg.value("method", "");
    json params = msg.value("params", json::object());
    json result;
    if (HandleOnUi(method, params, result)) {
        Reply(json{{"id", id}, {"result", result}});
        return;
    }
    std::thread([id, method, params] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            Reply(json{{"id", id}, {"result", HandleOnWorker(method, params)}});
        } catch (const std::exception& e) {
            Reply(json{{"id", id}, {"error", e.what()}});
        }
        CoUninitialize();
    }).detach();
}

void ServeResource(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ICoreWebView2WebResourceRequest* req = nullptr;
    if (FAILED(args->get_Request(&req)) || !req) return;
    LPWSTR uri = nullptr;
    req->get_Uri(&uri);
    std::wstring u = uri ? uri : L"";
    if (uri) CoTaskMemFree(uri);
    req->Release();
    std::wstring path = u.substr(wcslen(kOrigin) - 1);
    size_t q = path.find_first_of(L"?#");
    if (q != std::wstring::npos) path.resize(q);

    std::vector<uint8_t> body;
    std::wstring mime;
    int status = 404;
    if (path.rfind(L"/art/", 0) == 0) {
        std::wstring id = path.substr(5);
        bool ok = !id.empty() && id.find_first_not_of(L"0123456789abcdef") == std::wstring::npos;
        fs::path dir = AppDataDir() / L"art";
        if (ok && ReadFileBytes(dir / (id + L".jpg"), body)) { mime = L"image/jpeg"; status = 200; }
        else if (ok && ReadFileBytes(dir / (id + L".png"), body)) { mime = L"image/png"; status = 200; }
    } else {
        for (auto& f : kFiles)
            if (path == f.path) {
                const void* d;
                size_t n;
                if (Resource(f.res, &d, &n)) { body.assign((const uint8_t*)d, (const uint8_t*)d + n); mime = f.mime; status = 200; }
                break;
            }
    }
    IStream* stream = SHCreateMemStream(body.empty() ? nullptr : body.data(), (UINT)body.size());
    std::wstring headers = L"Content-Type: " + (mime.empty() ? std::wstring(L"text/plain") : mime) +
                           L"\r\nCache-Control: no-store\r\nContent-Security-Policy: default-src 'self' https://uspf.app; "
                           L"img-src 'self' https://uspf.app data:; style-src 'self' 'unsafe-inline'";
    ICoreWebView2WebResourceResponse* resp = nullptr;
    if (SUCCEEDED(g_env->CreateWebResourceResponse(stream, status, status == 200 ? L"OK" : L"Not Found", headers.c_str(), &resp))) {
        args->put_Response(resp);
        resp->Release();
    }
    if (stream) stream->Release();
}

void Resize() {
    if (!g_controller) return;
    RECT r;
    GetClientRect(g_hwnd, &r);
    g_controller->put_Bounds(r);
}

void Fatal(const std::wstring& msg) {
    MessageBoxW(g_hwnd, msg.c_str(), L"USPF", MB_ICONERROR | MB_OK);
    PostQuitMessage(1);
}

HRESULT OnController(HRESULT hr, ICoreWebView2Controller* c) {
    if (FAILED(hr) || !c) { Fatal(L"WebView2 could not start (0x" + Wide(Hex64((uint32_t)hr)).substr(8) + L")."); return S_OK; }
    g_controller = c;
    c->AddRef();
    c->get_CoreWebView2(&g_view);
    ICoreWebView2Controller2* c2 = nullptr;
    if (SUCCEEDED(c->QueryInterface(IID_ICoreWebView2Controller2, (void**)&c2)) && c2) {
        c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 7, 11, 20});
        c2->Release();
    }
    ICoreWebView2Settings* s = nullptr;
    if (SUCCEEDED(g_view->get_Settings(&s)) && s) {
        s->put_AreDevToolsEnabled(g_devtools);
        s->put_AreDefaultContextMenusEnabled(g_devtools);
        s->put_IsStatusBarEnabled(FALSE);
        s->put_IsZoomControlEnabled(FALSE);
        s->put_IsBuiltInErrorPageEnabled(FALSE);
        s->Release();
    }
    EventRegistrationToken tok;
    g_view->AddWebResourceRequestedFilter(L"https://uspf.app/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    g_view->add_WebResourceRequested(
        new Callback<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>(
            IID_ICoreWebView2WebResourceRequestedEventHandler,
            [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) { ServeResource(a); return S_OK; }),
        &tok);
    g_view->add_WebMessageReceived(
        new Callback<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
            IID_ICoreWebView2WebMessageReceivedEventHandler,
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) {
                LPWSTR s = nullptr;
                if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && s) { OnMessage(s); CoTaskMemFree(s); }
                return S_OK;
            }),
        &tok);
    // Stay on our own pages; links open in the default browser through openUrl.
    g_view->add_NavigationStarting(
        new Callback<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
            IID_ICoreWebView2NavigationStartingEventHandler,
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) {
                LPWSTR uri = nullptr;
                a->get_Uri(&uri);
                if (uri && wcsncmp(uri, kOrigin, wcslen(kOrigin)) != 0) a->put_Cancel(TRUE);
                if (uri) CoTaskMemFree(uri);
                return S_OK;
            }),
        &tok);
    g_view->add_NewWindowRequested(
        new Callback<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
            IID_ICoreWebView2NewWindowRequestedEventHandler,
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) { a->put_Handled(TRUE); return S_OK; }),
        &tok);
    Resize();
    g_view->Navigate(kOrigin);
    return S_OK;
}

HRESULT OnEnvironment(HRESULT hr, ICoreWebView2Environment* env) {
    if (FAILED(hr) || !env) {
        Fatal(L"The Microsoft Edge WebView2 Runtime is missing or broken. It ships with Windows 11; on Windows 10 install it from "
              L"https://developer.microsoft.com/microsoft-edge/webview2/ and start USPF again.");
        return S_OK;
    }
    g_env = env;
    env->AddRef();
    env->CreateCoreWebView2Controller(
        g_hwnd, new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
                    IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, OnController));
    return S_OK;
}

// The loader is embedded in this exe and written next to the WebView2 profile on first start.
bool StartWebView() {
    fs::path bin = AppDataDir() / L"bin" / L"WebView2Loader.dll";
    const void* d;
    size_t n;
    if (!Resource(L"WEBVIEW2_LOADER", &d, &n)) return false;
    std::vector<uint8_t> cur;
    if (!ReadFileBytes(bin, cur) || cur.size() != n || memcmp(cur.data(), d, n) != 0) WriteFileBytes(bin, d, n);
    HMODULE m = LoadLibraryW(bin.c_str());
    if (!m) return false;
    using Create = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                           ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    auto create = (Create)(void*)GetProcAddress(m, "CreateCoreWebView2EnvironmentWithOptions");
    if (!create) return false;
    fs::path data = AppDataDir() / L"WebView2";
    HRESULT hr = create(nullptr, data.c_str(), nullptr,
                        new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
                            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, OnEnvironment));
    if (FAILED(hr)) OnEnvironment(hr, nullptr);
    return true;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: Resize(); return 0;
        case WM_GETMINMAXINFO: {
            UINT dpi = GetDpiForWindow(h);
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {MulDiv(980, dpi, 96), MulDiv(640, dpi, 96)};
            return 0;
        }
        case WM_REPLY: {
            auto* s = (std::wstring*)lp;
            if (g_view) g_view->PostWebMessageAsJson(s->c_str());
            delete s;
            return 0;
        }
        case WM_DESTROY:
            if (g_controller) g_controller->Close();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
    g_devtools = cmd && wcsstr(cmd, L"--devtools");
    HANDLE single = CreateMutexW(nullptr, TRUE, L"USPF_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS && !(cmd && (wcsstr(cmd, L"--elevated") || wcsstr(cmd, L"--updated")))) {
        if (HWND other = FindWindowW(kClass, nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    if (cmd && wcsstr(cmd, L"--updated")) Sleep(500);  // let the old process exit
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring old = std::wstring(self) + L".old";
        DeleteFileW(old.c_str());  // left behind by a self-update
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(7, 11, 20));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    g_hwnd = CreateWindowExW(0, kClass, L"USPF - Unreal shader proxy fix", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1320, dpi, 96), MulDiv(860, dpi, 96), nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    COLORREF caption = RGB(7, 11, 20);
    DwmSetWindowAttribute(g_hwnd, 35 /*DWMWA_CAPTION_COLOR, Windows 11*/, &caption, sizeof caption);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    if (!StartWebView()) Fatal(L"USPF could not load WebView2Loader.dll.");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (single) CloseHandle(single);
    return 0;
}
