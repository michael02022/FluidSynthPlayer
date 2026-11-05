// Native OS file dialogs - used by the top bar's "Open..." SoundFont button
// and the default-SoundFont picker. Two implementations:
//   - Linux: zenity(1) (file_dialog_zenity.cpp, ported from SfzDrummer, a
//     sibling project)
//   - Windows: the common item dialog (file_dialog_win32.cpp)
// GUI thread only: every function blocks until the user closes the dialog,
// same as any native modal dialog would. Paths are UTF-8 on both platforms.
#pragma once

#include <string>
#include <vector>

struct FileDialogFilter {
    std::string name;    // shown in the dialog's filter dropdown
    std::string pattern; // e.g. "*.sf2", or several space-separated globs
};

// Returns the chosen path, or "" if the user cancelled, the dialog tool
// isn't available (zenity not installed) or the call otherwise failed -
// callers should treat "" as "nothing to do", not as an error to report.
// defaultPath may be a file (preselected) or a folder ending in a separator.
std::string saveFileDialog(const std::string& title, const std::string& defaultPath,
                           const std::vector<FileDialogFilter>& filters);
std::string openFileDialog(const std::string& title, const std::string& defaultPath,
                           const std::vector<FileDialogFilter>& filters);

// Folder picker - same shape/blocking behavior as openFileDialog, just
// directory-only, no filters (folders don't have file-type filters).
std::string selectFolderDialog(const std::string& title, const std::string& defaultPath);
