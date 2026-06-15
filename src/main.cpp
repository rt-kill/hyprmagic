#include "floaters.hpp"
#include "lua_bindings.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <stdexcept>
#include <string>

namespace {
    HANDLE           g_handle = nullptr;

    const CHyprColor NOTIFY_COLOR_ERROR{1.0, 0.2, 0.2, 1.0};
    constexpr int    NOTIFY_TIMEOUT_MS = 10000;
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

#ifndef HYPRMAGIC_NO_VERSION_CHECK
    // Compare only the git commit hash prefix (before _aq_), like splay/hyprwsg.
    const std::string COMPOSITOR_HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH     = __hyprland_api_get_client_hash();
    const auto        gitOnly         = [](const std::string& s) { return s.substr(0, s.find("_aq_")); };

    if (gitOnly(COMPOSITOR_HASH) != gitOnly(CLIENT_HASH)) {
        HyprlandAPI::addNotification(g_handle, "[hyprmagic] compiled for a different hyprland version; refusing to load.", NOTIFY_COLOR_ERROR, NOTIFY_TIMEOUT_MS);
        throw std::runtime_error("[hyprmagic] hyprland version mismatch: compositor='" + COMPOSITOR_HASH + "' client='" + CLIENT_HASH + "'");
    }
#endif

    // Lua bindings (hl.plugin.magic.*) — the floater keybinds call these.
    // No wsg:*-style dispatchers: the commands take multiple positional args,
    // which don't map to the single-string dispatcher signature, and the binds
    // use the Lua functions anyway.
    Magic::registerLuaBindings(handle);

    // magic:waybar-state <monitor> — semantic state for the waybar CFFI module.
    // hyprctl prints the handler's return string (auto-unregistered on unload).
    // Prefix-matched (exact=false) so the monitor arg rides after the first space.
    // hyprctl appends the final newline itself, and an EMPTY reply would print
    // "unknown request" — so strip a trailing '\n' and pad "" to "\n".
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
        .name  = "magic:waybar-state",
        .exact = false,
        .fn    = [](eHyprCtlOutputFormat, std::string request) {
            const auto  sp  = request.find(' ');
            std::string arg = sp == std::string::npos ? std::string{} : request.substr(sp + 1);
            auto        out = Magic::waybarState(std::move(arg));
            if (!out.empty() && out.back() == '\n')
                out.pop_back();
            return out.empty() ? std::string{"\n"} : out;
        },
    });

    return {
        .name        = "hyprmagic",
        .description = "Special-workspace floaters (toggle/yank/kill/safe-kill), in-process",
        .author      = "rolandtk",
        .version     = "0.1.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Lua functions are removed automatically on plugin unload; no hooks,
    // listeners, or caches to tear down.
}
