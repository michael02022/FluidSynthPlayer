// CLAP glue for FluidSynthPlayer: entry point, factory, plugin, ports,
// params, state and GUI. Structure follows SfzDrummer/SoloSampler's
// plugin.cpp (sibling projects); the engine is libfluidsynth instead of
// sfizz.
//
// Threads involved:
//  - host main thread: activate/deactivate, state, params info, GUI setup
//  - audio thread: process() - owns `Plugin::active` exclusively
//  - GUI thread (GuiWindow): draws, and requests loads/param changes
//  - loader thread: builds new FluidInstances (SoundFont loading can take
//    seconds) and hands them to the audio thread through `incoming`
//
// Handover protocol (lock-free, the audio thread never blocks or frees):
//   loader:  old = incoming.exchange(fresh); delete old   (never seen by audio)
//   audio:   if (n = incoming.exchange(null)) { retire(active); active = n }
//   main/loader: collectGarbage() deletes whatever audio retired.
#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <fluidsynth.h> // FLUIDSYNTH_VERSION (vendored 2.6.2)

#include "engine/FluidEngine.h"
#include "gui/editor_ui.hpp"
#include "gui/gui_window.hpp"
#include "gui/file_dialog.h"
#include "platform.hpp"
#include "shared.hpp"
#include "state/SoundFontInfo.h"

namespace fs = std::filesystem;
using platform::fileName;
using platform::u8path;

namespace {

constexpr uint32_t kDefaultW = static_cast<uint32_t>(kEditorDesignW),
                   kDefaultH = static_cast<uint32_t>(kEditorDesignH);
constexpr int kRetiredSlots = 8;

#ifdef _WIN32
const char* const kGuiApi = CLAP_WINDOW_API_WIN32;
#else
const char* const kGuiApi = CLAP_WINDOW_API_X11;
#endif

// Last sample rate any instance was activated at - the best guess for
// building a SoundFont before activation (state restored on project load),
// so the usual "same rate as last time" case doesn't load it twice.
std::atomic<double> g_lastSampleRate{44100.0};

struct Plugin {
    clap_plugin_t plug{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* hostParams = nullptr;
    std::unique_ptr<GuiWindow> window;
    SharedParams params;
    EditorUIState uiState;

    // ---- engine handover (see the header comment) ----
    FluidInstance* active = nullptr; // audio thread (or main while inactive)
    std::atomic<FluidInstance*> incoming{nullptr};
    std::array<std::atomic<FluidInstance*>, kRetiredSlots> retired{};
    std::atomic<double> sampleRate{44100.0};
    std::atomic<bool> activated{false};

    // Whether this instance's starting SoundFont has been decided: set by a
    // state restore (the project's own SoundFont wins, even if it's none),
    // by any load the user starts, or once the default SoundFont was applied.
    std::atomic<bool> initialSoundFontDecided{false};

    // Debug log only (FLUIDSYNTHPLAYER_DEBUG_MIDI_LOG): did the host ever
    // switch this instance to offline rendering?
    std::atomic<bool> offlineRenderSeen{false};

    // MIDI controller values the host has sent (audio thread only), and a
    // flag set whenever `active` is replaced so the next process() restores
    // them on the new synth - see ControllerShadow.
    ControllerShadow controllers;
    std::atomic<bool> replayControllersPending{false};

    // ---- loader thread ----
    struct LoadJob {
        std::string path;
        std::string settingsText;
        bool fromGui = false; // GUI loads may auto-pick the first preset
    };
    std::thread loaderThread;
    std::mutex loaderMutex;
    std::condition_variable loaderCv;
    bool hasJob = false;
    bool loaderBusy = false;
    bool loaderQuit = false;
    LoadJob job;

    explicit Plugin(const clap_host_t* h) : host(h) { sampleRate.store(g_lastSampleRate.load()); }
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// ---------------------------------------------------------------- helpers

void setToast(Plugin* p, std::string message, bool isError) {
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    p->params.guiState.toast = std::move(message);
    p->params.guiState.toastIsError = isError;
    p->params.guiState.toastSetAt = std::chrono::steady_clock::now();
}

void requestParamFlush(Plugin* p) {
    if (p->hostParams && p->hostParams->request_flush) p->hostParams->request_flush(p->host);
}

// GUI/loader-originated parameter change: store it and make sure the host
// hears about it (emitted as CLAP events by the next process()/flush()).
void setParamFromUi(Plugin* p, ParamId id, double value) {
    const auto& info = kParamInfos[id];
    value = std::clamp(value, info.min, info.max);
    p->params.values[id].store(value);
    p->params.guiValueDirty[id].store(true);
    requestParamFlush(p);
}

void collectGarbage(Plugin* p) {
    for (auto& slot : p->retired)
        if (FluidInstance* dead = slot.exchange(nullptr)) delete dead;
}

// Audio thread: parks an instance for collectGarbage(). With all slots full
// (only possible if the host never runs its main thread) the instance is
// kept alive by leaking it rather than freeing on the audio thread.
void retire(Plugin* p, FluidInstance* inst) {
    if (!inst) return;
    for (auto& slot : p->retired) {
        FluidInstance* expected = nullptr;
        if (slot.compare_exchange_strong(expected, inst)) {
            if (p->host && p->host->request_callback) p->host->request_callback(p->host);
            return;
        }
    }
}

std::string displayNameFor(const std::string& path, const SoundFontInfo& info) {
    const std::string file = fileName(path);
    if (info.name.empty() || info.name == file) return file;
    return info.name + "  -  " + file;
}

// ---------------------------------------------------------- global config
//
// Cross-instance preferences (external editor, last folder, default
// SoundFont): one small text file.

// ~/.config/FluidSynthPlayer/config.txt on Linux,
// %APPDATA%/FluidSynthPlayer/config.txt on Windows (see platform.hpp).
fs::path configFilePath() {
    return platform::userConfigDir() / "FluidSynthPlayer" / "config.txt";
}

void loadGlobalConfig(Plugin* p) {
    std::ifstream in(configFilePath());
    if (!in) return;
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    auto& gs = p->params.guiState;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("lastdir=", 0) == 0) gs.lastDir = line.substr(8);
        else if (line.rfind("defaultsf=", 0) == 0) gs.defaultSoundFont = line.substr(10);
        else if (line.rfind("editor=", 0) == 0) gs.externalEditor = line.substr(7);
    }
}

void saveGlobalConfig(Plugin* p) {
    std::string editor, lastDir, defaultSf;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        editor = p->params.guiState.externalEditor;
        lastDir = p->params.guiState.lastDir;
        defaultSf = p->params.guiState.defaultSoundFont;
    }
    const fs::path path = configFilePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return;
    out << "editor=" << editor << "\n";
    if (!lastDir.empty()) out << "lastdir=" << lastDir << "\n";
    if (!defaultSf.empty()) out << "defaultsf=" << defaultSf << "\n";
}

