// Linux implementation of gui_window.hpp: X11 + GLX, own GUI thread. Without
// a usable OpenGL context (remote X sessions) the editor is drawn in
// software instead (imgui_soft_renderer + XPutImage).
#include "gui_window.hpp"

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <sys/select.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_opengl2.h"

#include "imgui_setup.hpp"
#include "imgui_soft_renderer.hpp"

namespace {

// X errors on the plugin's own Display connections (one per editor, plus
// the GLX probe) are counted and ignored: a plugin can't let Xlib's default
// handler kill the host over a transient BadWindow (e.g. the parent going
// away before we do). The handler is process-wide, so errors on any other
// connection - the host's - go on to the handler that was there before.
std::mutex g_xErrMutex;
std::vector<std::pair<Display*, int>> g_ourDisplays; // display, error count
XErrorHandler g_prevXErrorHandler = nullptr;

int onXError(Display* d, XErrorEvent* e) {
    {
        std::lock_guard<std::mutex> lock(g_xErrMutex);
        for (auto& [ours, count] : g_ourDisplays)
            if (ours == d) {
                if (count++ < 8)
                    fprintf(stderr, "[FluidSynthPlayer] X11 error ignored: code=%d request=%d.%d\n",
                            (int)e->error_code, (int)e->request_code, (int)e->minor_code);
                return 0;
            }
    }
    return g_prevXErrorHandler ? g_prevXErrorHandler(d, e) : 0;
}

void installErrorHandlerOnce() {
    static std::once_flag once;
    std::call_once(once, [] { g_prevXErrorHandler = XSetErrorHandler(onXError); });
}

void registerDisplay(Display* d) {
    std::lock_guard<std::mutex> lock(g_xErrMutex);
    g_ourDisplays.emplace_back(d, 0);
}

void unregisterDisplay(Display* d) {
    std::lock_guard<std::mutex> lock(g_xErrMutex);
    g_ourDisplays.erase(std::remove_if(g_ourDisplays.begin(), g_ourDisplays.end(),
                                       [d](const auto& e) { return e.first == d; }),
                        g_ourDisplays.end());
}

int errorCount(Display* d) {
    std::lock_guard<std::mutex> lock(g_xErrMutex);
    for (const auto& [ours, count] : g_ourDisplays)
        if (ours == d) return count;
    return 0;
}

int kVisualAttrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                      GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, 0, None};

// Can this X display give the editor a direct (local) OpenGL context?
// Remote X sessions (ssh -X, VNC, xrdp, VcXsrv...) can't: no GLX at all, or
// only indirect GLX. X.Org refuses that by default since 1.20 (+iglx); where
// it is accepted (VcXsrv, LIBGL_ALWAYS_INDIRECT=1) it is slow and fragile -
// BadWindow errors, even crashed X servers. So anything but a direct context
// means software drawing: drawing with no usable context crashes some
// drivers, and the host with them. Checked once.
bool glxUsable(std::string& why) {
    static std::once_flag once;
    static bool usable = false;
    static std::string reason;
    std::call_once(once, [] {
        const char* indirect = getenv("LIBGL_ALWAYS_INDIRECT");
        if (indirect && *indirect && strcmp(indirect, "0") != 0) {
            // Don't even ask: indirect GLX requests alone can upset the server.
            reason = "LIBGL_ALWAYS_INDIRECT is set: indirect OpenGL only";
            return;
        }
        installErrorHandlerOnce();
        Display* dpy = XOpenDisplay(nullptr);
        if (!dpy) {
            reason = "could not open the X11 display";
            return;
        }
        registerDisplay(dpy);
        int errBase = 0, evBase = 0;
        if (!glXQueryExtension(dpy, &errBase, &evBase)) {
            reason = "the X server has no GLX (OpenGL) extension";
        } else if (XVisualInfo* vi = glXChooseVisual(dpy, DefaultScreen(dpy), kVisualAttrs)) {
            GLXContext ctx = glXCreateContext(dpy, vi, nullptr, True);
            XSync(dpy, False);
            if (!ctx) {
                reason = "the X server refused an OpenGL context (indirect GLX only?)";
            } else if (!glXIsDirect(dpy, ctx)) {
                reason = "only indirect OpenGL (remote display)";
                glXDestroyContext(dpy, ctx);
            } else {
                // Make it current on a small unmapped window: that's where
                // an unusable context shows up.
                const Window root = RootWindow(dpy, vi->screen);
                XSetWindowAttributes swa{};
                swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
                swa.border_pixel = 0;
                Window w = XCreateWindow(dpy, root, 0, 0, 16, 16, 0, vi->depth, InputOutput,
                                         vi->visual, CWColormap | CWBorderPixel, &swa);
                const int before = errorCount(dpy);
                usable = glXMakeCurrent(dpy, w, ctx) && glGetString(GL_VERSION) != nullptr;
                XSync(dpy, False);
                usable = usable && errorCount(dpy) == before;
                if (!usable) reason = "the OpenGL context could not be used";
                glXMakeCurrent(dpy, None, nullptr);
                glXDestroyContext(dpy, ctx);
                XDestroyWindow(dpy, w);
                XFreeColormap(dpy, swa.colormap);
            }
            XFree(vi);
        } else {
            reason = "no suitable GLX visual (24-bit RGB, double buffered)";
        }
        XSync(dpy, False);
        unregisterDisplay(dpy);
        XCloseDisplay(dpy);
    });
    why = reason;
    return usable;
}

