#include "engine/FluidEngine.h"

#include "state/SoundFontInfo.h"

#include <clap/version.h>
#include <fluidsynth.h>
#include <imgui.h>
#ifdef FSP_HAVE_SNDFILE
#include <sndfile.h> // only for its version string (Options > Info)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <cstdio>
#include <limits>
#include <mutex>
#include <sstream>

namespace {

// SF2 generator units (SF2.04 spec, section 8.1.3): LFO frequency in
// absolute cents of 8.176 Hz, delays in timecents down to -12000 (~1 ms).
constexpr double kSf2VibLfoBaseHz = 8.176;
constexpr double kSf2MinDelayTimecents = -12000.0;
// Upper bound of the Polyphony param - sizes FluidInstance::voiceBuf.
constexpr int kMaxPolyphony = 1024;

float timecentsFromSeconds(double seconds) {
    return static_cast<float>(1200.0 * std::log2(std::max(seconds, 1e-6)));
}

struct GenValue {
    int gen;
    float value;
};

// The LFO panel's vibrato, set on a voice in place of the preset's own
// (only used while AMT > 0 - at 0 the preset's vibrato stays untouched).
// Index 2, the delay, only matters to voices that are just starting.
std::array<GenValue, 3> vibratoGens(const SharedParams& params) {
    const double delay = params.values[kParamLfoDelay].load();
    return {{{GEN_VIBLFOTOPITCH, static_cast<float>(params.values[kParamLfoAmount].load())},
             {GEN_VIBLFOFREQ, static_cast<float>(1200.0 * std::log2(params.values[kParamLfoSpeed].load() /
                                                                    kSf2VibLfoBaseHz))},
             {GEN_VIBLFODELAY, delay > 0.001 ? timecentsFromSeconds(delay)
                                             : static_cast<float>(kSf2MinDelayTimecents)}}};
}

// ------------------------------------------------------------ log capture

std::mutex g_logMutex;
std::deque<std::string> g_logLines;
constexpr size_t kMaxLogLines = 200;

void fluidLogHook(int level, const char* message, void*) {
    static const char* kLevelNames[] = {"PANIC", "ERROR", "WARN", "INFO", "DEBUG"};
    if (level == FLUID_DBG) return;
    // try_lock: fluidsynth may log from the audio thread (e.g. a program
    // change to a preset the SoundFont doesn't have) - dropping a line is
    // far better than blocking audio behind a GUI read.
    std::unique_lock<std::mutex> lock(g_logMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    std::string line = std::string(kLevelNames[std::clamp(level, 0, 4)]) + ": " +
                       (message ? message : "");
    g_logLines.push_back(std::move(line));
    while (g_logLines.size() > kMaxLogLines) g_logLines.pop_front();
}

void applyChannelPatch(FluidInstance& inst, SharedParams& params, int channel, int bank,
                       int program) {
    bank = std::clamp(bank, 0, 16383);
    program = std::clamp(program, 0, 127);
    if (inst.sfontId >= 0)
        fluid_synth_program_select(inst.synth, channel, inst.sfontId, bank, program);
    params.channelBank[channel].store(bank);
    params.channelProgram[channel].store(program);
}

// Reads back what fluidsynth actually selected on `channel` after a MIDI
// Program Change and mirrors it into the GUI-facing atomics.
void mirrorChannelPatch(FluidInstance& inst, SharedParams& params, int channel) {
    int sfid = 0, bank = 0, program = 0;
    if (fluid_synth_get_program(inst.synth, channel, &sfid, &bank, &program) == FLUID_OK) {
        params.channelBank[channel].store(bank);
        params.channelProgram[channel].store(program);
    }
}

// The SF2 default modulators (same definitions as fluidsynth's own
// default_*_mod in fluid_synth.c), indexed by StandardController bit.
const std::array<fluid_mod_t*, kNumStandardControllers>& standardModulators() {
    static std::array<fluid_mod_t*, kNumStandardControllers> mods{};
    static std::once_flag once;
    std::call_once(once, [] {
        auto make = [](int src, int flags, int src2, int flags2, int dest, double amount) {
            fluid_mod_t* m = new_fluid_mod();
            fluid_mod_set_source1(m, src, flags);
            fluid_mod_set_source2(m, src2, flags2);
            fluid_mod_set_dest(m, dest);
            fluid_mod_set_amount(m, amount);
            return m;
        };
        const int uniLin = FLUID_MOD_LINEAR | FLUID_MOD_UNIPOLAR | FLUID_MOD_POSITIVE;
        const int attCurve = FLUID_MOD_CONCAVE | FLUID_MOD_UNIPOLAR | FLUID_MOD_NEGATIVE;
        const int biLin = FLUID_MOD_LINEAR | FLUID_MOD_BIPOLAR | FLUID_MOD_POSITIVE;
        mods[0] = make(1, FLUID_MOD_CC | uniLin, 0, 0, GEN_VIBLFOTOPITCH, 50);
        mods[1] = make(7, FLUID_MOD_CC | attCurve, 0, 0, GEN_ATTENUATION, 960);
        mods[2] = make(10, FLUID_MOD_CC | biLin, 0, 0, GEN_PAN, 500);
        mods[3] = make(11, FLUID_MOD_CC | attCurve, 0, 0, GEN_ATTENUATION, 960);
        mods[4] = make(91, FLUID_MOD_CC | uniLin, 0, 0, GEN_REVERBSEND, 200);
        mods[5] = make(93, FLUID_MOD_CC | uniLin, 0, 0, GEN_CHORUSSEND, 200);
        mods[6] = make(FLUID_MOD_PITCHWHEEL, FLUID_MOD_GC | biLin, FLUID_MOD_PITCHWHEELSENS,
                       FLUID_MOD_GC | uniLin, GEN_FINETUNE, 12700);
        mods[7] = make(FLUID_MOD_CHANNELPRESSURE, FLUID_MOD_GC | uniLin, 0, 0, GEN_VIBLFOTOPITCH, 50);
    });
    return mods;
}

// REVERB / CHORUS "Override Send": the send of every voice becomes exactly
// LEVEL - the preset's own send is replaced (fluid_voice_gen_set) and the
// CC91/CC93 default modulator is muted on the voice (overwritten with a
// zero-amount copy), so LEVEL 0 really means no send.
struct SendOverride {
    bool on;
    int gen;           // GEN_REVERBSEND / GEN_CHORUSSEND
    float value;       // 0.1% units
    int cc;            // 91 / 93
    fluid_mod_t* mute; // zero-amount copy of the CC -> send default modulator
};

std::array<SendOverride, 2> sendOverrides(const SharedParams& params) {
    static std::array<fluid_mod_t*, 2> mutes{};
    static std::once_flag once;
    std::call_once(once, [] {
        const auto& mods = standardModulators();
        for (int i = 0; i < 2; ++i) {
            mutes[i] = new_fluid_mod();
            fluid_mod_clone(mutes[i], mods[i == 0 ? 4 : 5]); // CC91 / CC93 -> send
            fluid_mod_set_amount(mutes[i], 0.0);
        }
    });
    auto make = [&](ParamId sw, ParamId level, int gen, int cc, fluid_mod_t* mute) {
        return SendOverride{std::lround(params.values[sw].load()) != 0, gen,
                            static_cast<float>(params.values[level].load() * 1000.0), cc, mute};
    };
    return {{make(kParamReverbSendOverride, kParamReverbSend, GEN_REVERBSEND, 91, mutes[0]),
             make(kParamChorusSendOverride, kParamChorusSend, GEN_CHORUSSEND, 93, mutes[1])}};
}

void applySendOverride(fluid_voice_t* v, const SendOverride& so) {
    fluid_voice_add_mod(v, so.mute, FLUID_VOICE_OVERWRITE);
    fluid_voice_gen_set(v, so.gen, so.value);
}

// The voice's modulator sums were computed at note-on: re-sending the
// channel's current CC recomputes them (without the muted modulator) and
// updates the send - same trick as "Restore MIDI controllers" below.
void resendCc(fluid_synth_t* s, int ch, int cc) {
    int val = 0;
    if (fluid_synth_get_cc(s, ch, cc, &val) == FLUID_OK) fluid_synth_cc(s, ch, cc, val);
}

// Patches the voices the last note-on just started on `ch` (they haven't
// rendered yet). Runs on every note-on, so lastPatchedVoiceId always tracks
// the newest voice and switching an option on never touches notes that are
// already sounding.
//
// Envelope override: replaces the volume envelope's attack, hold, decay,
// sustain and release with the panel's absolute values - fluid_voice_gen_set
// sets the voice's own generator value, where fluid_synth_set_gen would only
// add an offset. Delay, key-to-hold/decay and the modulation envelope stay
// the preset's.
//
// LFO (AMT > 0): replaces the vibrato's depth, speed and delay the same way
// (CC1 / aftertouch keep adding depth through their modulators).
//
// "Restore MIDI controllers": puts the disabled default modulators back by
// overwriting the SoundFont's zero-amount copies. The voice's modulator
// sums were already computed at note-on, and the public API has no
// "recompute modulators" call - re-sending the channel's current value of
// each restored controller is exactly what makes fluidsynth recompute them
// (same path as a real incoming CC, idempotent for every other voice).
void patchNewVoices(FluidInstance& inst, const SharedParams& params, int ch, int channelPressure) {
    if (inst.voiceBuf.empty()) return;
    const bool restore = inst.disabledControllers &&
                         std::lround(params.values[kParamRestoreControllers].load()) != 0;
    const bool envelope = std::lround(params.values[kParamEnvEnable].load()) != 0;
    const bool vibrato = params.values[kParamLfoAmount].load() > 0.0;
    const std::array<GenValue, 3> vibGens = vibratoGens(params);
    const std::array<SendOverride, 2> sends = sendOverrides(params);
    std::array<GenValue, 5> envGens{};
    const double hold = params.values[kParamEnvHold].load();
    if (envelope)
        envGens = {{{GEN_VOLENVATTACK, timecentsFromSeconds(params.values[kParamEnvAttack].load())},
                    // hold 0 = the SF2 minimum (~1 ms, no hold)
                    {GEN_VOLENVHOLD, hold > 0.001 ? timecentsFromSeconds(hold)
                                                  : static_cast<float>(kSf2MinDelayTimecents)},
                    {GEN_VOLENVDECAY, timecentsFromSeconds(params.values[kParamEnvDecay].load())},
                    // dB below full -> SF2 attenuation in centibels
                    {GEN_VOLENVSUSTAIN,
                     static_cast<float>(-params.values[kParamEnvSustain].load() * 10.0)},
                    {GEN_VOLENVRELEASE, timecentsFromSeconds(params.values[kParamEnvRelease].load())}}};

    fluid_synth_get_voicelist(inst.synth, inst.voiceBuf.data(),
                              static_cast<int>(inst.voiceBuf.size()), -1);
    const auto& mods = standardModulators();
    int64_t newest = inst.lastPatchedVoiceId;
    for (fluid_voice_t* v : inst.voiceBuf) {
        if (!v) break;
        const int64_t id = fluid_voice_get_id(v);
        if (id <= inst.lastPatchedVoiceId) continue;
        newest = std::max(newest, id);
        if (envelope)
            for (const GenValue& g : envGens) {
                fluid_voice_gen_set(v, g.gen, g.value);
                fluid_voice_update_param(v, g.gen);
            }
        if (vibrato)
            for (const GenValue& g : vibGens) {
                fluid_voice_gen_set(v, g.gen, g.value);
                fluid_voice_update_param(v, g.gen);
            }
        if (restore)
            for (int b = 0; b < kNumStandardControllers; ++b)
                if (inst.disabledControllers & (1u << b))
                    fluid_voice_add_mod(v, mods[b], FLUID_VOICE_OVERWRITE);
        for (const SendOverride& so : sends) // after restore: the override wins
            if (so.on) applySendOverride(v, so);
    }
    if (newest == inst.lastPatchedVoiceId) return; // nothing new was started
    inst.lastPatchedVoiceId = newest;
    // fluid_voice_update_param only queues the change for the running
    // voice; fluidsynth commits that queue when a synth API call returns.
    // Without one before the next render, a short attack/decay would
    // already be over when the new envelope arrives. (The CC re-sends
    // below are such calls too.)
    if (envelope || vibrato) fluid_synth_get_active_voice_count(inst.synth);
    for (const SendOverride& so : sends)
        if (so.on) resendCc(inst.synth, ch, so.cc);
    if (!restore) return;

    static const int kCcOfBit[kNumStandardControllers] = {1, 7, 10, 11, 91, 93, -1, -1};
    for (int b = 0; b < kNumStandardControllers; ++b) {
        if (!(inst.disabledControllers & (1u << b))) continue;
        if (kCcOfBit[b] >= 0) {
            int val = 0;
            if (fluid_synth_get_cc(inst.synth, ch, kCcOfBit[b], &val) == FLUID_OK)
                fluid_synth_cc(inst.synth, ch, kCcOfBit[b], val);
        } else if ((1u << b) == kCtlPitchBend) {
            int bend = 8192;
            if (fluid_synth_get_pitch_bend(inst.synth, ch, &bend) == FLUID_OK)
                fluid_synth_pitch_bend(inst.synth, ch, bend);
        } else if ((1u << b) == kCtlAftertouch) {
            fluid_synth_channel_pressure(inst.synth, ch, std::max(0, channelPressure));
        }
    }
}

// ------------------------------------------------- program event log
//
// 64-entry ring of packed 64-bit records (seq | kind | ch | a | b), each
// written with one atomic store, so the audio thread never locks or
// allocates and a reader never sees a torn entry.
enum ProgramEventKind : uint8_t {
    kEvProgramChange = 1, // a = program, b = bank (low 8 bits; 128 = drums)
    kEvProgramIgnored,    // a = program (Receive PC is off)
    kEvBankMsb,           // a = value
    kEvBankLsb,           // a = value
    kEvSysexReset,        // a = 0 GM, 1 GS, 2 XG
};
constexpr uint32_t kProgramLogSize = 64;
std::array<std::atomic<uint64_t>, kProgramLogSize> g_programLog{};
std::atomic<uint32_t> g_programLogSeq{0};

void logProgramEvent(ProgramEventKind kind, int ch, int a, int b) {
    const uint32_t seq = g_programLogSeq.fetch_add(1) + 1;
    const uint64_t packed = (uint64_t(seq) << 32) | (uint64_t(kind) << 24) |
                            (uint64_t(ch & 0xFF) << 16) | (uint64_t(a & 0xFF) << 8) |
                            uint64_t(b & 0xFF);
    g_programLog[seq % kProgramLogSize].store(packed, std::memory_order_release);
}

// GM System On (7E xx 09 01/03), GS Reset (41 xx 42 12 40 00 7F ..) and
// XG System On (43 1x 4C 00 00 7E ..): -1 if `data` is none of them.
int sysexResetKind(const uint8_t* d, uint32_t n) {
    if (n >= 4 && d[0] == 0x7E && d[2] == 0x09 && (d[3] == 0x01 || d[3] == 0x03)) return 0;
    if (n >= 8 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12 && d[4] == 0x40 && d[5] == 0x00 &&
        d[6] == 0x7F)
        return 1;
    if (n >= 7 && d[0] == 0x43 && (d[1] & 0xF0) == 0x10 && d[2] == 0x4C && d[3] == 0x00 &&
        d[4] == 0x00 && d[5] == 0x7E)
        return 2;
    return -1;
}

void applyVoiceMode(FluidInstance& inst, int mode) {
    fluid_synth_t* s = inst.synth;
    fluid_synth_reset_basic_channel(s, -1);
    // Note: only the OMNION modes honor the group size - an OMNIOFF_POLY
    // group is always a single channel, which would leave every other
    // channel disabled ("Channel N is disabled, event dropped!").
    if (mode == kVoicePoly) {
        // fluidsynth's own default: one omni-on poly group of 16 channels
        fluid_synth_set_basic_channel(s, 0, FLUID_CHANNEL_MODE_OMNION_POLY, kNumMidiChannels);
    } else {
        // Channel 1 alone becomes a mono group; channels 2-16 stay one
        // poly group so multitimbral mode keeps working around it.
        fluid_synth_set_basic_channel(s, 0, FLUID_CHANNEL_MODE_OMNIOFF_MONO, 1);
        fluid_synth_set_basic_channel(s, 1, FLUID_CHANNEL_MODE_OMNION_POLY,
                                      kNumMidiChannels - 1);
        fluid_synth_set_legato_mode(s, 0,
                                    mode == kVoiceLegato
                                        ? FLUID_CHANNEL_LEGATO_MODE_MULTI_RETRIGGER
                                        : FLUID_CHANNEL_LEGATO_MODE_RETRIGGER);
    }
}

bool isDrumBank(int bank) { return bank == 128; }

// Applies the user's "key = value" settings text to `settings`. Lines may
// also use "key value" (fluidsynth's own shell syntax); '#' starts a comment.
void applySettingsText(fluid_settings_t* settings, const std::string& text,
                       std::vector<std::string>& warnings) {
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        auto trim = [](std::string s) {
            const char* ws = " \t\r\n";
            s.erase(0, s.find_first_not_of(ws));
            s.erase(s.find_last_not_of(ws) + 1);
            return s;
        };
        line = trim(line);
        if (line.empty()) continue;
        std::string key, value;
        if (auto eq = line.find('='); eq != std::string::npos) {
            key = trim(line.substr(0, eq));
            value = trim(line.substr(eq + 1));
        } else if (auto sp = line.find_first_of(" \t"); sp != std::string::npos) {
            key = trim(line.substr(0, sp));
            value = trim(line.substr(sp + 1));
        } else {
            warnings.push_back("line " + std::to_string(lineNo) + ": missing value for '" +
                               line + "'");
            continue;
        }
        bool ok = false;
        switch (fluid_settings_get_type(settings, key.c_str())) {
            case FLUID_NUM_TYPE: {
                char* end = nullptr;
                double v = strtod(value.c_str(), &end);
                ok = end != value.c_str() &&
                     fluid_settings_setnum(settings, key.c_str(), v) == FLUID_OK;
                break;
            }
            case FLUID_INT_TYPE: {
                int v = 0;
                if (value == "true" || value == "on" || value == "yes") v = 1;
                else if (value == "false" || value == "off" || value == "no") v = 0;
                else v = atoi(value.c_str());
                ok = fluid_settings_setint(settings, key.c_str(), v) == FLUID_OK;
                break;
            }
            case FLUID_STR_TYPE:
                ok = fluid_settings_setstr(settings, key.c_str(), value.c_str()) == FLUID_OK;
                break;
            default:
                warnings.push_back("line " + std::to_string(lineNo) + ": unknown setting '" +
                                   key + "'");
                continue;
        }
        if (!ok)
            warnings.push_back("line " + std::to_string(lineNo) + ": invalid value '" + value +
                               "' for " + key);
    }
}

} // namespace

