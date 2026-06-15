#include "floaters.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/notification/NotificationOverlay.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <cctype>
#include <format>
#include <set>
#include <string>
#include <vector>

// Port of the Rust hyprmagic (floaters.rs). Special workspaces are named
// "special:<name>". Membership/visibility are derived by workspace id/name from
// g_pCompositor each call; effects go through Config::Actions::* + the executor.

namespace {

    const CHyprColor NOTIFY_COLOR_ERR{1.0, 0.2, 0.2, 1.0};

    inline SDispatchResult ok() {
        return {.success = true};
    }
    inline SDispatchResult fail(std::string msg) {
        return {.success = false, .error = std::move(msg)};
    }
    inline SDispatchResult fromAction(const Config::Actions::ActionResult& r, const char* what) {
        return r ? ok() : fail(std::format("{}: rejected by hyprland", what));
    }

    // String-returning queries can't propagate exceptions across the Lua/hyprctl
    // boundary; swallow to "" (logged). Mirrors hyprwsg_plugin's groups.cpp.
    template <typename Fn>
    std::string safeStr(const char* name, Fn&& body) {
        try {
            return body();
        } catch (const std::exception& e) {
            Log::logger->log(Log::ERR, std::format("[magic] {} threw: {}", name, e.what()));
        } catch (...) {
            Log::logger->log(Log::ERR, std::format("[magic] {} threw unknown exception", name));
        }
        return "";
    }

    template <typename Fn>
    SDispatchResult safeRun(const char* name, Fn&& body) {
        SDispatchResult result;
        try {
            result = body();
        } catch (const std::exception& e) {
            Log::logger->log(Log::ERR, std::format("[magic] {} threw: {}", name, e.what()));
            result = fail(std::format("{}: {}", name, e.what()));
        } catch (...) {
            Log::logger->log(Log::ERR, std::format("[magic] {} threw unknown exception", name));
            result = fail(std::format("{}: unknown exception", name));
        }
        if (!result.success && !result.error.empty())
            Notification::overlay()->addNotification(std::format("[magic] {}", result.error), NOTIFY_COLOR_ERR, 3000.f);
        return result;
    }

    std::string specialName(const std::string& name) {
        return "special:" + name;
    }

    // A floater name must be non-empty alphanumeric/-/_ (mirrors hyprwsg's
    // validName). This rejects: the empty name (which getWorkspaceIDNameFromString
    // would silently map to the default "special:" workspace), control bytes that
    // could forge waybar-state protocol rows, and — critically — ']' , which would
    // splice text past the "[workspace …]" rule into execInSpecial's /bin/sh -c.
    bool validName(const std::string& s) {
        if (s.empty())
            return false;
        for (unsigned char c : s)
            if (!(std::isalnum(c) || c == '-' || c == '_'))
                return false;
        return true;
    }

    // The id of special:<name> if it currently exists, else WORKSPACE_INVALID.
    WORKSPACEID specialId(const std::string& name) {
        auto ws = g_pCompositor->getWorkspaceByName(specialName(name));
        return ws ? ws->m_id : WORKSPACE_INVALID;
    }

    // Get-or-create the special workspace (mirrors the togglespecialworkspace
    // path in DispatcherTranslator.cpp). Null on failure.
    PHLWORKSPACE getOrCreateSpecial(const std::string& name) {
        const auto idname = getWorkspaceIDNameFromString(specialName(name));
        if (idname.id == WORKSPACE_INVALID || !g_pCompositor->isWorkspaceSpecial(idname.id))
            return nullptr;
        auto ws = g_pCompositor->getWorkspaceByID(idname.id);
        if (!ws) {
            if (auto m = Desktop::focusState()->monitor())
                ws = g_pCompositor->createNewWorkspace(idname.id, m->m_id, idname.name);
        }
        return ws;
    }

    std::vector<PHLWINDOW> clientsInSpecial(const std::string& name) {
        std::vector<PHLWINDOW> v;
        const WORKSPACEID      sid = specialId(name);
        if (sid == WORKSPACE_INVALID)
            return v;
        for (auto& w : g_pCompositor->m_windows)
            if (w && w->m_isMapped && w->workspaceID() == sid)
                v.push_back(w);
        return v;
    }

    bool isVisibleOnFocused(const std::string& name) {
        auto m = Desktop::focusState()->monitor();
        return m && m->m_activeSpecialWorkspace && m->m_activeSpecialWorkspace->m_name == specialName(name);
    }

