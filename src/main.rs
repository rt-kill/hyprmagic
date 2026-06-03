mod cli;
mod error;
mod floaters;
mod hyprland;
mod ipc;
mod waybar;

use std::collections::HashMap;

use clap::Parser;

use cli::{Cli, Command};

fn main() {
    let cli = Cli::parse();

    if let Err(e) = run(cli.command) {
        eprintln!("hyprmagic: {e}");
        std::process::exit(1);
    }
}

fn run(cmd: Command) -> error::Result<()> {
    match cmd {
        Command::Toggle {
            name,
            class,
            launch_cmd,
        } => floaters::toggle(&name, &class, &launch_cmd),
        Command::Yank { name } => floaters::yank(&name),
        Command::Kill { name } => floaters::kill(&name),
        Command::SafeKill { name, confirm_cmd, on_empty } => floaters::safe_kill(&name, &confirm_cmd, on_empty.as_deref()),
        Command::Waybar {
            monitor,
            icons,
            focused_bg,
            focused_fg,
            visible_bg,
            visible_fg,
            hidden_bg,
            hidden_fg,
        } => {
            let icon_map: HashMap<String, String> = icons
                .iter()
                .filter_map(|s| s.split_once('='))
                .map(|(k, v)| (k.to_string(), v.to_string()))
                .collect();
            let colors = waybar::Colors {
                focused_bg,
                focused_fg,
                visible_bg,
                visible_fg,
                hidden_bg,
                hidden_fg,
            };
            waybar::waybar_follow(&monitor, &icon_map, &colors)
        }
    }
}
