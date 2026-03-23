use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(name = "hyprmagic", about = "Hyprland special workspace floaters")]
pub struct Cli {
    #[command(subcommand)]
    pub command: Command,
}

#[derive(Subcommand)]
pub enum Command {
    /// Toggle a special workspace; launch the app if empty
    Toggle {
        /// Special workspace name
        name: String,
        /// Window class to match
        class: String,
        /// Launch command and arguments
        #[arg(trailing_var_arg = true, required = true)]
        launch_cmd: Vec<String>,
    },

    /// Pull focused window into the special workspace, or drop it out
    Yank {
        /// Special workspace name
        name: String,
    },

    /// Close all windows in the special workspace
    Kill {
        /// Special workspace name
        name: String,
    },

    /// Two-press kill: first shows confirm dialog, second kills all
    SafeKill {
        /// Special workspace name
        name: String,
        /// Confirm dialog command template ({name} is replaced with workspace name)
        #[arg(long, value_name = "CMD", default_value = "confirm_dialog -b 551111 safe-kill-confirm 'Kill all windows?' hyprmagic kill {name}")]
        confirm_cmd: String,
    },

    /// Long-running Waybar JSON output for special workspace indicators
    Waybar {
        /// Monitor name (use {} in waybar config)
        monitor: String,
        /// Icon mappings (e.g. -i foot-1='󰆍 ₁' -i firefox='')
        #[arg(short, long = "icon", value_name = "NAME=ICON")]
        icons: Vec<String>,
        /// Background color for focused workspace
        #[arg(long, default_value = "#f5f5f5")]
        focused_bg: String,
        /// Foreground color for focused workspace
        #[arg(long, default_value = "#0a0a0a")]
        focused_fg: String,
        /// Background color for visible-elsewhere workspaces
        #[arg(long, default_value = "#2a2a2a")]
        visible_bg: String,
        /// Foreground color for visible-elsewhere workspaces
        #[arg(long, default_value = "#f5f5f5")]
        visible_fg: String,
        /// Background color for hidden workspaces
        #[arg(long, default_value = "#1a1a1a")]
        hidden_bg: String,
        /// Foreground color for hidden workspaces
        #[arg(long, default_value = "#555555")]
        hidden_fg: String,
    },
}
