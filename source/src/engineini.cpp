// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
#include "engineini.h"

#include <cctype>

namespace uspf {
namespace {

std::vector<std::string> Lines(const std::string& t) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find('\n', i);
        std::string l = t.substr(i, (j == std::string::npos ? t.size() : j) - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        if (j == std::string::npos) break;
        i = j + 1;
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

bool IsSection(const std::string& t) { return t.size() >= 2 && t.front() == '[' && t.back() == ']'; }

// Key of a "Key=Value" line (lower-case), or empty.
std::string KeyOf(const std::string& line, std::string* value = nullptr) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || IsSection(t)) return "";
    size_t eq = t.find('=');
    if (eq == std::string::npos) return "";
    if (value) *value = Trim(t.substr(eq + 1));
    return Lower(Trim(t.substr(0, eq)));
}

}  // namespace

std::map<std::string, std::string> ReadSystemSettings(const std::string& text, const std::set<std::string>& keys) {
    std::map<std::string, std::string> out;
    bool in = false;
    for (auto& l : Lines(text)) {
        std::string t = Trim(l);
        if (IsSection(t)) { in = Lower(t) == "[systemsettings]"; continue; }
        if (!in) continue;
        std::string v, k = KeyOf(l, &v);
        if (!k.empty() && keys.count(k)) out[k] = v;
    }
    return out;
}

std::map<std::string, std::string> KeysInOtherSections(const std::string& text, const std::set<std::string>& keys) {
    std::map<std::string, std::string> out;
    std::string section;
    for (auto& l : Lines(text)) {
        std::string t = Trim(l);
        if (IsSection(t)) { section = t; continue; }
        if (Lower(section) == "[systemsettings]" || section.empty()) continue;
        std::string k = KeyOf(l);
        if (!k.empty() && keys.count(k)) out[k] = section;
    }
    return out;
}

std::string RewriteSystemSettings(const std::string& text, const std::string& begin, const std::string& end,
                                  const std::set<std::string>& keys, const std::vector<std::string>& block) {
    std::string nl = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
    std::vector<std::string> out;
    bool inBlock = false, inSys = false;
    for (auto& l : Lines(text)) {
        if (l == begin) { inBlock = true; continue; }
        if (inBlock) { if (l == end) inBlock = false; continue; }
        std::string t = Trim(l);
        if (IsSection(t)) inSys = Lower(t) == "[systemsettings]";
        else if (inSys) {
            std::string k = KeyOf(l);
            if (!k.empty() && keys.count(k)) continue;  // a managed key: replaced below
        }
        out.push_back(l);
    }
    if (!block.empty()) {
        std::vector<std::string> b;
        b.push_back(begin);
        b.insert(b.end(), block.begin(), block.end());
        b.push_back(end);
        int idx = -1;
        for (int i = 0; i < (int)out.size(); ++i)
            if (Lower(Trim(out[i])) == "[systemsettings]") { idx = i; break; }
        if (idx >= 0) out.insert(out.begin() + idx + 1, b.begin(), b.end());
        else {
            if (!out.empty() && !Trim(out.back()).empty()) out.push_back("");
            out.push_back("[SystemSettings]");
            out.insert(out.end(), b.begin(), b.end());
        }
    }
    std::string s;
    for (auto& l : out) s += l + nl;
    // Same content apart from a missing final newline: leave the file alone.
    auto norm = [](std::string x) {
        std::string o;
        for (char c : x) if (c != '\r') o += c;
        while (!o.empty() && o.back() == '\n') o.pop_back();
        return o;
    };
    return norm(s) == norm(text) ? text : s;
}

}  // namespace uspf
