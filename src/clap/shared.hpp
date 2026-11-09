// State shared between the audio thread, the host's main thread and the GUI
// thread. Same structure as SfzDrummer/SoloSampler's shared.hpp (sibling
// projects): plain atomics for anything the audio thread touches, plus one
// mutex-protected GuiState block for the bulky, non-realtime data (SoundFont
// path, preset list, status text...).
//
// Feature set: one SoundFont per instance, Bank/Patch selection with a
// preset browser, a volume-envelope override (ADSR),
// vibrato LFO, filter cutoff/resonance, built-in reverb/chorus with their own
// on/off switches, plus the FluidSynth-specific extras this fork has always
// exposed (polyphony, interpolation, raw fluidsynth settings).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

inline constexpr int kNumMidiChannels = 16;

// ------------------------------------------------------------------ params
//
// Every value here is a CLAP parameter (clap.params) - automatable unless
// marked otherwise in kParamInfos. The numeric ids are persisted in the
// host's project (automation lanes) and in the plugin state, so they must
// never be renumbered: append new ones at the end only.
enum ParamId : uint32_t {
    kParamGain = 0,
    kParamBank,
    kParamProgram,

    kParamEnvEnable,
    kParamEnvAttack,
    kParamEnvDecay,
    kParamEnvSustain,
    kParamEnvRelease,

    kParamLfoAmount,
    kParamLfoSpeed,
    kParamLfoDelay,

    kParamCutoff,
    kParamResonance,

    kParamReverbOn,
    kParamReverbLevel,
    kParamReverbRoom,
    kParamReverbDamp,
    kParamReverbWidth,

    kParamChorusOn,
    kParamChorusLevel,
    kParamChorusVoices,
    kParamChorusSpeed,
    kParamChorusDepth,

    kParamPolyphony,
    kParamBendRange,
    kParamTranspose,
    kParamFineTune,
    kParamInterpolation,
    kParamMidiMode,
    kParamReceivePC,
    kParamVoiceMode,
    kParamPortamento,

    // REVERB/CHORUS panels' "Override Send" switch + LEVEL knob: when on,
    // LEVEL is added to every voice's send amount into the built-in effect
    // (on top of what the SoundFont preset and CC91/CC93 ask for).
    kParamReverbSendOverride,
    kParamReverbSend,
    kParamChorusSendOverride,
    kParamChorusSend,

    // Some SoundFonts (e.g. converted E-mu EMAX II libraries) override the
    // standard SF2 MIDI modulators with zero amounts, which silently
    // disables CC7 volume, CC10 pan, pitch bend... When on, those
    // modulators are put back on every new voice - only the ones the
    // loaded SoundFont actually zeroes (see SoundFontInfo::disabledControllers).
    kParamRestoreControllers,

    // FluidSynth 2.6 (FluidSynthPlugin 0.8.0 exposes the same settings).
    // Engine and limiter are creation-time synth settings: changing them
    // rebuilds the synth (see isRebuildParam), so they aren't automatable.
    kParamReverbEngine,
    kParamLimiterOn,
    kParamLimiterCeiling,
    kParamLimiterRelease,
    kParamPortamentoTimeMode,

    // Interpolation used while the host renders offline (clap.render), like
    // FluidSynthPlugin's "realtime;offline" interpolation pair: 0 = same as
    // kParamInterpolation, n = kInterpValues[n - 1].
    kParamRenderInterpolation,

    // ENVELOPE panel's H fader (added after the others: ids never move).
    kParamEnvHold,

    // Editor size: 100% / 75% of the design size, for hosts without
    // free plugin window resizing. Saved with the project, hidden from hosts.
    kParamUiScale,

    kNumParams
};

// kParamMidiMode values
enum MidiMode : int {
    kMidiModeOmni = 0,  // every incoming channel plays the selected patch
    kMidiModeMulti = 1, // GM-style multitimbral: 16 channels, own programs
};

// kParamVoiceMode values (channel 1 / omni only - see FluidEngine.cpp)
enum VoiceMode : int {
    kVoicePoly = 0,
    kVoiceMono = 1,
    kVoiceLegato = 2,
};

