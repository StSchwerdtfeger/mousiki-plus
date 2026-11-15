#!/usr/bin/env bash
# milkdrop-unix.sh -- builds projectM (the MilkDrop visuals) for Linux and macOS and installs it with the "Cream of the Crop" presets
# and the MilkDrop texture pack. The same pinned sources and checksums as tools/milkdrop-windows.ps1.
#
#   tools/milkdrop-unix.sh                    for the user: ~/.local/lib (projectM), ~/.local/share/projectM/{presets,textures}
#   tools/milkdrop-unix.sh --exe-dir DIR      next to the program (the installers and portable zips): DIR/libprojectM-4*, DIR/milkdrop/
#
# Options:
#   --cache DIR     downloads and the projectM build (default: ~/.cache/mousiki-milkdrop, macOS ~/Library/Caches/mousiki-milkdrop)
#   --force         build and copy again even when what is installed matches the pins
#   --show-build    show the output of projectM's configure / build / install (else build.log in the cache, shown when a step fails)
#   --print-manifests   only print the checksums of the archives in the cache (to update a pin) and stop
#   --archs "a;b"   macOS: CMAKE_OSX_ARCHITECTURES for the projectM build (default: this Mac's own), e.g. "arm64;x86_64"
#
# Mousiki loads the libraries when the visuals are first asked for; without them the MilkDrop view is simply left out, so a failure here
# costs nothing else. Every download is pinned to one commit and checked file by file: the SHA-256 of every file in the archive, listed
# in order, gives one checksum per archive (the archive's own bytes are not used). A mismatch stops before anything is built or installed.
# Needs: cmake, a C++ compiler, curl, python3; about 80 MB of downloads and a few minutes of building, once.

set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE_DIR=""
CACHE=""
FORCE=0
SHOW_BUILD=0
PRINT_MANIFESTS=0
ARCHS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --exe-dir) shift; EXE_DIR="${1:-}" ;;
        --cache) shift; CACHE="${1:-}" ;;
        --force) FORCE=1 ;;
        --show-build) SHOW_BUILD=1 ;;
        --print-manifests) PRINT_MANIFESTS=1 ;;
        --archs) shift; ARCHS="${1:-}" ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' | sed '$d'; exit 0 ;;
        *) echo "milkdrop-unix.sh: unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ -t 1 ]; then C_CYAN=$'\033[36m'; C_OFF=$'\033[0m'; else C_CYAN=""; C_OFF=""; fi
