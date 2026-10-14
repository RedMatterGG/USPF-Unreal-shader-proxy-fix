// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// Run logs written by d3d12.dll (<game>\d3d12async-runs). The DLL keeps only the latest runs, so
// USPF copies every run it sees into %LOCALAPPDATA%\USPF\runs\<game id> to keep a history.
#pragma once
#include "games.h"

namespace uspf {

json ListRuns(const Game& g);                         // newest first, summaries only
json RunDetail(const Game& g, const std::string& run);  // per-second series, frame times, hitches
json ParseRunSummary(const std::string& txt);         // exposed for tests
json ParseCsv(const std::string& text);               // {header:[...], rows:[[...]]}; exposed for tests

}  // namespace uspf
