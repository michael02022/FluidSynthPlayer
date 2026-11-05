// ImGui setup shared by the platform windows (gui_window_x11.cpp,
// gui_window_win32.cpp): style and the embedded fonts.
#pragma once

// Configures the *current* ImGui context for the editor: no imgui.ini, dark
// style, and the embedded fonts (see fontawesome_data.hpp):
//   Fonts[0] = Roboto Medium  - body text, popups
//   Fonts[1] = Roboto Bold    - panel labels/fields
// each with Font Awesome 4 merged on top so a string can mix icon + text
// (ICON_FA_FOLDER " Open") without PushFont calls.
void setupEditorImGui();