// kParamInterpolation values -> fluid_interp enum. FluidSynth 2.6 replaced
// the old 7-point sinc (value 7) by 11/17/25-point sinc (5, 6, 7); index 3,
// which used to mean "7th order", now maps to the closest new one (sinc 11)
// so old projects don't silently jump to the very slow 25-point sinc.
inline constexpr int kNumInterpModes = 6;
inline constexpr int kInterpValues[kNumInterpModes] = {0, 1, 4, 5, 6, 7};
inline constexpr const char* kInterpNames[kNumInterpModes] = {
    "None", "Linear", "4th order", "Sinc 11th order", "Sinc 17th order",
    "Sinc 25th order (offline)"};

// kParamRenderInterpolation display names: "Same as playback" + kInterpNames.
inline constexpr const char* kRenderInterpNames[kNumInterpModes + 1] = {
    "Same as playback", "None", "Linear", "4th order", "Sinc 11th order", "Sinc 17th order",
    "Sinc 25th order"};

// kParamReverbEngine values -> "synth.reverb.engine" (FluidSynth 2.6).
// Dattorro is 2.6's default; FluidSynth 2.5 and older always used FDN, which
// is what a project saved before this parameter existed gets (stateLoad).
inline constexpr int kNumReverbEngines = 5;
inline constexpr const char* kReverbEngineSettings[kNumReverbEngines] = {"dat", "fdn", "free",
                                                                        "lex", "smith"};
inline constexpr const char* kReverbEngineNames[kNumReverbEngines] = {
    "Dattorro", "FDN (FluidSynth <= 2.5)", "Freeverb", "Lexicon", "Signalsmith"};
inline constexpr int kReverbEngineFdn = 1;

// kParamPortamentoTimeMode values -> fluid_portamento_time_mode.
inline constexpr const char* kPortaTimeModeNames[3] = {"Auto (7/14-bit)", "XG/GS (7-bit CC5)",
                                                       "Linear (14-bit CC5+37)"};
inline constexpr const char* kMidiModeNames[2] = {"Omni (single patch)", "Multitimbral (GM)"};
inline constexpr int kNumUiScales = 2;
inline constexpr const char* kUiScaleNames[kNumUiScales] = {"100%", "75%"};
inline constexpr double kUiScaleFactors[kNumUiScales] = {1.0, 0.75};
inline constexpr const char* kVoiceModeNames[3] = {"Poly", "Mono", "Legato"};

struct ParamInfo {
    ParamId id;
    const char* name;
    const char* module; // CLAP "module" path, groups params in host UIs
    double min, max, def;
    bool stepped;
    bool automatable;
};

