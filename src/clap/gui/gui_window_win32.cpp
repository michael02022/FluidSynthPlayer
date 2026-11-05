// Windows implementation of gui_window.hpp: Win32 + WGL (OpenGL 2) + ImGui.
//
// Everything runs on the thread that calls clap.gui (the host's main/UI
// thread): the window is a child of the host's HWND, redrawn ~60 times per
// second from a WM_TIMER. Several plugin instances (and other plugins) share
// that thread, so every entry point that touches ImGui or OpenGL switches to
// this editor's contexts and restores whatever was current before - also
// needed because a modal dialog opened from the UI (file_dialog_win32.cpp)
// runs a nested message loop in which other editors keep rendering.
#include "gui_window.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <GL/gl.h>

#include <cfloat>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_opengl2.h"

#include "imgui_setup.hpp"

namespace {

constexpr UINT_PTR kFrameTimerId = 1;
constexpr UINT kFrameIntervalMs = 16; // ~60 fps

// Editor whose frame is being drawn right now (see guiCurrentNativeWindow).
HWND g_currentFrameWindow = nullptr;

// ------------------------------------------------------------ window class
//
// Registered by the first editor window and unregistered with the last one:
// a DLL's window classes outlive it otherwise, and a re-loaded .clap would
// find the class still pointing at the unloaded module's window procedure.

int g_windowCount = 0;

HINSTANCE thisModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &m);
    return m;
}

// Unique per loaded module, so another plugin built from the same sources
// (or a second copy of this one) never collides with our class.
const wchar_t* windowClassName() {
    static wchar_t name[64] = {};
    if (!name[0])
        swprintf(name, 64, L"FluidSynthPlayerEditor_%p", static_cast<void*>(thisModule()));
    return name;
}

// ------------------------------------------------------------------ input

ImGuiKey vkToImGuiKey(WPARAM vk, LPARAM lParam) {
    // The numpad Enter is VK_RETURN with the extended-key flag.
    if (vk == VK_RETURN && (HIWORD(lParam) & KF_EXTENDED)) return ImGuiKey_KeypadEnter;
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_OEM_7: return ImGuiKey_Apostrophe;
        case VK_OEM_COMMA: return ImGuiKey_Comma;
        case VK_OEM_MINUS: return ImGuiKey_Minus;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_2: return ImGuiKey_Slash;
        case VK_OEM_1: return ImGuiKey_Semicolon;
        case VK_OEM_PLUS: return ImGuiKey_Equal;
        case VK_OEM_4: return ImGuiKey_LeftBracket;
        case VK_OEM_5: return ImGuiKey_Backslash;
        case VK_OEM_6: return ImGuiKey_RightBracket;
        case VK_OEM_3: return ImGuiKey_GraveAccent;
        case VK_CAPITAL: return ImGuiKey_CapsLock;
        case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
        case VK_DIVIDE: return ImGuiKey_KeypadDivide;
        case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case VK_ADD: return ImGuiKey_KeypadAdd;
        case VK_LSHIFT: return ImGuiKey_LeftShift;
        case VK_RSHIFT: return ImGuiKey_RightShift;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_LMENU: return ImGuiKey_LeftAlt;
        case VK_RMENU: return ImGuiKey_RightAlt;
        case VK_LWIN: return ImGuiKey_LeftSuper;
        case VK_RWIN: return ImGuiKey_RightSuper;
        default: break;
    }
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return (ImGuiKey)(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    return ImGuiKey_None;
}