    PHLMONITOR otherMonitorShowing(const std::string& name) {
        const std::string sn      = specialName(name);
        auto              focused = Desktop::focusState()->monitor();
        for (auto& m : g_pCompositor->m_monitors) {
            if (!m || (focused && m->m_id == focused->m_id))
                continue;
            if (m->m_activeSpecialWorkspace && m->m_activeSpecialWorkspace->m_name == sn)
                return m;
        }
        return nullptr;
    }

    // Exec `cmd` into special:<name>. Mirrors hl.dsp.exec_cmd(cmd, {workspace=…});
    // the exec rule places the window, and the executor runs it via /bin/sh -c.
    SDispatchResult execInSpecial(const std::string& name, const std::string& cmd) {
        auto pid = Config::Supplementary::executor()->spawnWithRules(std::format("[workspace {}] {}", specialName(name), cmd));
        return pid ? ok() : fail("exec failed");
    }

    std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
        if (from.empty())
            return s;
        for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
            s.replace(pos, from.size(), to);
        return s;
    }

} // namespace

namespace Magic {

    SDispatchResult toggle(std::string name, std::string klass, std::string launch) {
        return safeRun("toggle", [&]() -> SDispatchResult {
            if (!validName(name))
                return fail("toggle: invalid floater name");
            // launch given + workspace empty -> launch it in (no toggle, matches Rust).
            if (!launch.empty() && clientsInSpecial(name).empty())
                return execInSpecial(name, launch);
            auto ws = getOrCreateSpecial(name);
            if (!ws)
                return fail("toggle: bad special workspace");
            return fromAction(Config::Actions::toggleSpecial(ws), "toggle");
        });
    }

    SDispatchResult yank(std::string name) {
        return safeRun("yank", [&]() -> SDispatchResult {
            if (!validName(name))
                return fail("yank: invalid floater name");
            auto w = Desktop::focusState()->window();
            if (!w)
                return fail("no focused window");

            const WORKSPACEID sid       = specialId(name);
            const bool        inSpecial = sid != WORKSPACE_INVALID && w->workspaceID() == sid;

            if (inSpecial) {
                // Drop: move to the monitor's regular workspace, hide special, refocus.
                if (auto m = Desktop::focusState()->monitor(); m && m->m_activeWorkspace)
                    (void)Config::Actions::moveToWorkspace(m->m_activeWorkspace, /*silent=*/true, w);
                if (auto ws = getOrCreateSpecial(name))
                    (void)Config::Actions::toggleSpecial(ws);
                (void)Config::Actions::focus(w);
                return ok();
            }

            // Pull: move into the special, ensure visible, refocus.
            const bool visibleHere = isVisibleOnFocused(name);
            auto       ws          = getOrCreateSpecial(name);
            if (!ws)
                return fail("yank: bad special workspace");
            (void)Config::Actions::moveToWorkspace(ws, /*silent=*/true, w);
            if (!visibleHere) {
                if (auto other = otherMonitorShowing(name))
                    (void)Config::Actions::focusMonitor(other);
                else
                    (void)Config::Actions::toggleSpecial(ws);
            }
            (void)Config::Actions::focus(w);
            return ok();
        });
    }

    SDispatchResult kill(std::string name) {
        return safeRun("kill", [&]() -> SDispatchResult {
            if (!validName(name))
                return fail("kill: invalid floater name");
            auto toClose = clientsInSpecial(name); // snapshot before mutating
            for (auto& w : toClose)
                (void)Config::Actions::closeWindow(w);
            // Re-check: an app may pop its own close dialog and linger.
            if (clientsInSpecial(name).empty() && isVisibleOnFocused(name)) {
                if (auto ws = getOrCreateSpecial(name))
                    (void)Config::Actions::toggleSpecial(ws);
            }
            return ok();
        });
    }