// Ranges and units (see FluidEngine.cpp's applyParams for the generator
// math behind each one):
//  - Envelope (while its override switch is on) *replaces* the volume
//    envelope of every new note: A/H/D/R absolute times in seconds (hold 0
//    = none), S the sustain level in dB below full (0 dB = no decay). The
//    preset keeps its own delay and modulation envelope.
//  - LFO: at Amount 0 the preset's vibrato is untouched; above 0, amount
//    (cents), speed (Hz) and delay (s) replace the preset's vibrato.
//  - Cutoff is a shift in semitones; Resonance an added Q in dB.
inline constexpr ParamInfo kParamInfos[kNumParams] = {
    {kParamGain, "Volume", "Main", 0.0, 2.0, 0.5, false, true},
    {kParamBank, "Bank", "Main", 0.0, 128.0, 0.0, true, true},
    {kParamProgram, "Patch", "Main", 0.0, 127.0, 0.0, true, true},

    {kParamEnvEnable, "Envelope On", "Envelope", 0.0, 1.0, 0.0, true, true},
    {kParamEnvAttack, "Attack", "Envelope", 0.001, 20.0, 0.005, false, true},
    {kParamEnvDecay, "Decay", "Envelope", 0.001, 20.0, 1.0, false, true},
    {kParamEnvSustain, "Sustain", "Envelope", -100.0, 0.0, 0.0, false, true},
    {kParamEnvRelease, "Release", "Envelope", 0.001, 20.0, 0.3, false, true},

    {kParamLfoAmount, "LFO Amount", "LFO", 0.0, 200.0, 0.0, false, true},
    {kParamLfoSpeed, "LFO Speed", "LFO", 0.1, 20.0, 5.0, false, true},
    {kParamLfoDelay, "LFO Delay", "LFO", 0.0, 5.0, 0.0, false, true},

    {kParamCutoff, "Cutoff", "Filter", -96.0, 24.0, 0.0, false, true},
    {kParamResonance, "Resonance", "Filter", 0.0, 40.0, 0.0, false, true},

    {kParamReverbOn, "Reverb On", "Reverb", 0.0, 1.0, 1.0, true, true},
    {kParamReverbLevel, "Reverb Level", "Reverb", 0.0, 1.0, 0.9, false, true},
    {kParamReverbRoom, "Reverb Room", "Reverb", 0.0, 1.0, 0.2, false, true},
    {kParamReverbDamp, "Reverb Damp", "Reverb", 0.0, 1.0, 0.0, false, true},
    {kParamReverbWidth, "Reverb Width", "Reverb", 0.0, 100.0, 0.5, false, true},

    {kParamChorusOn, "Chorus On", "Chorus", 0.0, 1.0, 1.0, true, true},
    {kParamChorusLevel, "Chorus Level", "Chorus", 0.0, 10.0, 2.0, false, true},
    {kParamChorusVoices, "Chorus Voices", "Chorus", 0.0, 99.0, 3.0, true, false},
    {kParamChorusSpeed, "Chorus Speed", "Chorus", 0.1, 5.0, 0.3, false, true},
    {kParamChorusDepth, "Chorus Depth", "Chorus", 0.0, 64.0, 8.0, false, true},

    {kParamPolyphony, "Polyphony", "Settings", 1.0, 1024.0, 256.0, true, false},
    {kParamBendRange, "Bend Range", "Settings", 0.0, 48.0, 2.0, true, false},
    {kParamTranspose, "Transpose", "Settings", -36.0, 36.0, 0.0, true, true},
    {kParamFineTune, "Fine Tune", "Settings", -100.0, 100.0, 0.0, false, true},
    {kParamInterpolation, "Interpolation", "Settings", 0.0, 5.0, 2.0, true, false},
    {kParamMidiMode, "MIDI Mode", "Settings", 0.0, 1.0, 0.0, true, false},
    {kParamReceivePC, "Receive Program Change", "Settings", 0.0, 1.0, 1.0, true, false},
    {kParamVoiceMode, "Voice Mode", "Settings", 0.0, 2.0, 0.0, true, false},
    {kParamPortamento, "Portamento", "Settings", 0.0, 127.0, 0.0, true, true},

    {kParamReverbSendOverride, "Reverb Preset Override", "Reverb", 0.0, 1.0, 0.0, true, true},
    {kParamReverbSend, "Reverb Send", "Reverb", 0.0, 1.0, 0.3, false, true},
    {kParamChorusSendOverride, "Chorus Preset Override", "Chorus", 0.0, 1.0, 0.0, true, true},
    {kParamChorusSend, "Chorus Send", "Chorus", 0.0, 1.0, 0.3, false, true},

    {kParamRestoreControllers, "Restore MIDI Controllers", "Settings", 0.0, 1.0, 0.0, true, false},

    {kParamReverbEngine, "Reverb Engine", "Reverb", 0.0, 4.0, 0.0, true, false},
    {kParamLimiterOn, "Limiter", "Output", 0.0, 1.0, 0.0, true, false},
    {kParamLimiterCeiling, "Limiter Ceiling", "Output", -24.0, 0.0, 0.0, false, false},
    {kParamLimiterRelease, "Limiter Release", "Output", 1.0, 250.0, 40.0, false, false},
    {kParamPortamentoTimeMode, "Portamento Time Mode", "Settings", 0.0, 2.0, 0.0, true, false},
    // default: 25th-order sinc, upstream FluidSynthPlugin's default ("4;7")
    {kParamRenderInterpolation, "Render Interpolation", "Settings", 0.0, double(kNumInterpModes),
     double(kNumInterpModes), true, false},
    {kParamEnvHold, "Hold", "Envelope", 0.0, 20.0, 0.0, false, true},
    {kParamUiScale, "UI Size", "Settings", 0.0, double(kNumUiScales - 1), 0.0, true, false},
};

