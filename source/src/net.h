// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace uspf {
struct HttpResult { int status = 0; std::string body; std::string error; };
// HTTPS/HTTP GET with WinHTTP. Follows redirects, 15 s timeouts, sends "User-Agent: USPF/<version>"
// and the extra headers given (each "Name: value"). maxBytes caps the body.
// status 0 + error text on failure.
HttpResult HttpGet(const std::string& urlUtf8, const std::vector<std::string>& headers = {}, size_t maxBytes = 64u << 20);
}  // namespace uspf
