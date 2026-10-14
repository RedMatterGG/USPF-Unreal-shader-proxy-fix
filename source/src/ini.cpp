// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
#include "ini.h"

#include <algorithm>
#include <cctype>

namespace uspf {
namespace {

struct Line { std::string text, end; };

std::vector<Line> Split(const std::string& t) {
    std::vector<Line> out;
    size_t i = 0;
    while (i < t.size()) {
        size_t j = i;
        while (j < t.size() && t[j] != '\n' && t[j] != '\r') ++j;
        Line l{t.substr(i, j - i), ""};
        if (j < t.size()) {
            if (t[j] == '\r' && j + 1 < t.size() && t[j + 1] == '\n') { l.end = "\r\n"; j += 2; }
            else { l.end = std::string(1, t[j]); ++j; }
        }
        out.push_back(l);
        i = j;
    }
    return out;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool IsSection(const std::string& l) {
    std::string t = Trim(l);
    return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}
bool IsComment(const std::string& l) {
    std::string t = Trim(l);
    return !t.empty() && (t[0] == ';' || t[0] == '#');
}

}  // namespace

std::vector<IniKey> ParseIni(const std::string& text) {
    std::vector<IniKey> out;
    std::string pending, active;
    bool prevKey = false;
    for (auto& l : Split(text)) {
        std::string t = Trim(l.text);
        if (t.empty() || IsSection(t)) { pending.clear(); active.clear(); prevKey = false; continue; }
        if (IsComment(t)) {
            std::string c = t.substr(1);
            if (!c.empty() && c[0] == ' ') c.erase(0, 1);
            if (prevKey) { pending.clear(); prevKey = false; }  // a comment after keys starts new help
            pending += (pending.empty() ? "" : "\n") + c;
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) { pending.clear(); active.clear(); prevKey = false; continue; }
        if (!pending.empty()) { active = pending; pending.clear(); }
        else if (!prevKey) active.clear();
        out.push_back({Trim(t.substr(0, eq)), Trim(t.substr(eq + 1)), active});
        prevKey = true;
    }
    return out;
}

std::string UpdateIni(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values) {
    auto lines = Split(text);
    std::string nl = "\n";
    for (auto& l : lines) if (l.end == "\r\n") { nl = "\r\n"; break; }
    std::vector<std::pair<std::string, std::string>> missing;
    for (auto& [k, v] : values) {
        bool done = false;
        for (auto& l : lines) {
            if (IsComment(l.text) || IsSection(l.text)) continue;
            size_t eq = l.text.find('=');
            if (eq == std::string::npos || Lower(Trim(l.text.substr(0, eq))) != Lower(k)) continue;
            l.text = l.text.substr(0, eq + 1) + v;
            done = true;
            break;
        }
        if (!done) missing.push_back({k, v});
    }
    if (!missing.empty()) {
        int header = -1, end = (int)lines.size();
        for (int i = 0; i < (int)lines.size(); ++i)
            if (IsSection(lines[i].text) && Lower(Trim(lines[i].text)) == "[d3d12async]") header = i;
        if (header < 0) {
            if (!lines.empty() && lines.back().end.empty()) lines.back().end = nl;
            lines.push_back({"[d3d12async]", nl});
            header = (int)lines.size() - 1;
            end = (int)lines.size();
        } else {
            for (int i = header + 1; i < (int)lines.size(); ++i) if (IsSection(lines[i].text)) { end = i; break; }
        }
        int last = header;
        for (int i = header + 1; i < end; ++i) if (!Trim(lines[i].text).empty()) last = i;
        if (lines[last].end.empty()) lines[last].end = nl;
        std::vector<Line> add;
        for (auto& [k, v] : missing) add.push_back({k + "=" + v, nl});
        lines.insert(lines.begin() + last + 1, add.begin(), add.end());
    }
    std::string out;
    for (auto& l : lines) out += l.text + l.end;
    return out;
}

}  // namespace uspf
