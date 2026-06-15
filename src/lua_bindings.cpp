#include "lua_bindings.hpp"
#include "floaters.hpp"

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <string>

namespace {

    // Read Lua stack index `idx` as a string. Coerces numbers; "" for
    // nil/none/non-coercible. Deliberately does NOT luaL_error: a longjmp here
    // would skip the destructors of sibling argument temporaries already built
    // for the same call (a leak), and can't safely unwind C++ frames.
    std::string argStr(lua_State* L, int idx) {
        if (lua_isnoneornil(L, idx))
            return "";
        size_t      len = 0;
        const char* s   = lua_tolstring(L, idx, &len);
        return s ? std::string(s, len) : std::string();
    }

    // Every entry point is noexcept + try/catch: Hyprland's config Lua is pure C
    // (longjmp), so a C++ exception escaping into lua_pcall is undefined behavior.
    // The command bodies have their own safeRun, but argument construction here does not.
    int l_toggle(lua_State* L) noexcept {
        try { Magic::toggle(argStr(L, 1), argStr(L, 2), argStr(L, 3)); } catch (...) {}
        return 0;
    }
    int l_yank(lua_State* L) noexcept {
        try { Magic::yank(argStr(L, 1)); } catch (...) {}
        return 0;
    }
    int l_kill(lua_State* L) noexcept {
        try { Magic::kill(argStr(L, 1)); } catch (...) {}
        return 0;
    }
    int l_safekill(lua_State* L) noexcept {
        try { Magic::safeKill(argStr(L, 1), argStr(L, 2), argStr(L, 3)); } catch (...) {}
        return 0;
    }

} // namespace

namespace Magic {
    void registerLuaBindings(HANDLE handle) {
        const CHyprColor INFO{0.3, 0.7, 1.0, 1.0};
        int total = 0, ok = 0;
        auto add = [&](const char* name, PLUGIN_LUA_FN fn) {
            ++total;
            if (HyprlandAPI::addLuaFunction(handle, "magic", name, fn))
                ++ok;
        };

        add("toggle",    l_toggle);    // hl.plugin.magic.toggle(name, class, launch)
        add("yank",      l_yank);      // hl.plugin.magic.yank(name)
        add("kill",      l_kill);      // hl.plugin.magic.kill(name)
        add("safe_kill", l_safekill);  // hl.plugin.magic.safe_kill(name, confirm_cmd, on_empty)

        HyprlandAPI::addNotification(handle,
            "[magic] registered " + std::to_string(ok) + "/" + std::to_string(total) + " lua bindings",
            INFO, 8000);
    }
} // namespace Magic
