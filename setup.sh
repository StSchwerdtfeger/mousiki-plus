#!/usr/bin/env bash
#
# setup.sh -- Linux / macOS equivalent of setup.ps1: installs mousiki's build
# and runtime dependencies, then configures and builds it.
#
# mousiki itself is a C++17 binary (the music player AND the radio mode, one program;
# SHIFT and + switches between them), but it shells out to a few external tools at runtime:
#
#   ffmpeg  - decodes Opus (miniaudio's built-in decoders don't cover it, and
#             Opus is exactly what the yt-dlp cache stores), supplies ffprobe
#             track metadata, and decodes the audio that fpcalc fingerprints
#             for the AcoustID fetch.
#   yt-dlp  - online search, playlist listing and streaming.
#   python3 - runs scripts/fetch_lyrics.py (synced lyrics; standard library
#             only, no pip package) and scripts/fetch_meta.py (AcoustID metadata fetch,
#             which drives fpcalc -- built by CMake from third_party/chromaprint/).
#
# and, for the radio mode only:
#
#   curl    - asks the Radio Browser directory (SHIFT+S in the radio) for stations.
#   ffmpeg  - (the same one as above) also streams the stations and, with libmp3lame,
#             converts recordings to MP3. The radio plays through its own audio device.
#
# and, for the retro formats (the "What Mousiki plays" page of the first-start wizard):
#
#   ffmpeg with libopenmpt + libgme - tracker modules (.mod .xm .it ...) and console music (.nsf .spc ...).
#             Without those two this script upgrades the ffmpeg that is already installed; if it still lacks them
#             it says so (on macOS it rebuilds Homebrew's ffmpeg from the homebrew-ffmpeg tap with them).
#   sidplayfp - Commodore 64 SID tunes (.sid). Installed from the package manager (built from source on macOS
#             if Homebrew has no sidplayfp) and put on the PATH (a symlink in ~/.local/bin if it sits elsewhere).
#
# All of these are independent of each other: mousiki plays local files fine with
# none of them installed. The radio is built from src_radio/ and runs on Linux and macOS
# like the player (developed and tested on Linux; the macOS path is the same POSIX code
# but untested here).
#
# Usage:
#   ./setup.sh [options]
#
# Options:
#   --skip-deps         Configure and build only; don't touch the package manager / pip.
#   --debug             Debug build (default: Release).
#   --build-type TYPE   Release or Debug.
#   --no-install        Don't create the launchers. By default the commands "mousiki"
#                       and "lala" are created in ~/.local/bin (both start the app).
#   --install           Create the launchers (the default; kept for older scripts).
#   --no-upgrade        Don't upgrade an ffmpeg that is already installed (it is upgraded by default,
#                       so that libopenmpt / libgme support and fixes arrive).
#   -y, --yes           Don't ask before installing packages (uses sudo if needed).
#   -h, --help          Show this help.

set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build"

SKIP_DEPS=0
BUILD_TYPE="Release"
DO_INSTALL=1
ASSUME_YES=0
UPGRADE_FFMPEG=1

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_OFF=$'\033[0m'
else
    C_CYAN=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_OFF=""
fi

step() { printf '%s==> %s%s\n' "$C_CYAN" "$*" "$C_OFF"; }
warn() { printf '%sWarning: %s%s\n' "$C_YELLOW" "$*" "$C_OFF" >&2; }
die()  { printf '%sError: %s%s\n' "$C_RED" "$*" "$C_OFF" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() { sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' | sed '$d'; }

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-deps)  SKIP_DEPS=1 ;;
        --debug)      BUILD_TYPE="Debug" ;;
        --build-type)
            shift
            [ $# -gt 0 ] || die "--build-type needs a value (Release or Debug)"
            case "$1" in
                Release|Debug) BUILD_TYPE="$1" ;;
                *) die "--build-type must be Release or Debug, got '$1'" ;;
            esac
            ;;
        --install)    DO_INSTALL=1 ;;
        --no-install) DO_INSTALL=0 ;;
        --no-upgrade) UPGRADE_FFMPEG=0 ;;
        -y|--yes)     ASSUME_YES=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