// Standard SF2 default modulators (SF2.01 section 8.4) a SoundFont can
// disable by redefining them with amount 0 - bit flags.
enum StandardController : uint32_t {
    kCtlModWheel = 1u << 0,    // CC1  -> vibrato depth
    kCtlVolume = 1u << 1,      // CC7  -> attenuation
    kCtlPan = 1u << 2,         // CC10 -> pan
    kCtlExpression = 1u << 3,  // CC11 -> attenuation
    kCtlReverb = 1u << 4,      // CC91 -> reverb send
    kCtlChorus = 1u << 5,      // CC93 -> chorus send
    kCtlPitchBend = 1u << 6,   // pitch wheel -> fine tune
    kCtlAftertouch = 1u << 7,  // channel pressure -> vibrato depth
    kNumStandardControllers = 8,
};
inline constexpr const char* kStandardControllerNames[kNumStandardControllers] = {
    "CC1 modulation", "CC7 volume", "CC10 pan", "CC11 expression",
    "CC91 reverb", "CC93 chorus", "pitch bend", "channel aftertouch"};

// One SoundFont preset as listed by the patch browser.
struct PresetEntry {
    int bank = 0;
    int program = 0;
    std::string name;
};

// RIFF "INFO" chunk of the loaded SoundFont (see state/SoundFontInfo.h),
// shown in Options > Info.
struct SoundFontInfo {
    std::string version;   // ifil, e.g. "2.01"
    std::string engine;    // isng
    std::string name;      // INAM
    std::string romName;   // irom
    std::string date;      // ICRD
    std::string author;    // IENG
    std::string product;   // IPRD
    std::string copyright; // ICOP
    std::string comment;   // ICMT
    std::string software;  // ISFT
    uint64_t fileSize = 0;
    // StandardController bits this SoundFont disables (zero-amount
    // instrument modulators identical to the SF2 defaults).
    uint32_t disabledControllers = 0;
};

struct SharedParams {
    SharedParams() {
        for (uint32_t i = 0; i < kNumParams; ++i) {
            values[i].store(kParamInfos[i].def);
            guiGestureBegin[i].store(false);
            guiGestureEnd[i].store(false);
            guiValueDirty[i].store(false);
        }
        for (int c = 0; c < kNumMidiChannels; ++c) {
            channelBank[c].store(c == 9 ? 128 : 0);
            channelProgram[c].store(0);
            pendingChannelPatch[c].store(-1);
        }
    }

    // Plain (non-normalized) parameter values, indexed by ParamId. Written
    // by the audio thread (host automation), the GUI thread (user edits) and
    // the main thread (state load); read by everyone.
    std::array<std::atomic<double>, kNumParams> values;

    double get(ParamId id) const { return values[id].load(); }
    int getInt(ParamId id) const { return static_cast<int>(std::lround(values[id].load())); }

    // GUI -> host notification (see plugin.cpp's flushGuiParamEvents): the GUI
    // sets these and asks the host for a flush; the next process()/flush()
    // emits the matching CLAP_EVENT_PARAM_GESTURE_BEGIN/VALUE/END events so
    // the host can record automation and mark the project dirty.
    std::array<std::atomic<bool>, kNumParams> guiGestureBegin;
    std::array<std::atomic<bool>, kNumParams> guiValueDirty;
    std::array<std::atomic<bool>, kNumParams> guiGestureEnd;

    // Per-channel patch, mirrored from the engine by the audio thread after
    // every program change (MIDI or GUI) - drives the multitimbral channel
    // table. Channel 0 in omni mode always matches kParamBank/kParamProgram.
    std::array<std::atomic<int>, kNumMidiChannels> channelBank;
    std::array<std::atomic<int>, kNumMidiChannels> channelProgram;
    // GUI -> audio handoff (multitimbral mode): (bank << 8) | program, or -1.
    std::array<std::atomic<int>, kNumMidiChannels> pendingChannelPatch;