step() { printf '%s==> %s%s\n' "$C_CYAN" "$*" "$C_OFF"; }
fail() { echo "milkdrop-unix.sh: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then LIBGLOB="libprojectM-4*.dylib"; else LIBGLOB="libprojectM-4*.so*"; fi
if [ -z "$CACHE" ]; then
    if [ "$OS" = "Darwin" ]; then CACHE="$HOME/Library/Caches/mousiki-milkdrop"; else CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/mousiki-milkdrop"; fi
fi
if [ -n "$EXE_DIR" ]; then
    mkdir -p "$EXE_DIR" || fail "cannot make $EXE_DIR"
    EXE_DIR="$(cd "$EXE_DIR" && pwd)"
    LIB_DEST="$EXE_DIR"
    MD_DIR="$EXE_DIR/milkdrop"
    PRESETS_DEST="$MD_DIR/presets"
    TEXTURES_DEST="$MD_DIR/textures"
    DOC_DEST="$MD_DIR"
    STAMP_FILE="$MD_DIR/installed.txt"
else
    LIB_DEST="$HOME/.local/lib"
    MD_DIR="$HOME/.local/share/projectM"
    PRESETS_DEST="$MD_DIR/presets"
    TEXTURES_DEST="$MD_DIR/textures"
    DOC_DEST="$MD_DIR/mousiki-milkdrop"
    STAMP_FILE="$DOC_DEST/installed.txt"
fi
mkdir -p "$CACHE" || fail "cannot make $CACHE"
CACHE="$(cd "$CACHE" && pwd)"

# ---- the pins (the same as tools/milkdrop-windows.ps1: keep them in step; THIRD_PARTY_LICENSES.txt too) ----
PIN_NAMES=(projectm projectm-eval presets textures)
PIN_URLS=(
    "https://github.com/projectM-visualizer/projectm/archive/eae22de40144ee8ded92f3c04b93a4c2f82adcc0.zip"
    "https://github.com/projectM-visualizer/projectm-eval/archive/22fb0cfd8f2dfbcd2b68f2443e7f44e19b32c09a.zip"
    "https://github.com/projectM-visualizer/presets-cream-of-the-crop/archive/0180df21f5e0bd39b9060cc5de420ed2f1f9e509.zip"
    "https://github.com/projectM-visualizer/presets-milkdrop-texture-pack/archive/6368812f27bc747b517218fbf89d21d59afce4d9.zip"
)
PIN_SUMS=(
    "72181d0f93696f3a6e0a2f5b800c649f2ba9c30d6a16f5af0391cb6ddd04ef75"
    "b3b2e8a6f5548c1fe3d61bc1a996807a2f035b77400e94b91271401e3a179385"
    "ff7768cb1cf936757df67b023f07c36e7d17865f9f681f6ab8d586feb4f3d4f8"
    "9f9c431fdccfbf047c258b3bb9fd428bdb203b24cb16fcabefc43813078fb062"
)
STAMP=""
for i in 0 1 2 3; do STAMP+="${PIN_NAMES[$i]} ${PIN_SUMS[$i]}"$'\n'; done

have python3 || fail "python3 is not on PATH (it checks the downloads)"

# One SHA-256 over "<sha256 of the file> <relative path>\n" for every file below the archive's one top folder, ordered by path
# (ordinal, as UTF-16 code units: the order tools/milkdrop-windows.ps1 uses), and a helper that unpacks the same files.
PYZIP="$CACHE/zipfiles.py"
cat > "$PYZIP" <<'PY'
import hashlib, os, sys, zipfile

def files(z):
    out = {}
    for info in z.infolist():
        name = info.filename
        if name.endswith('/'):
            continue
        slash = name.find('/')
        if slash < 0:
            continue
        out[name[slash + 1:]] = info
    return out

def manifest(path):
    with zipfile.ZipFile(path) as z:
        fs = files(z)
        lines = []
        for n in sorted(fs, key=lambda s: s.encode('utf-16-be')):
            h = hashlib.sha256()
            with z.open(fs[n]) as f:
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    h.update(chunk)
            lines.append('%s %s\n' % (h.hexdigest(), n))
        return hashlib.sha256(''.join(lines).encode('utf-8')).hexdigest()

def extract(path, dest):
    import shutil
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    with zipfile.ZipFile(path) as z:
        for n, info in files(z).items():
            target = os.path.join(dest, *n.split('/'))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, 'wb') as out:
                shutil.copyfileobj(src, out)

if __name__ == '__main__':
    if sys.argv[1] == 'manifest':
        print(manifest(sys.argv[2]))
    else:
        extract(sys.argv[2], sys.argv[3])
PY
zip_manifest() { python3 -I "$PYZIP" manifest "$1"; }
zip_extract() { python3 -I "$PYZIP" extract "$1" "$2"; }

if [ "$PRINT_MANIFESTS" -eq 1 ]; then
    for i in 0 1 2 3; do
        z="$CACHE/${PIN_NAMES[$i]}.zip"
        if [ -f "$z" ]; then echo "${PIN_NAMES[$i]} $(zip_manifest "$z")"; else echo "${PIN_NAMES[$i]} (no $z)"; fi
    done
    exit 0
fi

# ---- already there? ----
have_libs() {
    local d="$1"
    if [ "$OS" = "Darwin" ]; then
        ls "$d"/libprojectM-4.4.dylib >/dev/null 2>&1 && ls "$d"/libprojectM-4-playlist.4.dylib >/dev/null 2>&1
    else
        ls "$d"/libprojectM-4.so.4 >/dev/null 2>&1 && ls "$d"/libprojectM-4-playlist.so.4 >/dev/null 2>&1
    fi
}
# The list of recommended presets (milkdrop_recommended.txt) goes beside the presets folder: Mousiki shows it as the "Recommended" folder
# of the preset overlay, looking every line up in the presets that are installed.
copy_recommended() {
    [ -f "$ROOT/milkdrop_recommended.txt" ] && cp -f "$ROOT/milkdrop_recommended.txt" "$(dirname "$PRESETS_DEST")/recommended.txt" 2>/dev/null
    return 0
}
if [ "$FORCE" -eq 0 ] && [ -f "$STAMP_FILE" ] && have_libs "$LIB_DEST" && [ "$(cat "$STAMP_FILE")" = "${STAMP%$'\n'}" ]; then
    step "MilkDrop (projectM + presets) is already installed and up to date"
    copy_recommended
    exit 0
