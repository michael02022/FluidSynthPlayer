#!/bin/sh
# Packs a Linux build of the plugin into a distribution zip:
#   dist/FluidSynthPlayer-<version>-linux-<arch>.zip
#     FluidSynthPlayer.clap, FluidSynthPlayer.vst3/ (if built), README.txt,
#     LICENSE.txt, licenses/...
#
# Build first (see README.md, "Linux"), then from the repository root:
#   sh scripts/package-linux.sh [build-dir] [out-dir]
# A build without libsndfile is packaged too, but without SF3 support (the
# README.txt in the zip says so and a warning is printed).
set -eu

cd "$(dirname "$0")/.."
BUILD_DIR=${1:-build-clap}
OUT_DIR=${2:-dist}

# ---- inputs ----
clap=$BUILD_DIR/FluidSynthPlayer.clap
[ -f "$clap" ] || { echo "error: $clap not found - build it first." >&2; exit 1; }
command -v zip >/dev/null || { echo "error: zip is not installed." >&2; exit 1; }
# The VST3 bundle (FSP_BUILD_VST3, on by default), wherever the generator put it.
vst3=
for d in "$BUILD_DIR/Release/FluidSynthPlayer.vst3" "$BUILD_DIR/FluidSynthPlayer.vst3"; do
    [ -d "$d" ] && { vst3=$d; break; }
done
[ -n "$vst3" ] || echo "warning: no FluidSynthPlayer.vst3 in $BUILD_DIR: packaging the CLAP only." >&2

version=$(sed -n 's/^project(FluidSynthPlayer VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
commit=$(git rev-parse --short HEAD)
dirty=
[ -z "$(git status --porcelain -- src external CMakeLists.txt cmake)" ] || dirty=" (with local changes)"
# Written by CMakeLists.txt at configure time: was libsndfile (SF3) found?
info=$BUILD_DIR/fsp_build_info.txt
[ -f "$info" ] || { echo "error: $info not found - reconfigure the build with CMake." >&2; exit 1; }
has_sf3=no
grep -q '^sf3=ON' "$info" && has_sf3=yes
[ $has_sf3 = yes ] || echo "warning: $BUILD_DIR was built without libsndfile: the package will have NO SF3 support." >&2

arch=$(uname -m)
# Newest glibc symbol version the plugin needs = oldest glibc it runs on.
glibc=$(objdump -T "$clap" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -n 1)

name=FluidSynthPlayer-$version-linux-$arch
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name/licenses"
pkg=$stage/$name

# ---- plugin (debug info stripped) ----
strip --strip-unneeded -o "$pkg/FluidSynthPlayer.clap" "$clap"
if [ -n "$vst3" ]; then
    so=Contents/$arch-linux/FluidSynthPlayer.so
    mkdir -p "$pkg/FluidSynthPlayer.vst3/Contents/$arch-linux"
    strip --strip-unneeded -o "$pkg/FluidSynthPlayer.vst3/$so" "$vst3/$so"
fi

# ---- licenses ----
cp LICENSE.txt "$pkg/LICENSE.txt"
while read -r dest src; do
    cp "$src" "$pkg/licenses/$dest"
done <<EOF
FluidSynth-LGPL-2.1.txt external/fluidsynth/LICENSE
gcem-Apache-2.0.txt external/fluidsynth/gcem/LICENSE
Signalsmith-audio-basics-MIT.txt external/fluidsynth/signalsmith-audio-basics/LICENSE.txt
Signalsmith-dsp-MIT.txt external/signalsmith/dsp/LICENSE.txt
Signalsmith-linear-MIT.txt external/signalsmith/linear/LICENSE.txt
Signalsmith-hilbert-iir-0BSD.txt external/signalsmith/hilbert-iir/LICENSE.txt
CLAP-MIT.txt external/clap/LICENSE
DearImGui-MIT.txt external/imgui/LICENSE.txt
FontAwesome-OFL-1.1.txt external/fontawesome/LICENSE-OFL.txt
Roboto-Apache-2.0.txt external/roboto/LICENSE.txt
clap-wrapper-MIT.txt external/clap-wrapper/LICENSE
VST3-SDK-MIT.txt external/vst3sdk/LICENSE.txt
EOF

# ---- README.txt ----
if [ $has_sf3 = yes ]; then
    formats="Formats: SF2, SF3 (Ogg/Vorbis compressed) and DLS SoundFonts."
    sndfile_req="- libsndfile 1.x (SF3 support), preinstalled on nearly every desktop;
  otherwise install \"libsndfile1\" (Debian/Ubuntu) or \"libsndfile\"
  (Fedora/Arch)"
else
    formats="Formats: SF2 and DLS SoundFonts. This build has NO SF3 support (it was
  built without libsndfile); SF3 files will not load."
    sndfile_req=
