#include "editor_ui.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "IconsFontAwesome4.h"
#include "engine/FluidEngine.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform.hpp"

namespace {

// ------------------------------------------------------------------ theme
//
// Neutral dark palette in the style of common dark UI kits: charcoal
// window, slightly lighter panel "cards", darker inset fields, light text
// and a single blue accent (fader stems, switches, knob value arcs,
// selection). Status colors (error red, OK green, warning amber) stand out
// against it unchanged.
const ImU32 kColBg = IM_COL32(30, 30, 33, 255);
const ImU32 kColPanelBg = IM_COL32(38, 38, 42, 255);
const ImU32 kColPanelBorder = IM_COL32(56, 56, 63, 255);
const ImU32 kColLabelBg = IM_COL32(52, 52, 59, 255);
const ImU32 kColText = IM_COL32(230, 230, 235, 255);
const ImU32 kColTextDim = IM_COL32(150, 150, 160, 255);
const ImU32 kColFieldBg = IM_COL32(24, 24, 27, 255);
const ImU32 kColFieldHover = IM_COL32(31, 31, 35, 255);
const ImU32 kColFieldBorder = IM_COL32(62, 62, 70, 255);
const ImU32 kColButton = IM_COL32(52, 52, 59, 255);
const ImU32 kColButtonHover = IM_COL32(66, 66, 75, 255);
const ImU32 kColAccent = IM_COL32(77, 154, 255, 255);
const ImU32 kColAccentDim = IM_COL32(77, 154, 255, 90);
const ImU32 kColLogoDark = IM_COL32(58, 58, 66, 255);
const ImU32 kColLogoLight = IM_COL32(105, 105, 118, 255);
const ImVec4 kAccent(0.42f, 0.68f, 1.00f, 1.f);
const ImVec4 kDimText(0.59f, 0.59f, 0.63f, 1.f);

void applyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(10, 10);
    st.FramePadding = ImVec2(6, 4);
    st.ItemSpacing = ImVec2(8, 6);
    st.WindowRounding = 4.f;
    st.PopupRounding = 4.f;
    st.FrameRounding = 3.f;
    st.GrabRounding = 2.f;
    st.TabRounding = 3.f;
    st.ScrollbarSize = 14.f;
    st.WindowBorderSize = 1.f;
    st.PopupBorderSize = 1.f;
    auto c = [](int r, int g, int b, int a = 255) {
        return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f);
    };
    ImVec4* col = st.Colors;
    col[ImGuiCol_Text] = c(230, 230, 235);
    col[ImGuiCol_TextDisabled] = c(120, 120, 130);
    col[ImGuiCol_WindowBg] = c(30, 30, 33);
    col[ImGuiCol_ChildBg] = c(0, 0, 0, 0);
    col[ImGuiCol_PopupBg] = c(36, 36, 40, 250);
    col[ImGuiCol_Border] = c(56, 56, 63);
    col[ImGuiCol_FrameBg] = c(24, 24, 27);
    col[ImGuiCol_FrameBgHovered] = c(34, 34, 39);
    col[ImGuiCol_FrameBgActive] = c(42, 42, 48);
    col[ImGuiCol_TitleBg] = c(38, 38, 42);
    col[ImGuiCol_TitleBgActive] = c(38, 38, 42);
    col[ImGuiCol_Button] = c(52, 52, 59);
    col[ImGuiCol_ButtonHovered] = c(66, 66, 75);
    col[ImGuiCol_ButtonActive] = c(77, 154, 255, 160);
    col[ImGuiCol_Header] = c(77, 154, 255, 70);
    col[ImGuiCol_HeaderHovered] = c(77, 154, 255, 110);
    col[ImGuiCol_HeaderActive] = c(77, 154, 255, 150);
    col[ImGuiCol_SliderGrab] = c(77, 154, 255);
    col[ImGuiCol_SliderGrabActive] = c(120, 180, 255);
    col[ImGuiCol_CheckMark] = c(77, 154, 255);
    col[ImGuiCol_Tab] = c(38, 38, 42);
    col[ImGuiCol_TabHovered] = c(66, 66, 75);
    col[ImGuiCol_TabSelected] = c(52, 52, 59);
    col[ImGuiCol_TabSelectedOverline] = c(77, 154, 255);
    col[ImGuiCol_ScrollbarBg] = c(24, 24, 27);
    col[ImGuiCol_ScrollbarGrab] = c(62, 62, 70);
    col[ImGuiCol_ScrollbarGrabHovered] = c(80, 80, 90);
    col[ImGuiCol_ScrollbarGrabActive] = c(77, 154, 255);
    col[ImGuiCol_Separator] = c(56, 56, 63);
    col[ImGuiCol_TableHeaderBg] = c(38, 38, 42);
    col[ImGuiCol_TableBorderLight] = c(48, 48, 54);
    col[ImGuiCol_TableRowBgAlt] = c(255, 255, 255, 8);
    col[ImGuiCol_TextSelectedBg] = c(77, 154, 255, 90);
    col[ImGuiCol_ModalWindowDimBg] = ImVec4(0.f, 0.f, 0.f, 0.5f);
}

// ------------------------------------------------------- layout context
//
// Everything on the main panel is placed on a 560x584 design grid and
// scaled uniformly (fonts included - ImGui 1.92 rasterizes any size).

struct Layout {
    ImVec2 origin;
    float s = 1.f;
    ImDrawList* dl = nullptr;
    ImFont* body = nullptr;
    ImFont* bold = nullptr;

    ImVec2 p(float x, float y) const { return ImVec2(origin.x + x * s, origin.y + y * s); }
    float px(float v) const { return v * s; }

    void text(float x, float y, float size, ImU32 col, const char* str, bool bolded = true,
              float centerW = 0.f) const {
        ImFont* f = bolded ? bold : body;
        float tx = x;
        if (centerW > 0.f) tx = x + (centerW - textW(size, str, bolded)) * 0.5f;
        dl->AddText(f, size * s, p(tx, y), col, str);
    }
    float textW(float size, const char* str, bool bolded = true) const {
        return (bolded ? bold : body)->CalcTextSizeA(size * s, FLT_MAX, 0.f, str).x / s;
    }
};

// Places an invisible hit box at design coords; returns true on click.
bool hitBox(const Layout& L, const char* id, float x, float y, float w, float h,
            ImGuiButtonFlags flags = 0) {
    ImGui::SetCursorScreenPos(L.p(x, y));
    return ImGui::InvisibleButton(id, ImVec2(L.px(w), L.px(h)), flags);
}

bool containsNoCase(const std::string& haystack, const char* needle) {
    if (!needle[0]) return true;
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= haystack.size(); ++i)
        if (!platform::strNCaseCmp(haystack.c_str() + i, needle, n)) return true;
    return false;
}

// ------------------------------------------------------- param plumbing

bool isLogParam(ParamId id) {
    return id == kParamEnvAttack || id == kParamEnvDecay || id == kParamEnvRelease ||
           id == kParamLfoSpeed || id == kParamChorusSpeed;
}

// Log-like curve that starts at exactly 0 (the Hold fader): 0 at the
// bottom, ~1 ms just above it, max at the top.
bool isZeroLogParam(ParamId id) { return id == kParamEnvHold; }
constexpr double kZeroLogBase = 20000.0;