fi

have cmake || fail "cmake is not on PATH (it is needed to build projectM)"
have curl || fail "curl is not on PATH"
if ! have c++ && ! have g++ && ! have clang++; then fail "no C++ compiler found"; fi

# ---- the sources ----
get_archive() {   # name url sum -> path of the verified archive in the cache
    local name="$1" url="$2" sum="$3" zip="$CACHE/$1.zip" got=""
    if [ -f "$zip" ]; then
        if [ "$(zip_manifest "$zip" 2>/dev/null)" = "$sum" ]; then echo "$zip"; return 0; fi
        rm -f "$zip"
    fi
    local try
    for try in 1 2; do
        step "Downloading $name ($url)" >&2
        curl -fsSL --retry 3 -o "$zip.part" "$url" || { rm -f "$zip.part"; continue; }
        got="$(zip_manifest "$zip.part" 2>/dev/null || echo "(not a zip)")"
        if [ "$got" = "$sum" ]; then mv -f "$zip.part" "$zip"; echo "$zip"; return 0; fi
        rm -f "$zip.part"
        [ "$try" -eq 1 ] && echo "Warning: $name: the files do not match the pin - a damaged download is possible, trying once more." >&2
    done
    fail "$name: the files at $url do not match the pinned checksum (expected $sum, got ${got:-nothing}). Nothing was built or installed."
}
ZIP_PROJECTM="$(get_archive "${PIN_NAMES[0]}" "${PIN_URLS[0]}" "${PIN_SUMS[0]}")" || exit 1
ZIP_EVAL="$(get_archive "${PIN_NAMES[1]}" "${PIN_URLS[1]}" "${PIN_SUMS[1]}")" || exit 1
ZIP_PRESETS="$(get_archive "${PIN_NAMES[2]}" "${PIN_URLS[2]}" "${PIN_SUMS[2]}")" || exit 1
ZIP_TEXTURES="$(get_archive "${PIN_NAMES[3]}" "${PIN_URLS[3]}" "${PIN_SUMS[3]}")" || exit 1

# ---- build projectM (the unchanged sources of the pin; projectM-eval goes where its git submodule would be) ----
SRC="$CACHE/src"; BLD="$CACHE/build"; INST="$CACHE/install"; BUILD_STAMP="$CACHE/build-pins.txt"
if ! { [ -f "$BUILD_STAMP" ] && [ "$(cat "$BUILD_STAMP")" = "$STAMP$ARCHS" ]; }; then rm -rf "$SRC" "$BLD" "$INST"; fi
step "Unpacking projectM"
zip_extract "$ZIP_PROJECTM" "$SRC"
zip_extract "$ZIP_EVAL" "$SRC/vendor/projectm-eval"

LOG="$CACHE/build.log"
: > "$LOG"
cm_step() {   # what, args...
    local what="$1"; shift
    printf '\n==== cmake %s\n' "$*" >> "$LOG"
    if [ "$SHOW_BUILD" -eq 1 ]; then cmake "$@" 2>&1 | tee -a "$LOG"; else cmake "$@" >> "$LOG" 2>&1; fi
    local rc=${PIPESTATUS[0]}
    if [ "$rc" -ne 0 ]; then
        if [ "$SHOW_BUILD" -eq 0 ]; then echo "---- the last lines of $LOG :" >&2; tail -n 40 "$LOG" >&2; fi
        if [ "$OS" != "Darwin" ]; then echo "(projectM needs the OpenGL development files: apt libgl1-mesa-dev libegl1-mesa-dev, dnf mesa-libGL-devel mesa-libEGL-devel, pacman mesa, zypper Mesa-libGL-devel Mesa-libEGL-devel)" >&2; fi
        fail "projectM: $what failed (the whole output: $LOG)"
    fi
}
CFG=(-S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DENABLE_PLAYLIST=ON -DENABLE_SYSTEM_GLM=OFF
     -DENABLE_SYSTEM_PROJECTM_EVAL=OFF -DENABLE_SDL_UI=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF "-DCMAKE_INSTALL_PREFIX=$INST")