// Software mode output: ImGui rendered into a 0x00RRGGBB framebuffer, and
// only the rectangle that changed since the last frame sent to the window -
// over a remote X link an idle editor costs nothing. Unused with OpenGL.
class SoftPresenter {
public:
    SoftPresenter(Display* dpy, Window win, Visual* visual, int depth)
        : dpy_(dpy), win_(win), visual_(visual), depth_(depth) {}
    ~SoftPresenter() { release(); }

    void invalidate() { full_ = true; } // e.g. Expose: resend everything

    void present(ImDrawData* dd, int w, int h) {
        if (w <= 0 || h <= 0) return;
        if (w != w_ || h != h_) resize(w, h);
        if (!img_) return;
        ImGui_SoftRenderer_RenderDrawData(dd, frame_.data(), w, h, kClearRgb);

        int x0 = 0, y0 = 0, x1 = w, y1 = h;
        if (!full_) { // bounding box of the pixels that changed
            x0 = w, y0 = h, x1 = -1, y1 = -1;
            for (int y = 0; y < h; ++y) {
                const uint32_t* a = &frame_[(size_t)y * w];
                const uint32_t* b = &shown_[(size_t)y * w];
                if (memcmp(a, b, (size_t)w * 4) == 0) continue;
                int l = 0, r = w - 1;
                while (a[l] == b[l]) ++l;
                while (a[r] == b[r]) --r;
                x0 = std::min(x0, l), x1 = std::max(x1, r + 1);
                y0 = std::min(y0, y), y1 = y + 1;
            }
            if (y1 < 0) return; // nothing changed
        }
        full_ = false;
        for (int y = y0; y < y1; ++y) {
            memcpy(&shown_[(size_t)y * w + x0], &frame_[(size_t)y * w + x0], size_t(x1 - x0) * 4);
            if (!direct_)
                for (int x = x0; x < x1; ++x) XPutPixel(img_, x, y, toPixel(shown_[(size_t)y * w + x]));
        }
        XPutImage(dpy_, win_, gc_, img_, x0, y0, x0, y0, unsigned(x1 - x0), unsigned(y1 - y0));
        XFlush(dpy_);
    }

    void release() {
        if (img_) {
            img_->data = nullptr; // the buffers are ours
            XDestroyImage(img_);
            img_ = nullptr;
        }
        if (gc_) {
            XFreeGC(dpy_, gc_);
            gc_ = nullptr;
        }
    }

private:
    static constexpr uint32_t kClearRgb = 0x1E1E21; // = the editor's window color

    void resize(int w, int h) {
        if (img_) {
            img_->data = nullptr;
            XDestroyImage(img_);
        }
        w_ = w, h_ = h;
        frame_.assign((size_t)w * h, 0);
        shown_.assign((size_t)w * h, 0);
        full_ = true;
        if (!gc_) gc_ = XCreateGC(dpy_, win_, 0, nullptr);
        img_ = XCreateImage(dpy_, visual_, depth_, ZPixmap, 0, nullptr, w, h, 32, 0);
        if (!img_) return;
        const uint16_t one = 1;
        const bool hostLsb = *(const uint8_t*)&one == 1;
        // The usual 32-bit 0x00RRGGBB layout: send our buffer as it is.
        direct_ = img_->bits_per_pixel == 32 && img_->bytes_per_line == w * 4 &&
                  img_->red_mask == 0xFF0000 && img_->green_mask == 0xFF00 &&
                  img_->blue_mask == 0xFF && (img_->byte_order == LSBFirst) == hostLsb;
        if (direct_) {
            img_->data = (char*)shown_.data();
        } else {
            converted_.assign((size_t)img_->bytes_per_line * h, 0);
            img_->data = converted_.data();
        }
    }

