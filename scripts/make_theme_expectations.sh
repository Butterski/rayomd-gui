#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Writes tests/data/themes/<slug>.expect: what Omarchy's own resolver, bin/omarchy-theme-color
# (bash and awk), prints with `--file <colors.toml> --all` for every built-in theme at one commit
# of omacom/omarchy, and tests/data/themes/fixtures/<name>.expect for the hand-written
# colors.toml files in tests/data/themes/fixtures/<name>/ (legacy palettes, mode detection and
# parser corner cases). The ui.theme_resolver tests check that ResolvePalette gives the same values.
#
#   scripts/make_theme_expectations.sh [commit]
#
# The resolver looks for light.mode beside the colors.toml, so each theme's file is put in a
# directory of its own, with light.mode when the theme has one. Needs bash, curl, GNU awk and
# python3; set GITHUB_TOKEN when the anonymous API rate limit is reached.
set -euo pipefail

commit="${1:-246f33a2fef66411c990860d88e0823ae83987fc}"
repo="omacom/omarchy"
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/tests/data/themes"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

auth=()
[[ -n ${GITHUB_TOKEN:-} ]] && auth=(-H "Authorization: Bearer $GITHUB_TOKEN")

api() {
  curl -fsSL "${auth[@]}" "https://api.github.com/repos/$repo/contents/$1?ref=$commit"
}

raw() {
  curl -fsSL "https://raw.githubusercontent.com/$repo/$commit/$1"
}

raw bin/omarchy-theme-color >"$work/omarchy-theme-color"

mkdir -p "$out"
rm -f "$out"/*.expect
count=0
for slug in $(api themes | python3 -c 'import json,sys; print("\n".join(e["name"] for e in json.load(sys.stdin) if e["type"] == "dir"))'); do
  mkdir -p "$work/themes/$slug"
  raw "themes/$slug/colors.toml" >"$work/themes/$slug/colors.toml"
  if api "themes/$slug" | python3 -c 'import json,sys; sys.exit(0 if any(e["name"] == "light.mode" for e in json.load(sys.stdin)) else 1)'; then
    touch "$work/themes/$slug/light.mode"
  fi
  HOME="$work" bash "$work/omarchy-theme-color" --file "$work/themes/$slug/colors.toml" --all >"$out/$slug.expect"
  count=$((count + 1))
done
for dir in "$out"/fixtures/*/; do
  name="$(basename "$dir")"
  HOME="$work" bash "$work/omarchy-theme-color" --file "${dir}colors.toml" --all 2>/dev/null >"$out/fixtures/$name.expect"
done
echo "wrote $count expectations to tests/data/themes (omarchy $commit), and the fixtures'"
