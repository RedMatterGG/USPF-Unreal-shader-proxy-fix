// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// d3d12async.ini: read keys with their comment help, change values in place (comments kept).
#pragma once
#include <string>
#include <utility>
#include <vector>
namespace uspf {
struct IniKey { std::string key, value, help; };
// Ordered keys. help = the ';' comment lines directly above a key, shared by the keys that follow
// it without a blank line in between.
std::vector<IniKey> ParseIni(const std::string& text);
// Replaces values in place (first non-comment "Key=" line, key matched case-insensitively);
// missing keys are appended to the [d3d12async] section.
std::string UpdateIni(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values);
}  // namespace uspf
