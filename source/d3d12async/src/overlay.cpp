// Small on-screen label showing that d3d12async is loaded (and, optionally, live stats), and a
// settings panel in the bottom left with hotkey toggles.
//
// It is a separate click-through, non-activating, topmost window that follows the game's
// window; it never draws into the game's frames, so it can't affect rendering. It shows
// in windowed and borderless modes; exclusive fullscreen hides it. It hides itself when
// the game isn't the foreground window.
#include "async_pso.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>

namespace d3a {
namespace {

std::atomic<bool> g_started{false};
HWND g_overlay = nullptr;
HWND g_panel = nullptr;           // settings panel, bottom left
ULONGLONG g_toggledAt = 0;        // highlight the panel briefly after a hotkey toggle
HWND g_game = nullptr;
HFONT g_font = nullptr, g_fontSmall = nullptr;
int g_lineH = 16, g_smallH = 14, g_pad = 8;
UINT g_dpi = 96;

struct FindCtx { DWORD pid; HWND best; LONG bestArea; };

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || h == g_overlay || !IsWindowVisible(h)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT r;
    GetClientRect(h, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) { c->bestArea = area; c->best = h; }
    return TRUE;
}

HWND FindGameWindow() {
    FindCtx c{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&c));
    return c.bestArea >= 320 * 200 ? c.best : nullptr;
}