OS="$(uname -s)"
case "$OS" in
    Linux)  PLATFORM="linux" ;;
    Darwin) PLATFORM="macos" ;;
    *)      die "unsupported OS '$OS' -- on Windows use setup.ps1" ;;
esac

PM=""
if [ "$PLATFORM" = "macos" ]; then
    PM="brew"
elif have apt-get; then PM="apt"
elif have dnf;     then PM="dnf"
elif have pacman;  then PM="pacman"
elif have zypper;  then PM="zypper"
elif have apk;     then PM="apk"
fi

# ---------------------------------------------------------------------------
# Package manager plumbing
# ---------------------------------------------------------------------------
SUDO=""
if [ "$PLATFORM" = "linux" ] && [ "$(id -u)" -ne 0 ]; then
    if have sudo; then SUDO="sudo"; fi
fi

APT_UPDATED=0

# pkg_install <package>...   -- returns non-zero if the install failed
pkg_install() {
    case "$PM" in
        apt)
            if [ "$APT_UPDATED" -eq 0 ]; then
                $SUDO apt-get update || return 1
                APT_UPDATED=1
            fi
            $SUDO apt-get install -y "$@" ;;
        dnf)    $SUDO dnf install -y "$@" ;;
        pacman) $SUDO pacman -S --needed --noconfirm "$@" ;;
        zypper) $SUDO zypper --non-interactive install "$@" ;;
        apk)    $SUDO apk add "$@" ;;
        brew)   brew install "$@" ;;
        *)      return 1 ;;
    esac
}

# Like pkg_install, but a failure only produces a warning (optional packages).
pkg_install_optional() {
    local what="$1"; shift
    pkg_install "$@" || warn "could not install $what ($*) -- continuing without it"
}

# ---------------------------------------------------------------------------
# Python helpers
# ---------------------------------------------------------------------------
# mousiki calls plain "python3" on Linux/macOS, so that is what has to work.
# SDL2 (optional): the scope and spectrogram windows. Mousiki dlopen()s it at run time.
have_sdl2() {
    if [ "$PLATFORM" = "macos" ]; then
        [ -e /opt/homebrew/lib/libSDL2-2.0.0.dylib ] || [ -e /usr/local/lib/libSDL2-2.0.0.dylib ] ||
            { have brew && brew list --versions sdl2 >/dev/null 2>&1; }
    else
        { ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null; } | grep -q 'libSDL2-2\.0\.so\.0'
    fi
}

pip_install_ytdlp() {
    have python3 || return 1
    python3 -m pip --version >/dev/null 2>&1 || return 1
    python3 -m pip install --quiet --user --upgrade yt-dlp 2>/dev/null && return 0
    python3 -m pip install --quiet --user --break-system-packages --upgrade yt-dlp
}


# ---------------------------------------------------------------------------
# FFmpeg upgrade + retro-format helpers
# ---------------------------------------------------------------------------
# Tracker modules and console music are read through FFmpeg's libopenmpt / libgme demuxers.
ffmpeg_has_demuxer() {
    have ffmpeg || return 1
    local out
    out="$(ffmpeg -hide_banner -demuxers 2>/dev/null)"
    grep -qw "$1" <<<"$out"
}
ffmpeg_formats_ok() { ffmpeg_has_demuxer libopenmpt && ffmpeg_has_demuxer libgme; }

