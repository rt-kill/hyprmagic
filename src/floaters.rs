use crate::error::Result;
use crate::hyprland;
use crate::ipc;

/// Get addresses of all clients in a special workspace.
fn clients_in_special(name: &str) -> Result<Vec<String>> {
    let ws_name = format!("special:{name}");
    let clients = hyprland::get_clients()?;
    Ok(clients
        .into_iter()
        .filter(|c| c.workspace.name == ws_name)
        .map(|c| c.address)
        .collect())
}

/// Check if the special workspace is visible on the focused monitor.
fn is_visible_on_focused(name: &str) -> Result<bool> {
    let ws_name = format!("special:{name}");
    let monitors = hyprland::get_monitors()?;
    Ok(monitors
        .iter()
        .any(|m| m.focused && m.special_workspace.name == ws_name))
}

/// Find a non-focused monitor that is showing this special workspace.
fn other_monitor_showing(name: &str) -> Result<Option<String>> {
    let ws_name = format!("special:{name}");
    let monitors = hyprland::get_monitors()?;
    Ok(monitors
        .into_iter()
        .find(|m| !m.focused && m.special_workspace.name == ws_name)
        .map(|m| m.name))
}

/// Toggle a special workspace. If `launch_cmd` is non-empty and the workspace
/// is empty, the command is exec'd into it; otherwise the workspace's
/// visibility is toggled. With no launch_cmd, this is a pure show/hide.
pub fn toggle(name: &str, _class: Option<&str>, launch_cmd: &[String]) -> Result<()> {
    if launch_cmd.is_empty() {
        // Pure toggle — no app to launch
        return hyprland::toggle_special_workspace(name);
    }
    let addrs = clients_in_special(name)?;
    if addrs.is_empty() {
        let cmd = shell_words(launch_cmd);
        hyprland::exec_in_special(name, &cmd)?;
    } else {
        hyprland::toggle_special_workspace(name)?;
    }
    Ok(())
}

/// Pull focused window into special workspace, or drop it out.
pub fn yank(name: &str) -> Result<()> {
    let active = hyprland::get_active_window()?;
    let addr = &active.address;
    let ws_name = format!("special:{name}");

    if active.workspace.name == ws_name {
        // Drop: move to active (non-special) workspace, hide, refocus
        let active_ws = hyprland::get_active_workspace()?;
        hyprland::move_to_workspace_silent(&active_ws.name, addr)?;
        hyprland::toggle_special_workspace(name)?;
        hyprland::focus_window(addr)?;
    } else {
        // Pull: move into special workspace and ensure it's visible
        let visible_here = is_visible_on_focused(name)?;

        hyprland::move_to_workspace_silent(&ws_name, addr)?;

        if !visible_here {
            if let Some(other) = other_monitor_showing(name)? {
                hyprland::focus_monitor(&other)?;
            } else {
                hyprland::toggle_special_workspace(name)?;
            }
        }
        hyprland::focus_window(addr)?;
    }
    Ok(())
}

/// Close all windows in the special workspace and hide it if empty.
pub fn kill(name: &str) -> Result<()> {
    let addrs = clients_in_special(name)?;
    let commands: Vec<String> = addrs
        .iter()
        .map(|a| {
            format!(
                "dispatch hl.dsp.window.close({{ window = \"address:{}\" }})",
                hyprland::lua_str(a)
            )
        })
        .collect();
    ipc::dispatch_batch(&commands)?;

    // Only hide if workspace is now empty (apps may show their own close dialog)
    let remaining = clients_in_special(name)?;
    if remaining.is_empty() && is_visible_on_focused(name)? {
        hyprland::toggle_special_workspace(name)?;
    }
    Ok(())
}

/// Two-press kill: first press shows confirm dialog, second kills all.
pub fn safe_kill(name: &str, confirm_cmd: &str, on_empty: Option<&str>) -> Result<()> {
    let ws_name = format!("special:{name}");
    let clients = hyprland::get_clients()?;

    // Check if workspace has any windows at all
    let has_clients = clients.iter().any(|c| c.workspace.name == ws_name);
    if !has_clients {
        if let Some(cmd) = on_empty {
            let cmd = cmd.replace("{name}", name);
            std::process::Command::new("sh")
                .args(["-c", &cmd])
                .spawn()
                .map_err(|e| crate::error::Error::Other(format!("on-empty exec: {e}")))?;
        }
        return Ok(());
    }

    // Check if confirm dialog is already present
    let has_dialog = clients
        .iter()
        .any(|c| c.workspace.name == ws_name && c.class == "safe-kill-confirm");

    if has_dialog {
        // Second press — kill everything
        kill(name)?;
    } else {
        // First press — show workspace and launch confirm dialog
        if !is_visible_on_focused(name)? {
            hyprland::toggle_special_workspace(name)?;
        }
        let dialog_cmd = confirm_cmd.replace("{name}", name);
        hyprland::exec_in_special(name, &dialog_cmd)?;
    }
    Ok(())
}

/// Join command parts, quoting any that contain spaces.
fn shell_words(parts: &[String]) -> String {
    parts
        .iter()
        .map(|p| {
            if p.contains(' ') || p.contains('\'') || p.contains('"') {
                format!("'{}'", p.replace('\'', "'\\''"))
            } else {
                p.clone()
            }
        })
        .collect::<Vec<_>>()
        .join(" ")
}