float toNorm(ParamId id, double v) {
    const auto& info = kParamInfos[id];
    float n;
    if (isZeroLogParam(id))
        n = static_cast<float>(std::log1p(std::max(v, 0.0) / info.max * (kZeroLogBase - 1.0)) /
                               std::log(kZeroLogBase));
    else if (isLogParam(id))
        n = static_cast<float>(std::log(v / info.min) / std::log(info.max / info.min));
    else
        n = static_cast<float>((v - info.min) / (info.max - info.min));
    return std::clamp(n, 0.f, 1.f);
}

double fromNorm(ParamId id, float n) {
    const auto& info = kParamInfos[id];
    n = std::clamp(n, 0.f, 1.f);
    double v = isZeroLogParam(id) ? info.max * (std::pow(kZeroLogBase, n) - 1.0) / (kZeroLogBase - 1.0)
               : isLogParam(id)   ? info.min * std::pow(info.max / info.min, n)
                                  : info.min + n * (info.max - info.min);
    if (info.stepped) v = std::round(v);
    return v;
}

void commitParam(const EditorCallbacks& cb, ParamId id, double v) {
    if (cb.beginGesture) cb.beginGesture(id);
    if (cb.setParam) cb.setParam(id, v);
    if (cb.endGesture) cb.endGesture(id);
}

void toggleParam(SharedParams& params, const EditorCallbacks& cb, ParamId id) {
    commitParam(cb, id, params.getInt(id) ? 0.0 : 1.0);
}

// Middle-click "type an exact value" popup, shared by knobs and sliders
// (same extra as the sibling projects' sliderExtras*).
void exactValuePopup(const EditorCallbacks& cb, ParamId id, const char* popupId, double current) {
    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle)) ImGui::OpenPopup(popupId);
    if (ImGui::BeginPopup(popupId)) {
        static double editValue = 0.0;
        if (ImGui::IsWindowAppearing()) {
            editValue = current;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::TextUnformatted(kParamInfos[id].name);
        ImGui::SetNextItemWidth(130.f);
        if (ImGui::InputDouble("##edit", &editValue, 0.0, 0.0, "%.4f",
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
            const auto& info = kParamInfos[id];
            commitParam(cb, id, std::clamp(editValue, info.min, info.max));
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("%s .. %s", formatParamValue(id, kParamInfos[id].min).c_str(),
                            formatParamValue(id, kParamInfos[id].max).c_str());
        ImGui::EndPopup();
    }
}

// Shared mouse behaviour of the main panel's knobs and faders. The last
// ImGui item must be the widget's InvisibleButton. `dragNorm` maps the
// current normalized value to the new one while the left button drags.
// Double/right-click resets, wheel nudges, middle-click types a value.
template <typename DragFn>
void paramInteraction(SharedParams& params, const EditorCallbacks& cb, ParamId id,
                      DragFn dragNorm) {
    const auto& info = kParamInfos[id];
    const double value = params.get(id);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // Only a left-button drag is a CLAP gesture; right/middle clicks also
    // activate the button but commit their own begin/value/end at once.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID gestureKey = ImGui::GetID("##gesture");
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        storage->SetBool(gestureKey, true);
        if (cb.beginGesture) cb.beginGesture(id);
    }
    if (active) {
        const double nv = fromNorm(id, dragNorm(toNorm(id, value)));
        if (nv != value && cb.setParam) cb.setParam(id, nv);
    }
    if (ImGui::IsItemDeactivated() && storage->GetBool(gestureKey)) {
        storage->SetBool(gestureKey, false);
        if (cb.endGesture) cb.endGesture(id);
    }
    if (hovered && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
                    ImGui::IsItemClicked(ImGuiMouseButton_Right)))
        commitParam(cb, id, info.def);
    if (hovered && !active) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) {
            const double nv = info.stepped
                                  ? std::clamp(value + (wheel > 0 ? 1.0 : -1.0), info.min, info.max)
                                  : fromNorm(id, toNorm(id, value) + wheel * 0.02f);
            commitParam(cb, id, nv);
        }
    }
    exactValuePopup(cb, id, "##exact", value);
    if (hovered || active)
        ImGui::SetTooltip("%s: %s", info.name, formatParamValue(id, params.get(id)).c_str());
}

// ------------------------------------------------------ main panel widgets

void drawPanel(const Layout& L, float x, float y, float w, float h, const char* label) {
    L.dl->AddRectFilled(L.p(x, y), L.p(x + w, y + h), kColPanelBg, L.px(6.f));
    L.dl->AddRect(L.p(x, y), L.p(x + w, y + h), kColPanelBorder, L.px(6.f), 0, L.px(1.f));
    if (label) {
        const float tw = L.textW(14.f, label);
        L.dl->AddRectFilled(L.p(x + 4, y + 4), L.p(x + 4 + tw + 12, y + 24), kColLabelBg,
                            L.px(2.f));
        L.text(x + 10, y + 6, 14.f, kColText, label);
    }
}