    // 0x00RRGGBB -> the visual's pixel value (any channel masks).
    unsigned long toPixel(uint32_t rgb) const {
        auto channel = [](uint32_t v8, unsigned long mask) -> unsigned long {
            if (!mask) return 0;
            int shift = 0, bits = 0;
            while (!((mask >> shift) & 1)) ++shift;
            while ((mask >> (shift + bits)) & 1) ++bits;
            const unsigned long v = bits >= 8 ? (unsigned long)v8 << (bits - 8) : v8 >> (8 - bits);
            return (v << shift) & mask;
        };
        return channel((rgb >> 16) & 0xFF, img_->red_mask) | channel((rgb >> 8) & 0xFF, img_->green_mask) |
               channel(rgb & 0xFF, img_->blue_mask);
    }

    Display* dpy_;
    Window win_;
    Visual* visual_;
    int depth_;
    GC gc_ = nullptr;
    XImage* img_ = nullptr;
    int w_ = 0, h_ = 0;
    bool full_ = true, direct_ = false;
    std::vector<uint32_t> frame_, shown_;
    std::vector<char> converted_;
};

ImGuiKey keysymToImGuiKey(KeySym k) {
    switch (k) {
        case XK_Tab: return ImGuiKey_Tab;
        case XK_Left: return ImGuiKey_LeftArrow;
        case XK_Right: return ImGuiKey_RightArrow;
        case XK_Up: return ImGuiKey_UpArrow;
        case XK_Down: return ImGuiKey_DownArrow;
        case XK_Page_Up: return ImGuiKey_PageUp;
        case XK_Page_Down: return ImGuiKey_PageDown;
        case XK_Home: return ImGuiKey_Home;
        case XK_End: return ImGuiKey_End;
        case XK_Insert: return ImGuiKey_Insert;
        case XK_Delete: case XK_KP_Delete: return ImGuiKey_Delete;
        case XK_BackSpace: return ImGuiKey_Backspace;
        case XK_space: return ImGuiKey_Space;
        case XK_Return: return ImGuiKey_Enter;
        case XK_KP_Enter: return ImGuiKey_KeypadEnter;
        case XK_Escape: return ImGuiKey_Escape;
        case XK_apostrophe: return ImGuiKey_Apostrophe;
        case XK_comma: return ImGuiKey_Comma;
        case XK_minus: return ImGuiKey_Minus;
        case XK_period: return ImGuiKey_Period;
        case XK_slash: return ImGuiKey_Slash;
        case XK_semicolon: return ImGuiKey_Semicolon;
        case XK_equal: return ImGuiKey_Equal;
        case XK_bracketleft: return ImGuiKey_LeftBracket;
        case XK_backslash: return ImGuiKey_Backslash;
        case XK_bracketright: return ImGuiKey_RightBracket;
        case XK_grave: return ImGuiKey_GraveAccent;
        case XK_Caps_Lock: return ImGuiKey_CapsLock;
        case XK_Shift_L: return ImGuiKey_LeftShift;
        case XK_Shift_R: return ImGuiKey_RightShift;
        case XK_Control_L: return ImGuiKey_LeftCtrl;
        case XK_Control_R: return ImGuiKey_RightCtrl;
        case XK_Alt_L: return ImGuiKey_LeftAlt;
        case XK_Alt_R: return ImGuiKey_RightAlt;
        case XK_Super_L: return ImGuiKey_LeftSuper;
        case XK_Super_R: return ImGuiKey_RightSuper;
        default: break;
    }
    if (k >= XK_0 && k <= XK_9)
        return (ImGuiKey)(ImGuiKey_0 + (k - XK_0));
    if (k >= XK_a && k <= XK_z)
        return (ImGuiKey)(ImGuiKey_A + (k - XK_a));
    if (k >= XK_A && k <= XK_Z)
        return (ImGuiKey)(ImGuiKey_A + (k - XK_A));
    if (k >= XK_F1 && k <= XK_F12)
        return (ImGuiKey)(ImGuiKey_F1 + (k - XK_F1));
    if (k >= XK_KP_0 && k <= XK_KP_9)
        return (ImGuiKey)(ImGuiKey_Keypad0 + (k - XK_KP_0));
    return ImGuiKey_None;
}