// Shift/Ctrl/Alt arrive as VK_SHIFT/VK_CONTROL/VK_MENU: tell left from right.
WPARAM sidedModifierVk(WPARAM vk, LPARAM lParam) {
    const bool extended = (HIWORD(lParam) & KF_EXTENDED) != 0;
    switch (vk) {
        case VK_SHIFT:
            return MapVirtualKeyW((lParam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT
                       ? VK_RSHIFT
                       : VK_LSHIFT;
        case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
        case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
        default: return vk;
    }
}

LPCWSTR win32CursorFor(ImGuiMouseCursor c) {
    switch (c) {
        case ImGuiMouseCursor_TextInput: return IDC_IBEAM;
        case ImGuiMouseCursor_ResizeAll: return IDC_SIZEALL;
        case ImGuiMouseCursor_ResizeEW: return IDC_SIZEWE;
        case ImGuiMouseCursor_ResizeNS: return IDC_SIZENS;
        case ImGuiMouseCursor_ResizeNESW: return IDC_SIZENESW;
        case ImGuiMouseCursor_ResizeNWSE: return IDC_SIZENWSE;
        case ImGuiMouseCursor_Hand: return IDC_HAND;
        case ImGuiMouseCursor_NotAllowed: return IDC_NO;
        default: return IDC_ARROW;
    }
}

std::string narrow(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

void* guiCurrentNativeWindow() { return g_currentFrameWindow; }

// ------------------------------------------------------------- Win32State

struct GuiWindow::Win32State {
    GuiWindow* owner = nullptr;
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC glrc = nullptr;
    ImGuiContext* imgui = nullptr;
    bool child = false;
    bool inFrame = false;      // re-entrancy guard (modal dialogs, WM_PAINT)
    bool trackingMouse = false;
    int buttonsDown = 0;       // bit per ImGui mouse button, for capture
    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);
    void render();
    void mouseButton(int button, bool down);
};

namespace {

// Makes `ctx` the current ImGui context for a scope, restoring the previous
// one afterwards (shared UI thread, see the file comment).
struct ImGuiContextScope {
    ImGuiContext* prev;
    explicit ImGuiContextScope(ImGuiContext* ctx) : prev(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(ctx);
    }
    ~ImGuiContextScope() { ImGui::SetCurrentContext(prev); }
};

// Same for the OpenGL context: the host (or another plugin) may render its
// own UI with OpenGL on this thread.
struct GlContextScope {
    HDC prevDc = wglGetCurrentDC();
    HGLRC prevRc = wglGetCurrentContext();
    GlContextScope(HDC dc, HGLRC rc) { wglMakeCurrent(dc, rc); }
    ~GlContextScope() { wglMakeCurrent(prevDc, prevRc); }
};

} // namespace

LRESULT CALLBACK GuiWindow::Win32State::wndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* st = reinterpret_cast<Win32State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (!st || !st->imgui) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return st->handle(msg, wParam, lParam);
}

void GuiWindow::Win32State::mouseButton(int button, bool down) {
    if (down) {
        // So keyboard shortcuts and text input get delivered to us.
        if (GetFocus() != hwnd) SetFocus(hwnd);
        if (buttonsDown == 0 && GetCapture() == nullptr) SetCapture(hwnd);
        buttonsDown |= 1 << button;
    } else {
        buttonsDown &= ~(1 << button);
        if (buttonsDown == 0 && GetCapture() == hwnd) ReleaseCapture();
    }
    ImGui::GetIO().AddMouseButtonEvent(button, down);
}

LRESULT GuiWindow::Win32State::handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    ImGuiContextScope scope(imgui);
    ImGuiIO& io = ImGui::GetIO();
    switch (msg) {
        case WM_TIMER:
            if (wParam == kFrameTimerId) render();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            render();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // OpenGL paints everything
        case WM_SIZE: {
            const uint32_t w = LOWORD(lParam), h = HIWORD(lParam);
            if (w > 0 && h > 0) {
                owner->width_.store(w);
                owner->height_.store(h);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                trackingMouse = true;
            }
            io.AddMousePosEvent((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse = false;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: mouseButton(0, true); return 0;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: mouseButton(1, true); return 0;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: mouseButton(2, true); return 0;
        case WM_LBUTTONUP: mouseButton(0, false); return 0;
        case WM_RBUTTONUP: mouseButton(1, false); return 0;
        case WM_MBUTTONUP: mouseButton(2, false); return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hwnd && buttonsDown) {
                for (int b = 0; b < 3; ++b)
                    if (buttonsDown & (1 << b)) io.AddMouseButtonEvent(b, false);
                buttonsDown = 0;
            }
            return 0;
        case WM_MOUSEWHEEL:
            io.AddMouseWheelEvent(0.f, (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA);
            return 0;
        case WM_MOUSEHWHEEL:
            io.AddMouseWheelEvent(-(float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA, 0.f);
            return 0;
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
            const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiMod_Super, ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0);
            const ImGuiKey k = vkToImGuiKey(sidedModifierVk(wParam, lParam), lParam);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
            // Alt+F4 and friends keep working for the host.
            if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            return 0;
        }
        case WM_CHAR:
            if (wParam > 0 && wParam < 0x10000) io.AddInputCharacterUTF16((unsigned short)wParam);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS; // arrows/Tab/Enter are ours too
        case WM_SETFOCUS:
            io.AddFocusEvent(true);
            return 0;
        case WM_KILLFOCUS:
            io.AddFocusEvent(false);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                const ImGuiMouseCursor c = ImGui::GetMouseCursor();
                SetCursor(c == ImGuiMouseCursor_None ? nullptr
                                                     : LoadCursorW(nullptr, win32CursorFor(c)));
                return TRUE;
            }
            break;
        case WM_DROPFILES: {
            auto drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> paths;
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring buf(len + 1, L'\0');
                DragQueryFileW(drop, i, buf.data(), len + 1);
                std::string path = narrow(buf.c_str());
                if (!path.empty()) paths.push_back(std::move(path));
            }
            DragFinish(drop);
            if (!paths.empty() && owner->onFileDrop_) owner->onFileDrop_(paths);
            return 0;
        }
        case WM_CLOSE:
            // Only reachable for the parent-less (floating/test) window: the
            // host owns the editor's lifetime, closing just hides it.
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void GuiWindow::Win32State::render() {
    if (inFrame || !imgui || !IsWindowVisible(hwnd)) return;
    inFrame = true;
    HWND prevFrameWindow = g_currentFrameWindow;
    g_currentFrameWindow = hwnd;
    ImGuiContextScope scope(imgui);
    GlContextScope gl(hdc, glrc);

    ImGuiIO& io = ImGui::GetIO();
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    io.DisplaySize = ImVec2((float)w, (float)h);
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - lastFrame).count();
    lastFrame = now;
    io.DeltaTime = dt > 0.f ? dt : 1.f / 60.f;

    ImGui_ImplOpenGL2_NewFrame();
    ImGui::NewFrame();
    if (owner->drawUI_) owner->drawUI_();
    ImGui::Render();

    // drawUI may have run a modal dialog whose message loop let other
    // editors render: make sure our GL context is the current one again.
    wglMakeCurrent(hdc, glrc);
    glViewport(0, 0, w, h);
    glClearColor(0.118f, 0.118f, 0.129f, 1.f); // = the editor's window color
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    SwapBuffers(hdc);

    g_currentFrameWindow = prevFrameWindow;
    inFrame = false;
}

// -------------------------------------------------------------- GuiWindow

GuiWindow::GuiWindow(std::function<void()> drawUI,
                     std::function<void(const std::vector<std::string>&)> onFileDrop)
    : drawUI_(std::move(drawUI)), onFileDrop_(std::move(onFileDrop)) {}

GuiWindow::~GuiWindow() { destroy(); }

bool GuiWindow::create(uint32_t w, uint32_t h) {
    if (running_.load()) return true;
    if (w > 0 && h > 0) {
        width_.store(w);
        height_.store(h);
    }
    win32_ = new Win32State;
    win32_->owner = this;
    running_.store(true);
    // The HWND itself is created by setParent() (embedded, the normal case)
    // or by the first setVisible(true) without a parent (floating).
    return true;
}

bool GuiWindow::createNativeWindow(uintptr_t parent) {
    Win32State* st = win32_;
    if (!st || st->hwnd) return st != nullptr;
    const HINSTANCE inst = thisModule();

    if (g_windowCount == 0) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC; // OpenGL wants a private DC
        wc.lpfnWndProc = &Win32State::wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = windowClassName();
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            fprintf(stderr, "[FluidSynthPlayer] RegisterClassEx failed (%lu)\n", GetLastError());
            return false;
        }
    }

    st->child = parent != 0;
    const DWORD style = (st->child ? WS_CHILD : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN |
                        WS_CLIPSIBLINGS;
    const DWORD exStyle = WS_EX_ACCEPTFILES | (st->child ? WS_EX_NOPARENTNOTIFY : 0);
    RECT r{0, 0, (LONG)width_.load(), (LONG)height_.load()};
    if (!st->child) AdjustWindowRectEx(&r, style, FALSE, exStyle);
    st->hwnd = CreateWindowExW(exStyle, windowClassName(), L"FluidSynthPlayer", style,
                               st->child ? 0 : CW_USEDEFAULT, st->child ? 0 : CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top,
                               reinterpret_cast<HWND>(parent), nullptr, inst, st);
    if (!st->hwnd) {
        fprintf(stderr, "[FluidSynthPlayer] CreateWindowEx failed (%lu)\n", GetLastError());
        if (g_windowCount == 0) UnregisterClassW(windowClassName(), inst);
        return false;
    }
    ++g_windowCount;

    // -------- OpenGL --------
    st->hdc = GetDC(st->hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int format = ChoosePixelFormat(st->hdc, &pfd);
    if (!format || !SetPixelFormat(st->hdc, format, &pfd) ||
        !(st->glrc = wglCreateContext(st->hdc))) {
        fprintf(stderr, "[FluidSynthPlayer] could not create an OpenGL context\n");
        return true; // the window exists (and gets destroyed normally) but stays blank
    }

    GlContextScope gl(st->hdc, st->glrc);
    // no vsync: the timer sets the pace (the swap must not block the host's
    // UI thread)
    using SwapIntervalFn = BOOL(WINAPI*)(int);
    if (auto f = reinterpret_cast<SwapIntervalFn>(
            reinterpret_cast<void*>(wglGetProcAddress("wglSwapIntervalEXT"))))
        f(0);

    // -------- ImGui --------
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        IMGUI_CHECKVERSION();
        st->imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(st->imgui);
        setupEditorImGui();
        ImGui::GetIO().DisplaySize = ImVec2((float)width_.load(), (float)height_.load());
        // Clipboard: ImGui's built-in Win32 implementation (imgui.cpp) is
        // already the real system clipboard - nothing to hook up here.
        ImGui_ImplOpenGL2_Init();
        ImGui::SetCurrentContext(prev);
    }

    SetTimer(st->hwnd, kFrameTimerId, kFrameIntervalMs, nullptr);
    return true;
}

