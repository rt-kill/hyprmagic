// Render probe: feed a `magic:waybar-state` reply on stdin + config overrides
// as key=value args (colors and icon-<name>=<glyph>), print text/tooltip/class
// separated by 0x1e. Driven by the self-contained golden test
// waybar/render_diff.sh (fixtures + assertions, no Rust binary or compositor).

#include "render.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    MagicConfig c;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const auto        eq  = arg.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = arg.substr(0, eq), v = arg.substr(eq + 1);
        if (k == "focused-bg")
            c.focusedBg = v;
        else if (k == "focused-fg")
            c.focusedFg = v;
        else if (k == "visible-bg")
            c.visibleBg = v;
        else if (k == "visible-fg")
            c.visibleFg = v;
        else if (k == "hidden-bg")
            c.hiddenBg = v;
        else if (k == "hidden-fg")
            c.hiddenFg = v;
        else if (k.starts_with("icon-"))
            c.icons[k.substr(5)] = v;
    }

    std::stringstream ss;
    ss << std::cin.rdbuf();
    const auto out = renderMagic(ss.str(), c);
    std::printf("%s\x1e%s\x1e%s", out.text.c_str(), out.tooltip.c_str(), out.cssClass.c_str());
    return 0;
}