void latin1ToUtf8(const char* in, int n, char* out) {
    int o = 0;
    for (int i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            out[o++] = (char)c;
        } else {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

// Decodes a file:// URI (including percent-encoding).
std::string uriToPath(const std::string& uri) {
    std::string u = uri;
    if (u.rfind("file://", 0) == 0) {
        u = u.substr(7);
        size_t slash = u.find('/');
        if (slash != std::string::npos && slash > 0)
            u = u.substr(slash); // strips the optional hostname
    }
    std::string out;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '%' && i + 2 < u.size()) {
            char hex[3] = {u[i + 1], u[i + 2], 0};
            out.push_back((char)strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out.push_back(u[i]);
        }
    }
    return out;
}

// -------- CLIPBOARD (Ctrl+C/X/V) --------
//
// Dear ImGui's built-in fallback clipboard (imgui.cpp's non-Win32/Apple
// Platform_*ClipboardTextFn_DefaultImpl) only round-trips within the ImGui
// context itself - with no platform backend hooking it up, Ctrl+C/Ctrl+V
// never touch the X11 CLIPBOARD selection, so copy/paste can't cross
// process boundaries (e.g. pasting a FluidSynth setting copied from a text
// editor into the Advanced tab). This wires ImGuiPlatformIO's clipboard
// callbacks to a real ICCCM CLIPBOARD implementation instead: on copy/cut we
// become the selection owner and answer other clients' SelectionRequest
// events (handled in threadMain's event loop), and on paste we ask the
// current owner to convert its selection into our window's property and
// wait (bounded) for the reply.
//
// Large-clipboard caveat: the ICCCM INCR protocol (for transfers too big for
// a single property change) isn't implemented, so a paste from a
// multi-megabyte clipboard fails cleanly rather than hanging. Settings
// text never gets remotely that large.
struct ClipboardState {
    Display* dpy = nullptr;
    Window win = 0;
    Atom aClipboard = None;
    Atom aTargets = None;
    Atom aUtf8String = None;
    Atom aIncr = None;
    Atom aPasteProp = None;
    std::string ownedText;   // what we currently offer as CLIPBOARD owner
    std::string pasteBuffer; // last text fetched by GetClipboardText()
};

// Blocks (up to timeoutMs) for a SelectionNotify addressed to (win, prop).
// XCheckIfEvent only removes the event that matches the predicate from
// Xlib's queue, so any other event that arrives while we wait (motion,
// unrelated selections, etc.) is left in place for the normal event loop to
// process on its next pass - nothing gets dropped.
bool waitForSelectionNotify(Display* dpy, Window win, Atom prop, XEvent* out,
                            int timeoutMs) {
    struct Match { Window win; Atom prop; };
    Match m{win, prop};
    auto predicate = [](Display*, XEvent* ev, XPointer arg) -> Bool {
        auto* m = (Match*)arg;
        // A property of None is ICCCM's way of saying the owner refused the
        // conversion - still our answer, so it counts as a match too.
        return (ev->type == SelectionNotify && ev->xselection.requestor == m->win &&
               (ev->xselection.property == m->prop ||
                ev->xselection.property == None))
                   ? True
                   : False;
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (XCheckIfEvent(dpy, out, predicate, (XPointer)&m))
            return true;
        auto remain = deadline - std::chrono::steady_clock::now();
        if (remain <= std::chrono::steady_clock::duration::zero())
            return false;
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(remain).count();
        struct timeval tv;
        tv.tv_sec = (long)(us / 1000000);
        tv.tv_usec = (long)(us % 1000000);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(dpy), &fds);
        select(ConnectionNumber(dpy) + 1, &fds, nullptr, nullptr, &tv);
    }
}

const char* x11GetClipboardText(ImGuiContext*) {
    auto* cb = (ClipboardState*)ImGui::GetPlatformIO().Platform_ClipboardUserData;
    if (!cb || !cb->dpy) return nullptr;

    if (XGetSelectionOwner(cb->dpy, cb->aClipboard) == cb->win) {
        cb->pasteBuffer = cb->ownedText; // we own it: skip the round trip
        return cb->pasteBuffer.c_str();
    }

    XDeleteProperty(cb->dpy, cb->win, cb->aPasteProp);
    XConvertSelection(cb->dpy, cb->aClipboard, cb->aUtf8String, cb->aPasteProp, cb->win,
                      CurrentTime);
    XFlush(cb->dpy);

    XEvent ev;
    if (!waitForSelectionNotify(cb->dpy, cb->win, cb->aPasteProp, &ev, 200) ||
        ev.xselection.property == None)
        return nullptr; // no owner, unresponsive, or target refused

    Atom type;
    int fmt;
    unsigned long count, after;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(cb->dpy, cb->win, cb->aPasteProp, 0, 1 << 22, True,
                           AnyPropertyType, &type, &fmt, &count, &after,
                           &data) != Success || !data)
        return nullptr;
    if (type == cb->aIncr) { // large-transfer protocol, unsupported (see above)
        XFree(data);
        return nullptr;
    }

    cb->pasteBuffer.assign((char*)data, count);
    XFree(data);
    return cb->pasteBuffer.c_str();
}