FluidInstance::~FluidInstance() {
    if (synth) delete_fluid_synth(synth);
    if (settings) delete_fluid_settings(settings);
}

EngineSetup engineSetupFrom(const SharedParams& params) {
    EngineSetup s;
    s.reverbEngine = std::clamp(params.getInt(kParamReverbEngine), 0, kNumReverbEngines - 1);
    s.limiter = params.getInt(kParamLimiterOn) != 0;
    s.limiterCeilingDb = params.get(kParamLimiterCeiling);
    s.limiterReleaseMs = params.get(kParamLimiterRelease);
    return s;
}

std::unique_ptr<FluidInstance> buildFluidInstance(const std::string& path, double sampleRate,
                                                  const std::string& settingsText,
                                                  const EngineSetup& setup,
                                                  std::vector<PresetEntry>& presets,
                                                  std::vector<std::string>& warnings,
                                                  std::string& error) {
    presets.clear();
    auto inst = std::make_unique<FluidInstance>();
    inst->path = path;
    inst->sampleRate = sampleRate;
    inst->settingsText = settingsText;
    inst->applied.fill(std::numeric_limits<double>::quiet_NaN());
    for (auto& ch : inst->heldKey) ch.fill(-1);

    inst->settings = new_fluid_settings();
    if (!inst->settings) {
        error = "could not create FluidSynth settings";
        return nullptr;
    }
    applySettingsText(inst->settings, settingsText, warnings);
    // Forced after the user's text: the plugin's own I/O layout and
    // threading model depend on these.
    fluid_settings_setnum(inst->settings, "synth.sample-rate", sampleRate);
    fluid_settings_setint(inst->settings, "synth.midi-channels", kNumMidiChannels);
    fluid_settings_setint(inst->settings, "synth.audio-channels", 1);
    fluid_settings_setint(inst->settings, "synth.audio-groups", 1);
    fluid_settings_setint(inst->settings, "synth.threadsafe-api", 0);
    fluid_settings_setint(inst->settings, "synth.cpu-cores", 1);
    fluid_settings_setstr(inst->settings, "synth.reverb.engine",
                          kReverbEngineSettings[std::clamp(setup.reverbEngine, 0,
                                                           kNumReverbEngines - 1)]);
    fluid_settings_setint(inst->settings, "synth.limiter.active", setup.limiter ? 1 : 0);
    // output-limit is linear amplitude; FluidSynth's own maximum is 1.0
    fluid_settings_setnum(inst->settings, "synth.limiter.output-limit",
                          std::min(0.9999, std::pow(10.0, setup.limiterCeilingDb / 20.0)));
    fluid_settings_setnum(inst->settings, "synth.limiter.release", setup.limiterReleaseMs);

    inst->synth = new_fluid_synth(inst->settings);
    if (!inst->synth) {
        error = "could not create FluidSynth synthesizer";
        return nullptr;
    }

    if (!path.empty()) {
        inst->sfontId = fluid_synth_sfload(inst->synth, path.c_str(), 0);
        if (inst->sfontId == FLUID_FAILED) {
            error = "FluidSynth could not load '" + path + "'";
            return nullptr;
        }
        inst->disabledControllers = scanDisabledControllers(path);
        int poly = 1024;
        fluid_settings_getint(inst->settings, "synth.polyphony", &poly);
        inst->voiceBuf.assign(static_cast<size_t>(std::max(poly, kMaxPolyphony)) + 1, nullptr);
        if (fluid_sfont_t* sf = fluid_synth_get_sfont_by_id(inst->synth, inst->sfontId)) {
            fluid_sfont_iteration_start(sf);
            while (fluid_preset_t* p = fluid_sfont_iteration_next(sf)) {
                const char* name = fluid_preset_get_name(p);
                presets.push_back({fluid_preset_get_banknum(p), fluid_preset_get_num(p),
                                   name ? name : ""});
            }
        }
        std::sort(presets.begin(), presets.end(), [](const PresetEntry& a, const PresetEntry& b) {
            return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
        });
    }
    return inst;
}

