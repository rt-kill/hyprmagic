#pragma once

// Special-workspace "floater" commands, ported from the Rust hyprmagic's
// floaters.rs. Each floater is a special workspace ("special:<name>"). Stateless:
// every command rebuilds its view from g_pCompositor on each call. In-process
// effects go through Config::Actions::* and the executor (no IPC, no dispatch
// strings). The waybar indicator is a native CFFI module (see ../waybar/) that
// reads waybarState() via the magic:waybar-state hyprctl command.

#include <hyprland/src/plugins/PluginAPI.hpp> // SDispatchResult

#include <string>

namespace Magic {
    // `name` is the special-workspace identifier (the part after "special:").
    // `klass` is RESERVED (window-class hint, currently unused — kept for parity
    // with the Rust CLI's positional arg and floaters.lua's `class` field).
    // toggle: launch empty -> pure show/hide; launch given + workspace empty ->
    //         exec it into the workspace; otherwise toggle visibility.
    SDispatchResult toggle(std::string name, std::string klass, std::string launch);
    // yank: pull the focused window into special:name, or drop it back out.
    SDispatchResult yank(std::string name);
    // kill: close every window in special:name, hide it if it ends up empty.
    SDispatchResult kill(std::string name);
    // safe_kill: empty -> run onEmpty; confirm dialog present -> kill; else show
    //            + launch confirmCmd ({name} is substituted) into the workspace.
    SDispatchResult safeKill(std::string name, std::string confirmCmd, std::string onEmpty);

    // Semantic state dump for the waybar CFFI module (hyprctl magic:waybar-state
    // <monitor>). Line-based, fields joined by 0x1f (US). Monitor arg matches by
    // connector name first, then description substring. Read-only.
    std::string waybarState(std::string monitorArg);
}