// ------------------------------------------------------------ loading

// Builds an instance and publishes it to the audio thread, then updates the
// GUI-facing state. Runs on the loader thread (GUI requests) or the main
// thread (state load / activate). Returns false if loading failed - in that
// case the previously loaded SoundFont keeps playing.
bool buildAndPublish(Plugin* p, const Plugin::LoadJob& j) {
    collectGarbage(p);

    std::vector<PresetEntry> presets;
    std::vector<std::string> warnings;
    std::string error;
    std::unique_ptr<FluidInstance> inst;
    // Retry if the host changed the sample rate while we were loading.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const double sr = p->sampleRate.load();
        inst = buildFluidInstance(j.path, sr, j.settingsText, engineSetupFrom(p->params), presets,
                                  warnings, error);
        if (!inst || p->sampleRate.load() == sr) break;
    }

    SoundFontInfo info;
    if (inst && !j.path.empty()) {
        readSoundFontInfo(j.path, info);
        info.disabledControllers = inst->disabledControllers;
    }

    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        auto& gs = p->params.guiState;
        gs.loading = false;
        if (!inst) {
            gs.toast = error.empty() ? "Could not load the SoundFont" : error;
            gs.toastIsError = true;
            gs.toastSetAt = std::chrono::steady_clock::now();
            return false;
        }
        gs.soundFontPath = j.path;
        gs.soundFontName = j.path.empty() ? "" : displayNameFor(j.path, info);
        gs.presets = presets;
        gs.info = info;
        gs.fluidSettingsText = j.settingsText;
        ++gs.loadGeneration;
        if (!warnings.empty()) {
            std::string msg = "FluidSynth settings:";
            for (const auto& w : warnings) msg += "\n" + w;
            gs.toast = msg;
            gs.toastIsError = true;
        } else if (!j.path.empty()) {
            gs.toast = "Loaded " + fileName(j.path) + " (" +
                       std::to_string(presets.size()) + " presets)";
            gs.toastIsError = false;
            if (info.disabledControllers && p->params.getInt(kParamRestoreControllers) == 0) {
                gs.toast += "\nThis SoundFont disables: " +
                            describeControllers(info.disabledControllers) +
                            ".\nEnable Options > Restore MIDI controllers to use them.";
                gs.toastIsError = true;
            }
        }
        gs.toastSetAt = std::chrono::steady_clock::now();
    }

    // GUI-initiated load: if the current Bank/Patch doesn't exist in the
    // new SoundFont, jump to its first preset rather than staying silent.
    if (j.fromGui && !presets.empty()) {
        const int bank = p->params.getInt(kParamBank);
        const int prog = p->params.getInt(kParamProgram);
        const bool found = std::any_of(presets.begin(), presets.end(), [&](const PresetEntry& e) {
            return e.bank == bank && e.program == prog;
        });
        if (!found) {
            setParamFromUi(p, kParamBank, presets.front().bank);
            setParamFromUi(p, kParamProgram, presets.front().program);
        }
    }

    // Always handed over through `incoming`, even while inactive (then
    // plugActivate adopts it) - only the audio thread, or the main thread
    // while audio is stopped, ever touches `active`.
    FluidInstance* old = p->incoming.exchange(inst.release());
    delete old; // superseded before anyone picked it up
    return true;
}

void loaderMain(Plugin* p) {
    for (;;) {
        Plugin::LoadJob j;
        {
            std::unique_lock<std::mutex> lock(p->loaderMutex);
            p->loaderCv.wait(lock, [p] { return p->hasJob || p->loaderQuit; });
            if (p->loaderQuit) return;
            j = std::move(p->job);
            p->hasJob = false;
            p->loaderBusy = true;
        }
        buildAndPublish(p, j);
        {
            std::lock_guard<std::mutex> lock(p->loaderMutex);
            p->loaderBusy = false;
        }
        p->loaderCv.notify_all();
    }
}

// Queues an asynchronous (re)load - latest request wins.
void requestLoad(Plugin* p, const std::string& path, bool fromGui) {
    p->initialSoundFontDecided.store(true);
    std::string settingsText;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        settingsText = p->params.guiState.fluidSettingsText;
        p->params.guiState.loading = true;
        p->params.guiState.loadingName =
            path.empty() ? "(empty)" : fileName(path);
    }
    {
        std::lock_guard<std::mutex> lock(p->loaderMutex);
        p->job = {path, settingsText, fromGui};
        p->hasJob = true;
    }
    p->loaderCv.notify_all();
}

void waitLoaderIdle(Plugin* p) {
    std::unique_lock<std::mutex> lock(p->loaderMutex);
    p->loaderCv.wait(lock, [p] { return (!p->hasJob && !p->loaderBusy) || p->loaderQuit; });
}