void applyParams(FluidInstance& inst, SharedParams& params) {
    fluid_synth_t* s = inst.synth;
    if (!s) return;

    const bool firstApply = std::isnan(inst.applied[0]);
    double v[kNumParams];
    bool changed[kNumParams];
    for (uint32_t i = 0; i < kNumParams; ++i) {
        v[i] = params.values[i].load(std::memory_order_relaxed);
        changed[i] = !(v[i] == inst.applied[i]); // NaN-safe: NaN != anything
        inst.applied[i] = v[i];
    }
    auto any = [&](std::initializer_list<ParamId> ids) {
        for (auto id : ids)
            if (changed[id]) return true;
        return false;
    };
    auto asInt = [&](ParamId id) { return static_cast<int>(std::lround(v[id])); };
    const bool multi = asInt(kParamMidiMode) == kMidiModeMulti;

    if (changed[kParamGain]) fluid_synth_set_gain(s, static_cast<float>(v[kParamGain]));
    if (changed[kParamPolyphony]) fluid_synth_set_polyphony(s, asInt(kParamPolyphony));
    {
        // Offline render (clap.render) may use its own, higher-quality method.
        const int renderMode = std::clamp(asInt(kParamRenderInterpolation), 0, kNumInterpModes);
        const int interp =
            params.offlineRender.load(std::memory_order_relaxed) && renderMode > 0
                ? kInterpValues[renderMode - 1]
                : kInterpValues[std::clamp(asInt(kParamInterpolation), 0, kNumInterpModes - 1)];
        if (interp != inst.appliedInterp) {
            fluid_synth_set_interp_method(s, -1, interp);
            inst.appliedInterp = interp;
        }
    }
    if (changed[kParamPortamentoTimeMode])
        fluid_synth_set_portamento_time_mode(s, std::clamp(asInt(kParamPortamentoTimeMode), 0, 2));

    if (changed[kParamReverbOn]) fluid_synth_reverb_on(s, -1, asInt(kParamReverbOn) != 0);
    if (changed[kParamReverbLevel]) fluid_synth_set_reverb_group_level(s, -1, v[kParamReverbLevel]);
    if (changed[kParamReverbRoom]) fluid_synth_set_reverb_group_roomsize(s, -1, v[kParamReverbRoom]);
    if (changed[kParamReverbDamp]) fluid_synth_set_reverb_group_damp(s, -1, v[kParamReverbDamp]);
    if (changed[kParamReverbWidth]) fluid_synth_set_reverb_group_width(s, -1, v[kParamReverbWidth]);

    if (changed[kParamChorusOn]) fluid_synth_chorus_on(s, -1, asInt(kParamChorusOn) != 0);
    if (changed[kParamChorusLevel]) fluid_synth_set_chorus_group_level(s, -1, v[kParamChorusLevel]);
    if (changed[kParamChorusVoices]) fluid_synth_set_chorus_group_nr(s, -1, asInt(kParamChorusVoices));
    if (changed[kParamChorusSpeed]) fluid_synth_set_chorus_group_speed(s, -1, v[kParamChorusSpeed]);
    if (changed[kParamChorusDepth]) fluid_synth_set_chorus_group_depth(s, -1, v[kParamChorusDepth]);

    if (changed[kParamBendRange])
        for (int c = 0; c < kNumMidiChannels; ++c)
            fluid_synth_pitch_wheel_sens(s, c, asInt(kParamBendRange));

    // LFO (AMT > 0): new notes get the panel's vibrato in place of the
    // preset's (patchNewVoices); turning AMT/SPD also retunes the vibrato
    // of the notes already sounding. The delay only matters to new notes.
    if (any({kParamLfoAmount, kParamLfoSpeed}) && v[kParamLfoAmount] > 0.0 &&
        !inst.voiceBuf.empty()) {
        const std::array<GenValue, 3> vib = vibratoGens(params);
        fluid_synth_get_voicelist(s, inst.voiceBuf.data(), static_cast<int>(inst.voiceBuf.size()),
                                  -1);
        for (fluid_voice_t* voice : inst.voiceBuf) {
            if (!voice) break;
            for (int i = 0; i < 2; ++i) {
                fluid_voice_gen_set(voice, vib[i].gen, vib[i].value);
                fluid_voice_update_param(voice, vib[i].gen);
            }
        }
        fluid_synth_get_active_voice_count(s); // commits the queued updates
    }

    // Generator offsets, identical on every channel (FluidSynth adds
    // API-set generator values on top of the preset's own - see
    // shared.hpp's kParamInfos comment for the units). The envelope and
    // LFO aren't among them: they replace values, see patchNewVoices.
    if (any({kParamCutoff, kParamResonance, kParamFineTune})) {
        for (int c = 0; c < kNumMidiChannels; ++c) {
            fluid_synth_set_gen(s, c, GEN_FILTERFC, static_cast<float>(v[kParamCutoff] * 100.0));
            fluid_synth_set_gen(s, c, GEN_FILTERQ, static_cast<float>(v[kParamResonance] * 10.0));
            fluid_synth_set_gen(s, c, GEN_FINETUNE, static_cast<float>(v[kParamFineTune]));
        }
    }

    // Override Send: new notes get it in patchNewVoices; turning LEVEL (or
    // the switch on) also sets it on the notes already sounding.
    if (any({kParamReverbSendOverride, kParamReverbSend, kParamChorusSendOverride,
             kParamChorusSend}) &&
        !inst.voiceBuf.empty()) {
        const std::array<SendOverride, 2> sends = sendOverrides(params);
        if (sends[0].on || sends[1].on) {
            fluid_synth_get_voicelist(s, inst.voiceBuf.data(),
                                      static_cast<int>(inst.voiceBuf.size()), -1);
            for (fluid_voice_t* voice : inst.voiceBuf) {
                if (!voice) break;
                for (const SendOverride& so : sends)
                    if (so.on) applySendOverride(voice, so);
            }
            for (int c = 0; c < kNumMidiChannels; ++c)
                for (const SendOverride& so : sends)
                    if (so.on) resendCc(s, c, so.cc);
        }
    }

    if (changed[kParamVoiceMode]) applyVoiceMode(inst, std::clamp(asInt(kParamVoiceMode), 0, 2));
    if (changed[kParamPortamento] || changed[kParamVoiceMode]) {
        const int porta = std::clamp(asInt(kParamPortamento), 0, 127);
        fluid_synth_set_portamento_mode(s, 0, FLUID_CHANNEL_PORTAMENTO_MODE_EACH_NOTE);
        fluid_synth_cc(s, 0, 5, porta);
        fluid_synth_cc(s, 0, 65, porta > 0 ? 127 : 0);
    }

    if (changed[kParamMidiMode] && !firstApply) {
        // Switching modes re-routes every channel: stop anything ringing.
        fluid_synth_all_sounds_off(s, -1);
        for (auto& ch : inst.heldKey) ch.fill(-1);
    }

    // Channel 1's patch always follows the Bank/Patch params (in both
    // modes); in multitimbral mode the other 15 keep whatever they had
    // before this instance existed (restored once after a SoundFont swap).
    if (any({kParamBank, kParamProgram}))
        applyChannelPatch(inst, params, 0, asInt(kParamBank), asInt(kParamProgram));
    if (!inst.channelsRestored) {
        inst.channelsRestored = true;
        for (int c = 1; c < kNumMidiChannels; ++c)
            applyChannelPatch(inst, params, c, params.channelBank[c].load(),
                              params.channelProgram[c].load());
    }
    if (multi) {
        for (int c = 1; c < kNumMidiChannels; ++c) {
            int req = params.pendingChannelPatch[c].exchange(-1);
            if (req >= 0) applyChannelPatch(inst, params, c, req >> 8, req & 0x7F);
        }
    }
}

