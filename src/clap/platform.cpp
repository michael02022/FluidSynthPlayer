#include "platform.hpp"

#include <cctype>
#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace platform {

int strCaseCmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const int ca = std::tolower(static_cast<unsigned char>(*a));
        const int cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb || ca == 0) return ca - cb;
    }
}

int strNCaseCmp(const char* a, const char* b, size_t n) {
    for (; n > 0; --n, ++a, ++b) {
        const int ca = std::tolower(static_cast<unsigned char>(*a));
        const int cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb || ca == 0) return ca - cb;
    }
    return 0;
}

#ifdef _WIN32

namespace {

// Environment variable as a path, read from the process environment itself
// (a statically linked C runtime keeps its own getenv() snapshot, taken
// when the .clap was loaded) through the UTF-16 API, so non-ANSI user names
// work.
fs::path envPath(const wchar_t* name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return n > 0 && n < 32768 ? fs::path(std::wstring(buf, n)) : fs::path();
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

} // namespace

fs::path userConfigDir() {
    fs::path p = envPath(L"XDG_CONFIG_HOME");
    if (p.empty()) p = envPath(L"APPDATA");
    if (p.empty()) p = envPath(L"USERPROFILE");
    if (p.empty()) p = fs::temp_directory_path();
    return p;
}

std::string userHomeDir() { return u8string(envPath(L"USERPROFILE")); }

std::string defaultSoundFontDir() {
    std::string home = userHomeDir();
    return home.empty() ? std::string() : home + "\\";
}

bool launchDetached(const std::string& program, const std::string& argument) {
    // ShellExecute (unlike CreateProcess) also resolves programs registered
    // under "App Paths", the usual way installers make e.g. "polyphone"
    // launchable by name.
    const std::wstring file = widen(program);
    const std::wstring params = L"\"" + widen(argument) + L"\"";
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", file.c_str(), params.c_str(), nullptr,
                                      SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

#else // POSIX

fs::path userConfigDir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg);
    const char* home = getenv("HOME");
    return fs::path(home ? home : "/tmp") / ".config";
}

std::string userHomeDir() {
    const char* home = getenv("HOME");
    return home ? home : "";
}

std::string defaultSoundFontDir() {
    std::error_code ec;
    if (fs::is_directory("/usr/share/sounds/sf2", ec)) return "/usr/share/sounds/sf2/";
    return userHomeDir() + "/";
}

// Double fork so no zombie is left behind and the program outlives the
// plugin/DAW if the user wants.
bool launchDetached(const std::string& program, const std::string& argument) {
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            execlp(program.c_str(), program.c_str(), argument.c_str(),
                   static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    if (pid < 0) return false;
    waitpid(pid, nullptr, 0);
    return true;
}

#endif

} // namespace platform
