use std::io::{Read, Write};
use std::net::Shutdown;
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::time::Duration;

use crate::error::{Error, Result};

const SOCKET_TIMEOUT: Duration = Duration::from_secs(5);

fn socket_dir() -> Result<PathBuf> {
    let his =
        std::env::var("HYPRLAND_INSTANCE_SIGNATURE").map_err(|_| Error::NoHyprlandInstance)?;
    let xdg_runtime = std::env::var("XDG_RUNTIME_DIR")
        .map_err(|_| Error::Other("XDG_RUNTIME_DIR not set".into()))?;
    Ok(PathBuf::from(xdg_runtime).join("hypr").join(his))
}

/// Path to the Hyprland event socket (.socket2.sock).
pub fn event_socket_path() -> Result<PathBuf> {
    Ok(socket_dir()?.join(".socket2.sock"))
}

fn socket_path() -> Result<PathBuf> {
    Ok(socket_dir()?.join(".socket.sock"))
}

fn connect() -> Result<UnixStream> {
    let path = socket_path()?;
    let stream = UnixStream::connect(&path).map_err(Error::SocketConnect)?;
    stream
        .set_read_timeout(Some(SOCKET_TIMEOUT))
        .map_err(Error::SocketIo)?;
    stream
        .set_write_timeout(Some(SOCKET_TIMEOUT))
        .map_err(Error::SocketIo)?;
    Ok(stream)
}

fn send_and_receive(request: &str, max_response: u64) -> Result<String> {
    let mut stream = connect()?;
    stream
        .write_all(request.as_bytes())
        .map_err(Error::SocketIo)?;
    stream.shutdown(Shutdown::Write).map_err(Error::SocketIo)?;
    let mut response = String::new();
    stream
        .take(max_response)
        .read_to_string(&mut response)
        .map_err(Error::SocketIo)?;
    Ok(response)
}

/// Send a query to Hyprland and return the response.
pub fn query(request: &str) -> Result<String> {
    send_and_receive(request, 4 * 1024 * 1024)
}

/// Send a dispatcher command and return the response.
pub fn dispatch(command: &str) -> Result<String> {
    send_and_receive(command, 64 * 1024)
}

/// Send multiple dispatch commands in a single batch.
/// Checks each sub-response for errors.
pub fn dispatch_batch(commands: &[String]) -> Result<()> {
    if commands.is_empty() {
        return Ok(());
    }
    let batch = format!("[[BATCH]]{}", commands.join(";"));
    let resp = send_and_receive(&batch, 64 * 1024)?;
    // Hyprland returns one response per command, newline-separated.
    for line in resp.lines() {
        let trimmed = line.trim();
        if !trimmed.is_empty() && !trimmed.starts_with("ok") {
            return Err(Error::Ipc(trimmed.to_string()));
        }
    }
    Ok(())
}
