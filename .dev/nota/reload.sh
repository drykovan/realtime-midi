#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/../.."
build_dir="${REALTIME_MIDI_BUILD_DIR:-build}"
cmake_bin="${CMAKE_BIN:-cmake}"
nota_app="${NOTA_APP:-/Applications/Nota.app}"
project="${NOTA_PROJECT:-$HOME/Documents/Nota Projects/realtime-midi-test.nota}"
plugin_dir="$HOME/Library/Audio/Plug-Ins/VST3"
bundle="Realtime MIDI.vst3"
attempt="$build_dir/nota-dev-attempt"
ready="$build_dir/nota-dev-ready"
log="$HOME/Library/Logs/Realtime MIDI Dev.log"
lock="$build_dir/nota-dev-lock"

build() (
    mkdir -p "$build_dir"
    mkdir "$lock" || { printf 'Another Nota build or deployment is in progress.\n' >&2; exit 1; }
    trap 'rmdir "$lock"' EXIT
    stamp="$(python3 -c 'from datetime import datetime, timezone; print(datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ"))')"
    printf '%s\n' "$stamp" > "$attempt"
    printf 'Building Realtime MIDI dev build %s\n' "$stamp"

    "$cmake_bin" -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
        -DREALTIME_MIDI_ENABLE_IAC=ON "-DREALTIME_MIDI_DEV_BUILD=$stamp"
    "$cmake_bin" --build "$build_dir" --config Release --target RealtimeMIDI_VST3 --parallel 8
    printf '%s\n' "$stamp" > "$ready"
)

deploy() (
    local stamp latest staged backup pid
    [[ -d "$nota_app" && -d "$project" ]] || { printf 'Check NOTA_APP and NOTA_PROJECT.\n' >&2; exit 1; }
    if pid="$(/usr/bin/pgrep -x Nota.App)"; then
        printf 'Nota is running (PID %s). Save and quit before replacing the VST3.\n' "$pid" >&2
        exit 1
    fi
    [[ -f "$ready" && -f "$attempt" ]] || { printf 'No finished dev build.\n' >&2; exit 1; }
    mkdir "$lock" || { printf 'Another Nota build or deployment is in progress.\n' >&2; exit 1; }
    staged=
    trap 'if [[ -n "$staged" ]]; then rm -rf -- "$staged"; fi; rmdir "$lock"' EXIT
    read -r stamp < "$ready"
    read -r latest < "$attempt"
    [[ "$stamp" == "$latest" ]] || { printf 'The latest dev build did not finish.\n' >&2; exit 1; }

    mkdir -p "$plugin_dir"
    staged="$(mktemp -d "$plugin_dir/.realtime-midi-stage.XXXXXX")"
    /usr/bin/ditto "$build_dir/RealtimeMIDI_artefacts/Release/VST3/$bundle" "$staged/$bundle"
    /usr/bin/codesign --verify --strict "$staged/$bundle"
    backup="$staged/previous"
    if [[ -e "$plugin_dir/$bundle" ]]; then mv "$plugin_dir/$bundle" "$backup"; fi
    if ! mv "$staged/$bundle" "$plugin_dir/$bundle"; then
        if [[ -e "$backup" ]] && ! mv "$backup" "$plugin_dir/$bundle"; then
            trap 'rmdir "$lock"' EXIT
            printf 'Previous bundle is at %s\n' "$backup" >&2
        fi
        exit 1
    fi

    printf '%s\n' "$stamp" > "$build_dir/nota-dev-installed"
    /usr/bin/open -a "$nota_app"
    printf 'Installed build %s. Open %s via Nota’s Open recent menu.\n' "$stamp" "${project##*/}"
    printf 'Runtime log: %s\n' "$log"
)

verify() {
    local stamp pid
    read -r stamp < "$build_dir/nota-dev-installed"
    pid="$(/usr/bin/pgrep -x Nota.App)" || { printf 'Nota is not running.\n' >&2; return 1; }

    if [[ -f "$log" ]] && awk -v stamp="$stamp" -v pid="$pid" \
        '$2 == "build" && $3 == stamp && $4 == "pid" && $5 == pid { found = 1 } END { exit !found }' "$log"; then
        printf 'Nota PID %s loaded Realtime MIDI build %s\n' "$pid" "$stamp"
    else
        printf 'Nota PID %s has not logged Realtime MIDI build %s.\n' "$pid" "$stamp" >&2
        return 1
    fi
}

case "${1:-all}" in
    build) build ;;
    deploy) deploy ;;
    verify) verify ;;
    all) build; deploy ;;
    *) printf 'Usage: %s [build|deploy|verify|all]\n' "$0" >&2; exit 2 ;;
esac
