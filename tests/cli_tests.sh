#!/bin/sh
# Command line tests for termtime: sh tests/cli_tests.sh ./termtime

BIN=${1:-./termtime}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/termtime-cli-XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

checks=0
failures=0

fail() {
  failures=$((failures + 1))
  echo "FAIL: $*" >&2
}

# expect_ok NAME CMD... : command succeeds
expect_ok() {
  name=$1
  shift
  checks=$((checks + 1))
  "$@" >"$TMP/out" 2>"$TMP/err" || fail "$name (exit $?): $(cat "$TMP/err")"
}

# expect_fail NAME TEXT CMD... : command fails and stderr contains TEXT
expect_fail() {
  name=$1
  text=$2
  shift 2
  checks=$((checks + 1))
  if "$@" >"$TMP/out" 2>"$TMP/err"; then
    fail "$name: expected failure"
  elif ! grep -qF -- "$text" "$TMP/err"; then
    fail "$name: stderr does not contain '$text': $(cat "$TMP/err")"
  fi
}

# expect_out NAME TEXT : last stdout contains TEXT
expect_out() {
  checks=$((checks + 1))
  grep -qF -- "$2" "$TMP/out" || fail "$1: stdout does not contain '$2'"
}

# A 256 color UTF-8 terminal unless a test says otherwise
export TERM=xterm-256color LC_ALL=C.UTF-8
unset COLORTERM

mkdir -p "$TMP/xdg/termtime" "$TMP/home/.config/termtime"
export HOME="$TMP/home"
unset XDG_CONFIG_HOME

expect_ok "help" "$BIN" --help
expect_out "help lists options" "--config FILE"
expect_ok "version" "$BIN" -v
expect_out "version" "termtime "
expect_fail "unknown option" "unknown option '--nope'" "$BIN" --nope
expect_fail "missing config argument" "needs a file" "$BIN" -c

# Config lookup order
printf '[line1]\nformat = "HOME"\n' >"$HOME/.config/termtime/config.toml"
expect_ok "home path" "$BIN" -p
expect_out "home path" "$HOME/.config/termtime/config.toml"

printf '[line1]\nformat = "XDG"\n' >"$TMP/xdg/termtime/config.toml"
expect_ok "xdg path" env XDG_CONFIG_HOME="$TMP/xdg" "$BIN" -p
expect_out "xdg path" "$TMP/xdg/termtime/config.toml"

printf "" >"$TMP/other.toml"
expect_ok "explicit path" env XDG_CONFIG_HOME="$TMP/xdg" "$BIN" -p -c "$TMP/other.toml"
expect_out "explicit path" "$TMP/other.toml"
expect_ok "explicit path with =" "$BIN" -p --config="$TMP/other.toml"
expect_out "explicit path with =" "$TMP/other.toml"

# Output when not on a terminal (and with --once)
printf '[general]\nspacing = 3\n[line1]\nformat = "1:2"\nscale = 2\n[line1.colors]\n":" = 196\n[line2]\nformat = "ab"\n' \
  >"$TMP/once.toml"
expect_ok "once" "$BIN" -1 -c "$TMP/once.toml"
checks=$((checks + 1))
lines=$(wc -l <"$TMP/out")
# 10 rows (scale 2) + 3 spacing + 5 rows (scale 1)
[ "$lines" -eq 18 ] || fail "once: expected 18 lines, got $lines"
expect_out "once colors" "$(printf '\033[38;5;196m')"
expect_out "once blocks" "█"
expect_ok "piped" sh -c "'$BIN' -c '$TMP/once.toml' | cat"
expect_out "piped blocks" "█"

printf '[line2]\nenabled = false\n' >"$TMP/single.toml"
expect_ok "single line" "$BIN" -1 -c "$TMP/single.toml"
checks=$((checks + 1))
lines=$(wc -l <"$TMP/out")
[ "$lines" -eq 10 ] || fail "single line: expected 10 lines, got $lines"

# Errors
expect_fail "missing file" "No such file" "$BIN" -1 -c "$TMP/missing.toml"
printf '[line1]\nscale = 0\n' >"$TMP/bad.toml"
expect_fail "bad scale" "line1.scale" "$BIN" -1 -c "$TMP/bad.toml"
printf '[line1]\nformat = "open\n' >"$TMP/bad.toml"
expect_fail "syntax error" "line 2: unterminated string" "$BIN" -1 -c "$TMP/bad.toml"
expect_fail "bad user config" "line 2" env XDG_CONFIG_HOME="$TMP/xdg" sh -c \
  "cp '$TMP/bad.toml' '$TMP/xdg/termtime/config.toml' && '$BIN' -1"

# Locales
printf '[general]\nlocale = "xx_NOPE"\n[line1]\nformat = "%%A"\n' >"$TMP/locale.toml"
expect_ok "unknown locale still runs" "$BIN" -1 -c "$TMP/locale.toml"
checks=$((checks + 1))
grep -qF "not available" "$TMP/err" || fail "unknown locale: no warning"

# Terminal detection: blocks and colors
printf '[line1]\nformat = "1"\ncolor = 9\n[line2]\nenabled = false\n' >"$TMP/term.toml"
esc=$(printf '\033')
expect_ok "linux console" env TERM=linux LC_ALL=C "$BIN" -1 -c "$TMP/term.toml"
expect_out "linux console: bright as bold" "${esc}[1;31m"
expect_out "linux console: reverse video blocks" "${esc}[7m"
checks=$((checks + 1))
grep -qF "█" "$TMP/out" && fail "linux console without UTF-8: no block glyphs"

expect_ok "linux console utf-8" env TERM=linux LC_ALL=C.UTF-8 "$BIN" -1 -c "$TMP/term.toml"
expect_out "linux console utf-8: block glyphs" "█"
expect_out "linux console utf-8: bright as bold" "${esc}[1;31m"

expect_ok "vt100" env TERM=vt100 LC_ALL=C "$BIN" -1 -c "$TMP/term.toml"
expect_out "vt100: 8 colors" "${esc}[31m"

expect_ok "truecolor" env TERM=xterm COLORTERM=truecolor "$BIN" -1 -c "$TMP/term.toml"
expect_out "truecolor: 256 colors" "${esc}[38;5;9m"

expect_ok "no TERM" env -u TERM LC_ALL=C.UTF-8 "$BIN" -1 -c "$TMP/term.toml"
expect_out "no TERM: 8 colors" "${esc}[31m"

printf '[general]\nblocks = "#"\ncolors = 16\n[line1]\nformat = "1"\ncolor = 9\n[line2]\nenabled = false\n' >"$TMP/forced.toml"
expect_ok "forced style" env TERM=linux LC_ALL=C "$BIN" -1 -c "$TMP/forced.toml"
expect_out "forced style: ascii blocks" "####"
expect_out "forced style: colors" "${esc}[1;31m"

# The shipped config works
expect_ok "shipped config" "$BIN" -1 -c config/config.toml

echo "cli tests: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
