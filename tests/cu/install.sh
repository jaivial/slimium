#!/bin/sh
# Install cu-slim: the cu binary built from the cu worktree branch
# slimium-integration (~/cu-wt/slimium), a launcher, and the slimium-chrome
# stand-in used until the real wrapper is at ~/.local/bin/slimium-chrome.
# Touches only ~/.local/bin/cu-slim, ~/.local/bin/cu-slim-bin and
# ~/.local/share/cu-slim/bin.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=${CU_SLIM_SRC_BIN:-$HOME/cu-wt/slimium/target/release/cu}
[ -x "$bin" ] || { echo "install: build cu first: cd ~/cu-wt/slimium && CARGO_TARGET_DIR=~/cu-wt/slimium/target cargo build --release" >&2; exit 1; }
mkdir -p "$HOME/.local/bin" "$HOME/.local/share/cu-slim/bin"
install -m 755 "$bin" "$HOME/.local/bin/cu-slim-bin.new" && mv -f "$HOME/.local/bin/cu-slim-bin.new" "$HOME/.local/bin/cu-slim-bin"
install -m 755 "$here/cu-slim" "$HOME/.local/bin/cu-slim"
install -m 755 "$here/slimium-chrome-standin.sh" "$HOME/.local/share/cu-slim/bin/slimium-chrome-standin.sh"
echo "installed: ~/.local/bin/cu-slim -> ~/.local/bin/cu-slim-bin ($(git -C "$(dirname "$bin")/../.." rev-parse --short HEAD 2>/dev/null || echo '?'))"
