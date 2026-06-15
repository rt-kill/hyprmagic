#include "render.hpp"

#include <format>
#include <vector>

namespace {

    constexpr char US = '\x1f';

    std::vector<std::string> splitOn(const std::string& s, char sep) {
        std::vector<std::string> out;
        size_t                   start = 0;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == sep) {
                out.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        return out;
    }

    // waybar.rs:45-58
    std::string pangoEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            switch (ch) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '\'': out += "&apos;"; break;
                case '"': out += "&quot;"; break;
                default: out += ch;
            }
        }
        return out;
    }

} // namespace

RenderOut renderMagic(const std::string& state, const MagicConfig& c) {
    struct Fl {
        std::string name, st;
    };
    std::vector<Fl> fls; // arrives sorted by name from the plugin

    for (const auto& line : splitOn(state, '\n')) {
        if (line.empty())
            continue;
        const auto f = splitOn(line, US);
        if (f[0] == "fl" && f.size() >= 3)
            fls.push_back({f[1], f[2]});
    }

    // waybar.rs:119-184
    std::string markup;
    std::string tooltip;
    bool        hasVisible = false;

    for (size_t i = 0; i < fls.size(); i++) {
        if (i > 0) {
            markup += ' ';
            tooltip += '\n';
        }
        const auto&       it   = c.icons.find(fls[i].name);
        const std::string icon = it != c.icons.end() ? it->second : pangoEscape(fls[i].name);

        const std::string *bg, *fg;
        if (fls[i].st == "focused") {
            bg         = &c.focusedBg;
            fg         = &c.focusedFg;
            hasVisible = true;
            tooltip += fls[i].name + " (focused)";
        } else if (fls[i].st == "visible") {
            bg         = &c.visibleBg;
            fg         = &c.visibleFg;
            hasVisible = true;
            tooltip += fls[i].name + " (visible elsewhere)";
        } else {
            bg = &c.hiddenBg;
            fg = &c.hiddenFg;
            tooltip += fls[i].name + " (hidden)";
        }
        markup += std::format("<span background='{}' foreground='{}' weight='bold'> {} </span>", *bg, *fg, icon);
    }

    const std::string cls = fls.empty() ? "empty" : (hasVisible ? "has-visible" : "all-hidden");
    return {markup, tooltip, cls};
}