void MakeFonts(UINT dpi) {
    if (g_font && dpi == g_dpi) return;
    g_dpi = dpi;
    if (g_font) DeleteObject(g_font);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    auto px = [&](int v) { return MulDiv(v, (int)dpi, 96); };
    g_font = CreateFontW(-px(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontSmall = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_lineH = px(18);
    g_smallH = px(16);
    g_pad = px(8);
}

constexpr int kLines = 4;

struct Live {  // per-second rates for the label, sampled by the overlay timer
    ULONGLONG at = 0;
    uint64_t creates = 0, callUs = 0, placeholders = 0, skipped = 0;
    unsigned newPerSec = 0, bgPerSec = 0, skippedPerSec = 0;
    double blockedMsPerSec = 0;
} g_live;

void SampleLive() {
    ULONGLONG now = GetTickCount64();
    if (g_live.at && now - g_live.at < 1000) return;
    uint64_t c = g_stats.psoCreates.load() + g_stats.computeCreates.load(), p = g_stats.psoPlaceholders.load(),
             k = g_stats.drawsSkipped.load(), us = 0;
    for (int i = 0; i < kCallCauses; ++i) us += g_stats.callUsByCause[i].load();
    if (g_live.at) {
        double dt = (now - g_live.at) / 1000.0;
        g_live.newPerSec = (unsigned)((c - g_live.creates) / dt + 0.5);
        g_live.bgPerSec = (unsigned)((p - g_live.placeholders) / dt + 0.5);
        g_live.skippedPerSec = (unsigned)((k - g_live.skipped) / dt + 0.5);
        g_live.blockedMsPerSec = (us - g_live.callUs) / 1000.0 / dt;
    }
    g_live.at = now;
    g_live.creates = c; g_live.placeholders = p; g_live.skipped = k; g_live.callUs = us;
}

// l[0] title, l[1] hint, l[2] live compile activity, l[3] last hitch. Returns the hitch line's colour.
COLORREF Lines(wchar_t l[kLines][200]) {
    const size_t n = 200;
    if (!g_cfg.enabled) swprintf(l[0], n, L"d3d12async off (Enabled=0)%ls", g_cfg.measureWhenDisabled ? L", measuring" : L"");
    else if (StartupActive() && StartupCapping()) swprintf(l[0], n, L"d3d12async startup: compiling normally, %d fps cap", g_cfg.startupFpsCap);
    else if (StartupActive()) swprintf(l[0], n, L"d3d12async startup: compiling normally");
    else swprintf(l[0], n, L"d3d12async active");
    if (g_cfg.enabled && StartupActive() && g_cfg.startupHotkey)
        swprintf(l[1], n, L"configure in d3d12async.ini  \u00b7  Ctrl+Shift+End: end startup phase now");
    else
        swprintf(l[1], n, L"configure in d3d12async.ini");
    l[2][0] = l[3][0] = 0;
    COLORREF hitchColor = RGB(160, 168, 178);
    if (!g_cfg.overlayStats) return hitchColor;
    SampleLive();
    if (g_cfg.enabled)
        swprintf(l[2], n, L"compiling %zu  ·  last 1 s: %u new, %u in background, %.0f ms blocked, %u draws skipped", PoolPending(),
                 g_live.newPerSec, g_live.bgPerSec, g_live.blockedMsPerSec, g_live.skippedPerSec);
    else
        swprintf(l[2], n, L"last 1 s: %u new pipelines, %.0f ms blocked on game threads", g_live.newPerSec, g_live.blockedMsPerSec);
    Hitch h;
    if (LastHitch(&h)) {
        double ago = RunSeconds() - h.atSeconds;
        std::string d = DescribeHitch(h);
        swprintf(l[3], n, L"hitches %llu (%llu compile)  ·  last %.0f ms, %.0f s ago: %hs", (unsigned long long)HitchCount(false),
                 (unsigned long long)HitchCount(true), h.frameMs, ago, d.c_str());
        if (ago < 5) hitchColor = h.kind == HitchKind::NotPipeline ? RGB(235, 185, 80) : RGB(240, 95, 85);
    } else if (!StartupPhase()) {
        swprintf(l[3], n, L"no hitches ≥ %d ms so far", g_cfg.hitchMs);
    }
    return hitchColor;
}

int GraphW() { return MulDiv(260, (int)g_dpi, 96); }
int GraphH() { return MulDiv(40, (int)g_dpi, 96); }
bool ShowGraph() { return g_cfg.overlayStats && g_cfg.overlayGraph; }

SIZE Measure() {
    wchar_t l[kLines][200];
    Lines(l);
    HDC dc = GetDC(nullptr);
    SIZE s{0, 0}, t;
    HGDIOBJ old = SelectObject(dc, g_font);
    GetTextExtentPoint32W(dc, l[0], (int)wcslen(l[0]), &t);
    s.cx = t.cx;
    SelectObject(dc, g_fontSmall);
    int rows = 0;
    for (int i = 1; i < kLines; ++i) {
        if (!l[i][0]) continue;
        ++rows;
        GetTextExtentPoint32W(dc, l[i], (int)wcslen(l[i]), &t);
        if (t.cx > s.cx) s.cx = t.cx;
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    if (ShowGraph() && GraphW() > s.cx) s.cx = GraphW();
    s.cx += g_pad * 2 + g_pad / 2;
    s.cy = g_pad * 2 + g_lineH + rows * g_smallH + (ShowGraph() ? GraphH() + g_pad / 2 : 0);
    return s;
}

void DrawGraph(HDC dc, int x, int y) {
    static float ms[256];
    static bool comp[256];
    int w = GraphW(), h = GraphH();
    RECT box = {x, y, x + w, y + h};
    HBRUSH bg = CreateSolidBrush(RGB(28, 31, 37));
    FillRect(dc, &box, bg);
    DeleteObject(bg);
    int n = RecentFrames(ms, comp, 256);
    int bars = std::min(n, w / 2);
    float top = 50.0f;  // scale: at least 50 ms, up to the slowest frame shown (max 250 ms)
    for (int i = n - bars; i < n; ++i) top = std::max(top, std::min(ms[i], 250.0f));
    auto yOf = [&](float v) { return y + h - (int)(std::min(v, top) / top * (h - 1)); };
    if (g_cfg.hitchMs > 0 && g_cfg.hitchMs < top) {  // hitch threshold line
        HPEN pen = CreatePen(PS_DOT, 1, RGB(110, 70, 70));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, x, yOf((float)g_cfg.hitchMs), nullptr);
        LineTo(dc, x + w, yOf((float)g_cfg.hitchMs));
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    HBRUSH ok = CreateSolidBrush(RGB(90, 170, 120)), slow = CreateSolidBrush(RGB(235, 185, 80)), bad = CreateSolidBrush(RGB(240, 95, 85));
    for (int i = 0; i < bars; ++i) {
        float v = ms[n - bars + i];
        int bx = x + w - (bars - i) * 2;
        RECT r = {bx, yOf(v), bx + 2, y + h};
        bool hitch = g_cfg.hitchMs > 0 && v >= g_cfg.hitchMs;
        FillRect(dc, &r, comp[n - bars + i] ? bad : hitch ? slow : ok);
    }
    DeleteObject(ok);
    DeleteObject(slow);
    DeleteObject(bad);
}

void Paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    HBRUSH bg = CreateSolidBrush(RGB(16, 18, 22));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    RECT bar = {0, 0, MulDiv(3, (int)g_dpi, 96), r.bottom};
    HBRUSH accent = CreateSolidBrush(g_cfg.enabled ? RGB(80, 200, 120) : RGB(200, 160, 60));
    FillRect(dc, &bar, accent);
    DeleteObject(accent);
    wchar_t l[kLines][200];
    COLORREF hitchColor = Lines(l);
    SetBkMode(dc, TRANSPARENT);
    int x = g_pad + MulDiv(3, (int)g_dpi, 96), y = g_pad;
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(235, 238, 242));
    TextOutW(dc, x, y, l[0], (int)wcslen(l[0]));
    y += g_lineH;
    SelectObject(dc, g_fontSmall);
    for (int i = 1; i < kLines; ++i) {
        if (!l[i][0]) continue;
        SetTextColor(dc, i == 3 ? hitchColor : RGB(160, 168, 178));
        TextOutW(dc, x, y, l[i], (int)wcslen(l[i]));
        y += g_smallH;
    }
    if (ShowGraph()) DrawGraph(dc, x, y + g_pad / 2);
    EndPaint(h, &ps);
}

// ---------------------------------------------------------------------------------
// Settings panel: bottom left of the game window. The overlay windows are click-through (a click
// would take focus and the mouse away from the game), so settings are changed with hotkeys
// while the game has focus, and the panel shows their current state.

constexpr int kPanelLines = 3;

void PanelLines(wchar_t l[kPanelLines][160]) {
    const size_t n = 160;
    swprintf(l[0], n, L"d3d12async settings");
    swprintf(l[1], n, L"Ctrl+Shift+P  queue-full placeholders: %ls", g_cfg.queueFullPlaceholder ? L"ON" : L"OFF");
    if (g_cfg.queueFullPlaceholder)
        swprintf(l[2], n, L"past %d waiting compiles: placeholders up to %d outstanding (now %lld), then compile normally",
                 g_cfg.maxQueue, g_cfg.maxPlaceholders, (long long)OutstandingPlaceholders());
    else
        swprintf(l[2], n, L"past %d waiting compiles: compile normally (game waits)", g_cfg.maxQueue);
}

SIZE MeasurePanel() {
    wchar_t l[kPanelLines][160];
    PanelLines(l);
    HDC dc = GetDC(nullptr);
    SIZE s{0, 0}, t;
    HGDIOBJ old = SelectObject(dc, g_font);
    GetTextExtentPoint32W(dc, l[0], (int)wcslen(l[0]), &t);
    s.cx = t.cx;
    SelectObject(dc, g_fontSmall);
    for (int i = 1; i < kPanelLines; ++i) {
        GetTextExtentPoint32W(dc, l[i], (int)wcslen(l[i]), &t);
        if (t.cx > s.cx) s.cx = t.cx;
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    s.cx += g_pad * 2 + g_pad / 2 + MulDiv(3, (int)g_dpi, 96);
    s.cy = g_pad * 2 + g_lineH + (kPanelLines - 1) * g_smallH;
    return s;
}

void PaintPanel(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    bool flash = GetTickCount64() - g_toggledAt < 1500;
    HBRUSH bg = CreateSolidBrush(flash ? RGB(30, 40, 52) : RGB(16, 18, 22));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    RECT bar = {0, 0, MulDiv(3, (int)g_dpi, 96), r.bottom};
    HBRUSH accent = CreateSolidBrush(g_cfg.queueFullPlaceholder ? RGB(235, 185, 80) : RGB(80, 200, 120));
    FillRect(dc, &bar, accent);
    DeleteObject(accent);
    wchar_t l[kPanelLines][160];
    PanelLines(l);
    SetBkMode(dc, TRANSPARENT);
    int x = g_pad + MulDiv(3, (int)g_dpi, 96), y = g_pad;
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(235, 238, 242));
    TextOutW(dc, x, y, l[0], (int)wcslen(l[0]));
    y += g_lineH;
    SelectObject(dc, g_fontSmall);
    for (int i = 1; i < kPanelLines; ++i) {
        SetTextColor(dc, i == 1 ? (g_cfg.queueFullPlaceholder ? RGB(235, 185, 80) : RGB(120, 210, 150)) : RGB(160, 168, 178));
        TextOutW(dc, x, y, l[i], (int)wcslen(l[i]));
        y += g_smallH;
    }
    EndPaint(h, &ps);
}

// Polled every 50 ms; acts on the key going down, and only while the game has focus.
void PollHotkeys() {
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('P') & 0x8000);
    if (down && !wasDown && g_game && GetForegroundWindow() == g_game) {
        bool on = !g_cfg.queueFullPlaceholder.load();
        g_cfg.queueFullPlaceholder = on;
        g_toggledAt = GetTickCount64();
        Log(LogLevel::Info, "hotkey Ctrl+Shift+P: queue-full placeholders %s at %.1f s (QueueFullMode=%s)", on ? "ON" : "OFF", RunSeconds(),
            on ? "placeholder" : "compile");
        if (g_panel) InvalidateRect(g_panel, nullptr, FALSE);
    }
    wasDown = down;
}

