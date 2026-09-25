#!/usr/bin/env bash
# Copy a built plugin into the folder DAWs actually scan (system-wide on
# macOS, matching the .pkg installer; user-local on Linux).
#
#   ./script/install-plugin.sh VST3 [Debug|Release]
#   ./script/install-plugin.sh AU   [Debug|Release]
#   ./script/install-plugin.sh AAX  [Debug|Release]
#
# Defaults to Release.
set -euo pipefail
cd "$(dirname "$0")/.."

format="${1:-}"
build_type="${2:-Release}"

case "$build_type" in
  Debug|Release) ;;
  *) echo "Invalid build type: $build_type (use Debug or Release)" >&2; exit 1 ;;
esac

os="$(uname -s)"
case "$format" in
  VST3)
    bundle="TONE3000-Plum.vst3"
    case "$os" in
      # System-wide folder, same as the .pkg: some hosts (LUNA) only scan
      # /Library for VST3 and ignore ~/Library entirely.
      Darwin) dest_dir="/Library/Audio/Plug-Ins/VST3" ;;
      Linux)  dest_dir="$HOME/.vst3" ;;
      *) echo "Unsupported OS for this script: $os" >&2; exit 1 ;;
    esac
    ;;
  AU)
    bundle="TONE3000-Plum.component"
    if [ "$os" != "Darwin" ]; then
      echo "AU is macOS only (detected $os)" >&2
      exit 1
    fi
    # System-wide folder, same as the .pkg, so a dev install always replaces
    # the pkg copy instead of registering a same-version twin next to it.
    dest_dir="/Library/Audio/Plug-Ins/Components"
    ;;
  AAX)
    bundle="TONE3000-Plum.aaxplugin"
    if [ "$os" != "Darwin" ]; then
      echo "AAX is macOS only in this script (detected $os)" >&2
      exit 1
    fi
    # Pro Tools (incl. Developer) loads from the system Avid folder —
    # same path as the .pkg. Needs sudo.
    dest_dir="/Library/Application Support/Avid/Audio/Plug-Ins"
    need_sudo=1
    ;;
  *)
    echo "Usage: $0 <VST3|AU|AAX> [Debug|Release]" >&2
    exit 1
    ;;
esac

src="build/plugin/TONE3000_artefacts/$build_type/$format/$bundle"
if [ ! -d "$src" ]; then
  echo "Not found: $src" >&2
  echo "Build it first: cmake -B build -S . -DCMAKE_BUILD_TYPE=$build_type && cmake --build build" >&2
  exit 1
fi

run() {
  if [ "${need_sudo:-0}" -eq 1 ]; then
    sudo "$@"
  else
    "$@"
  fi
}

run mkdir -p "$dest_dir"
run rm -rf "${dest_dir:?}/$bundle"
run cp -R "$src" "$dest_dir/"
echo "Installed $bundle ($build_type) to $dest_dir"

# Also drop shipped factory presets into the user Factory folder so a local
# plugin install (without the .pkg) still shows the TONE3000 Plum preset section.
factory_src="resources/factory-presets"  # cwd is the repo root (cd at top)
case "$os" in
  Darwin) factory_dest="$HOME/Library/Application Support/TONE3000 Plum/Presets/Factory" ;;
  Linux)  factory_dest="${XDG_CONFIG_HOME:-$HOME/.config}/TONE3000 Plum/Presets/Factory" ;;
  *) factory_dest="" ;;
esac
if [[ -n "$factory_dest" ]] && compgen -G "${factory_src}/*.t3kpreset" > /dev/null; then
  mkdir -p "$factory_dest"
  # Replace, don't overlay: presets dropped from (or renamed in) the repo set
  # would otherwise linger from a previous install.
  rm -f "$factory_dest"/*.t3kpreset
  cp "${factory_src}"/*.t3kpreset "$factory_dest/"
  echo "Installed factory presets to $factory_dest"
fi

echo "Rescan plugins in your DAW to pick up the change."
