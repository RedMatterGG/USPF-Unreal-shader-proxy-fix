// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// Removal of the Engine.ini streaming settings older USPF versions (0.2.0 - 0.2.2) wrote.
#pragma once
#include "games.h"

namespace uspf {

// Takes the old streaming lines (and their marker block) out of the game's Engine.ini files.
json RemoveStreaming(const Game& g);

// Exposed for tests.
std::string RemoveStreamingBlock(const std::string& text);
std::vector<std::pair<std::string, std::string>> ReadStreamingBlock(const std::string& text);

}  // namespace uspf