# pkg_upgrade <package>...   -- upgrade packages that are already installed (never installs new ones)
pkg_upgrade() {
    case "$PM" in
        apt)
            if [ "$APT_UPDATED" -eq 0 ]; then
                $SUDO apt-get update || return 1
                APT_UPDATED=1
            fi
            $SUDO apt-get install -y --only-upgrade "$@" ;;
        dnf)    $SUDO dnf upgrade -y "$@" ;;
        pacman) $SUDO pacman -S --noconfirm "$@" ;;
        zypper) $SUDO zypper --non-interactive update "$@" ;;
        apk)    $SUDO apk add --upgrade "$@" ;;
        brew)   brew upgrade "$@" ;;
        *)      return 1 ;;
    esac
}

# macOS: Homebrew's own ffmpeg has neither libopenmpt nor libgme. The homebrew-ffmpeg tap can build one with both
# (from source, so it takes a while). The stock ffmpeg is put back if that fails.
brew_ffmpeg_with_retro_formats() {
    brew tap homebrew-ffmpeg/ffmpeg >/dev/null 2>&1 || { warn "could not add the homebrew-ffmpeg/ffmpeg tap"; return 1; }
    brew uninstall --ignore-dependencies ffmpeg >/dev/null 2>&1
    if brew install homebrew-ffmpeg/ffmpeg/ffmpeg --with-libopenmpt --with-game-music-emu; then
        return 0
    fi
    warn "building ffmpeg with libopenmpt / libgme failed -- putting Homebrew's stock ffmpeg back"
    brew install ffmpeg || true
    return 1
}

# sidplayfp, found in a directory that is not on the PATH -> symlink it into ~/.local/bin.
link_sidplayfp_into_path() {
    have sidplayfp && return 0
    local d found=""
    for d in /usr/local/bin /opt/homebrew/bin /usr/games /usr/local/games /opt/sidplayfp/bin "$HOME/bin" "$HOME/.local/bin"; do
        if [ -x "$d/sidplayfp" ]; then found="$d/sidplayfp"; break; fi
    done
    [ -n "$found" ] || return 1
    case "$found" in
        "$HOME/.local/bin/sidplayfp") ;;
        *) mkdir -p "$HOME/.local/bin" && ln -sf "$found" "$HOME/.local/bin/sidplayfp" || return 1 ;;
    esac
    case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) PATH="$HOME/.local/bin:$PATH"; export PATH
        warn "sidplayfp is in ~/.local/bin, which is not on your PATH. Add this to your shell profile:  export PATH=\"\$HOME/.local/bin:\$PATH\"" ;;
    esac
    step "sidplayfp ($found) is now on the PATH via ~/.local/bin/sidplayfp"
}

# Fallback when the package manager has no sidplayfp program (Homebrew only ships the library libsidplayfp):
# build the player from the official release tarball (pinned and checksummed) into ~/.local.
SIDPLAYFP_VERSION="3.1.0"
SIDPLAYFP_SHA256="287cc5fe859e242f885f8f2c6da37add62b89b232c8a55d23b74e2c59281f4e7"
build_sidplayfp_from_source() {
    have curl || { warn "curl is needed to download sidplayfp"; return 1; }
    local tmp url="https://github.com/libsidplayfp/sidplayfp/releases/download/v${SIDPLAYFP_VERSION}/sidplayfp-${SIDPLAYFP_VERSION}.tar.gz"
    tmp="$(mktemp -d)" || return 1
    if [ "$PM" = "brew" ]; then pkg_install libsidplayfp pkgconf || { rm -rf "$tmp"; return 1; }; fi
    curl -fsSL -o "$tmp/sid.tar.gz" "$url" || { rm -rf "$tmp"; warn "could not download $url"; return 1; }
    local got=""
    if have sha256sum; then got="$(sha256sum "$tmp/sid.tar.gz" | cut -d' ' -f1)"; else got="$(shasum -a 256 "$tmp/sid.tar.gz" | cut -d' ' -f1)"; fi
    if [ "$got" != "$SIDPLAYFP_SHA256" ]; then
        rm -rf "$tmp"; warn "SHA-256 mismatch for sidplayfp-${SIDPLAYFP_VERSION}.tar.gz (got $got) -- not building it"; return 1
    fi
    ( cd "$tmp" && tar xzf sid.tar.gz && cd "sidplayfp-${SIDPLAYFP_VERSION}" &&
      ./configure --prefix="$HOME/.local" >"$tmp/configure.log" 2>&1 &&
      make -j"${JOBS:-2}" >"$tmp/make.log" 2>&1 && make install >>"$tmp/make.log" 2>&1 ) ||
        { warn "building sidplayfp failed (logs: $tmp/configure.log, $tmp/make.log)"; return 1; }
    rm -rf "$tmp"
    export PATH="$HOME/.local/bin:$PATH"
    return 0
}