void ControllerShadow::reset() {
    for (int c = 0; c < kNumMidiChannels; ++c) resetChannel(c);
}

void ControllerShadow::resetChannel(int ch) {
    cc[ch].fill(-1);
    pitchBend[ch] = -1;
    channelPressure[ch] = -1;
}

namespace {

// Controllers worth restoring on a new synth: plain continuous controllers
// and switches. Not bank select (the Bank/Patch params own that), not the
// RPN/NRPN/data-entry family (replaying those out of order would write
// garbage into whatever parameter is selected - bend range has its own
// param), not channel mode messages (120-127).
bool isReplayableCC(int num) {
    switch (num) {
        case 0: case 32:                      // bank select
        case 6: case 38:                      // data entry
        case 96: case 97: case 98: case 99:   // data inc/dec, NRPN
        case 100: case 101:                   // RPN
            return false;
        default:
            return num < 120;
    }
}

} // namespace

void replayControllers(FluidInstance& inst, const ControllerShadow& shadow) {
    fluid_synth_t* s = inst.synth;
    if (!s) return;
    for (int c = 0; c < kNumMidiChannels; ++c) {
        for (int num = 0; num < 128; ++num)
            if (shadow.cc[c][num] >= 0) fluid_synth_cc(s, c, num, shadow.cc[c][num]);
        if (shadow.pitchBend[c] >= 0) fluid_synth_pitch_bend(s, c, shadow.pitchBend[c]);
        if (shadow.channelPressure[c] >= 0)
            fluid_synth_channel_pressure(s, c, shadow.channelPressure[c]);
    }
}

