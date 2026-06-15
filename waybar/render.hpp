#pragma once

// Pure rendering for the waybar CFFI module: magic:waybar-state reply ->
// Pango markup + tooltip + CSS class. Ports the Rust hyprmagic waybar.rs
// build_output (waybar.rs:78-184). Kept free of GTK so render_test can diff
// it against the Rust binary's output.
//
// ── magic:waybar-state wire protocol (AUTHORITATIVE SPEC) ────────────────
// Producer: Magic::waybarState (../src/floaters.cpp). Consumer: renderMagic.
// US = 0x1f (field sep), lines = '\n'. Producer and consumer are one codebase
// that always rebuilds together, so the format is unversioned by design — if
// you change it, change BOTH ends here and in floaters.cpp.
//   monitor<US><resolved-name>           (empty name = monitor not resolved)
//   fl<US><name><US><state>              per occupied floater, sorted by name;
//                                        state ∈ {focused, visible, hidden}
// Floater names are validated alnum/-/_ by the producer, so none can carry
// US/newline and forge a row.

#include <map>
#include <string>

struct RenderOut {
    std::string text;     // Pango markup
    std::string tooltip;  // one line per occupied floater
    std::string cssClass; // empty | has-visible | all-hidden
};

struct MagicConfig {
    // Defaults mirror the Rust CLI's clap defaults (cli.rs:56-72).
    std::string focusedBg = "#f5f5f5";
    std::string focusedFg = "#0a0a0a";
    std::string visibleBg = "#2a2a2a";
    std::string visibleFg = "#f5f5f5";
    std::string hiddenBg  = "#1a1a1a";
    std::string hiddenFg  = "#555555";
    // floater name -> icon markup; missing names fall back to the escaped name
    std::map<std::string, std::string> icons;
};

// `state` is the raw `magic:waybar-state <monitor>` reply: US (0x1f) separated
// fields, newline separated lines, no trailing newline required.
RenderOut renderMagic(const std::string& state, const MagicConfig& c);
