use serde::Deserialize;

use crate::error::{Error, Result};
use crate::ipc;

#[derive(Debug, Deserialize)]
pub struct ActiveWindow {
    pub address: String,
    pub workspace: ClientWorkspace,
}

#[derive(Debug, Deserialize)]
pub struct ActiveWorkspace {
    pub name: String,
}

#[derive(Debug, Deserialize)]
pub struct Client {
    pub address: String,
    pub workspace: ClientWorkspace,
    #[allow(dead_code)]
    pub title: String,
    pub class: String,
}

#[derive(Debug, Deserialize)]
pub struct ClientWorkspace {
    #[allow(dead_code)]
    pub id: i32,
    pub name: String,
}

#[derive(Debug, Deserialize)]
pub struct Monitor {
    #[allow(dead_code)]
    pub id: i32,
    pub name: String,
    #[serde(rename = "specialWorkspace")]
    pub special_workspace: MonitorSpecialWorkspace,
    pub focused: bool,
}

#[derive(Debug, Deserialize)]
pub struct MonitorSpecialWorkspace {
    #[allow(dead_code)]
    pub id: i32,
    pub name: String,
}

pub fn get_active_window() -> Result<ActiveWindow> {
    let resp = ipc::query("j/activewindow")?;
    serde_json::from_str(&resp).map_err(Error::Parse)
}

pub fn get_active_workspace() -> Result<ActiveWorkspace> {
    let resp = ipc::query("j/activeworkspace")?;
    serde_json::from_str(&resp).map_err(Error::Parse)
}

pub fn get_clients() -> Result<Vec<Client>> {
    let resp = ipc::query("j/clients")?;
    serde_json::from_str(&resp).map_err(Error::Parse)
}

pub fn get_monitors() -> Result<Vec<Monitor>> {
    let resp = ipc::query("j/monitors")?;
    serde_json::from_str(&resp).map_err(Error::Parse)
}

fn check_dispatch_response(response: &str) -> Result<()> {
    for line in response.lines() {
        let trimmed = line.trim();
        if !trimmed.is_empty() && !trimmed.starts_with("ok") {
            return Err(Error::Ipc(trimmed.to_string()));
        }
    }
    Ok(())
}

/// Escape a string for safe inclusion inside a Lua "..." literal.
pub(crate) fn lua_str(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            _ => out.push(c),
        }
    }
    out
}

pub fn toggle_special_workspace(name: &str) -> Result<()> {
    let resp = ipc::dispatch(&format!(
        "dispatch hl.dsp.workspace.toggle_special(\"{}\")",
        lua_str(name)
    ))?;
    check_dispatch_response(&resp)
}

pub fn move_to_workspace_silent(ws_name: &str, address: &str) -> Result<()> {
    let resp = ipc::dispatch(&format!(
        "dispatch hl.dsp.window.move({{ workspace = \"{}\", follow = false, window = \"address:{}\" }})",
        lua_str(ws_name),
        lua_str(address)
    ))?;
    check_dispatch_response(&resp)
}

pub fn focus_window(address: &str) -> Result<()> {
    let resp = ipc::dispatch(&format!(
        "dispatch hl.dsp.focus({{ window = \"address:{}\" }})",
        lua_str(address)
    ))?;
    check_dispatch_response(&resp)
}

pub fn focus_monitor(name: &str) -> Result<()> {
    let resp = ipc::dispatch(&format!(
        "dispatch hl.dsp.focus({{ monitor = \"{}\" }})",
        lua_str(name)
    ))?;
    check_dispatch_response(&resp)
}

#[allow(dead_code)]
pub fn close_window(address: &str) -> Result<()> {
    let resp = ipc::dispatch(&format!(
        "dispatch hl.dsp.window.close({{ window = \"address:{}\" }})",
        lua_str(address)
    ))?;
    check_dispatch_response(&resp)
}

pub fn exec_in_special(name: &str, command: &str) -> Result<()> {
    let resp = ipc::dispatch(&format!(
        "dispatch hl.dsp.exec_cmd(\"{}\", {{ workspace = \"special:{}\" }})",
        lua_str(command),
        lua_str(name)
    ))?;
    check_dispatch_response(&resp)
}