void loadFromGui(Plugin* p, const std::string& path) {
    if (!isSoundFontFile(path)) {
        setToast(p, "Not a SoundFont file: " + fileName(path), true);
        return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(u8path(path), ec)) {
        setToast(p, "File not found: " + path, true);
        return;
    }
    requestLoad(p, path, true);
}

std::string currentSoundFontPath(Plugin* p) {
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    return p->params.guiState.soundFontPath;
}

// Stores the default SoundFont for new instances in the global config.
void setDefaultSoundFont(Plugin* p, const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.defaultSoundFont = path;
    }
    saveGlobalConfig(p);
    setToast(p, path.empty() ? std::string("Default SoundFont cleared")
                             : "Default SoundFont: " + fileName(path) +
                                   "\n(loaded by every new instance)",
             false);
}

// Brand-new instance (no state restored, nothing loaded by the user yet):
// starts with the configured default SoundFont. Runs at most once per
// instance. `sync` = build right here (plugActivate: the host may render
// right after activating) instead of on the loader thread (GUI opened
// before activation).
void maybeLoadDefaultSoundFont(Plugin* p, bool sync) {
    if (p->initialSoundFontDecided.exchange(true)) return;
    std::string path, settings;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        path = p->params.guiState.defaultSoundFont;
        settings = p->params.guiState.fluidSettingsText;
        if (!p->params.guiState.soundFontPath.empty()) return;
    }
    if (path.empty()) return;
    std::error_code ec;
    if (!fs::is_regular_file(u8path(path), ec)) {
        setToast(p, "Default SoundFont not found: " + path, true);
        return;
    }
    if (sync) {
        buildAndPublish(p, {path, settings, true});
    } else {
        requestLoad(p, path, true);
        p->initialSoundFontDecided.store(true);
    }
}

// ------------------------------------------------------------- GUI actions

// Remembers the folder of the last SoundFont picked/dropped, so the next
// "Open" dialog (in any instance) starts there.
void rememberDir(Plugin* p, const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.lastDir = platform::parentDir(path);
    }
    saveGlobalConfig(p);
}

// Folder button: the pop-up file chooser (zenity on Linux, like the sibling
// projects' preset dialogs; the native dialog on Windows). Starts in the
// loaded SoundFont's folder, else the last one used, else the system
// SoundFont folder, else the home folder.
void handleOpenDialog(Plugin* p) {
    std::string start;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        const auto& gs = p->params.guiState;
        if (!gs.soundFontPath.empty()) start = gs.soundFontPath;
        else if (!gs.lastDir.empty()) start = gs.lastDir + "/";
    }
    if (start.empty()) start = platform::defaultSoundFontDir();
    const std::string path =
        openFileDialog("Open SoundFont", start,
                       {{"SoundFonts (sf2, sf3, dls)", "*.sf2 *.SF2 *.sf3 *.SF3 *.dls *.DLS"},
                        {"All files", "*"}});
    if (path.empty()) return;
    loadFromGui(p, path);
    rememberDir(p, path);
}

// Launches the configured external SoundFont editor, fully detached (it
// outlives the plugin/DAW if the user wants).
void handleOpenExternalEditor(Plugin* p) {
    std::string editor, path;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        editor = p->params.guiState.externalEditor;
        path = p->params.guiState.soundFontPath;
    }
    if (editor.empty() || path.empty()) {
        setToast(p, "Set an external editor in the Advanced tab first", true);
        return;
    }
    if (!platform::launchDetached(editor, path)) {
        setToast(p, "Could not start " + editor, true);
        return;
    }
    setToast(p, "Opened in " + editor + " - use Reload after saving", false);
}

// ------------------------------------------------------------ descriptor

#define CLAP_STR_(x) #x
#define CLAP_STR(x) CLAP_STR_(x)

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                           CLAP_PLUGIN_FEATURE_SAMPLER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDesc = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.michael02022.fluidsynthplayer",
    .name = "FluidSynth Player",
    .vendor = "michael02022",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = FSP_PLUGIN_VERSION,
    // String literals only (the descriptor is static): the FluidSynth
    // version is the vendored, statically linked one, so compile-time ==
    // runtime here.
    .description = "SoundFont (SF2/SF3/DLS) player. Based on FluidSynthPlugin " FSP_UPSTREAM_VERSION
                   ", FluidSynth " FLUIDSYNTH_VERSION ", CLAP " CLAP_STR(CLAP_VERSION_MAJOR) "."
                   CLAP_STR(CLAP_VERSION_MINOR) "." CLAP_STR(CLAP_VERSION_REVISION) ".",
    .features = kFeatures,
};

// ------------------------------------------------------------ audio ports

uint32_t audioPortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 1; }

bool audioPortsGet(const clap_plugin_t*, uint32_t index, bool isInput,
                   clap_audio_port_info_t* info) {
    if (isInput || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "Out");
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kExtAudioPorts = {.count = audioPortsCount, .get = audioPortsGet};

// ------------------------------------------------------------- note ports

uint32_t notePortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }

bool notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info) {
    if (!isInput || index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    snprintf(info->name, sizeof(info->name), "MIDI In");
    return true;
}

const clap_plugin_note_ports_t kExtNotePorts = {.count = notePortsCount, .get = notePortsGet};

// ----------------------------------------------------------------- params

uint32_t paramsCount(const clap_plugin_t*) { return kNumParams; }

bool paramsGetInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index >= kNumParams) return false;
    const ParamInfo& pi = kParamInfos[index];
    memset(info, 0, sizeof(*info));
    info->id = pi.id;
    info->flags = 0;
    if (pi.automatable) info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    if (pi.stepped) info->flags |= CLAP_PARAM_IS_STEPPED;
    if (pi.id == kParamInterpolation || pi.id == kParamRenderInterpolation ||
        pi.id == kParamMidiMode || pi.id == kParamVoiceMode ||
        pi.id == kParamReverbEngine || pi.id == kParamPortamentoTimeMode)
        info->flags |= CLAP_PARAM_IS_ENUM;
    info->cookie = nullptr;
    snprintf(info->name, sizeof(info->name), "%s", pi.name);
    snprintf(info->module, sizeof(info->module), "%s", pi.module);
    info->min_value = pi.min;
    info->max_value = pi.max;
    info->default_value = pi.def;
    return true;
}

