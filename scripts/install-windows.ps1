# Installs FluidSynthPlayer (CLAP and VST3) on Windows into the standard
# plugin folders:
#   CLAP: C:\Program Files\Common Files\CLAP\FluidSynthPlayer.clap
#   VST3: C:\Program Files\Common Files\VST3\FluidSynthPlayer.vst3\  (bundle)
# or, with -CurrentUser (no administrator rights needed):
#   CLAP: %LOCALAPPDATA%\Programs\Common\CLAP\
#   VST3: %LOCALAPPDATA%\Programs\Common\VST3\
#
# Source: -Source <folder with CLAP\ and VST3\>, else the folder this script
# is in (an extracted distribution zip), else the newest
# dist\FluidSynthPlayer-*-windows-x64 folder of the repository (made by
# scripts\package-windows.ps1).
#
#   powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1 [-CurrentUser] [-Source <dir>]
param(
    [string]$Source = "",
    [switch]$CurrentUser,
    # Internal: keeps the elevated window open at the end so the result
    # can be read.
    [switch]$Pause
)
$ErrorActionPreference = "Stop"

# ---- find what to install ----
function Test-PluginFolder([string]$dir) {
    $dir -and ((Test-Path "$dir\CLAP\FluidSynthPlayer.clap") -or
               (Test-Path "$dir\VST3\FluidSynthPlayer.vst3"))
}
if (-not $Source) {
    if (Test-PluginFolder $PSScriptRoot) {
        $Source = $PSScriptRoot
    } else {
        $dist = Join-Path (Split-Path -Parent $PSScriptRoot) "dist"
        $Source = Get-ChildItem $dist -Directory -Filter "FluidSynthPlayer-*-windows-x64" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not (Test-PluginFolder $Source)) {
    throw "No CLAP\ or VST3\ folder found (Source: '$Source'). Run scripts\package-windows.ps1 first, or pass -Source."
}
$Source = (Resolve-Path $Source).Path

# ---- system-wide install needs administrator rights: re-launch elevated ----
if (-not $CurrentUser) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
        IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        Write-Host "Administrator rights are needed for C:\Program Files - asking for them..."
        $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
                     "-Source", "`"$Source`"", "-Pause")
        $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
        exit $p.ExitCode
    }
}

if ($CurrentUser) {
    $clapDir = Join-Path $env:LOCALAPPDATA "Programs\Common\CLAP"
    $vst3Dir = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3"
} else {
    $clapDir = Join-Path $env:CommonProgramFiles "CLAP"
    $vst3Dir = Join-Path $env:CommonProgramFiles "VST3"
}

$failed = $false
function Install-Step([string]$what, [scriptblock]$action) {
    try {
        & $action
        Write-Host "  ok   $what"
    } catch {
        Write-Host "  FAIL $what" -ForegroundColor Red
        Write-Host "       $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "       (is the DAW still running? close it and try again)" -ForegroundColor Red
        $script:failed = $true
    }
}

Write-Host "Installing FluidSynthPlayer from $Source"
if (Test-Path "$Source\CLAP\FluidSynthPlayer.clap") {
    Install-Step "CLAP -> $clapDir\FluidSynthPlayer.clap" {
        New-Item -ItemType Directory -Force $clapDir | Out-Null
        Copy-Item "$Source\CLAP\FluidSynthPlayer.clap" "$clapDir\FluidSynthPlayer.clap" -Force
    }
}
if (Test-Path "$Source\VST3\FluidSynthPlayer.vst3") {
    Install-Step "VST3 -> $vst3Dir\FluidSynthPlayer.vst3" {
        New-Item -ItemType Directory -Force $vst3Dir | Out-Null
        # Replace the whole bundle so no file of an older version is left.
        $target = "$vst3Dir\FluidSynthPlayer.vst3"
        if (Test-Path $target) { Remove-Item -Recurse -Force $target }
        Copy-Item -Recurse "$Source\VST3\FluidSynthPlayer.vst3" $target
    }
}

if ($failed) {
    Write-Host "Installation incomplete." -ForegroundColor Red
} else {
    Write-Host "Done. Rescan the plugins in your DAW."
}
if ($Pause) { Read-Host "Press Enter to close" | Out-Null }
exit ([int]$failed)
