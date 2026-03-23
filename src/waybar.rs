use std::collections::HashMap;
use std::fmt::Write as FmtWrite;
use std::io::{self, Read, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

use crate::error::Result;
use crate::hyprland;
use crate::ipc;

pub struct Colors {
    pub focused_bg: String,
    pub focused_fg: String,
    pub visible_bg: String,
    pub visible_fg: String,
    pub hidden_bg: String,
    pub hidden_fg: String,
}

/// Debounce timeout: after receiving events, wait this long for more
/// before emitting an update.
const DEBOUNCE: Duration = Duration::from_millis(20);

/// Maximum size of the pending event buffer before truncation.
const MAX_PENDING: usize = 64 * 1024;

fn is_relevant_event(event: &str) -> bool {
    event.starts_with("openwindow>>")
        || event.starts_with("closewindow>>")
        || event.starts_with("movewindow>>")
        || event.starts_with("activespecial>>")
        || event.starts_with("createworkspace>>")
        || event.starts_with("destroyworkspace>>")
}

/// Look up an icon from the CLI-provided map, falling back to the workspace name.
fn icon_for(name: &str, icons: &HashMap<String, String>) -> String {
    match icons.get(name) {
        Some(icon) => icon.clone(),
        None => pango_escape(name),
    }
}

/// Escape a string for safe inclusion in Pango markup.
fn pango_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '\'' => out.push_str("&apos;"),
            '"' => out.push_str("&quot;"),
            _ => out.push(c),
        }
    }
    out
}

/// Build the Waybar JSON output and write it to stdout.
/// Returns false if stdout is broken (Waybar closed), signaling we should exit.
fn emit_output(monitor: &str, icons: &HashMap<String, String>, colors: &Colors) -> bool {
    match build_output(monitor, icons, colors) {
        Ok(json) => {
            let stdout = io::stdout();
            let mut out = stdout.lock();
            if writeln!(out, "{json}").is_err() || out.flush().is_err() {
                return false;
            }
        }
        Err(e) => {
            eprintln!("hyprmagic waybar: {e}");
        }
    }
    true
}

fn build_output(monitor: &str, icons: &HashMap<String, String>, colors: &Colors) -> Result<String> {
    let clients = hyprland::get_clients()?;
    let monitors = hyprland::get_monitors()?;

    // Find all special workspaces that have at least one client.
    let mut occupied: Vec<String> = Vec::new();
    for client in &clients {
        if let Some(name) = client.workspace.name.strip_prefix("special:") {
            let name = name.to_string();
            if !occupied.contains(&name) {
                occupied.push(name);
            }
        }
    }
    occupied.sort();

    // Which special workspace is visible on *this* monitor?
    let focused_on_this: Option<String> = monitors
        .iter()
        .find(|m| m.name == monitor)
        .and_then(|m| {
            m.special_workspace
                .name
                .strip_prefix("special:")
                .filter(|n| !n.is_empty())
                .map(String::from)
        });

    // Which special workspaces are visible on *other* monitors?
    let visible_elsewhere: Vec<String> = monitors
        .iter()
        .filter(|m| m.name != monitor)
        .filter_map(|m| {
            m.special_workspace
                .name
                .strip_prefix("special:")
                .filter(|n| !n.is_empty())
                .map(String::from)
        })
        .collect();

    // Build Pango markup with three visual states.
    let mut markup = String::new();
    let mut has_visible = false;

    let focused_bg = &colors.focused_bg;
    let focused_fg = &colors.focused_fg;
    let visible_bg = &colors.visible_bg;
    let visible_fg = &colors.visible_fg;
    let hidden_bg = &colors.hidden_bg;
    let hidden_fg = &colors.hidden_fg;

    for (i, name) in occupied.iter().enumerate() {
        if i > 0 {
            markup.push(' ');
        }
        let icon = icon_for(name, icons);
        if focused_on_this.as_deref() == Some(name) {
            has_visible = true;
            let _ = write!(
                markup,
                "<span background='{focused_bg}' foreground='{focused_fg}' weight='bold'> {icon} </span>"
            );
        } else if visible_elsewhere.contains(name) {
            has_visible = true;
            let _ = write!(
                markup,
                "<span background='{visible_bg}' foreground='{visible_fg}' weight='bold'> {icon} </span>"
            );
        } else {
            let _ = write!(
                markup,
                "<span background='{hidden_bg}' foreground='{hidden_fg}' weight='bold'> {icon} </span>"
            );
        }
    }

    // Determine CSS class.
    let class = if occupied.is_empty() {
        "empty"
    } else if has_visible {
        "has-visible"
    } else {
        "all-hidden"
    };

    // Tooltip: list occupied workspaces with visibility.
    let tooltip: Vec<String> = occupied
        .iter()
        .map(|n| {
            if focused_on_this.as_deref() == Some(n) {
                format!("{n} (focused)")
            } else if visible_elsewhere.contains(n) {
                format!("{n} (visible elsewhere)")
            } else {
                format!("{n} (hidden)")
            }
        })
        .collect();

    Ok(serde_json::json!({
        "text": markup,
        "tooltip": tooltip.join("\n"),
        "class": class,
    })
    .to_string())
}