void handleMidi(FluidInstance& inst, SharedParams& params, ControllerShadow& shadow,
                uint8_t status, uint8_t d1, uint8_t d2) {
    fluid_synth_t* s = inst.synth;
    if (!s) return;
    const int type = status & 0xF0;
    const bool multi = std::lround(params.values[kParamMidiMode].load()) == kMidiModeMulti;
    const int inChannel = status & 0x0F;
    const int ch = multi ? inChannel : 0;
    const bool receivePC = std::lround(params.values[kParamReceivePC].load()) != 0;

    switch (type) {
        case 0x90:
            if (d2 > 0) {
                // Drum kits are never transposed - a kit's keys are
                // instruments, not pitches.
                const bool drums = isDrumBank(params.channelBank[ch].load());
                const int transpose =
                    drums ? 0 : static_cast<int>(std::lround(params.values[kParamTranspose].load()));
                const int key = d1 + transpose;
                if (key < 0 || key > 127) return;
                if (inst.heldKey[inChannel][d1] >= 0)
                    fluid_synth_noteoff(s, ch, inst.heldKey[inChannel][d1]);
                inst.heldKey[inChannel][d1] = static_cast<int16_t>(key);
                fluid_synth_noteon(s, ch, key, d2);
                patchNewVoices(inst, params, ch, shadow.channelPressure[ch]);
                params.noteActive[d1].store(true);
                params.noteVelocity01[d1].store(d2 / 127.f);
                params.channelActivity[inChannel].store(d2 / 127.f);
                return;
            }
            [[fallthrough]]; // note-on with velocity 0 = note-off
        case 0x80: {
            const int key = inst.heldKey[inChannel][d1];
            if (key < 0) return;
            inst.heldKey[inChannel][d1] = -1;
            fluid_synth_noteoff(s, ch, key);
            params.noteActive[d1].store(false);
            return;
        }
        case 0xA0:
            fluid_synth_key_pressure(s, ch, d1, d2);
            return;
        case 0xB0:
            if (d1 == 0 || d1 == 32) { // bank select
                logProgramEvent(d1 == 0 ? kEvBankMsb : kEvBankLsb, inChannel, d2, 0);
                if (!receivePC) return;
                if (!multi) {
                    if (d1 == 0) inst.omniBankMsb = d2;
                    return;
                }
            }
            if (d1 == 120 || d1 == 123) { // all sounds/notes off
                for (auto& k : inst.heldKey[inChannel]) k = -1;
            }
            if (d1 == 121) { // reset all controllers: nothing left to restore
                const auto keep = shadow.cc[ch];
                shadow.resetChannel(ch);
                // RP-015: volume, pan and effect sends survive a reset
                for (int n : {7, 10, 91, 93}) shadow.cc[ch][n] = keep[n];
            } else if (isReplayableCC(d1)) {
                shadow.cc[ch][d1] = d2;
            }
            fluid_synth_cc(s, ch, d1, d2);
            return;
        case 0xC0:
            if (!receivePC) {
                logProgramEvent(kEvProgramIgnored, inChannel, d1, 0);
                return;
            }
            if (!multi) {
                int bank = inst.omniBankMsb >= 0
                               ? inst.omniBankMsb
                               : static_cast<int>(std::lround(params.values[kParamBank].load()));
                // GM2/GS drum-kit convention: MSB 120/127 -> bank 128.
                if (bank == 120 || bank == 127) bank = 128;
                params.values[kParamBank].store(bank);
                params.values[kParamProgram].store(d1);
                inst.programChangedByMidi = true;
                logProgramEvent(kEvProgramChange, inChannel, d1, bank);
                return; // plugin.cpp re-runs applyParams right away
            }
            fluid_synth_program_change(s, ch, d1);
            mirrorChannelPatch(inst, params, ch);
            logProgramEvent(kEvProgramChange, inChannel, d1, params.channelBank[ch].load());
            if (ch == 0) {
                params.values[kParamBank].store(params.channelBank[0].load());
                params.values[kParamProgram].store(params.channelProgram[0].load());
                inst.applied[kParamBank] = params.values[kParamBank].load();
                inst.applied[kParamProgram] = params.values[kParamProgram].load();
                inst.programChangedByMidi = true;
            }
            return;
        case 0xD0:
            shadow.channelPressure[ch] = d1;
            fluid_synth_channel_pressure(s, ch, d1);
            return;
        case 0xE0:
            shadow.pitchBend[ch] = (d2 << 7) | d1;
            fluid_synth_pitch_bend(s, ch, (d2 << 7) | d1);
            return;
        default:
            return;
    }
}

