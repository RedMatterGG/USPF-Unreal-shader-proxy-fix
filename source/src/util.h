// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// Small helpers shared by the backend: strings, files, registry, paths.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace uspf {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr const char* kAppVersion = "0.3.1";
constexpr const char* kGithubOwner = "RedMatterGG";
constexpr const char* kGithubRepo = "USPF-Unreal-shader-proxy-fix";

std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);
std::string Utf8(const fs::path& p);
fs::path PathU8(const std::string& s);

std::string Lower(std::string s);
std::string NormalizedName(const std::string& s);  // lower-case letters and digits only
std::string Trim(const std::string& s);

std::optional<std::string> ReadFileText(const fs::path& p);
bool ReadFileBytes(const fs::path& p, std::vector<uint8_t>& out);
bool WriteFileBytes(const fs::path& p, const void* data, size_t n);  // atomic: temp file + rename
bool WriteFileText(const fs::path& p, const std::string& s);

std::optional<std::wstring> RegString(HKEY root, const wchar_t* subkey, const wchar_t* value, REGSAM view = 0);

fs::path KnownFolder(int csidl);       // SHGetFolderPathW
fs::path LocalAppData();
fs::path DocumentsDir();
fs::path AppDataDir();                 // %LOCALAPPDATA%\USPF (created)

// Embedded RCDATA resource by name (see resources.rc).
bool Resource(const wchar_t* name, const void** data, size_t* size);

std::string Hex64(uint64_t v);
uint64_t Fnv1a(const std::string& s);

// Win32 error text for the last error or an HRESULT.
std::string ErrorText(DWORD code);

// Files and folders that make a game "Unreal": <Root>\Engine, <Project>\Binaries\Win64\*-Shipping.exe.
struct UnrealTarget {
    fs::path exe;          // the real renderer exe (not a launcher stub)
    fs::path root;         // install root
    std::string project;   // Unreal project folder name (OakGame, Stalker2), empty if not Unreal
    bool isUnreal = false;
};
std::optional<UnrealTarget> ResolveTarget(const fs::path& exe);
std::optional<UnrealTarget> FindUnrealInRoot(const fs::path& root, const std::string& preferStem = "");

// Product name / description from an exe's version resource.
std::vector<std::string> ExeVersionNames(const fs::path& exe);

}  // namespace uspf