    SDispatchResult safeKill(std::string name, std::string confirmCmd, std::string onEmpty) {
        return safeRun("safe_kill", [&]() -> SDispatchResult {
            if (!validName(name))
                return fail("safe_kill: invalid floater name");
            const WORKSPACEID sid        = specialId(name);
            bool              hasClients = false;
            bool              hasDialog  = false;
            if (sid != WORKSPACE_INVALID) {
                for (auto& w : g_pCompositor->m_windows) {
                    if (!w || !w->m_isMapped || w->workspaceID() != sid)
                        continue;
                    hasClients = true;
                    if (w->m_class == "safe-kill-confirm")
                        hasDialog = true;
                }
            }

            if (!hasClients) {
                if (!onEmpty.empty()) {
                    auto pid = Config::Supplementary::executor()->spawn(replaceAll(onEmpty, "{name}", name));
                    if (!pid)
                        return fail("on-empty exec failed");
                }
                return ok();
            }

            if (hasDialog)
                return kill(std::move(name)); // second press → kill everything

            // First press: show the workspace, launch the confirm dialog into it.
            if (!isVisibleOnFocused(name)) {
                if (auto ws = getOrCreateSpecial(name))
                    (void)Config::Actions::toggleSpecial(ws);
            }
            // Fallback default so safe_kill never silently no-ops when a binding
            // omits confirmCmd (non-app floaters in floaters.lua). The confirm
            // action calls the IN-PROCESS kill via `hyprctl eval`.
            //
            // Quoting must survive TWO shell layers: execInSpecial's `/bin/sh -c`
            // and confirm_dialog's `CMD="$*"; bash -c "$CMD"`. So the whole
            // `hyprctl eval "..."` is ONE single-quoted arg (passes through sh
            // verbatim, incl. the backslashes) and the Lua is double-quoted with
            // \"-escaped inner quotes (bash's `-c` collapses them). This mirrors
            // floaters.lua's build_confirm_cmd exactly — keep them in sync.
            if (confirmCmd.empty())
                confirmCmd = "confirm_dialog -b 551111 safe-kill-confirm 'Kill all windows?' "
                             "'hyprctl eval \"return hl.plugin.magic.kill(\\\"{name}\\\")\"'";
            return execInSpecial(name, replaceAll(confirmCmd, "{name}", name));
        });
    }

    // State dump for the waybar CFFI module (ports the semantic half of the
    // Rust waybar.rs build_output; icons/colors/markup stay waybar-side).
    // Wire format spec is authoritative in waybar/render.hpp — mirror below.
    // Format (US = 0x1f):
    //   monitor<US><resolved-name>          (empty name = monitor not resolved)
    //   fl<US><name><US><state>             per occupied floater, sorted by name;
    //                                       state: focused|visible|hidden
    //
    // The floater list does NOT depend on resolving the monitor: like the Rust
    // build_output, an unresolved monitor still lists every occupied floater
    // (focused never matches; visible = any monitor showing it), so a detection
    // miss or a hotplug transient degrades to dimmed-but-present, not a blank bar.
    std::string waybarState(std::string monitorArg) {
        return safeStr("waybar_state", [&]() {
            constexpr char US = '\x1f';
            PHLMONITOR     mon;
            for (auto& m : g_pCompositor->m_monitors)
                if (m && m->m_name == monitorArg) {
                    mon = m;
                    break;
                }
            if (!mon && !monitorArg.empty())
                for (auto& m : g_pCompositor->m_monitors)
                    if (m && m->m_description.find(monitorArg) != std::string::npos) {
                        mon = m;
                        break;
                    }

            std::string out = "monitor";
            out += US;
            out += mon ? mon->m_name : "";
            out += '\n';

            // Occupied floaters = special workspaces with a mapped client
            // (waybar.rs:83-92), sorted by name via std::set. Names with control
            // bytes are skipped: they can't be a real floater (floaters.lua names
            // are alnum/-/_) and would forge protocol rows on the bar.
            std::set<std::string> occupied;
            for (auto& w : g_pCompositor->m_windows) {
                if (!w || !w->m_isMapped)
                    continue;
                auto ws = g_pCompositor->getWorkspaceByID(w->workspaceID());
                if (!ws || !ws->m_name.starts_with("special:"))
                    continue;
                std::string name = ws->m_name.substr(8);
                if (!validName(name))
                    continue;
                occupied.insert(std::move(name));
            }

            for (const auto& name : occupied) {
                const std::string sn = specialName(name);
                const char*       state = "hidden";
                if (mon && mon->m_activeSpecialWorkspace && mon->m_activeSpecialWorkspace->m_name == sn)
                    state = "focused";
                else
                    for (auto& m : g_pCompositor->m_monitors)
                        if (m && (!mon || m->m_id != mon->m_id) && m->m_activeSpecialWorkspace && m->m_activeSpecialWorkspace->m_name == sn) {
                            state = "visible";
                            break;
                        }
                out += "fl";
                out += US;
                out += name;
                out += US;
                out += state;
                out += '\n';
            }
            return out;
        });
    }

} // namespace Magic
