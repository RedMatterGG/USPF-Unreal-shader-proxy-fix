// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// First half drafted by a FreeLLMAPI agent (gemini-3.8-flash; its reply was cut off), completed here.
#include <windows.h>
#include <winhttp.h>

#include "net.h"
#include "util.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace uspf {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HINTERNET() const { return h; }
};

HttpResult MakeError(DWORD err) {
    HttpResult res;
    wchar_t buffer[512] = {};
    DWORD len = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               GetModuleHandleW(L"winhttp.dll"), err, 0, buffer, 512, nullptr);
    while (len > 0 && (buffer[len - 1] == L'\r' || buffer[len - 1] == L'\n' || buffer[len - 1] == L' ')) buffer[--len] = 0;
    res.error = len ? Utf8(std::wstring(buffer, len)) : "WinHTTP error " + std::to_string(err);
    return res;
}

}  // namespace

HttpResult HttpGet(const std::string& urlUtf8, const std::vector<std::string>& headers, size_t maxBytes) {
    std::wstring wUrl = Wide(urlUtf8);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.size(), 0, &uc)) return MakeError(GetLastError());
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path;
    if (uc.dwUrlPathLength) path.append(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    std::wstring ua = Wide(std::string("USPF/") + kAppVersion);
    HINTERNET raw = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!raw) raw = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!raw) return MakeError(GetLastError());
    Handle session(raw);
    WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000);
    Handle connect(WinHttpConnect(session, host.c_str(), uc.nPort, 0));
    if (!connect) return MakeError(GetLastError());
    Handle request(WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return MakeError(GetLastError());
    for (auto& hd : headers) {
        std::wstring w = Wide(hd);
        WinHttpAddRequestHeaders(request, w.c_str(), (DWORD)w.size(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
        return MakeError(GetLastError());

    HttpResult res;
    DWORD status = 0, sz = sizeof status;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &sz, WINHTTP_NO_HEADER_INDEX))
        return MakeError(GetLastError());
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) return MakeError(GetLastError());
        if (!avail) break;
        if (res.body.size() + avail > maxBytes) { HttpResult e; e.error = "response too large"; return e; }
        size_t old = res.body.size();
        res.body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, res.body.data() + old, avail, &read)) return MakeError(GetLastError());
        res.body.resize(old + read);
        if (!read) break;
    }
    res.status = (int)status;
    return res;
}

}  // namespace uspf
