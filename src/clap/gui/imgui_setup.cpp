#include "imgui_setup.hpp"

#include "imgui.h"

#include "IconsFontAwesome4.h"
#include "fontawesome_data.hpp"

// Thread-local ImGui context definition (see imconfig.h): on Linux each
// plugin instance renders on its own thread with its own context; on
// Windows all editors share the host's UI thread and every entry point
// switches to its own context (and restores the previous one) explicitly.
thread_local ImGuiContext* g_sclImGuiTls = nullptr;

void setupEditorImGui() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini in the DAW's CWD
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScrollbarSize = 16.f;

    // Sizes are only the reference size: ImGui 1.92 rasterizes any size on
    // demand (PushFont(font, size)).
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    auto addFont = [&](unsigned char* data, unsigned int len) {
        ImFontConfig cfg;
        // The buffers are global arrays embedded in the binary, not
        // memory allocated by ImGui -- it must not free them.
        cfg.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF(data, (int)len, 14.f, &cfg);
        ImFontConfig iconCfg;
        iconCfg.MergeMode = true;
        iconCfg.PixelSnapH = true;
        iconCfg.FontDataOwnedByAtlas = false;
        iconCfg.GlyphMinAdvanceX = 14.f;
        io.Fonts->AddFontFromMemoryTTF(fontawesome_webfont_ttf, (int)fontawesome_webfont_ttf_len,
                                       14.f, &iconCfg, iconRanges);
    };
    addFont(Roboto_Medium_ttf, Roboto_Medium_ttf_len);
    addFont(Roboto_Bold_ttf, Roboto_Bold_ttf_len);
}