    // GUI piano -> audio thread, same one-held-key convention as the
    // sibling projects.
    std::atomic<bool> pendingPreviewNoteOn{false};
    std::atomic<int> previewNoteOnNumber{0};
    std::atomic<int> previewNoteOnVelocity{0};
    std::atomic<bool> pendingPreviewNoteOff{false};
    std::atomic<int> previewNoteOffNumber{0};
    // Which channel the piano plays (multitimbral mode: the selected row of
    // the channel table; omni: always 0).
    std::atomic<int> previewChannel{0};
    // GUI "Panic" button -> audio thread: all sounds off on every channel.
    std::atomic<bool> pendingPanic{false};
    // Host -> audio thread: true while the host renders offline (set by
    // clap.render on the main thread, read by applyParams).
    std::atomic<bool> offlineRender{false};

    // Audio -> GUI visualization.
    std::array<std::atomic<bool>, 128> noteActive{};
    std::array<std::atomic<float>, 128> noteVelocity01{};
    std::atomic<int> activeVoiceCount{0};
    std::array<std::atomic<float>, kNumMidiChannels> channelActivity{}; // decaying 0..1
    std::atomic<float> peakL{0.f}, peakR{0.f};

    // Bulky GUI-facing data, guarded by its own mutex: the host's main
    // thread (state save/load), the loader thread and the GUI thread all
    // touch it concurrently. Never locked by the audio thread.
    struct GuiState {
        std::mutex mutex;

        std::string soundFontPath;  // "" = nothing loaded
        std::string soundFontName;  // display name (file name or INAM)
        std::vector<PresetEntry> presets;
        SoundFontInfo info;
        bool loading = false;
        std::string loadingName;    // "Loading <name> soundfont..."
        uint64_t loadGeneration = 0; // bumped on every finished (re)load

        // Raw "key = value" FluidSynth settings applied before the synth is
        // created - the original FluidSynthPlugin's whole UI, kept here as
        // the "Advanced" tab. Changing it needs a rebuild (Apply button).
        std::string fluidSettingsText;

        // Global, cross-instance preferences (config file, see plugin.cpp's
        // loadGlobalConfig): the external SoundFont editor executable and
        // the folder the "Open SoundFont" dialog starts in.
        // Default: Polyphone - on Windows by the path its installer uses
        // (it isn't on PATH there).
#ifdef _WIN32
        std::string externalEditor = "C:\\Program Files\\Polyphone\\polyphone.exe";
#else
        std::string externalEditor = "polyphone";
#endif
        std::string lastDir;
        // SoundFont a brand-new instance starts with ("" = none) - never
        // applied when the host restores a saved state (see plugin.cpp's
        // maybeLoadDefaultSoundFont).
        std::string defaultSoundFont;
        // UI Size (kParamUiScale index) a brand-new instance opens with: the
        // last one chosen in any instance.
        int uiScale = 0;

        // Toast message (errors and notices) with its timestamp, auto-
        // dismissed by editor_ui.cpp's drawToast.
        std::string toast;
        bool toastIsError = false;
        std::chrono::steady_clock::time_point toastSetAt{};
    } guiState;
};

// Parameters that are FluidSynth *creation-time* settings: a change needs a
// new synth (plugin.cpp rebuilds it when the GUI gesture ends).
inline bool isRebuildParam(uint32_t id) {
    return id == kParamReverbEngine || id == kParamLimiterOn || id == kParamLimiterCeiling ||
           id == kParamLimiterRelease;
}

// Formats a parameter's plain value for display - shared by clap.params'
// value_to_text and the GUI so both always agree.
std::string formatParamValue(ParamId id, double value);

// "CC7 volume, CC10 pan, ..." for a StandardController bit mask.
inline std::string describeControllers(uint32_t mask) {
    std::string out;
    for (int b = 0; b < kNumStandardControllers; ++b)
        if (mask & (1u << b)) out += (out.empty() ? "" : ", ") + std::string(kStandardControllerNames[b]);
    return out;
}