// Flat gray button with a centered icon/text.
bool drawButton(const Layout& L, const char* id, float x, float y, float w, float h,
                const char* icon, const char* tooltip = nullptr, float iconSize = 18.f) {
    const bool clicked = hitBox(L, id, x, y, w, h,
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    L.dl->AddRectFilled(L.p(x, y), L.p(x + w, y + h), hovered ? kColButtonHover : kColButton,
                        L.px(3.f));
    L.dl->AddRect(L.p(x, y), L.p(x + w, y + h), kColFieldBorder, L.px(3.f), 0, L.px(1.f));
    const float tw = L.textW(iconSize, icon);
    L.text(x + (w - tw) * 0.5f, y + (h - iconSize) * 0.5f - 1.f, iconSize, kColText, icon);
    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// Rotary knob: tick ring, black body, white pointer, label underneath.
void drawKnob(const Layout& L, SharedParams& params, const EditorCallbacks& cb, ParamId id,
              float cx, float cy, float r, const char* label, bool enabled = true) {
    ImGui::PushID(static_cast<int>(id));
    hitBox(L, "##knob", cx - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8,
           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
               ImGuiButtonFlags_MouseButtonMiddle);
    paramInteraction(params, cb, id, [](float n) {
        const ImGuiIO& io = ImGui::GetIO();
        return n - io.MouseDelta.y * (io.KeyShift ? 0.0008f : 0.005f);
    });
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();

    const float norm = toNorm(id, params.get(id));
    const ImVec2 c = L.p(cx, cy);
    const float R = L.px(r);
    const float a0 = IM_PI * 0.75f, a1 = IM_PI * 2.25f;
    for (int i = 0; i <= 20; ++i) {
        const float a = a0 + (a1 - a0) * i / 20.f;
        const float r0 = R + L.px(3.f), r1 = R + L.px(i % 5 == 0 ? 7.f : 5.f);
        L.dl->AddLine(ImVec2(c.x + std::cos(a) * r0, c.y + std::sin(a) * r0),
                      ImVec2(c.x + std::cos(a) * r1, c.y + std::sin(a) * r1),
                      IM_COL32(64, 64, 72, 255), L.px(1.2f));
    }
    // value arc on the tick ring, from the default position (bipolar knobs
    // like Sustain/Cutoff read as +/- from "untouched")
    const float a = a0 + (a1 - a0) * norm;
    if (enabled) {
        const float aDef = a0 + (a1 - a0) * toNorm(id, kParamInfos[id].def);
        if (std::fabs(a - aDef) > 0.01f) {
            L.dl->PathArcTo(c, R + L.px(3.5f), std::min(a, aDef), std::max(a, aDef), 32);
            L.dl->PathStroke(kColAccent, 0, L.px(2.5f));
        }
    }
    L.dl->AddCircleFilled(c, R, IM_COL32(18, 18, 20, 255), 48);
    L.dl->AddCircleFilled(c, R - L.px(2.5f),
                          hot ? IM_COL32(58, 58, 65, 255) : IM_COL32(46, 46, 52, 255), 48);
    L.dl->AddCircleFilled(ImVec2(c.x - R * 0.18f, c.y - R * 0.22f), R * 0.55f,
                          IM_COL32(255, 255, 255, 8), 32); // soft top highlight
    const ImU32 pointer = enabled ? IM_COL32(235, 235, 240, 255) : IM_COL32(110, 110, 120, 255);
    L.dl->AddLine(ImVec2(c.x + std::cos(a) * R * 0.30f, c.y + std::sin(a) * R * 0.30f),
                  ImVec2(c.x + std::cos(a) * (R - L.px(4.f)), c.y + std::sin(a) * (R - L.px(4.f))),
                  pointer, L.px(2.6f));
    L.text(cx - 40, cy + r + 9, 13.f, enabled ? kColText : kColTextDim, label, true, 80.f);
    ImGui::PopID();
}

// Vertical fader (ENVELOPE panel): ruler ticks, light handle, orange stem
// from the handle down to the bottom while the envelope override is on.
void drawVSlider(const Layout& L, SharedParams& params, const EditorCallbacks& cb, ParamId id,
                 float x, float yTop, float yBot, const char* label, bool enabled) {
    ImGui::PushID(static_cast<int>(id));
    hitBox(L, "##fader", x - 16, yTop - 10, 32, yBot - yTop + 20,
           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
               ImGuiButtonFlags_MouseButtonMiddle);
    const float top = L.p(x, yTop).y, bot = L.p(x, yBot).y;
    paramInteraction(params, cb, id, [&](float n) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift) return n - io.MouseDelta.y / (bot - top) * 0.15f;
        return (bot - io.MousePos.y) / (bot - top); // handle follows the mouse
    });

    for (int i = 0; i <= 12; ++i) {
        const float ty = yTop + (yBot - yTop) * i / 12.f;
        const float len = i % 3 == 0 ? 8.f : 5.f;
        L.dl->AddLine(L.p(x - 7 - len, ty), L.p(x - 7, ty), IM_COL32(64, 64, 72, 255), L.px(1.f));
        L.dl->AddLine(L.p(x + 7, ty), L.p(x + 7 + len, ty), IM_COL32(64, 64, 72, 255), L.px(1.f));
    }
    L.dl->AddLine(L.p(x, yTop), L.p(x, yBot), IM_COL32(18, 18, 20, 255), L.px(3.f));

    const float norm = toNorm(id, params.get(id));
    const float hy = yBot - (yBot - yTop) * norm;
    if (enabled && norm > 0.f) L.dl->AddLine(L.p(x, hy), L.p(x, yBot), kColAccent, L.px(2.f));
    const ImU32 face = enabled ? IM_COL32(205, 205, 212, 255) : IM_COL32(110, 110, 120, 255);
    L.dl->AddRectFilled(L.p(x - 12, hy - 7), L.p(x + 12, hy + 7), IM_COL32(18, 18, 20, 255),
                        L.px(2.f));
    L.dl->AddRectFilled(L.p(x - 11, hy - 6), L.p(x + 11, hy + 6), face, L.px(2.f));
    L.dl->AddLine(L.p(x - 10, hy), L.p(x + 10, hy), IM_COL32(77, 154, 255, 255), L.px(1.5f));
    L.text(x - 20, yBot + 14, 14.f, enabled ? kColText : kColTextDim, label, true, 40.f);
    ImGui::PopID();
}

// The narrow upright switch ("Override Send" / "envelope override").
void drawOverrideSwitch(const Layout& L, SharedParams& params, const EditorCallbacks& cb,
                        ParamId id, float x, float y, const char* tooltip) {
    ImGui::PushID(static_cast<int>(id));
    if (hitBox(L, "##ovr", x - 3, y - 3, 16, 26)) toggleParam(params, cb, id);
    const bool on = params.getInt(id) != 0;
    L.dl->AddRectFilled(L.p(x, y), L.p(x + 10, y + 20),
                        on ? kColAccent : IM_COL32(52, 52, 59, 255), L.px(2.f));
    L.dl->AddRect(L.p(x, y), L.p(x + 10, y + 20), IM_COL32(18, 18, 20, 255), L.px(2.f), 0,
                  L.px(1.f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", tooltip, on ? "On" : "Off");
    ImGui::PopID();
}

// Round "ENABLE" radio.
void drawRadio(const Layout& L, SharedParams& params, const EditorCallbacks& cb, ParamId id,
               float x, float y, const char* label, const char* tooltip) {
    ImGui::PushID(static_cast<int>(id));
    const float w = 20.f + L.textW(13.f, label);
    if (hitBox(L, "##radio", x - 2, y - 2, w + 4, 18)) toggleParam(params, cb, id);
    const bool on = params.getInt(id) != 0;
    const ImVec2 c = L.p(x + 7, y + 7);
    L.dl->AddCircleFilled(c, L.px(7.f), IM_COL32(24, 24, 27, 255), 24);
    L.dl->AddCircle(c, L.px(7.f), on ? kColAccent : IM_COL32(90, 90, 100, 255), 24, L.px(1.2f));
    if (on) L.dl->AddCircleFilled(c, L.px(3.5f), kColAccent, 16);
    L.text(x + 19, y, 13.f, kColText, label);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
}

// Field with clipped text (SoundFont name / patch name).
bool drawField(const Layout& L, const char* id, float x, float y, float w, float h,
               const char* str, ImU32 textCol, float size = 17.f) {
    const bool clicked = hitBox(L, id, x, y, w, h,
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    L.dl->AddRectFilled(L.p(x, y), L.p(x + w, y + h), hovered ? kColFieldHover : kColFieldBg,
                        L.px(2.f));
    L.dl->AddRect(L.p(x, y), L.p(x + w, y + h), kColFieldBorder, L.px(2.f), 0, L.px(1.f));
    L.dl->PushClipRect(L.p(x + 4, y), L.p(x + w - 4, y + h), true);
    L.text(x + 8, y + (h - size) * 0.5f - 1.f, size, textCol, str);
    L.dl->PopClipRect();
    return clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// ------------------------------------------------------- popup widgets
// (regular ImGui widgets, used inside the Options / SETUP / patch popups)

void rowLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 8.5f);
}

void paramSliderFloat(SharedParams& params, const EditorCallbacks& cb, ParamId id,
                      const char* label, float width = -1.f) {
    const auto& info = kParamInfos[id];
    float v = static_cast<float>(params.get(id));
    ImGui::PushID(static_cast<int>(id));
    rowLabel(label);
    ImGui::SetNextItemWidth(width);
    const std::string fmt = formatParamValue(id, v);
    if (ImGui::SliderFloat("##slider", &v, static_cast<float>(info.min),
                           static_cast<float>(info.max), fmt.c_str(),
                           isLogParam(id) ? ImGuiSliderFlags_Logarithmic : 0))
        if (cb.setParam) cb.setParam(id, v);
    if (ImGui::IsItemActivated() && cb.beginGesture) cb.beginGesture(id);
    if (ImGui::IsItemDeactivated() && cb.endGesture) cb.endGesture(id);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) commitParam(cb, id, info.def);
    exactValuePopup(cb, id, "##exact", v);
    ImGui::PopID();
}

void paramSliderInt(SharedParams& params, const EditorCallbacks& cb, ParamId id,
                    const char* label, float width = -1.f) {
    const auto& info = kParamInfos[id];
    int v = params.getInt(id);
    ImGui::PushID(static_cast<int>(id));
    rowLabel(label);
    ImGui::SetNextItemWidth(width);
    const std::string fmt = formatParamValue(id, v);
    if (ImGui::SliderInt("##slider", &v, static_cast<int>(info.min), static_cast<int>(info.max),
                         fmt.c_str()))
        if (cb.setParam) cb.setParam(id, v);
    if (ImGui::IsItemActivated() && cb.beginGesture) cb.beginGesture(id);
    if (ImGui::IsItemDeactivated() && cb.endGesture) cb.endGesture(id);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) commitParam(cb, id, info.def);
    exactValuePopup(cb, id, "##exact", v);
    ImGui::PopID();
}

