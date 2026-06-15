#!/usr/bin/env bash
# Self-contained golden test for renderMagic — no Rust binary, no live compositor.
# Feeds fixed magic:waybar-state fixtures (US=0x1f, see render.hpp) through
# ./build/render_test and asserts the rendered class + a markup substring.
# Run after touching render.cpp / render.hpp:  waybar/render_diff.sh
set -uo pipefail
RT="$(dirname "$0")/../build/render_test"
[ -x "$RT" ] || { echo "build first: cmake --build build --target render_test"; exit 2; }

fail=0
# args: name  state(\x-escaped)  expect-class  expect-markup-substr  [render_test args...]
check() {
  local name=$1 state=$2 wantCls=$3 wantSub=$4; shift 4
  local out cls txt
  out=$(printf '%b' "$state" | "$RT" "$@")
  cls=$(printf '%s' "$out" | python3 -c 'import sys;print(sys.stdin.read().split("\x1e")[2])')
  txt=$(printf '%s' "$out" | python3 -c 'import sys;print(sys.stdin.read().split("\x1e")[0])')
  if [ "$cls" != "$wantCls" ]; then echo "FAIL $name: class [$cls] != [$wantCls]"; fail=1
  elif [ -n "$wantSub" ] && ! printf '%s' "$txt" | grep -qF -- "$wantSub"; then
    echo "FAIL $name: markup missing [$wantSub]"; fail=1
  else echo "OK   $name (class=$cls)"; fi
}

check "focused color" 'monitor\x1fDP-3\nfl\x1ffoot-1\x1ffocused\n'  has-visible "background='#aaaaaa'" focused-bg=#aaaaaa icon-foot-1=F
check "icon fallback" 'monitor\x1fDP-3\nfl\x1fweb\x1fhidden\n'      all-hidden  "> web <"
check "visible state" 'monitor\x1fDP-3\nfl\x1fslack\x1fvisible\n'   has-visible "background='#2a2a2a'"
check "pango escape"  'monitor\x1fDP-3\nfl\x1fa-b\x1fhidden\n'      all-hidden  "a-b"
check "no floaters"   'monitor\x1fDP-3\n'                           empty       ""

exit $fail
