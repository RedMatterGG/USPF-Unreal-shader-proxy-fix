// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace uspf {
// Largest icon of an exe (up to 256x256) encoded as PNG with alpha. Empty on failure.
// Caller has already called CoInitializeEx.
std::vector<uint8_t> ExeIconPng(const std::wstring& exePath);
}  // namespace uspf