void paramCombo(SharedParams& params, const EditorCallbacks& cb, ParamId id, const char* label,
                const char* const* items, int count) {
    const int idx = std::clamp(params.getInt(id), 0, count - 1);
    ImGui::PushID(static_cast<int>(id));
    rowLabel(label);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##combo", items[idx])) {
        for (int i = 0; i < count; ++i) {
            if (ImGui::Selectable(items[i], i == idx)) commitParam(cb, id, i);
            if (i == idx) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void paramCheckbox(SharedParams& params, const EditorCallbacks& cb, ParamId id,
                   const char* label, const char* text) {
    bool v = params.getInt(id) != 0;
    ImGui::PushID(static_cast<int>(id));
    rowLabel(label);
    if (ImGui::Checkbox(text, &v)) commitParam(cb, id, v ? 1.0 : 0.0);
    ImGui::PopID();
}

void sectionHeader(const char* text) {
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "%s", text);
    ImGui::Separator();
}

void dimWrapped(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------ patches

std::string presetNameFor(const std::vector<PresetEntry>& presets, int bank, int program) {
    for (const auto& p : presets)
        if (p.bank == bank && p.program == program) return p.name;
    return "";
}

void assignPatch(SharedParams& params, const EditorCallbacks& cb, int channel, int bank,
                 int program) {
    if (channel != 0) {
        params.pendingChannelPatch[channel].store((bank << 8) | (program & 0x7F));
        // Optimistic mirror; the audio thread confirms it next block.
        params.channelBank[channel].store(bank);
        params.channelProgram[channel].store(program);
        return;
    }
    if (cb.beginGesture) {
        cb.beginGesture(kParamBank);
        cb.beginGesture(kParamProgram);
    }
    if (cb.setParam) {
        cb.setParam(kParamBank, bank);
        cb.setParam(kParamProgram, program);
    }
    if (cb.endGesture) {
        cb.endGesture(kParamBank);
        cb.endGesture(kParamProgram);
    }
}

// Prev/next arrows: steps through the SoundFont's own preset list (bank
// order), wrapping around.
void stepPatch(SharedParams& params, const EditorCallbacks& cb,
               const std::vector<PresetEntry>& presets, int dir) {
    if (presets.empty()) return;
    const int bank = params.getInt(kParamBank), prog = params.getInt(kParamProgram);
    int idx = -1;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].bank == bank && presets[i].program == prog) idx = static_cast<int>(i);
    const int n = static_cast<int>(presets.size());
    idx = idx < 0 ? (dir > 0 ? 0 : n - 1) : (idx + dir + n) % n;
    assignPatch(params, cb, 0, presets[idx].bank, presets[idx].program);
}