[ -n "$ARCHS" ] && [ "$OS" = "Darwin" ] && CFG+=("-DCMAKE_OSX_ARCHITECTURES=$ARCHS")
if [ "$OS" = "Darwin" ]; then CFG+=("-DCMAKE_INSTALL_NAME_DIR=@rpath"); else CFG+=("-DCMAKE_INSTALL_RPATH=\$ORIGIN"); fi
step "Configuring projectM"
cm_step "cmake configure" "${CFG[@]}"
if have nproc; then JOBS="$(nproc)"; elif [ "$OS" = "Darwin" ]; then JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"; else JOBS=2; fi
step "Building projectM (a few minutes; the output goes to $LOG)"
cm_step "the build" --build "$BLD" --config Release --parallel "$JOBS"
cm_step "cmake --install" --install "$BLD" --config Release
printf '%s' "$STAMP$ARCHS" > "$BUILD_STAMP"

# ---- install ----
LIBDIR=""
for d in "$INST/lib" "$INST/lib64" "$INST/lib/x86_64-linux-gnu" "$INST/lib/aarch64-linux-gnu"; do
    if ls "$d"/$LIBGLOB >/dev/null 2>&1; then LIBDIR="$d"; break; fi
done
[ -n "$LIBDIR" ] || fail "projectM: the libraries were not found in $INST after the build"
have_libs "$LIBDIR" || fail "projectM: libprojectM-4 / libprojectM-4-playlist were not found in $LIBDIR"
step "Installing projectM and the presets"
mkdir -p "$LIB_DEST" "$DOC_DEST"
rm -f "$LIB_DEST"/$LIBGLOB 2>/dev/null
cp -a "$LIBDIR"/$LIBGLOB "$LIB_DEST/" || fail "cannot copy the libraries to $LIB_DEST"
if [ "$OS" = "Darwin" ]; then
    # (the playlist library asks for projectM's by @rpath: its own folder is on its path, so the pair works wherever they stand)
    for f in "$LIB_DEST"/libprojectM-4*.dylib; do
        [ -L "$f" ] && continue
        install_name_tool -add_rpath @loader_path "$f" 2>/dev/null || true
    done
fi
zip_extract "$ZIP_PRESETS" "$PRESETS_DEST"
zip_extract "$ZIP_TEXTURES" "$TEXTURES_DEST"
copy_recommended
cp -f "$SRC/COPYING" "$DOC_DEST/LICENSE-projectM.txt"
cp -f "$SRC/vendor/projectm-eval/LICENSE.md" "$DOC_DEST/LICENSE-projectm-eval.md"
cp -f "$SRC/vendor/hlslparser/LICENSE" "$DOC_DEST/LICENSE-hlslparser.txt"

cat > "$DOC_DEST/README.txt" <<EOF
MilkDrop visuals for Mousiki
============================
libprojectM-4 and libprojectM-4-playlist are projectM, the open-source rebuild of the Winamp visualizer MilkDrop:
https://github.com/projectM-visualizer/projectm -- LGPL-2.1 (LICENSE-projectM.txt). They were built unchanged from this source
(the corresponding source code):
  ${PIN_URLS[0]}
  ${PIN_URLS[1]}  (projectM-eval, MIT, in vendor/projectm-eval)
Both archives are also attached to every Mousiki release on GitHub, as projectm-source-<commit>.zip:
https://github.com/StSchwerdtfeger/mousiki-plus/releases
Compiled into them (from projectM's vendor folder): projectM-eval (MIT, LICENSE-projectm-eval.md), hlslparser (MIT,
LICENSE-hlslparser.txt), GLM (MIT), glad (a generated OpenGL loader: WTFPL or CC0-1.0, and Apache-2.0 for the Khronos
OpenGL registry it is made from), stb_image (public domain or MIT).
You may replace the two libraries with your own build of projectM (4.2 or newer); Mousiki loads them at run time.

presets/   "Cream of the Crop" -- 9,795 MilkDrop presets, curated and sorted by ISOSCELES, the default preset pack of
           projectM: https://github.com/projectM-visualizer/presets-cream-of-the-crop (see presets/README.md, presets/LICENSE.md)
           ${PIN_URLS[2]}
textures/  the MilkDrop texture pack: https://github.com/projectM-visualizer/presets-milkdrop-texture-pack
           ${PIN_URLS[3]}
The presets were written by many authors over two decades and released freely; see presets/LICENSE.md.

Other presets: set MilkDropPresets= in Mousiki's config.txt.
EOF
printf '%s' "${STAMP%$'\n'}" > "$STAMP_FILE"
step "MilkDrop is ready: projectM in $LIB_DEST, presets in $PRESETS_DEST"
