// Native OS file dialogs via zenity(1) - used by the top bar's "Open..."
// SoundFont button and the external-editor picker, complementing the
// embedded ImGui file browser (file_explorer.hpp). Ported verbatim from
// SfzDrummer (sibling project). GUI thread only: every function blocks
// until the user closes the dialog, same as any native modal dialog would.
#pragma once

#include <string>
#include <vector>

struct ZenityFilter {
    std::string name;    // shown in the dialog's filter dropdown
    std::string pattern; // e.g. "*.sf2", or several space-separated globs
};

// Returns the chosen path, or "" if the user cancelled, zenity isn't
// installed, or the call otherwise failed - callers should treat "" as
// "nothing to do", not as an error to report.
std::string zenitySaveFile(const std::string& title, const std::string& defaultPath,
                           const std::vector<ZenityFilter>& filters);
std::string zenityOpenFile(const std::string& title, const std::string& defaultPath,
                           const std::vector<ZenityFilter>& filters);

// Folder picker - same shape/blocking
// behavior as zenityOpenFile, just directory-only (--directory), no
// filters (folders don't have file-type filters).
std::string zenitySelectFolder(const std::string& title, const std::string& defaultPath);
