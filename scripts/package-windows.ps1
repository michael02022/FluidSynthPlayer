# Packs a Windows build of the CLAP plugin into a distribution zip:
#   dist/FluidSynthPlayer-<version>-windows-x64.zip
#     FluidSynthPlayer.clap, README.txt, LICENSE.txt, licenses/...
#
# Build first (see README.md, "Windows"), then from the repository root:
#   powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1 [-BuildDir build-win]
# A build without libsndfile is packaged too, but without SF3 support (the
# README.txt in the zip says so and a warning is printed).
param(
    [string]$BuildDir = "build-win",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# ---- inputs ----
$clap = @("$BuildDir\FluidSynthPlayer.clap", "$BuildDir\Release\FluidSynthPlayer.clap") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $clap) { throw "FluidSynthPlayer.clap not found in $BuildDir - build it first." }

$cmake = Get-Content CMakeLists.txt -Raw
$version = [regex]::Match($cmake, 'project\(FluidSynthPlayer VERSION ([0-9.]+)').Groups[1].Value
$commit = (git rev-parse --short HEAD).Trim()
$dirty = if (git status --porcelain -- src external CMakeLists.txt cmake) { " (with local changes)" } else { "" }
# Written by CMakeLists.txt at configure time: was libsndfile (SF3) found?
$info = "$BuildDir\fsp_build_info.txt"
if (-not (Test-Path $info)) { throw "$info not found - reconfigure the build with CMake." }
$hasSf3 = (Get-Content $info -Raw) -match "sf3=ON"
if (-not $hasSf3) { Write-Warning "$BuildDir was built without libsndfile: the package will have NO SF3 support." }

$name = "FluidSynthPlayer-$version-windows-x64"
$stage = Join-Path $OutDir $name
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null

# ---- plugin (debug info stripped: MinGW builds carry it from static libs) ----
Copy-Item $clap "$stage\FluidSynthPlayer.clap"
$strip = Get-Command strip -ErrorAction SilentlyContinue
if ($strip) { & $strip.Source --strip-unneeded "$stage\FluidSynthPlayer.clap" }

# ---- licenses ----
Copy-Item LICENSE.txt "$stage\LICENSE.txt"
$licenses = [ordered]@{
    "FluidSynth-LGPL-2.1.txt"           = "external\fluidsynth\LICENSE"
    "gcem-Apache-2.0.txt"               = "external\fluidsynth\gcem\LICENSE"
    "Signalsmith-audio-basics-MIT.txt"  = "external\fluidsynth\signalsmith-audio-basics\LICENSE.txt"
    "Signalsmith-dsp-MIT.txt"           = "external\signalsmith\dsp\LICENSE.txt"
    "Signalsmith-linear-MIT.txt"        = "external\signalsmith\linear\LICENSE.txt"
    "Signalsmith-hilbert-iir-0BSD.txt"  = "external\signalsmith\hilbert-iir\LICENSE.txt"
    "CLAP-MIT.txt"                      = "external\clap\LICENSE"
    "DearImGui-MIT.txt"                 = "external\imgui\LICENSE.txt"
    "FontAwesome-OFL-1.1.txt"           = "external\fontawesome\LICENSE-OFL.txt"
    "Roboto-Apache-2.0.txt"             = "external\roboto\LICENSE.txt"
}
foreach ($k in $licenses.Keys) { Copy-Item $licenses[$k] "$stage\licenses\$k" }

# libsndfile and its codecs, statically linked when SF3 is supported.
$sndfileNotice = ""
if ($hasSf3) {
    $share = [regex]::Match((Get-Content $info -Raw), "vcpkg_share=(.+)").Groups[1].Value.Trim()
    $codecs = [ordered]@{
        "libsndfile-LGPL-2.1.txt"   = "libsndfile"
        "libogg-BSD-3-Clause.txt"   = "libogg"
        "libvorbis-BSD-3-Clause.txt" = "libvorbis"
        "libFLAC-BSD-3-Clause.txt"  = "libflac"
        "opus-BSD-3-Clause.txt"     = "opus"
    }
    if ($share -and (Test-Path "$share\libsndfile\copyright")) {
        foreach ($k in $codecs.Keys) {
            $src = "$share\$($codecs[$k])\copyright"
            if (Test-Path $src) { Copy-Item $src "$stage\licenses\$k" }
        }
    } else {
        Write-Warning ("libsndfile was not installed by vcpkg: add the licenses of libsndfile " +
                       "and of the codecs it was built with to $stage\licenses by hand.")
    }
    $sndfileNotice = "`n`n" + @"
SF3 decoding uses libsndfile (LGPL 2.1) with libogg, libvorbis, libFLAC
and Opus (BSD 3-clause), also linked statically. libsndfile is the
unmodified upstream release (https://github.com/libsndfile/libsndfile);
rebuilding the plugin as described in the repository's README links it
against any other libsndfile build.
"@
}

# ---- README.txt ----
$sf3Line = if ($hasSf3) {
    "Formats: SF2, SF3 (Ogg/Vorbis compressed) and DLS SoundFonts."
} else {
    "Formats: SF2 and DLS SoundFonts. This build has NO SF3 support (it was`r`n  built without libsndfile); SF3 files will not load."
}
$readme = @"
FluidSynth Player $version - CLAP plugin for Windows (x64)
=========================================================

SoundFont player instrument based on FluidSynth 2.6 (statically linked),
with an ImGui interface. $sf3Line

Built from https://github.com/michael02022/FluidSynthPlayer
commit $commit$dirty.

Requirements
------------
- Windows 10 or 11, 64-bit
- A DAW / host with CLAP support (e.g. REAPER, Bitwig Studio, FL Studio)
- OpenGL 1.1 (any graphics driver)
No other DLLs or runtimes are needed: FluidSynthPlayer.clap is self-contained.

Installation
------------
Copy FluidSynthPlayer.clap into one of the standard CLAP folders, then let
the DAW rescan its plugins:

  All users (needs administrator rights):
    C:\Program Files\Common Files\CLAP\

  Current user only:
    %LOCALAPPDATA%\Programs\Common\CLAP\

Close the DAW before replacing an older version (a loaded plugin file is
locked by Windows).

Usage notes
-----------
- Load a SoundFont with the folder button or by dropping a file from
  Explorer onto the plugin window.
- The pencil button opens the SoundFont in an external editor: set it in
  Options > Advanced (full path to the .exe, e.g. Polyphone).
- Global preferences (default SoundFont, last folder, external editor) are
  stored in %APPDATA%\FluidSynthPlayer\config.txt.

License
-------
FluidSynth Player is MIT licensed (LICENSE.txt). It contains third-party
components under their own licenses (see the licenses folder):
  FluidSynth (LGPL 2.1), gcem (Apache 2.0), Signalsmith libraries (MIT,
  0BSD), CLAP (MIT), Dear ImGui (MIT), Font Awesome 4 font (SIL OFL 1.1),
  Roboto font (Apache 2.0).

FluidSynth is linked statically. As required by the LGPL, the complete
source code - including FluidSynth (external/fluidsynth) and the build
scripts needed to rebuild/relink the plugin against a modified FluidSynth -
is available at the repository and commit given above.$sndfileNotice
"@
# CRLF line endings for Notepad users.
[System.IO.File]::WriteAllText((Join-Path (Resolve-Path $stage) "README.txt"), ($readme -replace "`r?`n", "`r`n"))

# ---- zip (Windows' bsdtar writes standard zips with "/" separators) ----
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $OutDir $name
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
Write-Host "Created $zip (SF3 support: $hasSf3)"
