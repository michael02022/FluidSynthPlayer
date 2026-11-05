// FluidSynthPlayer's ImGui widget layer - draws the whole plugin window
// each frame. Same split as SfzDrummer/SoloSampler's editor_ui (sibling
// projects): it knows nothing about CLAP or FluidSynth, only reads/writes
// SharedParams and calls back into plugin.cpp for anything with side
// effects (loading, host notification, dialogs).
//
// Compact single-panel layout: SoundFont name + folder/edit buttons, patch
// name with prev/next arrows, REVERB / CHORUS (built-in + preset override +
// level), ENVELOPE sliders, LFO and MISC knobs, and an Options gear for
// everything else. Everything is drawn on a fixed 560x584 design grid
// scaled uniformly to the window size.
#pragma once

#include <functional>
#include <string>

#include "shared.hpp"

// GUI-thread-only interactive state - never touched by the audio thread.
struct EditorUIState {
    char presetFilter[128] = {};
    // Multitimbral mode: which channel (0-15) the patch browser assigns to.
    int browserChannel = 0;
    // Set when a new SoundFont finished loading / the browser opens, so the
    // patch list scrolls to the selected preset once.
    uint64_t seenLoadGeneration = 0;
    bool scrollToCurrentPatch = true;

    // Options > Advanced: editable copy of the FluidSynth settings text
    // (applied only on "Apply"), re-synced from GuiState while not dirty.
    std::string settingsDraft;
    bool settingsDraftDirty = false;
    char externalEditorBuf[512] = {};
    bool externalEditorBufInit = false;

    bool resetConfirmTrigger = false;
    const void* themedContext = nullptr; // ImGui context the theme was applied to
};

struct EditorCallbacks {
    std::function<void()> openSoundFontDialog; // pop-up file chooser
    std::function<void()> reloadSoundFont;
    std::function<void()> unloadSoundFont;
    std::function<void()> openInExternalEditor;
    std::function<void(const std::string&)> applyFluidSettings; // rebuilds the synth
    std::function<void(const std::string&)> setExternalEditor;
    // Default SoundFont for new instances: set it to a path ("" clears it),
    // or pick one with the pop-up file chooser.
    std::function<void(const std::string&)> setDefaultSoundFont;
    std::function<void()> chooseDefaultSoundFont;

    // Parameter edits: setParam stores the value and tells the host;
    // begin/endGesture bracket a continuous drag (CLAP param gestures).
    std::function<void(ParamId, double)> setParam;
    std::function<void(ParamId)> beginGesture;
    std::function<void(ParamId)> endGesture;
};

void drawEditorUI(SharedParams& params, EditorUIState& ui, const EditorCallbacks& cb);

// Design size of the panel (the window may be resized; everything scales).
inline constexpr float kEditorDesignW = 560.f;
inline constexpr float kEditorDesignH = 584.f;