void x11SetClipboardText(ImGuiContext*, const char* text) {
    auto* cb = (ClipboardState*)ImGui::GetPlatformIO().Platform_ClipboardUserData;
    if (!cb || !cb->dpy) return;
    cb->ownedText = text ? text : "";
    XSetSelectionOwner(cb->dpy, cb->aClipboard, cb->win, CurrentTime);
    XFlush(cb->dpy);
}

} // namespace

// Modal dialogs on Linux are separate zenity processes: no owner needed.
void* guiCurrentNativeWindow() { return nullptr; }

GuiWindow::GuiWindow(std::function<void()> drawUI,
                     std::function<void(const std::vector<std::string>&)> onFileDrop)
    : drawUI_(std::move(drawUI)), onFileDrop_(std::move(onFileDrop)) {}

GuiWindow::~GuiWindow() { destroy(); }

bool GuiWindow::create(uint32_t w, uint32_t h) {
    if (running_.load()) return true;
    std::string why;
    const char* forceSoft = getenv("FLUIDSYNTHPLAYER_SOFTWARE_RENDER");
    if (forceSoft && *forceSoft && strcmp(forceSoft, "0") != 0) {
        soft_ = true;
        fprintf(stderr, "[FluidSynthPlayer] drawing the editor in software "
                        "(FLUIDSYNTHPLAYER_SOFTWARE_RENDER)\n");
    } else if (!glxUsable(why)) {
        soft_ = true;
        fprintf(stderr, "[FluidSynthPlayer] no OpenGL for the editor (%s): drawing it in software\n",
                why.c_str());
    }
    if (w > 0 && h > 0) {
        width_.store(w);
        height_.store(h);
    }
    quit_.store(false);
    thread_ = std::thread([this] { threadMain(); });
    running_.store(true);
    return true;
}

