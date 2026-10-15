#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Runs `rayomd-gui --snapshot` on a headless compositor and checks the PNG (plan Phase 9 step 11).
#
# Usage: run_compositor.sh weston|sway RAYOMD_GUI OUT_DIR
#
# Everything runs in a temporary HOME with a private XDG_RUNTIME_DIR (mode 0700) that the
# compositor uses too, and an Omarchy current theme made from tests/e2e/test-theme.toml
# (background #123456, accent #ff5555). There is no settings file, so the app follows Omarchy.
# OUT_DIR receives out.png, stdout.txt, stderr.txt and compositor.log. Weston has no
# xdg-decoration, so the app draws its own header; sway answers server-side decorations.
set -eu

if [ $# -ne 3 ]; then
    echo "usage: $0 weston|sway RAYOMD_GUI OUT_DIR" >&2
    exit 2
fi
compositor=$1
app=$2
out=$3
here=$(cd "$(dirname "$0")" && pwd)
source_dir=$(cd "$here/../.." && pwd)
case $app in /*) ;; *) app=$(pwd)/$app ;; esac

mkdir -p "$out"
out=$(cd "$out" && pwd)
rm -f "$out/out.png" "$out/stdout.txt" "$out/stderr.txt" "$out/compositor.log"

work=$(mktemp -d "${TMPDIR:-/tmp}/rgui-e2e.XXXXXX")
compositor_pid=
cleanup() {
    if [ -n "$compositor_pid" ]; then
        kill "$compositor_pid" 2>/dev/null || true
        wait "$compositor_pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

export HOME="$work/home"
export XDG_RUNTIME_DIR="$work/runtime"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_STATE_HOME="$HOME/.local/state"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_DATA_HOME="$HOME/.local/share"
export RAYOMD_FONT=/usr/share/fonts/TTF/DejaVuSans.ttf
unset WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY DBUS_SESSION_BUS_ADDRESS HYPRLAND_INSTANCE_SIGNATURE \
    XCOMPOSEFILE SWAYSOCK || true
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME/omarchy/current/theme"
mkdir -m 0700 "$XDG_RUNTIME_DIR"
cp "$source_dir/tests/e2e/test-theme.toml" "$XDG_STATE_HOME/omarchy/current/theme/colors.toml"
echo test > "$XDG_STATE_HOME/omarchy/current/theme.name"

case $compositor in
weston)
    socket=rgui-test
    weston --backend=headless --socket=$socket --width=1600 --height=1000 --idle-time=0 --fake-seat \
        >"$out/compositor.log" 2>&1 &
    compositor_pid=$!
    ;;
sway)
    sway=$(command -v sway)
    # Arch gives sway the file capability cap_sys_nice, and exec refuses a binary whose file
    # capabilities exceed the bounding set (containers drop cap_sys_nice), root or not. A copy
    # has no file capabilities.
    cp "$sway" "$work/sway"
    sway="$work/sway"
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
        "$sway" -c "$source_dir/tests/e2e/sway.conf" >"$out/compositor.log" 2>&1 &
    compositor_pid=$!
    socket=
    ;;
*)
    echo "unknown compositor: $compositor" >&2
    exit 2
    ;;
esac

# Wait for the compositor's socket (at most 10 s).
tries=0
while :; do
    if [ -z "$socket" ]; then
        for candidate in "$XDG_RUNTIME_DIR"/wayland-*; do
            case $candidate in *.lock) continue ;; esac
            [ -S "$candidate" ] && socket=$(basename "$candidate") && break
        done
    fi
    if [ -n "$socket" ] && [ -S "$XDG_RUNTIME_DIR/$socket" ]; then
        break
    fi
    if ! kill -0 "$compositor_pid" 2>/dev/null; then
        echo "$compositor exited before listening:" >&2
        cat "$out/compositor.log" >&2
        exit 1
    fi
    tries=$((tries + 1))
    if [ $tries -gt 100 ]; then
        echo "$compositor did not listen within 10 s:" >&2
        cat "$out/compositor.log" >&2
        exit 1
    fi
    sleep 0.1
done
export WAYLAND_DISPLAY=$socket

status=0
"$app" --snapshot "$out/out.png" "$source_dir/external/rayomd/tester.md" \
    >"$out/stdout.txt" 2>"$out/stderr.txt" || status=$?
cat "$out/stdout.txt"
cat "$out/stderr.txt" >&2
if [ $status -ne 0 ]; then
    echo "rayomd-gui exited with status $status" >&2
    echo "--- $compositor log:" >&2
    cat "$out/compositor.log" >&2
    exit 1
fi
python3 "$source_dir/tests/e2e/check_snapshot.py" "$out/out.png" 123456 ff5555 --timings "$out/stdout.txt"