bool paramsGetValue(const clap_plugin_t* plugin, clap_id id, double* out) {
    if (id >= kNumParams) return false;
    *out = self(plugin)->params.values[id].load();
    return true;
}

bool paramsValueToText(const clap_plugin_t*, clap_id id, double value, char* out, uint32_t size) {
    if (id >= kNumParams || size == 0) return false;
    snprintf(out, size, "%s", formatParamValue(static_cast<ParamId>(id), value).c_str());
    return true;
}

bool paramsTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* out) {
    if (id >= kNumParams || !text) return false;
    char* end = nullptr;
    double v = strtod(text, &end);
    if (end == text) {
        // Accept the display names of enum-like params too.
        auto match = [&](const char* const* names, int n) {
            for (int i = 0; i < n; ++i)
                if (!platform::strCaseCmp(text, names[i])) {
                    v = i;
                    return true;
                }
            return false;
        };
        bool ok = false;
        if (id == kParamInterpolation) ok = match(kInterpNames, kNumInterpModes);
        else if (id == kParamRenderInterpolation) ok = match(kRenderInterpNames, kNumInterpModes + 1);
        else if (id == kParamReverbEngine) ok = match(kReverbEngineNames, kNumReverbEngines);
        else if (id == kParamPortamentoTimeMode) ok = match(kPortaTimeModeNames, 3);
        else if (id == kParamMidiMode) ok = match(kMidiModeNames, 2);
        else if (id == kParamVoiceMode) ok = match(kVoiceModeNames, 3);
        else if (!platform::strCaseCmp(text, "on")) { v = 1; ok = true; }
        else if (!platform::strCaseCmp(text, "off")) { v = 0; ok = true; }
        if (!ok) return false;
    } else if (id == kParamGain && strstr(text, "dB")) {
        v = std::pow(10.0, v / 20.0);
    } else if ((id == kParamEnvAttack || id == kParamEnvDecay || id == kParamEnvRelease) &&
               text[0] == 'x') {
        v = strtod(text + 1, nullptr);
    }
    const auto& info = kParamInfos[id];
    *out = std::clamp(v, info.min, info.max);
    return true;
}

// Applies one incoming host param event (process() or flush()).
void handleParamEvent(Plugin* p, const clap_event_param_value_t* ev) {
    if (ev->param_id >= kNumParams) return;
    const auto& info = kParamInfos[ev->param_id];
    p->params.values[ev->param_id].store(std::clamp(ev->value, info.min, info.max));
}

// Emits the GUI-originated gesture/value events and pending MIDI-driven
// program changes as CLAP output events.
void flushGuiParamEvents(Plugin* p, const clap_output_events_t* out) {
    if (!out) return;
    auto pushGesture = [&](uint32_t id, uint16_t type) {
        clap_event_param_gesture_t ev{};
        ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
        ev.param_id = id;
        out->try_push(out, &ev.header);
    };
    auto pushValue = [&](uint32_t id) {
        clap_event_param_value_t ev{};
        ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        ev.param_id = id;
        ev.cookie = nullptr;
        ev.note_id = -1;
        ev.port_index = -1;
        ev.channel = -1;
        ev.key = -1;
        ev.value = p->params.values[id].load();
        out->try_push(out, &ev.header);
    };
    for (uint32_t i = 0; i < kNumParams; ++i) {
        if (p->params.guiGestureBegin[i].exchange(false)) pushGesture(i, CLAP_EVENT_PARAM_GESTURE_BEGIN);
        if (p->params.guiValueDirty[i].exchange(false)) pushValue(i);
        if (p->params.guiGestureEnd[i].exchange(false)) pushGesture(i, CLAP_EVENT_PARAM_GESTURE_END);
    }
}

void paramsFlush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                 const clap_output_events_t* out) {
    Plugin* p = self(plugin);
    if (in) {
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_event_header_t* hdr = in->get(in, i);
            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_PARAM_VALUE)
                handleParamEvent(p, reinterpret_cast<const clap_event_param_value_t*>(hdr));
        }
    }
    flushGuiParamEvents(p, out);
}

const clap_plugin_params_t kExtParams = {
    .count = paramsCount,
    .get_info = paramsGetInfo,
    .get_value = paramsGetValue,
    .value_to_text = paramsValueToText,
    .text_to_value = paramsTextToValue,
    .flush = paramsFlush,
};

// ------------------------------------------------------------------ state
//
// Versioned little binary blob: SoundFont path, FluidSynth settings text,
// every parameter as (id, value) pairs (so adding params never breaks old
// projects) and the 16 channel patches of multitimbral mode. The SoundFont
// itself is referenced by path, never embedded.

constexpr uint32_t kStateMagic = 0x4C505346; // "FSPL"
constexpr uint32_t kStateVersion = 1;

bool streamWriteAll(const clap_ostream_t* s, const void* data, uint64_t size) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    while (size > 0) {
        int64_t n = s->write(s, ptr, size);
        if (n <= 0) return false;
        ptr += n;
        size -= static_cast<uint64_t>(n);
    }
    return true;
}

bool streamReadAll(const clap_istream_t* s, void* data, uint64_t size) {
    auto* ptr = static_cast<uint8_t*>(data);
    while (size > 0) {
        int64_t n = s->read(s, ptr, size);
        if (n <= 0) return false;
        ptr += n;
        size -= static_cast<uint64_t>(n);
    }
    return true;
}

template <typename T>
bool writeVal(const clap_ostream_t* s, const T& v) { return streamWriteAll(s, &v, sizeof(T)); }
template <typename T>
bool readVal(const clap_istream_t* s, T& v) { return streamReadAll(s, &v, sizeof(T)); }