install_sidplayfp() {
    case "$PM" in
        apt|dnf|pacman|zypper|apk) pkg_install sidplayfp ;;
        brew) pkg_install sidplayfp >/dev/null 2>&1 || build_sidplayfp_from_source ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
confirm_install() {
    [ "$ASSUME_YES" -eq 1 ] && return 0
    [ -t 0 ] || return 0   # non-interactive (CI, pipe): don't hang on a prompt
    local reply
    printf 'Install / update packages with %s%s? [Y/n] ' "${SUDO:+$SUDO }" "$PM"
    read -r reply
    case "$reply" in [nN]*) return 1 ;; *) return 0 ;; esac
}

install_deps() {
    if [ -z "$PM" ]; then
        warn "no supported package manager found (apt, dnf, pacman, zypper, apk, brew)."
        warn "Install a C++17 compiler, CMake >= 3.16, ffmpeg, yt-dlp, python3 and ALSA/PulseAudio"
        warn "yourself, then re-run with --skip-deps."
        return 0
    fi

    if [ "$PLATFORM" = "macos" ]; then
        if ! have brew; then
            warn "Homebrew not found. Install it from https://brew.sh, then re-run this script"
            warn "(or install cmake, ffmpeg, yt-dlp and python3 yourself and use --skip-deps)."
            return 0
        fi
        if ! xcode-select -p >/dev/null 2>&1; then
            warn "Xcode Command Line Tools are missing (they provide the C++ compiler)."
            warn "Run:  xcode-select --install   -- then re-run this script when it has finished."
            exit 1
        fi
    elif [ -n "$SUDO" ] || [ "$(id -u)" -eq 0 ]; then
        :
    else
        warn "not root and 'sudo' not found -- package installation will probably fail."
    fi

    # Work out what is missing, so that nothing is touched if everything is there.
    local need_compiler=0 need_cmake=0 need_ffmpeg=0 need_ytdlp=0 need_python=0 need_audio=0 need_curl=0 need_sdl=0
    if ! have c++ && ! have g++ && ! have clang++; then need_compiler=1; fi
    have cmake   || need_cmake=1
    have ffmpeg  || need_ffmpeg=1
    have ffprobe || need_ffmpeg=1
    have yt-dlp  || need_ytdlp=1
    have python3 || need_python=1
    have curl    || need_curl=1     # radio mode: Radio Browser search
    have_sdl2    || need_sdl=1      # optional: scope / spectrogram windows
    # Retro formats: an ffmpeg without libopenmpt / libgme, and the sidplayfp program (SID tunes).
    local need_sid=0 upgrade_ffmpeg=0
    if ! have sidplayfp && ! link_sidplayfp_into_path; then need_sid=1; fi
    if have ffmpeg && [ "$need_ffmpeg" -eq 0 ] && [ "$UPGRADE_FFMPEG" -eq 1 ]; then upgrade_ffmpeg=1; fi
    # ALSA/PulseAudio: miniaudio dlopen()s them at runtime, so there is no
    # reliable "is it installed" test that doesn't depend on the distro.
    # Installing the (tiny) dev packages is idempotent and guarantees both.
    [ "$PLATFORM" = "linux" ] && need_audio=1

    if [ $((need_compiler + need_cmake + need_ffmpeg + need_ytdlp + need_python + need_audio + need_curl + need_sdl + need_sid + upgrade_ffmpeg)) -gt 0 ]; then
        confirm_install || { warn "skipping package installation at your request."; return 0; }
    fi

    # --- compiler + cmake ---------------------------------------------------
    if [ "$need_compiler" -eq 1 ]; then
        step "Installing a C++ compiler"
        case "$PM" in
            apt)    pkg_install build-essential ;;
            dnf)    pkg_install gcc-c++ make ;;
            pacman) pkg_install base-devel ;;
            zypper) pkg_install gcc-c++ make ;;
            apk)    pkg_install build-base ;;
            brew)   : ;;  # handled by the Xcode CLT check above
        esac || warn "compiler installation failed"
    fi
    if [ "$need_cmake" -eq 1 ]; then
        step "Installing CMake"
        pkg_install cmake || warn "CMake installation failed"
    fi

    # --- audio libraries (Linux) ---------------------------------------------
    if [ "$need_audio" -eq 1 ]; then
        step "Installing ALSA / PulseAudio libraries"
        case "$PM" in
            apt)    pkg_install_optional "audio libraries" libasound2-dev libpulse-dev ;;
            dnf)    pkg_install_optional "audio libraries" alsa-lib-devel pulseaudio-libs-devel ;;
            pacman) pkg_install_optional "audio libraries" alsa-lib libpulse ;;
            zypper) pkg_install_optional "audio libraries" alsa-devel libpulse-devel ;;
            apk)    pkg_install_optional "audio libraries" alsa-lib-dev pulseaudio-dev ;;
        esac
    fi

    # --- ffmpeg --------------------------------------------------------------
    if [ "$need_ffmpeg" -eq 1 ]; then
        step "Installing FFmpeg"
        if [ "$PM" = "dnf" ]; then
            # Fedora's stock repos only carry ffmpeg-free (reduced codec set);
            # the full ffmpeg needs RPM Fusion. Try both, explain if neither works.
            pkg_install ffmpeg || pkg_install ffmpeg-free ||
                warn "FFmpeg not available from your repositories -- enable RPM Fusion (https://rpmfusion.org) and install 'ffmpeg'."
        else
            pkg_install ffmpeg || warn "FFmpeg installation failed -- Opus playback, metadata and AcoustID will not work"
        fi
    fi

    # --- ffmpeg already there: upgrade it, then make sure it has libopenmpt + libgme -----
    if [ "$upgrade_ffmpeg" -eq 1 ]; then
        step "Upgrading FFmpeg (already installed)"
        pkg_upgrade ffmpeg || warn "could not upgrade ffmpeg (it may have been installed outside the package manager) -- keeping the installed one"
    fi
    if have ffmpeg && ! ffmpeg_formats_ok; then
        if [ "$PM" = "brew" ]; then
            step "Rebuilding FFmpeg with libopenmpt and libgme (tracker modules, console music) -- this takes a few minutes"
            brew_ffmpeg_with_retro_formats || true
        fi
        if ! ffmpeg_formats_ok; then
            ffmpeg_has_demuxer libopenmpt || warn "your ffmpeg has no libopenmpt -- tracker modules (.mod .xm .it .s3m ...) will not play."
            ffmpeg_has_demuxer libgme     || warn "your ffmpeg has no libgme -- console music (.nsf .spc .vgm ...) will not play."
            if [ "$PM" = "dnf" ] && rpm -q ffmpeg-free >/dev/null 2>&1; then
                warn "Fedora's ffmpeg-free is a reduced build. Enable RPM Fusion (https://rpmfusion.org), then:  sudo dnf swap ffmpeg-free ffmpeg --allowerasing"
            else
                warn "Install an FFmpeg build that includes them (most distro packages do), and make sure it is the first ffmpeg on your PATH."
            fi
        fi
    fi

    # --- sidplayfp (Commodore 64 SID tunes) --------------------------------------
    if [ "$need_sid" -eq 1 ]; then
        step "Installing sidplayfp (C64 SID tunes)"
        install_sidplayfp || warn "could not install sidplayfp -- .sid tunes will not play (everything else works)"
        link_sidplayfp_into_path || true
    fi

    # --- curl (radio mode) ----------------------------------------------------
    if [ "$need_curl" -eq 1 ]; then
        step "Installing curl (radio mode: station search)"
        pkg_install curl || warn "could not install curl -- the radio's RADIO BROWSER search (SHIFT+S) will not work"
    fi

    # --- SDL2 (optional: scope and spectrogram windows) ----------------------
    if [ "$need_sdl" -eq 1 ]; then
        step "Installing SDL2 (optional: the scope and spectrogram windows)"
        case "$PM" in
            apt)    pkg_install_optional "SDL2" libsdl2-2.0-0 ;;
            dnf)    pkg_install_optional "SDL2" SDL2 ;;
            pacman) pkg_install_optional "SDL2" sdl2 ;;
            zypper) pkg_install_optional "SDL2" libSDL2-2_0-0 ;;
            apk)    pkg_install_optional "SDL2" sdl2 ;;
            brew)   pkg_install_optional "SDL2" sdl2 ;;
        esac
    fi

    # --- python3 -------------------------------------------------------------
    if [ "$need_python" -eq 1 ]; then
        step "Installing Python 3"
        case "$PM" in
            apt)    pkg_install python3 python3-pip ;;
            dnf)    pkg_install python3 python3-pip ;;
            pacman) pkg_install python python-pip ;;
            zypper) pkg_install python3 python3-pip ;;
            apk)    pkg_install python3 py3-pip ;;
            brew)   pkg_install python ;;
        esac || warn "Python installation failed -- lyrics, fast online search and AcoustID will be unavailable"
    fi

    # (the Python scripts use only the standard library: no pip package is needed)
    if ! have python3; then
        warn "no Python 3 found. Lyrics, fast online search and AcoustID will be unavailable; everything else still works."
    fi

    # --- yt-dlp --------------------------------------------------------------
    if [ "$need_ytdlp" -eq 1 ]; then
        step "Installing yt-dlp"
        if ! pkg_install yt-dlp 2>/dev/null; then
            warn "yt-dlp is not in your package repositories -- trying pip instead"
            pip_install_ytdlp ||
                warn "could not install yt-dlp. Online search/streaming will be unavailable (see https://github.com/yt-dlp/yt-dlp#installation)."
        fi
    fi

    # --- clipboard helper (Linux, optional) -----------------------------------
    # mousiki pastes via pbpaste (macOS, built in), wl-paste (Wayland) or xclip (X11).
    if [ "$PLATFORM" = "linux" ] && ! have xclip && ! have wl-paste; then
        step "Installing a clipboard helper (optional, for pasting into search)"
        if [ -n "${WAYLAND_DISPLAY:-}" ]; then
            pkg_install_optional "clipboard helper" wl-clipboard
        else
            pkg_install_optional "clipboard helper" xclip
        fi
    fi

    # ~/.local/bin is where pip --user puts yt-dlp; warn if it isn't on PATH.
    if have yt-dlp; then :; elif [ -x "$HOME/.local/bin/yt-dlp" ]; then
        warn "yt-dlp was installed to ~/.local/bin, which is not on your PATH."
        warn "Add this to your shell profile:  export PATH=\"\$HOME/.local/bin:\$PATH\""
    fi
}