void handleSysex(FluidInstance& inst, const uint8_t* data, uint32_t size) {
    if (!inst.synth || !data || size == 0) return;
    fluid_synth_sysex(inst.synth, reinterpret_cast<const char*>(data), static_cast<int>(size),
                      nullptr, nullptr, nullptr, 0);
    const int reset = sysexResetKind(data, size);
    if (reset >= 0) {
        // A GM/GS/XG reset makes fluidsynth put every channel back on
        // program 0 and drop all API-set generator offsets/effect settings,
        // while the plugin's parameters (the patch saved with the project,
        // the knobs) still say otherwise. Only an explicit Program Change
        // should move the patch, so re-apply everything on the next
        // applyParams() (see plugin.cpp's dispatch).
        inst.applied.fill(std::numeric_limits<double>::quiet_NaN());
        inst.channelsRestored = false;
        inst.appliedInterp = -1;
        inst.omniBankMsb = -1;
        logProgramEvent(kEvSysexReset, 0, reset, 0);
    }
}

void renderFluid(FluidInstance& inst, float* left, float* right, uint32_t frames) {
    if (!inst.synth || frames == 0) return;
    fluid_synth_write_float(inst.synth, static_cast<int>(frames), left, 0, 1, right, 0, 1);
}

void allSoundsOff(FluidInstance& inst) {
    if (!inst.synth) return;
    fluid_synth_all_sounds_off(inst.synth, -1);
    for (auto& ch : inst.heldKey) ch.fill(-1);
}

