#!/usr/bin/env bash
# Launch the complete Windows release on Linux through UMU and Proton.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./Launch.sh [play|mute|check|help] [extra game arguments...]
  play (default)  Play with sound through UMU and Proton.
  mute            Play without sound.
  check           Check the release and UMU without launching or downloading.

Install umu-run using https://github.com/Open-Wine-Components/umu-launcher
or add build_native/Release/DarkRecompPreview.exe to Steam and select Proton.
See STEAM_DECK.md. WINEPREFIX, GAMEID and PROTONPATH overrides are respected.
Setup checks require the complete game dump in Darkness beside Launch.sh.
Additional --game-dir arguments are forwarded; check still validates Darkness.
EOF
}

mode=play
case "${1:-}" in
    play|mute|check) mode=$1; shift ;;
    help|--help|-h) usage; exit 0 ;;
esac

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
cd -- "$root"
binary_dir="$root/build_native/Release"
preview="$binary_dir/DarkRecompPreview.exe"
shopt -s nocasematch
missing=0
for file in DarkRecomp.exe DarkRecompPreview.exe \
    avcodec-darkxma-62.dll avutil-darkxma-60.dll libwinpthread-1.dll \
    msvcp140.dll vcruntime140.dll vcruntime140_1.dll \
    CubeWnd.pc.xcr GameContext_Create.pc.xdf; do
    found=0
    # Windows release DLL names can retain mixed casing. Wine resolves them
    # case-insensitively even when the extracted folder is on Linux ext4.
    for candidate in "$binary_dir"/*; do
        if [[ -f "$candidate" && -s "$candidate" && ${candidate##*/} == "$file" ]]; then
            found=1
            [[ $file == DarkRecompPreview.exe ]] && preview=$candidate
            break
        fi
    done
    if (( ! found )); then
        printf 'Missing release file: %s\n' "$binary_dir/$file" >&2
        missing=1
    fi
done
if [[ ! -s "$root/Darkness/default.xex" ]]; then
    printf 'Missing game file: %s/Darkness/default.xex\n' "$root" >&2
    missing=1
fi
for directory in Content System; do
    if [[ ! -d "$root/Darkness/$directory" ]]; then
        printf 'Missing game folder: %s/Darkness/%s\n' "$root" "$directory" >&2
        missing=1
    fi
done
if (( missing )); then
    printf 'Extract the entire Windows release ZIP and copy your own complete game dump into Darkness. See START_HERE.txt.\n' >&2
    exit 1
fi
if ! command -v umu-run >/dev/null 2>&1; then
    printf 'umu-run was not found. Install UMU from https://github.com/Open-Wine-Components/umu-launcher\n' >&2
    printf 'Alternatively, add build_native/Release/DarkRecompPreview.exe to Steam and select Proton. See STEAM_DECK.md.\n' >&2
    exit 1
fi

if [[ $mode == check ]]; then
    printf 'Setup looks ready. ./Launch.sh starts the Proton test with sound.\n'
    exit 0
fi

if [[ -z ${WINEPREFIX:-} ]]; then
    if [[ -n ${XDG_DATA_HOME:-} ]]; then
        WINEPREFIX="$XDG_DATA_HOME/darkrecomp/proton"
    elif [[ -n ${HOME:-} ]]; then
        WINEPREFIX="$HOME/.local/share/darkrecomp/proton"
    else
        printf 'Set WINEPREFIX, XDG_DATA_HOME or HOME to choose a Linux Proton prefix.\n' >&2
        exit 1
    fi
fi
export WINEPREFIX
export GAMEID="${GAMEID:-umu-default}"
export PROTONPATH="${PROTONPATH:-UMU-Proton}"
sound=--sound
[[ $mode == mute ]] && sound=--mute
exec umu-run "$preview" "$sound" "$@"