bool writeString(const clap_ostream_t* s, const std::string& str) {
    const uint32_t len = static_cast<uint32_t>(str.size());
    return writeVal(s, len) && (len == 0 || streamWriteAll(s, str.data(), len));
}

bool readString(const clap_istream_t* s, std::string& out) {
    uint32_t len = 0;
    if (!readVal(s, len) || len > (1u << 24)) return false;
    out.resize(len);
    return len == 0 || streamReadAll(s, out.data(), len);
}

bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    Plugin* p = self(plugin);
    std::string path, settings;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        path = p->params.guiState.soundFontPath;
        settings = p->params.guiState.fluidSettingsText;
    }
    if (!writeVal(stream, kStateMagic) || !writeVal(stream, kStateVersion)) return false;
    if (!writeString(stream, path) || !writeString(stream, settings)) return false;
    const uint32_t count = kNumParams;
    if (!writeVal(stream, count)) return false;
    for (uint32_t i = 0; i < kNumParams; ++i) {
        const double v = p->params.values[i].load();
        if (!writeVal(stream, i) || !writeVal(stream, v)) return false;
    }
    for (int c = 0; c < kNumMidiChannels; ++c) {
        const int32_t bank = p->params.channelBank[c].load();
        const int32_t prog = p->params.channelProgram[c].load();
        if (!writeVal(stream, bank) || !writeVal(stream, prog)) return false;
    }
    return true;
}

bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    Plugin* p = self(plugin);
    // A restored project keeps its own SoundFont (or none): never the default.
    p->initialSoundFontDecided.store(true);
    uint32_t magic = 0, version = 0;
    if (!readVal(stream, magic) || magic != kStateMagic || !readVal(stream, version) ||
        version == 0 || version > kStateVersion)
        return false;
    std::string path, settings;
    if (!readString(stream, path) || !readString(stream, settings)) return false;
    uint32_t count = 0;
    if (!readVal(stream, count) || count > 4096) return false;
    // Unknown ids (from a newer version) are skipped; params missing from an
    // older state fall back to their defaults.
    std::array<double, kNumParams> values;
    for (uint32_t i = 0; i < kNumParams; ++i) values[i] = kParamInfos[i].def;
    bool hasReverbEngine = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        double v = 0.0;
        if (!readVal(stream, id) || !readVal(stream, v)) return false;
        if (id < kNumParams && std::isfinite(v))
            values[id] = std::clamp(v, kParamInfos[id].min, kParamInfos[id].max);
        hasReverbEngine |= id == kParamReverbEngine;
    }
    // Saved before FluidSynth 2.6 support: those projects were mixed with
    // 2.5's only reverb, FDN - keep it instead of 2.6's new default.
    if (!hasReverbEngine) values[kParamReverbEngine] = kReverbEngineFdn;
    std::array<int32_t, kNumMidiChannels> banks{}, progs{};
    for (int c = 0; c < kNumMidiChannels; ++c)
        if (!readVal(stream, banks[c]) || !readVal(stream, progs[c])) return false;

    for (uint32_t i = 0; i < kNumParams; ++i) p->params.values[i].store(values[i]);
    for (int c = 0; c < kNumMidiChannels; ++c) {
        p->params.channelBank[c].store(std::clamp<int32_t>(banks[c], 0, 16383));
        p->params.channelProgram[c].store(std::clamp<int32_t>(progs[c], 0, 127));
        p->params.pendingChannelPatch[c].store(-1);
    }
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.fluidSettingsText = settings;
    }

    // Synchronous on purpose (the main thread may block here): a host that
    // renders right after restoring a project must not lose the first notes
    // to a still-loading SoundFont. A missing file keeps the path (so saving
    // the project again doesn't silently drop it) and reports it.
    waitLoaderIdle(p);
    std::error_code ec;
    if (!path.empty() && !fs::is_regular_file(u8path(path), ec)) {
        Plugin::LoadJob empty{"", settings, false};
        buildAndPublish(p, empty);
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.soundFontPath = path;
        p->params.guiState.soundFontName = fileName(path) + "  (missing!)";
        p->params.guiState.toast = "SoundFont not found: " + path;
        p->params.guiState.toastIsError = true;
        p->params.guiState.toastSetAt = std::chrono::steady_clock::now();
    } else {
        buildAndPublish(p, {path, settings, false});
    }
    if (p->hostParams && p->hostParams->rescan) p->hostParams->rescan(p->host, CLAP_PARAM_RESCAN_VALUES);
    return true;
}

const clap_plugin_state_t kExtState = {.save = stateSave, .load = stateLoad};

// ------------------------------------------------------------------ render
//
// clap.render: the host says whether it is rendering offline (bounce/export,
// faster than real time) - then Render Interpolation applies (see
// applyParams). No hard real-time requirement: the plugin can run offline.

bool renderHasHardRealtimeRequirement(const clap_plugin_t*) { return false; }

bool renderSet(const clap_plugin_t* plugin, clap_plugin_render_mode mode) {
    Plugin* p = self(plugin);
    const bool offline = mode == CLAP_RENDER_OFFLINE;
    p->params.offlineRender.store(offline);
    if (offline) p->offlineRenderSeen.store(true);
    return true;
}

const clap_plugin_render_t kExtRender = {
    .has_hard_realtime_requirement = renderHasHardRealtimeRequirement,
    .set = renderSet,
};

// -------------------------------------------------------------------- gui

bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool isFloating) {
    return !isFloating && api && strcmp(api, kGuiApi) == 0;
}

bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* isFloating) {
    *api = kGuiApi;
    *isFloating = false;
    return true;
}