if [ "$SKIP_DEPS" -eq 0 ]; then
    install_deps
fi

# ---------------------------------------------------------------------------
# Toolchain check
# ---------------------------------------------------------------------------
have cmake || die "cmake is not on PATH. Install CMake >= 3.16 (or drop --skip-deps)."

if ! have c++ && ! have g++ && ! have clang++; then
    if [ "$PLATFORM" = "macos" ]; then
        die "no C++ compiler found. Run 'xcode-select --install' and re-run this script."
    else
        die "no C++17 compiler found. Install g++ or clang++ (e.g. build-essential) and re-run this script."
    fi
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if have nproc; then
    JOBS="$(nproc)"
elif [ "$PLATFORM" = "macos" ]; then
    JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
else
    JOBS=2
fi

step "Configuring ($BUILD_TYPE)"
# miniaudio, kissfft and chromaprint are vendored in third_party/, so
# configuring needs no network access.
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" || die "cmake configure failed"

step "Building"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$JOBS" || die "cmake build failed"

EXE=""
for cand in "$BUILD_DIR/mousiki" "$BUILD_DIR/$BUILD_TYPE/mousiki"; do
    if [ -x "$cand" ]; then EXE="$cand"; break; fi
done
[ -n "$EXE" ] || die "build reported success but the mousiki executable was not found under $BUILD_DIR"