void Follow() {
    if (!g_game || !IsWindow(g_game)) g_game = FindGameWindow();
    bool show = g_game && !IsIconic(g_game) && GetForegroundWindow() == g_game;
    if (!g_cfg.overlayFollowFocus) show = g_game && !IsIconic(g_game);
    if (!show) {
        ShowWindow(g_overlay, SW_HIDE);
        if (g_panel) ShowWindow(g_panel, SW_HIDE);
        return;
    }
    using GetDpi = UINT(WINAPI*)(HWND);
    static GetDpi getDpi = (GetDpi)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    MakeFonts(getDpi ? getDpi(g_game) : 96);
    POINT origin{0, 0};
    ClientToScreen(g_game, &origin);
    RECT client;
    GetClientRect(g_game, &client);
    SIZE s = Measure();
    int margin = MulDiv(16, (int)g_dpi, 96);
    int y = origin.y + (client.bottom - client.top) / 3;  // left side, upper third: away from most HUD corners
    SetWindowPos(g_overlay, HWND_TOPMOST, origin.x + margin, y, s.cx + MulDiv(3, (int)g_dpi, 96), s.cy,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    InvalidateRect(g_overlay, nullptr, FALSE);
    if (g_panel) {
        SIZE p = MeasurePanel();
        int py = origin.y + (client.bottom - client.top) - margin - p.cy;  // bottom left
        SetWindowPos(g_panel, HWND_TOPMOST, origin.x + margin, py, p.cx, p.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        InvalidateRect(g_panel, nullptr, FALSE);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            if (h == g_panel) PaintPanel(h);
            else Paint(h);
            return 0;
        case WM_TIMER:
            if (h != g_overlay) return 0;
            if (wp == 2) { PollHotkeys(); return 0; }
            Follow();
            return 0;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void OverlayThread() {
    HINSTANCE inst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&OverlayThread), &inst);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"d3d12async_overlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"d3d12async", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
    if (!g_overlay) {
        Log(LogLevel::Warn, "overlay: could not create its window (error %lu)", GetLastError());
        return;
    }
    SetLayeredWindowAttributes(g_overlay, 0, (BYTE)g_cfg.overlayOpacity, LWA_ALPHA);
    if (g_cfg.settingsPanel) {
        g_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                  wc.lpszClassName, L"d3d12async settings", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
        if (g_panel) SetLayeredWindowAttributes(g_panel, 0, (BYTE)g_cfg.overlayOpacity, LWA_ALPHA);
    }
    MakeFonts(96);
    SetTimer(g_overlay, 1, 250, nullptr);
    SetTimer(g_overlay, 2, 50, nullptr);  // hotkeys (work with the panel hidden too)
    Log(LogLevel::Info, "overlay started");
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

}  // namespace

void OverlayStart() {
    if (!g_cfg.overlay) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    std::thread(OverlayThread).detach();
}

}  // namespace d3a