EditorCallbacks makeEditorCallbacks(Plugin* p) {
    EditorCallbacks cb;
    cb.openSoundFontDialog = [p] { handleOpenDialog(p); };
    cb.reloadSoundFont = [p] {
        const std::string path = currentSoundFontPath(p);
        if (!path.empty()) requestLoad(p, path, false);
    };
    cb.unloadSoundFont = [p] { requestLoad(p, "", false); };
    cb.openInExternalEditor = [p] { handleOpenExternalEditor(p); };
    cb.applyFluidSettings = [p](const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
            p->params.guiState.fluidSettingsText = text;
        }
        requestLoad(p, currentSoundFontPath(p), false);
    };
    cb.setExternalEditor = [p](const std::string& editor) {
        {
            std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
            p->params.guiState.externalEditor = editor;
        }
        saveGlobalConfig(p);
        setToast(p, "External editor set to " + editor, false);
    };
    cb.setDefaultSoundFont = [p](const std::string& path) { setDefaultSoundFont(p, path); };
    cb.chooseDefaultSoundFont = [p] {
        std::string start;
        {
            std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
            const auto& gs = p->params.guiState;
            start = !gs.defaultSoundFont.empty() ? gs.defaultSoundFont
                    : !gs.soundFontPath.empty()  ? gs.soundFontPath
                    : !gs.lastDir.empty()        ? gs.lastDir + "/"
                                                 : platform::defaultSoundFontDir();
        }
        const std::string path = openFileDialog(
            "Default SoundFont for new instances", start,
            {{"SoundFonts (sf2, sf3, dls)", "*.sf2 *.SF2 *.sf3 *.SF3 *.dls *.DLS"}, {"All files", "*"}});
        if (path.empty()) return;
        if (!isSoundFontFile(path)) {
            setToast(p, "Not a SoundFont file: " + fileName(path), true);
            return;
        }
        setDefaultSoundFont(p, path);
    };
    cb.setParam = [p](ParamId id, double v) { setParamFromUi(p, id, v); };
    cb.beginGesture = [p](ParamId id) {
        p->params.guiGestureBegin[id].store(true);
        requestParamFlush(p);
    };
    cb.endGesture = [p](ParamId id) {
        p->params.guiGestureEnd[id].store(true);
        requestParamFlush(p);
        // Creation-time FluidSynth settings (reverb engine, limiter): build a
        // new synth with the current SoundFont once the edit is finished -
        // not on every drag step, a rebuild reloads the whole SoundFont.
        if (isRebuildParam(id)) requestLoad(p, currentSoundFontPath(p), false);
    };
    return cb;
}

bool guiCreate(const clap_plugin_t* plugin, const char* api, bool isFloating) {
    if (!guiIsApiSupported(plugin, api, isFloating)) return false;
    Plugin* p = self(plugin);
    if (p->window) return true;
    // GUI opened before the host activated the plugin: start loading the
    // default SoundFont now so the panel isn't empty (no-op otherwise).
    if (!p->activated.load()) maybeLoadDefaultSoundFont(p, false);
    auto callbacks = std::make_shared<EditorCallbacks>(makeEditorCallbacks(p));
    p->window = std::make_unique<GuiWindow>(
        [p, callbacks] { drawEditorUI(p->params, p->uiState, *callbacks); },
        [p](const std::vector<std::string>& paths) {
            // File drop (XDND / WM_DROPFILES): the first SoundFont among the dropped files is
            // loaded (one SoundFont per instance).
            for (const auto& path : paths) {
                if (!isSoundFontFile(path)) continue;
                loadFromGui(p, path);
                rememberDir(p, path);
                return;
            }
            setToast(p, "Drop a SoundFont file (.sf2, .sf3 or .dls)", true);
        });
    return p->window->create(kDefaultW, kDefaultH);
}

void guiDestroy(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (p->window) {
        p->window->destroy();
        p->window.reset();
    }
}

bool guiSetScale(const clap_plugin_t*, double) { return true; }

bool guiGetSize(const clap_plugin_t* plugin, uint32_t* w, uint32_t* h) {
    Plugin* p = self(plugin);
    if (p->window) p->window->getSize(*w, *h);
    else {
        *w = kDefaultW;
        *h = kDefaultH;
    }
    return true;
}

bool guiCanResize(const clap_plugin_t*) { return true; }

bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    return true;
}

bool guiAdjustSize(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    // The panel scales uniformly; below half size it stops being usable.
    *w = std::max<uint32_t>(*w, kDefaultW / 2);
    *h = std::max<uint32_t>(*h, kDefaultH / 2);
    return true;
}

bool guiSetSize(const clap_plugin_t* plugin, uint32_t w, uint32_t h) {
    Plugin* p = self(plugin);
    if (p->window) p->window->setSize(w, h);
    return true;
}

bool guiSetParent(const clap_plugin_t* plugin, const clap_window_t* win) {
    Plugin* p = self(plugin);
    if (!p->window || !win) return false;
#ifdef _WIN32
    p->window->setParent(reinterpret_cast<uintptr_t>(win->win32));
#else
    p->window->setParent(static_cast<uintptr_t>(win->x11));
#endif
    return true;
}

bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return true; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}

bool guiShow(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->window) return false;
    p->window->setVisible(true);
    return true;
}

bool guiHide(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->window) return false;
    p->window->setVisible(false);
    return true;
}

const clap_plugin_gui_t kExtGui = {
    .is_api_supported = guiIsApiSupported,
    .get_preferred_api = guiGetPreferredApi,
    .create = guiCreate,
    .destroy = guiDestroy,
    .set_scale = guiSetScale,
    .get_size = guiGetSize,
    .can_resize = guiCanResize,
    .get_resize_hints = guiGetResizeHints,
    .adjust_size = guiAdjustSize,
    .set_size = guiSetSize,
    .set_parent = guiSetParent,
    .set_transient = guiSetTransient,
    .suggest_title = guiSuggestTitle,
    .show = guiShow,
    .hide = guiHide,
};

// ----------------------------------------------------------------- plugin

