// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// Finding installed games (Steam, Epic, GOG, added by hand) and their per-user folders.
#pragma once
#include "util.h"

namespace uspf {

struct Game {
    std::string id;        // stable: hash of the renderer exe's folder
    std::string name;
    std::string source;    // steam | epic | gog | manual
    std::string appid;     // Steam appid when known (for artwork)
    UnrealTarget target;
};

// Scans the libraries, merges in games added by hand, drops hidden ones. Cached until Rescan.
std::vector<Game> Games(bool rescan);
std::optional<Game> FindGame(const std::string& id);
json GameJson(const Game& g);

// Adds a game by its exe (launcher stubs resolve to the Unreal renderer exe). Returns its id.
std::optional<Game> AddGame(const fs::path& exe, std::string* error);
void HideGame(const std::string& id);

// Folders scanned for games installed outside a store (X:\Games, Steam libraries Steam itself
// doesn't list, ...): the usual places on every fixed drive plus folders added by hand.
json LibraryFoldersJson();   // [{path, added (by hand), exists}]
bool AddLibraryFolder(const fs::path& dir);
void RemoveLibraryFolder(const fs::path& dir);
// Folder name of a repack / copied install made readable: "Palworld.v1.0.1.ZeiGames.com" -> "Palworld".
std::string CleanFolderName(const std::string& name);

// Every name a game's per-user folders may be called by (project, install folder, exe, product name).
std::vector<std::string> GameNames(const Game& g);
// Saved folders under %LOCALAPPDATA%, Documents\My Games, Documents, the install root.
std::vector<fs::path> SavedDirs(const Game& g);
// Saved\Config\<Platform> folders (where Engine.ini lives).
std::vector<fs::path> ConfigDirs(const Game& g);

// Steam: <steam>\appcache\librarycache art for an appid, if the client has it.
std::optional<fs::path> SteamLocalArt(const std::string& appid);

// Minimal Valve KeyValues (VDF/ACF) text parser; keys lower-cased. Exposed for tests.
json ParseVdf(const std::string& text);

}  // namespace uspf
