// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// Editing [SystemSettings] in a game's Engine.ini. Unreal rewrites that file when the game exits
// and drops comment lines, so USPF's "; >>> ..." markers can't be relied on: the settings it
// manages are found and replaced by their keys instead, wherever they are in [SystemSettings].
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

namespace uspf {

// Last value of each key (matched case-insensitively) inside [SystemSettings] sections.
std::map<std::string, std::string> ReadSystemSettings(const std::string& text, const std::set<std::string>& lowerKeys);

// Removes the marked block (begin..end markers) and every [SystemSettings] line whose key is in
// lowerKeys, then, if block is not empty, inserts block (markers included) right after the first
// [SystemSettings] header (a new section at the end if there is none). Other lines, sections and
// the file's line endings are kept. Returns text unchanged if nothing would change.
std::string RewriteSystemSettings(const std::string& text, const std::string& beginMarker, const std::string& endMarker,
                                  const std::set<std::string>& lowerKeys, const std::vector<std::string>& block);

// Keys from lowerKeys that are also set in a section other than [SystemSettings]
// ([ConsoleVariables] overrides [SystemSettings]; project sections are overridden by it).
std::map<std::string, std::string> KeysInOtherSections(const std::string& text, const std::set<std::string>& lowerKeys);

}  // namespace uspf