bool plugInit(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    installFluidLogHook();
    p->hostParams = static_cast<const clap_host_params_t*>(
        p->host->get_extension(p->host, CLAP_EXT_PARAMS));
    loadGlobalConfig(p);
    p->loaderThread = std::thread(loaderMain, p);
    return true;
}

void plugDestroy(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    // Debug-only: FLUIDSYNTHPLAYER_DEBUG_MIDI_LOG=<file> appends the program
    // messages this process received (see recentProgramEvents) plus the
    // final Bank/Patch, so a headless host run (REAPER -renderproject) can
    // be checked for unexpected program changes. No effect when unset.
    if (const char* logPath = getenv("FLUIDSYNTHPLAYER_DEBUG_MIDI_LOG")) {
        if (FILE* f = fopen(logPath, "a")) {
            for (const auto& e : recentProgramEvents()) fprintf(f, "%s\n", e.c_str());
            fprintf(f, "final bank=%d patch=%d\n", p->params.getInt(kParamBank),
                    p->params.getInt(kParamProgram));
            fprintf(f, "offline render mode %s\n",
                    p->offlineRenderSeen.load() ? "seen" : "never set");
            fclose(f);
        }
    }
    guiDestroy(plugin);
    {
        std::lock_guard<std::mutex> lock(p->loaderMutex);
        p->loaderQuit = true;
    }
    p->loaderCv.notify_all();
    if (p->loaderThread.joinable()) p->loaderThread.join();
    collectGarbage(p);
    delete p->incoming.exchange(nullptr);
    delete p->active;
    delete p;
}

bool plugActivate(const clap_plugin_t* plugin, double sampleRate, uint32_t, uint32_t) {
    Plugin* p = self(plugin);
    p->sampleRate.store(sampleRate);
    g_lastSampleRate.store(sampleRate);
    waitLoaderIdle(p);
    collectGarbage(p);
    maybeLoadDefaultSoundFont(p, true);
    // Audio isn't running: the main thread may own `active` directly.
    if (FluidInstance* fresh = p->incoming.exchange(nullptr)) {
        delete p->active;
        p->active = fresh;
        p->replayControllersPending.store(true);
    }
    if (!p->active || p->active->sampleRate != sampleRate) {
        std::string path, settings;
        {
            std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
            path = p->params.guiState.soundFontPath;
            settings = p->params.guiState.fluidSettingsText;
        }
        std::error_code ec;
        if (!path.empty() && !fs::is_regular_file(u8path(path), ec)) path.clear();
        std::vector<PresetEntry> presets;
        std::vector<std::string> warnings;
        std::string error;
        const EngineSetup setup = engineSetupFrom(p->params);
        auto inst = buildFluidInstance(path, sampleRate, settings, setup, presets, warnings, error);
        if (!inst) inst = buildFluidInstance("", sampleRate, settings, setup, presets, warnings, error);
        if (!inst) return false;
        delete p->active;
        p->active = inst.release();
        p->replayControllersPending.store(true);
    }
    p->activated.store(true);
    return true;
}

void plugDeactivate(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    p->activated.store(false);
    // Audio has stopped: adopt anything the loader published meanwhile.
    if (FluidInstance* fresh = p->incoming.exchange(nullptr)) {
        delete p->active;
        p->active = fresh;
        p->replayControllersPending.store(true);
    }
    collectGarbage(p);
}

bool plugStartProcessing(const clap_plugin_t*) { return true; }
void plugStopProcessing(const clap_plugin_t*) {}

void plugReset(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (p->active) allSoundsOff(*p->active);
}

void dispatchMidi(Plugin* p, FluidInstance& inst, uint8_t status, uint8_t d1, uint8_t d2) {
    handleMidi(inst, p->params, p->controllers, status, d1, d2);
    if (inst.programChangedByMidi) applyParams(inst, p->params);
}

