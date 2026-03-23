#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("Hyprland IPC error: {0}")]
    Ipc(String),

    #[error("failed to connect to Hyprland socket: {0}")]
    SocketConnect(#[source] std::io::Error),

    #[error("socket I/O error: {0}")]
    SocketIo(#[source] std::io::Error),

    #[error("failed to parse Hyprland response: {0}")]
    Parse(#[source] serde_json::Error),

    #[error("HYPRLAND_INSTANCE_SIGNATURE not set — is Hyprland running?")]
    NoHyprlandInstance,

    #[error("{0}")]
    Other(String),
}

pub type Result<T> = std::result::Result<T, Error>;