fi

cat > "$pkg/README.txt" <<EOF
FluidSynth Player $version - CLAP / VST3 plugin for Linux ($arch)
================================================================

SoundFont player instrument based on FluidSynth 2.6 (statically linked),
with an ImGui interface. $formats

Built from https://github.com/michael02022/FluidSynthPlayer
commit $commit$dirty.

Requirements
------------
- $arch Linux with glibc $glibc or newer
- A DAW / host with CLAP or VST3 support (e.g. REAPER, Bitwig Studio,
  Ardour)
- X11 or XWayland, with OpenGL
$sndfile_req
- Optional: zenity, for the folder button's file dialog (drag & drop
  works without it)

Installation
------------
Copy FluidSynthPlayer.clap into one of the standard CLAP folders, and/or
the FluidSynthPlayer.vst3 folder into one of the VST3 folders, then let the
DAW rescan its plugins (use one format per DAW; both are the same plugin):

  Current user only:
    ~/.clap/            ~/.vst3/

  All users (needs root):
    /usr/lib/clap/      /usr/lib/vst3/

Usage notes
-----------
- Load a SoundFont with the folder button or by dropping a file from the
  file manager onto the plugin window.
- The pencil button opens the SoundFont in an external editor: set it in
  Options > Advanced (command, e.g. polyphone).
- Global preferences (default SoundFont, last folder, external editor) are
  stored in ~/.config/FluidSynthPlayer/config.txt.

License
-------
FluidSynth Player is MIT licensed (LICENSE.txt). It contains third-party
components under their own licenses (see the licenses folder):
  FluidSynth (LGPL 2.1), gcem (Apache 2.0), Signalsmith libraries (MIT,
  0BSD), CLAP (MIT), Dear ImGui (MIT), Font Awesome 4 font (SIL OFL 1.1),
  Roboto font (Apache 2.0); the VST3 also clap-wrapper (MIT) and the
  Steinberg VST3 SDK (MIT). VST is a trademark of Steinberg Media
  Technologies GmbH.

FluidSynth is linked statically. As required by the LGPL, the complete
source code - including FluidSynth (external/fluidsynth) and the build
scripts needed to rebuild/relink the plugin against a modified FluidSynth -
is available at the repository and commit given above. libsndfile is not
included: the plugin uses the system's shared library.
EOF

# ---- zip (normal permissions, whatever the build file system says) ----
find "$pkg" -type d -exec chmod 755 {} +
find "$pkg" -type f -exec chmod 644 {} +
chmod 755 "$pkg/FluidSynthPlayer.clap"
[ -z "$vst3" ] || chmod 755 "$pkg/FluidSynthPlayer.vst3/$so"
mkdir -p "$OUT_DIR"
zip_path=$(cd "$OUT_DIR" && pwd)/$name.zip
rm -f "$zip_path"
(cd "$stage" && zip -qr -X "$zip_path" "$name")
echo "Created $OUT_DIR/$name.zip (SF3 support: $has_sf3, VST3: $([ -n "$vst3" ] && echo yes || echo no), glibc >= $glibc)"