int fluidActiveVoices(FluidInstance& inst) {
    return inst.synth ? fluid_synth_get_active_voice_count(inst.synth) : 0;
}

void installFluidLogHook() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int level : {FLUID_PANIC, FLUID_ERR, FLUID_WARN, FLUID_INFO, FLUID_DBG})
            fluid_set_log_function(level, fluidLogHook, nullptr);
    });
}

std::vector<std::string> recentProgramEvents() {
    std::vector<std::string> out;
    const uint32_t last = g_programLogSeq.load(std::memory_order_acquire);
    const uint32_t first = last > kProgramLogSize ? last - kProgramLogSize + 1 : 1;
    for (uint32_t seq = first; seq <= last && seq != 0; ++seq) {
        const uint64_t e = g_programLog[seq % kProgramLogSize].load(std::memory_order_acquire);
        if (uint32_t(e >> 32) != seq) continue; // overwritten meanwhile
        const int kind = int((e >> 24) & 0xFF), ch = int((e >> 16) & 0xFF) + 1;
        const int a = int((e >> 8) & 0xFF), b = int(e & 0xFF);
        char buf[96];
        switch (kind) {
            case kEvProgramChange:
                snprintf(buf, sizeof(buf), "ch %d: Program Change %d -> patch [%03d:%03d]", ch, a,
                         b, a);
                break;
            case kEvProgramIgnored:
                snprintf(buf, sizeof(buf), "ch %d: Program Change %d ignored (Receive off)", ch, a);
                break;
            case kEvBankMsb:
                snprintf(buf, sizeof(buf), "ch %d: Bank Select MSB (CC0) = %d", ch, a);
                break;
            case kEvBankLsb:
                snprintf(buf, sizeof(buf), "ch %d: Bank Select LSB (CC32) = %d", ch, a);
                break;
            case kEvSysexReset:
                snprintf(buf, sizeof(buf), "%s reset SysEx (patch and settings re-applied)",
                         a == 0 ? "GM" : a == 1 ? "GS" : "XG");
                break;
            default:
                continue;
        }
        out.emplace_back(buf);
    }
    return out;
}

