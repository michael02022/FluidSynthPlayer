// Thin wrapper around one libfluidsynth synthesizer with one SoundFont
// loaded. An instance is always *built* off the audio thread (main thread or
// the plugin's loader thread - loading a SoundFont can take seconds) and only
// then handed over to the audio thread, which owns it exclusively from that
// point on (see plugin.cpp's swap protocol). That's why the synth is created
// with synth.threadsafe-api = 0: after the handover exactly one thread ever
// calls into it.
//
// Every SharedParams value is pushed into the synth lazily by applyParams()
// at the start of each audio block - only the values that actually changed
// since the last block cost anything.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "shared.hpp"

typedef struct _fluid_hashtable_t fluid_settings_t;
typedef struct _fluid_synth_t fluid_synth_t;
typedef struct _fluid_voice_t fluid_voice_t;

struct FluidInstance {
    ~FluidInstance();

    fluid_settings_t* settings = nullptr;
    fluid_synth_t* synth = nullptr;
    int sfontId = -1;
    double sampleRate = 44100.0;
    std::string path;
    std::string settingsText; // the Advanced-tab text this was built with

    // ---- audio-thread-only bookkeeping (see FluidEngine.cpp) ----

    // Last value pushed into the synth per parameter; NaN = never applied,
    // so the first applyParams() after the handover applies everything.
    std::array<double, kNumParams> applied{};
    bool channelsRestored = false;
    // fluid_interp value currently set on every channel (-1 = none yet):
    // depends on kParamInterpolation, kParamRenderInterpolation and
    // whether the host is rendering offline.
    int appliedInterp = -1;

    // Transpose bookkeeping: which (already transposed) key a held
    // (channel, incoming key) actually started, so its note-off still hits
    // the right voice even if Transpose changed in between. -1 = not held.
    std::array<std::array<int16_t, 128>, kNumMidiChannels> heldKey{};

    // Omni mode's own bank-select tracking (multitimbral mode lets
    // fluidsynth handle CC0/CC32 per channel itself).
    int omniBankMsb = -1;

    // StandardController bits the loaded SoundFont disables (see
    // scanDisabledControllers) and the "Restore MIDI controllers" patching
    // state: voices are patched right after their note-on, before they
    // render a single sample. voiceBuf is preallocated (no allocation on
    // the audio thread); lastPatchedVoiceId skips voices already patched.
    uint32_t disabledControllers = 0;
    std::vector<fluid_voice_t*> voiceBuf;
    // -1 = none yet: a fresh synth numbers its first note's voices 0.
    int64_t lastPatchedVoiceId = -1;

    // Set by handleMidi when an incoming Program Change moved channel 1's
    // patch: plugin.cpp then tells the host the Bank/Patch params changed.
    bool programChangedByMidi = false;
};

// Last value of every continuous MIDI controller (plus pitch bend and
// channel pressure) per *synth* channel, recorded by handleMidi. A
// FluidInstance is replaced whenever a SoundFont is (re)loaded or the synth
// rebuilt, and a fresh fluidsynth synth starts with default controllers -
// replayControllers() restores what the host already sent (CC7 volume,
// CC10 pan, CC11 expression, CC64 sustain...), which the host won't resend
// until the next CC event. Audio thread only.
struct ControllerShadow {
    ControllerShadow() { reset(); }
    void reset();
    void resetChannel(int ch);

    std::array<std::array<int16_t, 128>, kNumMidiChannels> cc{}; // -1 = never received
    std::array<int, kNumMidiChannels> pitchBend{};                 // -1 = never received
    std::array<int, kNumMidiChannels> channelPressure{};          // -1 = never received
};

// Builds a fresh synthesizer at `sampleRate`, applies the user's raw
// FluidSynth settings text and loads `path` (may be empty: a synth with no
// SoundFont is valid and silent). On success fills `presets` (sorted by
// bank, then program) and returns the instance; `warnings` collects
// non-fatal problems (unknown settings keys...). Returns nullptr and sets
// `error` if the SoundFont can't be loaded. Never call from the audio thread.
// Creation-time synth settings driven by plugin parameters (FluidSynth 2.6
// reverb engine and output limiter), applied after the user's raw settings
// text so the panel always reflects what's running.
struct EngineSetup {
    int reverbEngine = 0;  // index into kReverbEngineSettings
    bool limiter = false;
    double limiterCeilingDb = 0.0;
    double limiterReleaseMs = 40.0;
};
EngineSetup engineSetupFrom(const SharedParams& params);

std::unique_ptr<FluidInstance> buildFluidInstance(const std::string& path, double sampleRate,
                                                  const std::string& settingsText,
                                                  const EngineSetup& setup,
                                                  std::vector<PresetEntry>& presets,
                                                  std::vector<std::string>& warnings,
                                                  std::string& error);

// ---- audio thread only ----

// Pushes every changed SharedParams value (and pending GUI channel-patch
// requests) into the synth.
void applyParams(FluidInstance& inst, SharedParams& params);

// Feeds one MIDI 1.0 channel message (status byte includes the channel),
// honoring MIDI mode, transpose and program-change reception.
void handleMidi(FluidInstance& inst, SharedParams& params, ControllerShadow& shadow,
                uint8_t status, uint8_t d1, uint8_t d2);

// Re-sends every controller recorded in `shadow` to a freshly swapped-in
// instance (see ControllerShadow).
void replayControllers(FluidInstance& inst, const ControllerShadow& shadow);

// SysEx payload *without* the leading F0 / trailing F7 (GM/GS/XG resets,
// MIDI tuning...).
void handleSysex(FluidInstance& inst, const uint8_t* data, uint32_t size);

void renderFluid(FluidInstance& inst, float* left, float* right, uint32_t frames);

void allSoundsOff(FluidInstance& inst);

int fluidActiveVoices(FluidInstance& inst);

// Program-related MIDI received (Program Change, Bank Select, GM/GS/XG
// reset SysEx), newest last, formatted for the Info tab - shows where an
// unexpected patch change came from (e.g. a controller that sends PC 0
// when it connects). Lock-free ring written by the audio thread.
std::vector<std::string> recentProgramEvents();

// Installs a process-wide FluidSynth log hook that keeps the last messages
// for the GUI's Info tab (fluidsynth's logging is global, not per synth).
void installFluidLogHook();
std::vector<std::string> recentFluidLogLines();

// Version string of the libfluidsynth actually loaded at runtime.
std::string fluidRuntimeVersion();

// Every component that makes up the plugin with its version, for the Info
// tab: {name, version, note}.
struct ComponentVersion {
    std::string name, version, note;
};
std::vector<ComponentVersion> componentVersions();