EXE_DIR="$(dirname "$EXE")"
if [ ! -d "$EXE_DIR/scripts" ]; then
    warn "scripts/ was not copied next to the executable -- lyrics, fast online search and AcoustID will not work."
elif [ ! -x "$EXE_DIR/scripts/fpcalc" ]; then
    warn "fpcalc was not built -- the AcoustID metadata fetch will not work."
fi

# Radio mode: its default stations.txt / radio_config.txt are copied next to the executable by CMake.
if [ ! -f "$EXE_DIR/stations.txt" ] || [ ! -f "$EXE_DIR/radio_config.txt" ]; then
    warn "stations.txt or radio_config.txt were not copied next to the executable -- the radio starts with its built-in defaults."
fi
# Recording in the radio converts to MP3 through ffmpeg's libmp3lame.
if have ffmpeg && ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libmp3lame; then
    warn "your ffmpeg has no libmp3lame encoder -- recording a radio stream to MP3 will fail (everything else works)."
fi

# ---------------------------------------------------------------------------
# Launchers: "mousiki" and "lala" (skip with --no-install)
# ---------------------------------------------------------------------------
# Wrapper scripts rather than symlinks: macOS' executable-path lookup does not
# resolve symlinks, so a symlinked binary would not find its scripts/ folder.
# Both names start the same program.
if [ "$DO_INSTALL" -eq 1 ]; then
    BIN_DIR="$HOME/.local/bin"
    mkdir -p "$BIN_DIR"
    for NAME in mousiki lala; do
        LAUNCHER="$BIN_DIR/$NAME"
        printf '#!/bin/sh\nexec "%s" "$@"\n' "$EXE" > "$LAUNCHER"
        chmod +x "$LAUNCHER"
        step "Launcher created: $LAUNCHER"
    done
    case ":$PATH:" in
        *":$BIN_DIR:"*) ;;
        *) warn "$BIN_DIR is not on your PATH. Add this to your shell profile:  export PATH=\"\$HOME/.local/bin:\$PATH\"" ;;
    esac
fi

echo
printf '%sBuilt: %s%s\n' "$C_GREEN" "$EXE" "$C_OFF"
echo "Run it with:  '$EXE'"
[ "$DO_INSTALL" -eq 1 ] && echo "or simply:    mousiki   (or: lala)"
echo
echo "Config will be generated at: \$HOME/.config/mousiki/config.txt"
echo "Radio mode settings: \$HOME/.config/mousiki/radio_config.txt (a default ships as radio_config.txt in the project folder)."
echo "Switch between the music player and the radio with SHIFT and the + key (the * character)."
echo "Keep the scripts/ folder (and stations.txt / radio_config.txt) next to the executable if you move it."
if [ "$PLATFORM" = "linux" ]; then
    echo "Audio goes through PulseAudio/PipeWire-pulse or ALSA; use a UTF-8 terminal."
fi