std::vector<std::string> recentFluidLogLines() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return {g_logLines.begin(), g_logLines.end()};
}

std::vector<ComponentVersion> componentVersions() {
#ifdef FSP_HAVE_SNDFILE
    // sf_version_string() is "libsndfile-1.2.2" - keep just the number
    std::string sndfileVersion = sf_version_string() ? sf_version_string() : "?";
    if (sndfileVersion.rfind("libsndfile-", 0) == 0) sndfileVersion.erase(0, 11);
    const char* sndfileRole = "SF3 decoding";
#else
    // Built without libsndfile (see CMakeLists.txt): SF3 can't be loaded.
    const std::string sndfileVersion = "-";
    const char* sndfileRole = "not built in: no SF3 support";
#endif
    char clapVersion[32];
    snprintf(clapVersion, sizeof(clapVersion), "%d.%d.%d", CLAP_VERSION_MAJOR, CLAP_VERSION_MINOR,
             CLAP_VERSION_REVISION);
    return {
        {"FluidSynth Player", FSP_PLUGIN_VERSION, "this CLAP plugin"},
        {"FluidSynthPlugin", FSP_UPSTREAM_VERSION, "upstream project it is based on (T. Tensi)"},
        {"FluidSynth", fluidRuntimeVersion(), "statically linked (DLS, SF3, Signalsmith)"},
        {"libsndfile", sndfileVersion, sndfileRole},
        {"CLAP", clapVersion, "plugin API"},
        {"Dear ImGui", IMGUI_VERSION, "user interface"},
    };
}

std::string fluidRuntimeVersion() {
    int major = 0, minor = 0, micro = 0;
    fluid_version(&major, &minor, &micro);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(micro);
}
