#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp> // HANDLE

namespace Magic {
    // Register hl.plugin.magic.* Lua functions. Call once in PLUGIN_INIT.
    void registerLuaBindings(HANDLE handle);
}