/// Long-running Waybar module: emits JSON on startup, then re-emits
/// whenever a relevant Hyprland event arrives on the event socket.
pub fn waybar_follow(monitor: &str, icons: &HashMap<String, String>, colors: &Colors) -> Result<()> {
    if !emit_output(monitor, icons, colors) {
        return Ok(());
    }

    loop {
        // Connect to event socket (retry on failure)
        let mut stream = match ipc::event_socket_path()
            .and_then(|p| UnixStream::connect(&p).map_err(crate::error::Error::SocketConnect))
        {
            Ok(s) => s,
            Err(e) => {
                eprintln!("hyprmagic waybar: event socket: {e}");
                std::thread::sleep(Duration::from_secs(2));
                continue;
            }
        };

        let mut raw_buf: Vec<u8> = Vec::new();
        let mut buf = [0u8; 4096];
        let mut dirty = false;

        loop {
            // Block indefinitely when idle; use short debounce timeout when dirty.
            let timeout = if dirty { Some(DEBOUNCE) } else { None };
            if let Err(e) = stream.set_read_timeout(timeout) {
                eprintln!("hyprmagic waybar: set_read_timeout: {e}");
                break;
            }

            match stream.read(&mut buf) {
                Ok(0) => break, // EOF — Hyprland closed the socket
                Ok(n) => {
                    raw_buf.extend_from_slice(&buf[..n]);

                    // Process complete lines from the raw byte buffer.
                    while let Some(pos) = raw_buf.iter().position(|&b| b == b'\n') {
                        let line = String::from_utf8_lossy(&raw_buf[..pos]);
                        if is_relevant_event(&line) {
                            dirty = true;
                        }
                        raw_buf.drain(..=pos);
                    }

                    // Prevent unbounded accumulation of partial lines.
                    if raw_buf.len() > MAX_PENDING {
                        raw_buf.clear();
                    }
                }
                Err(ref e)
                    if e.kind() == io::ErrorKind::WouldBlock
                        || e.kind() == io::ErrorKind::TimedOut =>
                {
                    // Debounce expired — emit update if we have pending changes
                    if dirty {
                        if !emit_output(monitor, icons, colors) {
                            return Ok(());
                        }
                        dirty = false;
                    }
                }
                Err(_) => break, // Socket error — reconnect
            }
        }

        // Emit final update if we have pending changes before reconnecting.
        if dirty && !emit_output(monitor, icons, colors) {
            return Ok(());
        }

        eprintln!("hyprmagic waybar: event socket closed, reconnecting...");
        std::thread::sleep(Duration::from_secs(2));
    }
}