void GuiWindow::destroy() {
    if (!running_.load()) return;
    quit_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void GuiWindow::setParent(uintptr_t x11Window) {
    std::lock_guard<std::mutex> l(cmdMutex_);
    pendingParent_ = static_cast<unsigned long>(x11Window);
}

void GuiWindow::setVisible(bool visible) {
    std::lock_guard<std::mutex> l(cmdMutex_);
    pendingVisible_ = visible ? 1 : 0;
}

void GuiWindow::setSize(uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> l(cmdMutex_);
    pendingW_ = w;
    pendingH_ = h;
    hasPendingSize_ = true;
    // The GUI thread applies it; get_size right after set_size must already
    // report the new size.
    width_.store(w);
    height_.store(h);
}

void GuiWindow::threadMain() {
    installErrorHandlerOnce();

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "[FluidSynthPlayer] could not open the X11 display\n");
        return;
    }
    registerDisplay(dpy);
    const int screen = DefaultScreen(dpy);

    const bool soft = soft_;
    XVisualInfo softVisual{};
    XVisualInfo* vi = soft ? (XMatchVisualInfo(dpy, screen, 24, TrueColor, &softVisual) ? &softVisual
                                                                                         : nullptr)
                           : glXChooseVisual(dpy, screen, kVisualAttrs);
    if (!vi) {
        fprintf(stderr, soft ? "[FluidSynthPlayer] no 24-bit TrueColor visual for the editor\n"
                             : "[FluidSynthPlayer] no GLX visual available\n");
        unregisterDisplay(dpy);
        XCloseDisplay(dpy);
        return;
    }
    auto freeVisual = [&] {
        if (!soft) XFree(vi);
    };

    Colormap cmap =
        XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = cmap;
    swa.border_pixel = 0;
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
                     KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                     FocusChangeMask | PropertyChangeMask;

    uint32_t w = width_.load(), h = height_.load();
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, w, h, 0,
                               vi->depth, InputOutput, vi->visual,
                               CWColormap | CWBorderPixel | CWBackPixel |
                                   CWEventMask,
                               &swa);
    XStoreName(dpy, win, "FluidSynthPlayer");

    // -------- XDND atoms --------
    Atom aXdndAware = XInternAtom(dpy, "XdndAware", False);
    Atom aXdndEnter = XInternAtom(dpy, "XdndEnter", False);
    Atom aXdndPosition = XInternAtom(dpy, "XdndPosition", False);
    Atom aXdndStatus = XInternAtom(dpy, "XdndStatus", False);
    Atom aXdndLeave = XInternAtom(dpy, "XdndLeave", False);
    Atom aXdndDrop = XInternAtom(dpy, "XdndDrop", False);
    Atom aXdndFinished = XInternAtom(dpy, "XdndFinished", False);
    Atom aXdndSelection = XInternAtom(dpy, "XdndSelection", False);
    Atom aXdndActionCopy = XInternAtom(dpy, "XdndActionCopy", False);
    Atom aXdndTypeList = XInternAtom(dpy, "XdndTypeList", False);
    Atom aUriList = XInternAtom(dpy, "text/uri-list", False);
    Atom aDndProp = XInternAtom(dpy, "FLUIDSYNTHPLAYER_DND", False);

    // -------- CLIPBOARD atoms (see ClipboardState above) --------
    Atom aClipboard = XInternAtom(dpy, "CLIPBOARD", False);
    Atom aTargets = XInternAtom(dpy, "TARGETS", False);
    Atom aUtf8String = XInternAtom(dpy, "UTF8_STRING", False);
    Atom aIncr = XInternAtom(dpy, "INCR", False);
    ClipboardState clipboardState;
    clipboardState.dpy = dpy;
    clipboardState.win = win;
    clipboardState.aClipboard = aClipboard;
    clipboardState.aTargets = aTargets;
    clipboardState.aUtf8String = aUtf8String;
    clipboardState.aIncr = aIncr;
    clipboardState.aPasteProp = XInternAtom(dpy, "FLUIDSYNTHPLAYER_CLIPBOARD", False);
    {
        Atom dndVersion = 5;
        XChangeProperty(dpy, win, aXdndAware, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)&dndVersion, 1);
    }
    Window dndSource = 0;
    bool dndUriOk = false;

    GLXContext glc = nullptr;
    if (!soft) {
        glc = glXCreateContext(dpy, vi, nullptr, GL_TRUE);
        if (!glc || !glXMakeCurrent(dpy, win, glc)) {
            // glxUsable() said yes, but never draw without a current context.
            fprintf(stderr, "[FluidSynthPlayer] no usable OpenGL context: the editor stays empty\n");
            if (glc) glXDestroyContext(dpy, glc);
            XDestroyWindow(dpy, win);
            XFreeColormap(dpy, cmap);
            freeVisual();
            XSync(dpy, False);
            unregisterDisplay(dpy);
            XCloseDisplay(dpy);
            return;
        }
        // no vsync: our own clock sets the pace (the swap must not block)
        using SwapIntervalFn = void (*)(Display*, GLXDrawable, int);
        if (auto f = (SwapIntervalFn)glXGetProcAddressARB(
                (const GLubyte*)"glXSwapIntervalEXT"))
            f(dpy, win, 0);
    }
    SoftPresenter presenter(dpy, win, vi->visual, vi->depth);

    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    setupEditorImGui();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);

    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    platformIO.Platform_GetClipboardTextFn = x11GetClipboardText;
    platformIO.Platform_SetClipboardTextFn = x11SetClipboardText;
    platformIO.Platform_ClipboardUserData = &clipboardState;

    if (soft) ImGui_SoftRenderer_Init();
    else ImGui_ImplOpenGL2_Init();

    bool mapped = false;
    auto lastFrame = std::chrono::steady_clock::now();

    while (!quit_.load()) {
        // -------- host commands --------
        {
            std::lock_guard<std::mutex> l(cmdMutex_);
            if (pendingParent_) {
                XReparentWindow(dpy, win, (Window)pendingParent_, 0, 0);
                pendingParent_ = 0;
            }
            if (hasPendingSize_) {
                XResizeWindow(dpy, win, pendingW_, pendingH_);
                width_.store(pendingW_);
                height_.store(pendingH_);
                hasPendingSize_ = false;
            }
            if (pendingVisible_ == 1 && !mapped) {
                XMapWindow(dpy, win);
                mapped = true;
            } else if (pendingVisible_ == 0 && mapped) {
                XUnmapWindow(dpy, win);
                mapped = false;
            }
            pendingVisible_ = -1;
        }

        // -------- X11 events --------
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
                case Expose:
                    presenter.invalidate();
                    break;
                case ConfigureNotify:
                    width_.store((uint32_t)ev.xconfigure.width);
                    height_.store((uint32_t)ev.xconfigure.height);
                    io.DisplaySize = ImVec2((float)ev.xconfigure.width,
                                            (float)ev.xconfigure.height);
                    break;
                case MotionNotify:
                    io.AddMousePosEvent((float)ev.xmotion.x,
                                        (float)ev.xmotion.y);
                    break;
                case EnterNotify:
                    io.AddMousePosEvent((float)ev.xcrossing.x,
                                        (float)ev.xcrossing.y);
                    break;
                case LeaveNotify:
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                    break;
                case ButtonPress:
                case ButtonRelease: {
                    bool down = ev.type == ButtonPress;
                    unsigned b = ev.xbutton.button;
                    if (down) // so keyboard shortcuts get delivered
                        XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
                    if (b == Button1) io.AddMouseButtonEvent(0, down);
                    else if (b == Button2) io.AddMouseButtonEvent(2, down);
                    else if (b == Button3) io.AddMouseButtonEvent(1, down);
                    else if (down && b == Button4) io.AddMouseWheelEvent(0, 1);
                    else if (down && b == Button5) io.AddMouseWheelEvent(0, -1);
                    else if (down && b == 6) io.AddMouseWheelEvent(1, 0);
                    else if (down && b == 7) io.AddMouseWheelEvent(-1, 0);
                    break;
                }
                case KeyPress:
                case KeyRelease: {
                    // filters the auto-repeat Release/Press pair
                    if (ev.type == KeyRelease && XPending(dpy)) {
                        XEvent next;
                        XPeekEvent(dpy, &next);
                        if (next.type == KeyPress &&
                            next.xkey.keycode == ev.xkey.keycode &&
                            next.xkey.time == ev.xkey.time)
                            break; // the following KeyPress re-emits it
                    }
                    bool down = ev.type == KeyPress;
                    char buf[8];
                    KeySym ks = NoSymbol;
                    int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks,
                                          nullptr);
                    unsigned st = ev.xkey.state;
                    io.AddKeyEvent(ImGuiMod_Ctrl, (st & ControlMask) != 0);
                    io.AddKeyEvent(ImGuiMod_Shift, (st & ShiftMask) != 0);
                    io.AddKeyEvent(ImGuiMod_Alt, (st & Mod1Mask) != 0);
                    KeySym lower, upper;
                    XConvertCase(ks, &lower, &upper);
                    ImGuiKey k = keysymToImGuiKey(lower);
                    if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
                    if (down && n > 0 && !(st & ControlMask) &&
                        (unsigned char)buf[0] >= 32) {
                        char utf8[16];
                        latin1ToUtf8(buf, n, utf8);
                        io.AddInputCharactersUTF8(utf8);
                    }
                    break;
                }
                case FocusIn:
                    io.AddFocusEvent(true);
                    break;
                case FocusOut:
                    io.AddFocusEvent(false);
                    break;
                case ClientMessage: {
                    Atom t = ev.xclient.message_type;
                    if (t == aXdndEnter) {
                        dndSource = (Window)ev.xclient.data.l[0];
                        dndUriOk = false;
                        if (ev.xclient.data.l[1] & 1) {
                            // more than 3 types: read XdndTypeList
                            Atom type;
                            int fmt;
                            unsigned long count, after;
                            unsigned char* data = nullptr;
                            if (XGetWindowProperty(dpy, dndSource, aXdndTypeList,
                                                   0, 1024, False, XA_ATOM,
                                                   &type, &fmt, &count, &after,
                                                   &data) == Success && data) {
                                Atom* atoms = (Atom*)data;
                                for (unsigned long i = 0; i < count; ++i)
                                    if (atoms[i] == aUriList) dndUriOk = true;
                                XFree(data);
                            }
                        } else {
                            for (int i = 2; i <= 4; ++i)
                                if ((Atom)ev.xclient.data.l[i] == aUriList)
                                    dndUriOk = true;
                        }
                    } else if (t == aXdndPosition && dndSource) {
                        XEvent st{};
                        st.xclient.type = ClientMessage;
                        st.xclient.display = dpy;
                        st.xclient.window = dndSource;
                        st.xclient.message_type = aXdndStatus;
                        st.xclient.format = 32;
                        st.xclient.data.l[0] = (long)win;
                        st.xclient.data.l[1] = dndUriOk ? 1 : 0;
                        st.xclient.data.l[4] = (long)aXdndActionCopy;
                        XSendEvent(dpy, dndSource, False, NoEventMask, &st);
                    } else if (t == aXdndDrop && dndSource) {
                        if (dndUriOk)
                            XConvertSelection(dpy, aXdndSelection, aUriList,
                                              aDndProp, win,
                                              (Time)ev.xclient.data.l[2]);
                    } else if (t == aXdndLeave) {
                        dndSource = 0;
                    }
                    break;
                }
                case SelectionNotify: {
                    if (ev.xselection.property != aDndProp) break;
                    Atom type;
                    int fmt;
                    unsigned long count, after;
                    unsigned char* data = nullptr;
                    if (XGetWindowProperty(dpy, win, aDndProp, 0, 1 << 20,
                                           True, AnyPropertyType, &type, &fmt,
                                           &count, &after, &data) == Success &&
                        data) {
                        std::string list((char*)data, count);
                        XFree(data);
                        // Collects every line of the uri-list (a single drag
                        // gesture can drop several files at once, e.g. a
                        // multi-selection from the OS file manager) and fires
                        // onFileDrop_ exactly ONCE with all of them together -
                        // the callback decides what "one drop" means (replace
                        // vs. build a stack), so it must see the whole batch
                        // at once, not one call per file.
                        std::vector<std::string> paths;
                        size_t pos = 0;
                        while (pos < list.size()) {
                            size_t eol = list.find_first_of("\r\n", pos);
                            std::string line = list.substr(
                                pos, eol == std::string::npos ? eol
                                                              : eol - pos);
                            if (!line.empty() && line[0] != '#') {
                                std::string path = uriToPath(line);
                                if (!path.empty()) paths.push_back(std::move(path));
                            }
                            if (eol == std::string::npos) break;
                            pos = list.find_first_not_of("\r\n", eol);
                        }
                        if (!paths.empty() && onFileDrop_) onFileDrop_(paths);
                    }
                    if (dndSource) {
                        XEvent fin{};
                        fin.xclient.type = ClientMessage;
                        fin.xclient.display = dpy;
                        fin.xclient.window = dndSource;
                        fin.xclient.message_type = aXdndFinished;
                        fin.xclient.format = 32;
                        fin.xclient.data.l[0] = (long)win;
                        fin.xclient.data.l[1] = 1;
                        fin.xclient.data.l[2] = (long)aXdndActionCopy;
                        XSendEvent(dpy, dndSource, False, NoEventMask, &fin);
                        dndSource = 0;
                    }
                    break;
                }
                case SelectionRequest: {
                    // Another client is asking us (we own CLIPBOARD) to
                    // convert our selection into one of its properties -
                    // ICCCM's side of x11SetClipboardText/x11GetClipboardText
                    // above.
                    const XSelectionRequestEvent& req = ev.xselectionrequest;
                    XSelectionEvent resp{};
                    resp.type = SelectionNotify;
                    resp.display = req.display;
                    resp.requestor = req.requestor;
                    resp.selection = req.selection;
                    resp.target = req.target;
                    resp.time = req.time;
                    resp.property = None; // refuse by default

                    if (req.selection == aClipboard) {
                        // Pre-ICCCM requestors leave property unset (None);
                        // convention is to reuse the target atom instead.
                        Atom prop = req.property != None ? req.property : req.target;
                        if (req.target == aTargets) {
                            Atom offered[] = {aTargets, aUtf8String, XA_STRING};
                            XChangeProperty(dpy, req.requestor, prop, XA_ATOM, 32,
                                           PropModeReplace, (unsigned char*)offered, 3);
                            resp.property = prop;
                        } else if (req.target == aUtf8String || req.target == XA_STRING) {
                            // XA_STRING is technically Latin-1, but settings
                            // text is effectively ASCII in practice, so
                            // serving the same UTF-8 bytes for both targets
                            // is a safe shortcut.
                            XChangeProperty(dpy, req.requestor, prop, req.target, 8,
                                           PropModeReplace,
                                           (const unsigned char*)clipboardState.ownedText.data(),
                                           (int)clipboardState.ownedText.size());
                            resp.property = prop;
                        }
                    }
                    XSendEvent(dpy, req.requestor, False, NoEventMask, (XEvent*)&resp);
                    break;
                }
                case SelectionClear:
                    if (ev.xselectionclear.selection == aClipboard)
                        clipboardState.ownedText.clear();
                    break;
                default:
                    break;
            }
        }

        // -------- ImGui frame --------
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;
        io.DeltaTime = dt > 0.f ? dt : 1.f / 60.f;

        if (!soft) ImGui_ImplOpenGL2_NewFrame();
        ImGui::NewFrame();
        if (drawUI_) drawUI_();
        ImGui::Render();

        if (soft) {
            presenter.present(ImGui::GetDrawData(), (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        } else {
            glViewport(0, 0, (int)width_.load(), (int)height_.load());
            glClearColor(0.118f, 0.118f, 0.129f, 1.f); // = the editor's window color
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
            glXSwapBuffers(dpy, win);
        }

        // ~60 fps (30 in software: CPU drawing, and possibly a network link)
        auto frameEnd = std::chrono::steady_clock::now();
        auto spent = frameEnd - now;
        auto budget = std::chrono::microseconds(soft ? 33300 : 16600);
        if (spent < budget)
            std::this_thread::sleep_for(budget - spent);
    }

    if (soft) {
        ImGui_SoftRenderer_Shutdown();
    } else {
        ImGui_ImplOpenGL2_Shutdown();
    }
    ImGui::DestroyContext(ctx);
    if (glc) {
        glXMakeCurrent(dpy, None, nullptr);
        glXDestroyContext(dpy, glc);
    }
    presenter.release();
    XDestroyWindow(dpy, win);
    XFreeColormap(dpy, cmap);
    freeVisual();
    XSync(dpy, False); // errors of this connection still count as ours
    unregisterDisplay(dpy);
    XCloseDisplay(dpy);
}