clap_process_status plugProcess(const clap_plugin_t* plugin, const clap_process_t* process) {
    Plugin* p = self(plugin);
    const uint32_t frames = process->frames_count;
    if (process->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;
    const auto& outBuf = process->audio_outputs[0];
    if (!outBuf.data32 || outBuf.channel_count < 2 || !outBuf.data32[0] || !outBuf.data32[1])
        return CLAP_PROCESS_CONTINUE;
    float* left = outBuf.data32[0];
    float* right = outBuf.data32[1];

    // Pick up a freshly loaded SoundFont (never blocks, never frees).
    if (FluidInstance* fresh = p->incoming.exchange(nullptr)) {
        if (fresh->sampleRate == p->sampleRate.load()) {
            if (p->active) allSoundsOff(*p->active);
            retire(p, p->active);
            p->active = fresh;
            p->replayControllersPending.store(true);
            for (auto& n : p->params.noteActive) n.store(false);
        } else {
            retire(p, fresh); // built for a stale rate; keep the current one
        }
    }
    FluidInstance* inst = p->active;

    const clap_input_events_t* in = process->in_events;
    const uint32_t numEvents = in ? in->size(in) : 0;

    if (!inst) {
        memset(left, 0, frames * sizeof(float));
        memset(right, 0, frames * sizeof(float));
        for (uint32_t i = 0; i < numEvents; ++i) {
            const clap_event_header_t* hdr = in->get(in, i);
            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_PARAM_VALUE)
                handleParamEvent(p, reinterpret_cast<const clap_event_param_value_t*>(hdr));
        }
        flushGuiParamEvents(p, process->out_events);
        return CLAP_PROCESS_CONTINUE;
    }

    applyParams(*inst, p->params);
    // After applyParams, so controllers the host sent win over the
    // plugin's own defaults (e.g. CC5/CC65 from the Portamento param).
    if (p->replayControllersPending.exchange(false)) replayControllers(*inst, p->controllers);

    if (p->params.pendingPanic.exchange(false)) allSoundsOff(*inst);
    // GUI piano, injected at the start of the block.
    const int previewCh = std::clamp(p->params.previewChannel.load(), 0, kNumMidiChannels - 1);
    if (p->params.pendingPreviewNoteOff.exchange(false))
        dispatchMidi(p, *inst, static_cast<uint8_t>(0x80 | previewCh),
                     static_cast<uint8_t>(p->params.previewNoteOffNumber.load()), 0);
    if (p->params.pendingPreviewNoteOn.exchange(false))
        dispatchMidi(p, *inst, static_cast<uint8_t>(0x90 | previewCh),
                     static_cast<uint8_t>(p->params.previewNoteOnNumber.load()),
                     static_cast<uint8_t>(p->params.previewNoteOnVelocity.load()));

    // Sample-accurate event placement: render up to each event's own
    // timestamp before applying it - this fork's original reason to exist
    // (FluidSynth's own player quantizes events to its processing raster).
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < numEvents; ++i) {
        const clap_event_header_t* hdr = in->get(in, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        const uint32_t t = std::min(hdr->time, frames);
        if (t > cursor) {
            renderFluid(*inst, left + cursor, right + cursor, t - cursor);
            cursor = t;
        }
        switch (hdr->type) {
            case CLAP_EVENT_PARAM_VALUE:
                handleParamEvent(p, reinterpret_cast<const clap_event_param_value_t*>(hdr));
                applyParams(*inst, p->params);
                break;
            case CLAP_EVENT_MIDI: {
                const auto* ev = reinterpret_cast<const clap_event_midi_t*>(hdr);
                if (ev->data[0] >= 0x80 && ev->data[0] < 0xF0)
                    dispatchMidi(p, *inst, ev->data[0], ev->data[1] & 0x7F, ev->data[2] & 0x7F);
                break;
            }
            case CLAP_EVENT_MIDI_SYSEX: {
                const auto* ev = reinterpret_cast<const clap_event_midi_sysex_t*>(hdr);
                const uint8_t* d = ev->buffer;
                uint32_t n = ev->size;
                if (n > 0 && d[0] == 0xF0) { ++d; --n; }
                if (n > 0 && d[n - 1] == 0xF7) --n;
                handleSysex(*inst, d, n);
                applyParams(*inst, p->params); // after a reset SysEx: re-apply everything
                break;
            }
            case CLAP_EVENT_NOTE_ON:
            case CLAP_EVENT_NOTE_OFF:
            case CLAP_EVENT_NOTE_CHOKE: {
                const auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
                if (ev->key < 0 || ev->key > 127) break;
                const uint8_t ch = static_cast<uint8_t>(std::clamp<int>(ev->channel, 0, 15));
                if (hdr->type == CLAP_EVENT_NOTE_ON) {
                    const int vel = std::clamp(static_cast<int>(std::lround(ev->velocity * 127.0)), 1, 127);
                    dispatchMidi(p, *inst, static_cast<uint8_t>(0x90 | ch),
                                 static_cast<uint8_t>(ev->key), static_cast<uint8_t>(vel));
                } else {
                    const int vel = std::clamp(static_cast<int>(std::lround(ev->velocity * 127.0)), 0, 127);
                    dispatchMidi(p, *inst, static_cast<uint8_t>(0x80 | ch),
                                 static_cast<uint8_t>(ev->key), static_cast<uint8_t>(vel));
                }
                break;
            }
            default:
                break;
        }
    }
    if (cursor < frames) renderFluid(*inst, left + cursor, right + cursor, frames - cursor);

    // Meters / voice counter for the GUI.
    float pkL = 0.f, pkR = 0.f;
    for (uint32_t i = 0; i < frames; ++i) {
        pkL = std::max(pkL, std::fabs(left[i]));
        pkR = std::max(pkR, std::fabs(right[i]));
    }
    if (pkL > p->params.peakL.load()) p->params.peakL.store(pkL);
    if (pkR > p->params.peakR.load()) p->params.peakR.store(pkR);
    p->params.activeVoiceCount.store(fluidActiveVoices(*inst));

    if (inst->programChangedByMidi) {
        inst->programChangedByMidi = false;
        p->params.guiValueDirty[kParamBank].store(true);
        p->params.guiValueDirty[kParamProgram].store(true);
    }
    flushGuiParamEvents(p, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

const void* plugGetExtension(const clap_plugin_t*, const char* id) {
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kExtAudioPorts;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kExtNotePorts;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &kExtParams;
    if (!strcmp(id, CLAP_EXT_GUI)) return &kExtGui;
    if (!strcmp(id, CLAP_EXT_STATE)) return &kExtState;
    if (!strcmp(id, CLAP_EXT_RENDER)) return &kExtRender;
    return nullptr;
}

void plugOnMainThread(const clap_plugin_t* plugin) { collectGarbage(self(plugin)); }

// ---------------------------------------------------------------- factory

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                   const char* pluginId) {
    if (strcmp(pluginId, kDesc.id) != 0) return nullptr;
    Plugin* p = new (std::nothrow) Plugin(host);
    if (!p) return nullptr;
    p->plug.desc = &kDesc;
    p->plug.plugin_data = p;
    p->plug.init = plugInit;
    p->plug.destroy = plugDestroy;
    p->plug.activate = plugActivate;
    p->plug.deactivate = plugDeactivate;
    p->plug.start_processing = plugStartProcessing;
    p->plug.stop_processing = plugStopProcessing;
    p->plug.reset = plugReset;
    p->plug.process = plugProcess;
    p->plug.get_extension = plugGetExtension;
    p->plug.on_main_thread = plugOnMainThread;
    return &p->plug;
}

uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDesc(const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &kDesc : nullptr;
}

const clap_plugin_factory_t kFactory = {
    .get_plugin_count = factoryCount,
    .get_plugin_descriptor = factoryDesc,
    .create_plugin = factoryCreate,
};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* id) {
    if (!strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entryInit,
    .deinit = entryDeinit,
    .get_factory = entryGetFactory,
};
