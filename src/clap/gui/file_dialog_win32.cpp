// Windows implementation of file_dialog.h: the standard Open/Save dialogs
// (comdlg32) and the common item dialog in folder mode. Runs on the GUI
// thread (the host's main thread on Windows, see gui_window_win32.cpp); the
// dialog is modal to the top-level window hosting the plugin editor.
#include "file_dialog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

#include <filesystem>
#include <vector>

#include "gui_window.hpp"

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

HWND ownerWindow() {
    HWND w = static_cast<HWND>(guiCurrentNativeWindow());
    return w ? GetAncestor(w, GA_ROOT) : nullptr;
}

// Splits defaultPath into (initial folder, preselected file name): a path
// ending in a separator or naming an existing folder is a folder.
void splitDefaultPath(const std::string& defaultPath, std::wstring& dir, std::wstring& file) {
    std::wstring p = widen(defaultPath);
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    std::error_code ec;
    if (p.empty()) return;
    if (p.back() == L'\\' || std::filesystem::is_directory(std::filesystem::path(p), ec)) {
        dir = p;
        return;
    }
    const size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        file = p;
    } else {
        dir = p.substr(0, slash + 1);
        file = p.substr(slash + 1);
    }
}

// "Name\0*.a;*.b\0...\0\0" from the zenity-style space-separated globs.
std::wstring filterString(const std::vector<FileDialogFilter>& filters) {
    std::wstring out;
    for (const auto& f : filters) {
        std::wstring pattern = widen(f.pattern);
        for (auto& c : pattern)
            if (c == L' ') c = L';';
        if (pattern == L"*") pattern = L"*.*";
        out += widen(f.name);
        out.push_back(L'\0');
        out += pattern;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

std::string runFileDialog(bool save, const std::string& title, const std::string& defaultPath,
                          const std::vector<FileDialogFilter>& filters) {
    std::wstring dir, file;
    splitDefaultPath(defaultPath, dir, file);
    const std::wstring wTitle = widen(title);
    const std::wstring wFilter = filterString(filters);

    std::vector<wchar_t> buf(32768, L'\0');
    if (!file.empty() && file.size() < buf.size()) std::copy(file.begin(), file.end(), buf.begin());

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ownerWindow();
    ofn.lpstrFilter = filters.empty() ? nullptr : wFilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = wTitle.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY |
                (save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? narrow(buf.data()) : std::string();
}

} // namespace

std::string saveFileDialog(const std::string& title, const std::string& defaultPath,
                           const std::vector<FileDialogFilter>& filters) {
    return runFileDialog(true, title, defaultPath, filters);
}

std::string openFileDialog(const std::string& title, const std::string& defaultPath,
                           const std::vector<FileDialogFilter>& filters) {
    return runFileDialog(false, title, defaultPath, filters);
}

std::string selectFolderDialog(const std::string& title, const std::string& defaultPath) {
    // The common item dialog needs COM (STA). The host has normally set it up
    // on its main thread already; S_FALSE / RPC_E_CHANGED_MODE are fine.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
        dlg->SetTitle(widen(title).c_str());
        std::wstring dir, file;
        splitDefaultPath(defaultPath, dir, file);
        if (!dir.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_IShellItem,
                                                      reinterpret_cast<void**>(&folder)))) {
                dlg->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(ownerWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = narrow(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(coInit)) CoUninitialize();
    return result;
}