// Patch selector pop-up: searchable list grouped by bank. Click = select; the popup stays open so patches can be
// auditioned from the keyboard; click outside / Esc closes it.
void drawPatchBrowser(SharedParams& params, EditorUIState& ui, const EditorCallbacks& cb,
                      const std::vector<PresetEntry>& presets, bool hasSf) {
    const bool multi = params.getInt(kParamMidiMode) == kMidiModeMulti;
    if (!multi) ui.browserChannel = 0;
    const int ch = ui.browserChannel;
    const int bank = ch == 0 ? params.getInt(kParamBank) : params.channelBank[ch].load();
    const int program = ch == 0 ? params.getInt(kParamProgram) : params.channelProgram[ch].load();

    if (multi) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
        char preview[32];
        snprintf(preview, sizeof(preview), "Channel %d%s", ch + 1, ch == 9 ? " (drums)" : "");
        if (ImGui::BeginCombo("##chan", preview)) {
            for (int c = 0; c < kNumMidiChannels; ++c) {
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "Channel %d%s", c + 1, c == 9 ? " (drums)" : "");
                if (ImGui::Selectable(lbl, c == ch)) {
                    ui.browserChannel = c;
                    ui.scrollToCurrentPatch = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##presetfilter", ICON_FA_SEARCH " search presets", ui.presetFilter,
                             sizeof(ui.presetFilter));

    ImGui::BeginChild("##presetlist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (presets.empty())
        ImGui::TextWrapped(hasSf ? "This SoundFont has no presets."
                                 : "No SoundFont loaded - click the folder button or drop a "
                                   ".sf2 / .sf3 file onto the plugin.");
    std::map<int, std::vector<const PresetEntry*>> byBank;
    for (const auto& p : presets)
        if (containsNoCase(p.name, ui.presetFilter)) byBank[p.bank].push_back(&p);
    const bool filtering = ui.presetFilter[0] != 0;
    for (auto& [b, list] : byBank) {
        char header[64];
        snprintf(header, sizeof(header), "Bank %03d%s  (%zu)", b, b == 128 ? " - Drum kits" : "",
                 list.size());
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (filtering || b == bank || byBank.size() <= 2) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (ui.scrollToCurrentPatch && b == bank) ImGui::SetNextItemOpen(true);
        ImGui::PushID(b);
        if (ImGui::TreeNodeEx(header, flags)) {
            for (const PresetEntry* p : list) {
                const bool current = p->bank == bank && p->program == program;
                char row[160];
                snprintf(row, sizeof(row), "%03d  %s", p->program, p->name.c_str());
                ImGui::PushID(p->program);
                if (current) ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
                if (ImGui::Selectable(row, current)) assignPatch(params, cb, ch, p->bank, p->program);
                if (current) ImGui::PopStyleColor();
                if (current && ui.scrollToCurrentPatch) {
                    ImGui::SetScrollHereY(0.3f);
                    ui.scrollToCurrentPatch = false;
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (ui.scrollToCurrentPatch && byBank.count(bank) == 0) ui.scrollToCurrentPatch = false;
    ImGui::EndChild();
}

// ------------------------------------------------------------ Options

int stdStringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}

void drawOptionsGeneral(SharedParams& params, EditorUIState& ui, const EditorCallbacks& cb) {
    sectionHeader("Window");
    paramCombo(params, cb, kParamUiScale, "UI size", kUiScaleNames, kNumUiScales);
    rowLabel("");
    ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
    ImGui::TextWrapped("For DAWs that can't resize plugin windows. Saved with the project; new "
                       "instances open at the last size chosen.");
    ImGui::PopStyleColor();

    sectionHeader("Default SoundFont");
    {
        std::string def, current;
        {
            std::lock_guard<std::mutex> lock(params.guiState.mutex);
            def = params.guiState.defaultSoundFont;
            current = params.guiState.soundFontPath;
        }
        rowLabel("New instances");
        if (def.empty()) {
            ImGui::TextColored(kDimText, "(none - start empty)");
        } else {
            const std::string name = def.substr(def.find_last_of('/') + 1);
            ImGui::TextColored(kAccent, "%s", name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", def.c_str());
        }
        rowLabel("");
        ImGui::BeginDisabled(current.empty() || current == def);
        if (ImGui::Button("Use current") && cb.setDefaultSoundFont) cb.setDefaultSoundFont(current);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Choose...") && cb.chooseDefaultSoundFont) cb.chooseDefaultSoundFont();
        ImGui::SameLine();
        ImGui::BeginDisabled(def.empty());
        if (ImGui::Button("Clear") && cb.setDefaultSoundFont) cb.setDefaultSoundFont("");
        ImGui::EndDisabled();
        rowLabel("");
        ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
        ImGui::TextWrapped("Loaded when the plugin is added to a track. Projects reopen with "
                           "their own saved SoundFont.");
        ImGui::PopStyleColor();
    }

    sectionHeader("Output");
    paramSliderFloat(params, cb, kParamGain, "Volume");
    paramCheckbox(params, cb, kParamLimiterOn, "Limiter", "Output limiter (FluidSynth 2.6)");
    {
        const bool lim = params.getInt(kParamLimiterOn) != 0;
        ImGui::BeginDisabled(!lim);
        paramSliderFloat(params, cb, kParamLimiterCeiling, "Ceiling");
        paramSliderFloat(params, cb, kParamLimiterRelease, "Release");
        ImGui::EndDisabled();
        rowLabel("");
        ImGui::TextColored(kDimText, "Limiter changes rebuild the synth (reloads the SoundFont).");
    }
    rowLabel("Voices");
    ImGui::Text("%d", params.activeVoiceCount.load());
    ImGui::SameLine();
    if (ImGui::Button("Panic (all sounds off)")) params.pendingPanic = true;

    sectionHeader("MIDI");
    paramCombo(params, cb, kParamMidiMode, "MIDI mode", kMidiModeNames, 2);
    paramCheckbox(params, cb, kParamReceivePC, "Program change", "Receive bank select + PC");
    {
        uint32_t disabled = 0;
        {
            std::lock_guard<std::mutex> lock(params.guiState.mutex);
            disabled = params.guiState.info.disabledControllers;
        }
        paramCheckbox(params, cb, kParamRestoreControllers, "Controllers",
                      "Restore controllers the SoundFont disables");
        rowLabel("");
        if (disabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, params.getInt(kParamRestoreControllers)
                                                     ? ImVec4(0.70f, 1.0f, 0.75f, 1.f)
                                                     : ImVec4(1.0f, 0.75f, 0.55f, 1.f));
            ImGui::TextWrapped("This SoundFont disables: %s.",
                               describeControllers(disabled).c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(kDimText, "This SoundFont keeps all standard controllers.");
        }
    }
    paramSliderInt(params, cb, kParamBendRange, "Bend range");
    paramSliderInt(params, cb, kParamTranspose, "Transpose");
    paramSliderFloat(params, cb, kParamFineTune, "Fine tune");

    sectionHeader("Voices");
    paramCombo(params, cb, kParamVoiceMode, "Voice mode", kVoiceModeNames, 3);
    paramSliderInt(params, cb, kParamPortamento, "Portamento");
    paramCombo(params, cb, kParamPortamentoTimeMode, "Porta. time CC", kPortaTimeModeNames, 3);
    paramSliderInt(params, cb, kParamPolyphony, "Polyphony");
    paramCombo(params, cb, kParamInterpolation, "Interpolation", kInterpNames, kNumInterpModes);
    if (params.getInt(kParamInterpolation) == kNumInterpModes - 1) {
        rowLabel("");
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.45f, 1.f),
                           "Very CPU-heavy - meant for offline rendering.");
    }
    paramCombo(params, cb, kParamRenderInterpolation, "Render interp.", kRenderInterpNames,
               kNumInterpModes + 1);
    rowLabel("");
    ImGui::TextColored(kDimText, params.offlineRender.load() ? "Rendering offline now."
                                                             : "Used when the DAW renders/exports.");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Reset all parameters...")) ui.resetConfirmTrigger = true;
    if (ui.resetConfirmTrigger) {
        ImGui::OpenPopup("Reset parameters?");
        ui.resetConfirmTrigger = false;
    }
    if (ImGui::BeginPopupModal("Reset parameters?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Reset every knob and setting to its default?");
        ImGui::TextUnformatted("(the SoundFont and the selected patch are kept)");
        if (ImGui::Button("Reset", ImVec2(120, 0))) {
            for (uint32_t i = 0; i < kNumParams; ++i) {
                const auto id = static_cast<ParamId>(i);
                if (id == kParamBank || id == kParamProgram) continue;
                if (params.get(id) != kParamInfos[i].def) commitParam(cb, id, kParamInfos[i].def);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void drawOptionsChannels(SharedParams& params, EditorUIState& ui,
                         const std::vector<PresetEntry>& presets) {
    const bool multi = params.getInt(kParamMidiMode) == kMidiModeMulti;
    dimWrapped(multi ? "Multitimbral mode: every MIDI channel has its own patch (program "
                       "changes, or select a channel here and pick its patch in the patch "
                       "selector). Channel 10 plays drum kits."
                     : "Omni mode: all MIDI channels play the patch shown on the main panel. "
                       "Set MIDI mode to Multitimbral (General tab) for 16 independent "
                       "channels.");
    if (!ImGui::BeginTable("##channels", 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize());
    ImGui::TableSetupColumn("Bank:Patch", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFontSize() * 5.5f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    for (int c = 0; c < kNumMidiChannels; ++c) {
        const int src = multi ? c : 0; // omni: every channel plays channel 1's patch
        const int bank = params.channelBank[src].load();
        const int prog = params.channelProgram[src].load();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%2d", c + 1);
        ImGui::BeginDisabled(!multi);
        if (ImGui::Selectable(lbl, multi && ui.browserChannel == c,
                              ImGuiSelectableFlags_SpanAllColumns))
            ui.browserChannel = c;
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        const float act = params.channelActivity[c].load();
        if (act > 0.01f) params.channelActivity[c].store(act * 0.9f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + rowH * 0.35f, p.y + rowH * 0.45f), rowH * 0.22f,
            ImGui::GetColorU32(ImVec4(0.25f + 0.7f * act, 0.3f + 0.6f * act, 0.3f, 1.f)));
        ImGui::TableNextColumn();
        ImGui::Text("%03d:%03d", bank, prog);
        ImGui::TableNextColumn();
        const std::string n = presetNameFor(presets, bank, prog);
        if (n.empty()) ImGui::TextColored(kDimText, "(none)");
        else ImGui::TextUnformatted(n.c_str());
    }
    ImGui::EndTable();
}

void drawOptionsAdvanced(SharedParams& params, EditorUIState& ui, const EditorCallbacks& cb) {
    std::string current, editor;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        current = params.guiState.fluidSettingsText;
        editor = params.guiState.externalEditor;
    }
    if (!ui.settingsDraftDirty) ui.settingsDraft = current;
    if (!ui.externalEditorBufInit) {
        ui.externalEditorBufInit = true;
        snprintf(ui.externalEditorBuf, sizeof(ui.externalEditorBuf), "%s", editor.c_str());
    }

    sectionHeader("FluidSynth settings");
    dimWrapped("One \"key = value\" per line, e.g. synth.min-note-length = 10, "
               "synth.overflow.percussion = 4000, synth.dynamic-sample-loading = 1. "
               "Applying rebuilds the synthesizer.");
    const float h = std::max(80.f, ImGui::GetContentRegionAvail().y -
                                       ImGui::GetFrameHeightWithSpacing() * 5.f);
    if (ImGui::InputTextMultiline("##fluidsettings", ui.settingsDraft.data(),
                                  ui.settingsDraft.capacity() + 1, ImVec2(-1, h),
                                  ImGuiInputTextFlags_CallbackResize |
                                      ImGuiInputTextFlags_AllowTabInput,
                                  stdStringResize, &ui.settingsDraft))
        ui.settingsDraftDirty = true;
    ImGui::BeginDisabled(!ui.settingsDraftDirty);
    if (ImGui::Button(ICON_FA_COGS " Apply (rebuild synth)") && cb.applyFluidSettings) {
        cb.applyFluidSettings(ui.settingsDraft);
        ui.settingsDraftDirty = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) ui.settingsDraftDirty = false;
    ImGui::EndDisabled();

    sectionHeader("External SoundFont editor");
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 5.f);
    ImGui::InputText("##exteditor", ui.externalEditorBuf, sizeof(ui.externalEditorBuf));
    ImGui::SameLine();
    if (ImGui::Button("Save") && cb.setExternalEditor) cb.setExternalEditor(ui.externalEditorBuf);
#ifdef _WIN32
    dimWrapped("Program launched by the pencil button with the SoundFont path (e.g. "
               "C:\\Program Files\\Polyphone\\polyphone.exe). Right-click the SoundFont "
               "name to reload it after saving.");
#else
    dimWrapped("Program launched by the pencil button with the SoundFont path (e.g. "
               "polyphone). Right-click the SoundFont name to reload it after saving.");
#endif
}

void infoRow(const char* key, const std::string& value) {
    if (value.empty()) return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(kDimText, "%s", key);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

void drawOptionsInfo(SharedParams& params) {
    SoundFontInfo info;
    std::string path;
    size_t presetCount = 0, bankCount = 0;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        info = params.guiState.info;
        path = params.guiState.soundFontPath;
        presetCount = params.guiState.presets.size();
        std::vector<int> banks;
        for (const auto& p : params.guiState.presets) banks.push_back(p.bank);
        std::sort(banks.begin(), banks.end());
        bankCount = static_cast<size_t>(std::unique(banks.begin(), banks.end()) - banks.begin());
    }
    sectionHeader("Versions");
    if (ImGui::BeginTable("##versions", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
        ImGui::TableSetupColumn("d", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& c : componentVersions()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kDimText, "%s", c.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(kAccent, "%s", c.version.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(kDimText, "%s", c.note.c_str());
        }
        ImGui::EndTable();
    }

    sectionHeader("SoundFont");
    if (path.empty()) {
        ImGui::TextColored(kDimText, "No SoundFont loaded.");
    } else if (ImGui::BeginTable("##sfinfo", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.5f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        infoRow("Name", info.name);
        infoRow("File", path);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f MB", info.fileSize / (1024.0 * 1024.0));
        infoRow("Size", buf);
        snprintf(buf, sizeof(buf), "%zu presets in %zu banks", presetCount, bankCount);
        infoRow("Presets", buf);
        infoRow("Disables", describeControllers(info.disabledControllers));
        infoRow("Version", info.version);
        infoRow("Engine", info.engine);
        infoRow("ROM", info.romName);
        infoRow("Author", info.author);
        infoRow("Product", info.product);
        infoRow("Date", info.date);
        infoRow("Copyright", info.copyright);
        infoRow("Software", info.software);
        infoRow("Comment", info.comment);
        ImGui::EndTable();
    }
    sectionHeader("Program messages received");
    {
        const auto events = recentProgramEvents();
        if (events.empty()) {
            ImGui::TextColored(kDimText, "None - the patch saved with the project is in use.");
        } else {
            ImGui::BeginChild("##progevents", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 5.5f),
                              ImGuiChildFlags_Borders);
            for (const auto& e : events) ImGui::TextUnformatted(e.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.f);
            ImGui::EndChild();
            ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
            ImGui::TextWrapped("To keep the saved patch, turn off General > Program change > "
                               "Receive, or stop the controller sending them.");
            ImGui::PopStyleColor();
        }
    }
    sectionHeader("FluidSynth log");
    ImGui::Text("libfluidsynth %s", fluidRuntimeVersion().c_str());
    ImGui::BeginChild("##fluidlog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const auto& line : recentFluidLogLines()) {
        if (line.rfind("ERROR", 0) == 0 || line.rfind("PANIC", 0) == 0)
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.6f, 1.f), "%s", line.c_str());
        else if (line.rfind("WARN", 0) == 0)
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.5f, 1.f), "%s", line.c_str());
        else
            ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

// Floating, auto-dismissing notification over the bottom of the panel -
// SfzDrummer's drawErrorToast with an extra non-error (green) variant.
void drawToast(SharedParams& params, const Layout& L) {
    constexpr double kVisibleSeconds = 4.0;
    constexpr double kFadeSeconds = 0.6;
    std::string message;
    bool isError = false;
    double elapsed = 0.0;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        if (!params.guiState.toast.empty()) {
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                    params.guiState.toastSetAt)
                          .count();
            if (elapsed >= kVisibleSeconds) params.guiState.toast.clear();
            else {
                message = params.guiState.toast;
                isError = params.guiState.toastIsError;
            }
        }
    }
    if (message.empty()) return;
    float alpha = 1.0f;
    if (elapsed > kVisibleSeconds - kFadeSeconds)
        alpha = static_cast<float>(std::clamp((kVisibleSeconds - elapsed) / kFadeSeconds, 0.0, 1.0));

    // Over the bottom strip (gear + logo), so it never hides a control.
    const float w = L.px(440.f);
    ImGui::SetNextWindowPos(L.p(296.f, 580.f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.95f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushStyleColor(ImGuiCol_Text, isError ? ImVec4(1.0f, 0.70f, 0.70f, 1.0f)
                                                 : ImVec4(0.70f, 1.0f, 0.75f, 1.0f));
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar();
}

// ------------------------------------------------------------ main panel

void drawHeader(SharedParams& params, EditorUIState& ui, const EditorCallbacks& cb,
                const Layout& L, const std::vector<PresetEntry>& presets, const std::string& sfName,
                const std::string& sfPath, bool loading, const std::string& loadingName) {
    drawPanel(L, 8, 8, 544, 104, nullptr);
    if (drawButton(L, "##open", 16, 16, 40, 32, ICON_FA_FOLDER_O,
                   "Open a SoundFont (.sf2 / .sf3 / .dls)") &&
        cb.openSoundFontDialog)
        cb.openSoundFontDialog();

    std::string shown;
    ImU32 col = kColText;
    if (loading) {
        static const char* kSpin = "|/-\\";
        shown = std::string(1, kSpin[static_cast<int>(ImGui::GetTime() * 10.0) % 4]) +
                " Loading " + loadingName + " soundfont...";
        col = IM_COL32(255, 222, 140, 255);
    } else if (sfPath.empty()) {
        shown = "(drop a SoundFont here, or click the folder)";
        col = kColTextDim;
    } else {
        shown = sfName;
    }
    if (drawField(L, "##sfname", 62, 16, 438, 32, shown.c_str(), col) && cb.openSoundFontDialog)
        cb.openSoundFontDialog();
    if (ImGui::IsItemHovered() && !sfPath.empty())
        ImGui::SetTooltip("%s\nClick: open another SoundFont\nRight-click: reload / unload",
                          sfPath.c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##sfmenu");

    if (drawButton(L, "##edit", 506, 16, 38, 32, ICON_FA_PENCIL,
                   "Edit the SoundFont in the external editor (right-click: more)") &&
        cb.openInExternalEditor)
        cb.openInExternalEditor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##sfmenu");
    if (ImGui::BeginPopup("##sfmenu")) {
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN_O "  Open...") && cb.openSoundFontDialog)
            cb.openSoundFontDialog();
        ImGui::BeginDisabled(sfPath.empty() || loading);
        if (ImGui::MenuItem(ICON_FA_REFRESH "  Reload from disk") && cb.reloadSoundFont)
            cb.reloadSoundFont();
        if (ImGui::MenuItem(ICON_FA_PENCIL "  Edit in external editor") && cb.openInExternalEditor)
            cb.openInExternalEditor();
        if (ImGui::MenuItem(ICON_FA_TIMES "  Unload") && cb.unloadSoundFont) cb.unloadSoundFont();
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_STAR "  Set as default SoundFont") && cb.setDefaultSoundFont)
            cb.setDefaultSoundFont(sfPath);
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    L.text(18, 53, 11.f, kColTextDim, "PATCH NAME");
    const int bank = params.getInt(kParamBank), prog = params.getInt(kParamProgram);
    const std::string name = presetNameFor(presets, bank, prog);
    char buf[200];
    snprintf(buf, sizeof(buf), "[%04d:%03d]: %s%s", bank, prog,
             name.empty() ? "(none)" : name.c_str(),
             bank == 128 && !name.empty() ? " (DrumKit)" : "");
    if (drawField(L, "##patch", 16, 70, 484, 34, buf, name.empty() ? kColTextDim : kColText)) {
        ui.scrollToCurrentPatch = true;
        ImGui::OpenPopup("##patches");
    }
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) stepPatch(params, cb, presets, wheel > 0 ? -1 : 1);
        ImGui::SetTooltip("Click: patch selector  -  mouse wheel: previous / next");
    }
    if (drawButton(L, "##prev", 503, 70, 21, 34, ICON_FA_CARET_LEFT, "Previous patch", 16.f))
        stepPatch(params, cb, presets, -1);
    if (drawButton(L, "##next", 526, 70, 21, 34, ICON_FA_CARET_RIGHT, "Next patch", 16.f))
        stepPatch(params, cb, presets, +1);

    ImGui::SetNextWindowPos(L.p(16, 106));
    ImGui::SetNextWindowSize(ImVec2(L.px(528), L.px(390)));
    if (ImGui::BeginPopup("##patches")) {
        drawPatchBrowser(params, ui, cb, presets, !sfPath.empty());
        ImGui::EndPopup();
    }
}

// REVERB / CHORUS: SETUP (effect unit settings), ENABLE radio,
// send-override switch and send LEVEL knob.
void drawFxPanel(SharedParams& params, const EditorCallbacks& cb, const Layout& L, float x,
                 bool reverb) {
    const float y = 120;
    const char* label = reverb ? "REVERB" : "CHORUS";
    const ParamId on = reverb ? kParamReverbOn : kParamChorusOn;
    const ParamId ovr = reverb ? kParamReverbSendOverride : kParamChorusSendOverride;
    const ParamId send = reverb ? kParamReverbSend : kParamChorusSend;
    drawPanel(L, x, y, 268, 106, label);
    ImGui::PushID(label);
    if (drawButton(L, "##setup", x + 14, y + 34, 106, 30, ICON_FA_SLIDERS "  SETUP",
                   reverb ? "Built-in reverb settings" : "Built-in chorus settings", 13.f))
        ImGui::OpenPopup("##setup");
    drawRadio(L, params, cb, on, x + 14, y + 78, "ENABLE",
              reverb ? "Enable FluidSynth's built-in reverb" : "Enable FluidSynth's built-in chorus");
    const bool builtin = params.getInt(on) != 0;
    drawOverrideSwitch(L, params, cb, ovr, x + 160, y + 42, "Override Send");
    drawKnob(L, params, cb, send, x + 214, y + 52, 26, "LEVEL", builtin && params.getInt(ovr) != 0);

    ImGui::SetNextWindowPos(L.p(x + 14, y + 66));
    if (ImGui::BeginPopup("##setup")) {
        ImGui::TextColored(kAccent, "%s unit", label);
        ImGui::Separator();
        const float w = ImGui::GetFontSize() * 14.f;
        if (reverb) {
            paramCombo(params, cb, kParamReverbEngine, "Engine", kReverbEngineNames,
                       kNumReverbEngines);
            paramSliderFloat(params, cb, kParamReverbLevel, "Level", w);
            paramSliderFloat(params, cb, kParamReverbRoom, "Room size", w);
            paramSliderFloat(params, cb, kParamReverbDamp, "Damping", w);
            paramSliderFloat(params, cb, kParamReverbWidth, "Width", w);
        } else {
            paramSliderFloat(params, cb, kParamChorusLevel, "Level", w);
            paramSliderInt(params, cb, kParamChorusVoices, "Voices", w);
            paramSliderFloat(params, cb, kParamChorusSpeed, "Speed", w);
            paramSliderFloat(params, cb, kParamChorusDepth, "Depth", w);
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
        ImGui::TextUnformatted("Override Send on: every note's send is LEVEL,\n"
                               "replacing the preset's own and CC91/CC93.");
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void drawOptionsPopup(SharedParams& params, EditorUIState& ui, const EditorCallbacks& cb,
                      const Layout& L, const std::vector<PresetEntry>& presets) {
    ImGui::SetNextWindowPos(L.p(14, 14));
    ImGui::SetNextWindowSize(ImVec2(L.px(532), L.px(556)));
    if (!ImGui::BeginPopupModal("Options", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;
    // Debug-only: FLUIDSYNTHPLAYER_DEBUG_TAB=<tab> preselects a tab (see the
    // debug hook in drawEditorUI).
    static const char* debugTab = getenv("FLUIDSYNTHPLAYER_DEBUG_TAB");
    const bool appearing = ImGui::IsWindowAppearing();
    auto tabFlags = [&](const char* n) {
        return (debugTab && appearing && !strcmp(debugTab, n)) ? ImGuiTabItemFlags_SetSelected : 0;
    };
    const float closeH = ImGui::GetFrameHeightWithSpacing() + 6.f;
    ImGui::BeginChild("##optbody", ImVec2(0, -closeH));
    if (ImGui::BeginTabBar("##opttabs")) {
        if (ImGui::BeginTabItem("General", nullptr, tabFlags("General"))) {
            drawOptionsGeneral(params, ui, cb);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Channels", nullptr, tabFlags("Channels"))) {
            drawOptionsChannels(params, ui, presets);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced", nullptr, tabFlags("Advanced"))) {
            drawOptionsAdvanced(params, ui, cb);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info", nullptr, tabFlags("Info"))) {
            drawOptionsInfo(params);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(ImGui::GetFontSize() * 7.f, 0)) ||
        (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive()))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace

void drawEditorUI(SharedParams& params, EditorUIState& ui, const EditorCallbacks& cb) {
    if (ui.themedContext != ImGui::GetCurrentContext()) {
        ui.themedContext = ImGui::GetCurrentContext();
        applyTheme();
    }
    ImGuiIO& io = ImGui::GetIO();

    // Snapshot of the mutex-guarded state for this frame.
    std::string sfName, sfPath, loadingName;
    std::vector<PresetEntry> presets;
    bool loading = false;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        sfName = params.guiState.soundFontName;
        sfPath = params.guiState.soundFontPath;
        loading = params.guiState.loading;
        loadingName = params.guiState.loadingName;
        presets = params.guiState.presets;
        gen = params.guiState.loadGeneration;
    }
    if (gen != ui.seenLoadGeneration) {
        ui.seenLoadGeneration = gen;
        ui.scrollToCurrentPatch = true;
    }

    Layout L;
    L.s = std::max(0.5f, std::min(io.DisplaySize.x / kEditorDesignW,
                                  io.DisplaySize.y / kEditorDesignH));
    L.origin = ImVec2(std::floor((io.DisplaySize.x - kEditorDesignW * L.s) * 0.5f),
                      std::floor((io.DisplaySize.y - kEditorDesignH * L.s) * 0.5f));
    L.body = io.Fonts->Fonts[0];
    L.bold = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : io.Fonts->Fonts[0];

    ImGui::PushFont(L.body, 14.f * L.s);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(3);
    L.dl = ImGui::GetWindowDrawList();

    drawHeader(params, ui, cb, L, presets, sfName, sfPath, loading, loadingName);
    drawFxPanel(params, cb, L, 8, true);
    drawFxPanel(params, cb, L, 284, false);

    // ENVELOPE: with the switch next to the label on, A/H/D/S/R replace the
    // preset's volume envelope (absolute times / sustain level).
    {
        const float x = 8, y = 234;
        drawPanel(L, x, y, 268, 300, "ENVELOPE");
        drawOverrideSwitch(L, params, cb, kParamEnvEnable, x + 104, y + 4, "Envelope override");
        const bool on = params.getInt(kParamEnvEnable) != 0;
        const float top = y + 46, bot = y + 256;
        drawVSlider(L, params, cb, kParamEnvAttack, x + 38, top, bot, "A", on);
        drawVSlider(L, params, cb, kParamEnvHold, x + 86, top, bot, "H", on);
        drawVSlider(L, params, cb, kParamEnvDecay, x + 134, top, bot, "D", on);
        drawVSlider(L, params, cb, kParamEnvSustain, x + 182, top, bot, "S", on);
        drawVSlider(L, params, cb, kParamEnvRelease, x + 230, top, bot, "R", on);
    }
    // LFO (vibrato): AMT 0 leaves each preset's own vibrato alone; above 0
    // the three knobs replace it.
    {
        const float x = 284, y = 234;
        drawPanel(L, x, y, 268, 142, "LFO");
        const bool lfoOn = params.get(kParamLfoAmount) > 0.0;
        drawKnob(L, params, cb, kParamLfoDelay, x + 50, y + 74, 24, "DEL", lfoOn);
        drawKnob(L, params, cb, kParamLfoAmount, x + 134, y + 74, 24, "AMT");
        drawKnob(L, params, cb, kParamLfoSpeed, x + 218, y + 74, 24, "SPD", lfoOn);
    }
    // MISC: filter cutoff / resonance offsets.
    {
        const float x = 284, y = 384;
        drawPanel(L, x, y, 268, 150, "MISC");
        drawKnob(L, params, cb, kParamCutoff, x + 142, y + 76, 24, "CUT");
        drawKnob(L, params, cb, kParamResonance, x + 218, y + 76, 24, "RES");
    }

    if (drawButton(L, "##options", 10, 546, 30, 30, ICON_FA_COG, "Options", 18.f))
        ImGui::OpenPopup("Options");
    {
        const char* a = "FLUIDSYNTH ";
        const char* b = "PLAYER";
        const float size = 27.f;
        const float wa = L.textW(size, a), wb = L.textW(size, b);
        const float x = 548 - wa - wb;
        L.text(x, 548, size, kColLogoDark, a);
        L.text(x + wa, 548, size, kColLogoLight, b);
    }

    // Debug-only, opt-in: FLUIDSYNTHPLAYER_DEBUG_POPUP=patches|options|reverb
    // opens that popup once, so test/clap's headless screenshot run can check
    // it without synthetic clicks (same convention as the sibling projects'
    // *_DEBUG_* hooks). No effect when unset.
    {
        static const char* debugPopup = getenv("FLUIDSYNTHPLAYER_DEBUG_POPUP");
        static int debugFrames = 0;
        if (debugPopup && debugFrames++ == 3) {
            if (!strcmp(debugPopup, "patches")) ImGui::OpenPopup("##patches");
            else if (!strcmp(debugPopup, "options")) ImGui::OpenPopup("Options");
            else if (!strcmp(debugPopup, "reverb")) {
                ImGui::PushID("REVERB");
                ImGui::OpenPopup("##setup");
                ImGui::PopID();
            }
        }
    }
    drawOptionsPopup(params, ui, cb, L, presets);

    ImGui::End();
    drawToast(params, L);
    ImGui::PopFont();
}
