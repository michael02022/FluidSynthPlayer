// Small OS abstraction layer for the CLAP build (Linux and Windows).
//
// Path convention: every path the plugin stores or passes around (state,
// config file, GUI, FluidSynth) is a UTF-8 std::string. On Linux that is
// what the file system uses anyway; on Windows std::filesystem::path(string)
// would read it in the ANSI code page instead, so all conversions go through
// u8path()/u8string() below. FluidSynth itself accepts UTF-8 paths on
// Windows (fluid_fopen converts them to UTF-16 for _wfopen).
#pragma once

#include <filesystem>
#include <string>

namespace platform {

// UTF-8 string <-> std::filesystem::path, lossless on every platform.
inline std::filesystem::path u8path(const std::string& utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

inline std::string u8string(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// Shorthands for the frequent "show just the file name / its folder" cases.
inline std::string fileName(const std::string& utf8Path) {
    return u8string(u8path(utf8Path).filename());
}
inline std::string parentDir(const std::string& utf8Path) {
    return u8string(u8path(utf8Path).parent_path());
}

// Case-insensitive ASCII comparisons (strcasecmp/strncasecmp aren't
// available with MSVC).
int strCaseCmp(const char* a, const char* b);
int strNCaseCmp(const char* a, const char* b, size_t n);

// Folder holding the plugin's global config (FluidSynthPlayer/config.txt
// lives below it): $XDG_CONFIG_HOME if set, else ~/.config on Linux and
// %APPDATA% on Windows.
std::filesystem::path userConfigDir();

// The user's home folder (UTF-8), "" if unknown.
std::string userHomeDir();

// Where the "Open SoundFont" dialog starts when nothing better is known:
// the system SoundFont folder on Linux (/usr/share/sounds/sf2) if present,
// else the home folder. Always ends with a separator.
std::string defaultSoundFontDir();

// Starts `program` with `argument` as its only argument, fully detached
// from the host (it may outlive the DAW). `program` may be a bare name
// looked up in PATH. Returns false if it couldn't be started (on Linux only
// fork() failures are detected; a missing executable fails silently in the
// detached child).
bool launchDetached(const std::string& program, const std::string& argument);

} // namespace platform