void GuiWindow::destroy() {
    if (!running_.load()) return;
    Win32State* st = win32_;
    if (st && st->hwnd) {
        KillTimer(st->hwnd, kFrameTimerId);
        if (ImGuiContext* ctx = st->imgui) {
            st->imgui = nullptr; // the window procedure ignores us from now on
            ImGuiContext* prev = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(ctx);
            {
                GlContextScope gl(st->hdc, st->glrc); // frees the font texture
                ImGui_ImplOpenGL2_Shutdown();
            }
            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(prev == ctx ? nullptr : prev);
        }
        if (st->glrc) {
            if (wglGetCurrentContext() == st->glrc) wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(st->glrc);
        }
        if (st->hdc) ReleaseDC(st->hwnd, st->hdc);
        DestroyWindow(st->hwnd);
        if (--g_windowCount == 0) UnregisterClassW(windowClassName(), thisModule());
    }
    delete st;
    win32_ = nullptr;
    running_.store(false);
}

void GuiWindow::setParent(uintptr_t parent) {
    if (!win32_ || !parent) return;
    if (!win32_->hwnd) {
        createNativeWindow(parent);
        return;
    }
    // Re-parenting an existing window (floating -> embedded or another
    // parent): switch it to a child window first.
    HWND hwnd = win32_->hwnd;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style = (style & ~(LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_POPUP)) | WS_CHILD;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetParent(hwnd, reinterpret_cast<HWND>(parent));
    win32_->child = true;
    SetWindowPos(hwnd, nullptr, 0, 0, (int)width_.load(), (int)height_.load(),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void GuiWindow::setVisible(bool visible) {
    if (!win32_) return;
    if (visible && !win32_->hwnd && !createNativeWindow(0)) return;
    if (win32_->hwnd) ShowWindow(win32_->hwnd, visible ? SW_SHOWNA : SW_HIDE);
}

void GuiWindow::setSize(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    width_.store(w);
    height_.store(h);
    if (!win32_ || !win32_->hwnd) return;
    RECT r{0, 0, (LONG)w, (LONG)h};
    if (!win32_->child)
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(win32_->hwnd, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLongPtrW(win32_->hwnd, GWL_EXSTYLE));
    SetWindowPos(win32_->hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
