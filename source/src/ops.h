// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// What the installer script used to do: install / uninstall d3d12.dll, the Unreal Engine.ini
// precache block, the d3d12async.ini editor, shader-cache clearing; plus artwork and updates.
#pragma once
#include "games.h"

namespace uspf {

json GameStatus(const Game& g);
json Install(const Game& g, bool force, bool engineIni, const std::string& loadMode);
json Uninstall(const Game& g, bool engineIni);
json SetEngineIni(const Game& g, bool enable, const std::string& loadMode);

json IniGet(const Game& g);                    // schema from the bundled ini + the game's current values
json IniSet(const Game& g, const json& values);
json IniReset(const Game& g);

json CacheList(const Game* g);                 // driver caches (all games) + this game's own
json CacheClear(const Game* g, const json& kinds);

std::optional<fs::path> ArtFile(const Game& g);  // cached banner or icon, downloading if needed
json CheckUpdate();
json ApplyUpdate(const std::string& assetUrl);  // downloads the new exe, swaps it in, restarts

bool IsElevated();
// Settings block written to Engine.ini (between markers); exposed for tests.
std::vector<std::string> EngineIniBlock(const std::string& loadMode);
std::string ApplyEngineIniBlock(const std::string& text, bool enable, const std::string& loadMode);

}  // namespace uspf
