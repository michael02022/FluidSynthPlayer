// Plugin editor window: ImGui over OpenGL 2, embeddable via
// clap.gui/set_parent. One implementation per platform, same interface:
//
//  - Linux (gui_window_x11.cpp): X11 + GLX with its own GUI thread per
//    plugin instance; all of the instance's X11 traffic happens on its own
//    Display, on its own thread. Implements XDND as a drag & drop target
//    for files. Ported verbatim (aside from naming) from SfzDrummer/
//    NeoLooper (sibling projects): this window-level XDND handling (one
//    hand-written X11 event pump, not a Component-tree hit-test system) is
//    what structurally avoids the JUCE/Reaper drag-and-drop bug that
//    motivated dropping JUCE for the CLAP build of this plugin.
//
//  - Windows (gui_window_win32.cpp): Win32 + WGL. The window lives on the
//    host's main thread (the thread calling clap.gui) and is redrawn from a
//    WM_TIMER: a child HWND owned by another thread than its parent shares
//    the parent's input queue and can deadlock the host (e.g. on destroy),
//    so Windows plugins conventionally stay on the UI thread. Files are
//    accepted via WM_DROPFILES; ImGui's own Win32 clipboard code is used.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ImGuiContext;

class GuiWindow {
public:
    // drawUI: builds the ImGui interface (called every frame, GUI thread).
    // onFileDrop: called once per drop gesture with every file path it
    // carried (GUI thread, UTF-8 paths) - a single-file drop still calls it
    // with a one-element vector, so the callback always sees exactly what
    // the user dropped together in one go, never split into several calls.
    GuiWindow(std::function<void()> drawUI,
              std::function<void(const std::vector<std::string>&)> onFileDrop);
    ~GuiWindow();

    bool create(uint32_t w, uint32_t h);
    void destroy();

    // Callable from the host's main thread. nativeParent is the X11 Window
    // id on Linux, the parent HWND on Windows.
    void setParent(uintptr_t nativeParent);
    void setVisible(bool visible);
    void setSize(uint32_t w, uint32_t h);

    void getSize(uint32_t& w, uint32_t& h) const {
        w = width_.load();
        h = height_.load();
    }

private:
    std::function<void()> drawUI_;
    std::function<void(const std::vector<std::string>&)> onFileDrop_;

    std::atomic<bool> running_{false};
    std::atomic<uint32_t> width_{560}, height_{680};

#ifdef _WIN32
    // HWND, GL and ImGui contexts plus the window procedure; a nested type
    // so its window procedure may reach the members above.
    struct Win32State;
    Win32State* win32_ = nullptr;
    bool createNativeWindow(uintptr_t parent);
#else
    void threadMain();

    std::thread thread_;
    std::atomic<bool> quit_{false};

    // command mailbox, host -> GUI thread
    std::mutex cmdMutex_;
    unsigned long pendingParent_ = 0;
    int pendingVisible_ = -1; // -1 nothing, 0 hide, 1 show
    uint32_t pendingW_ = 0, pendingH_ = 0;
    bool hasPendingSize_ = false;
#endif
};

// The native window (HWND on Windows) of the editor whose frame is being
// drawn right now on the calling thread, or nullptr - lets modal OS dialogs
// opened from the UI pick the right owner window. Always nullptr on Linux.
void* guiCurrentNativeWindow();
