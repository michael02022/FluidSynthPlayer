// Display formatting for parameter values - shared by clap.params'
// value_to_text (host automation lanes) and the editor's knobs.
#include <cmath>
#include <cstdio>
#include <string>

#include "shared.hpp"

std::string formatParamValue(ParamId id, double value) {
    char buf[64];
    const int iv = static_cast<int>(std::lround(value));
    switch (id) {
        case kParamGain:
            if (value <= 0.0) return "-inf dB";
            snprintf(buf, sizeof(buf), "%.1f dB", 20.0 * std::log10(value));
            break;
        case kParamBank:
            snprintf(buf, sizeof(buf), iv == 128 ? "%d (drums)" : "%d", iv);
            break;
        case kParamProgram:
            snprintf(buf, sizeof(buf), "%d", iv);
            break;
        case kParamReverbSend:
        case kParamChorusSend:
            snprintf(buf, sizeof(buf), "%.0f%%", value * 100.0);
            break;
        case kParamRestoreControllers:
        case kParamLimiterOn:
        case kParamReverbSendOverride:
        case kParamChorusSendOverride:
        case kParamEnvEnable:
        case kParamReverbOn:
        case kParamChorusOn:
        case kParamReceivePC:
            return iv ? "On" : "Off";
        case kParamEnvAttack:
        case kParamEnvDecay:
        case kParamEnvRelease:
            snprintf(buf, sizeof(buf), "x%.2f", value);
            break;
        case kParamEnvSustain:
            snprintf(buf, sizeof(buf), "%+.1f dB", value);
            break;
        case kParamLfoAmount:
            snprintf(buf, sizeof(buf), "%.0f ct", value);
            break;
        case kParamLfoSpeed:
            snprintf(buf, sizeof(buf), "%.2f Hz", value);
            break;
        case kParamLfoDelay:
            snprintf(buf, sizeof(buf), "%.2f s", value);
            break;
        case kParamCutoff:
            snprintf(buf, sizeof(buf), "%+.1f st", value);
            break;
        case kParamResonance:
            snprintf(buf, sizeof(buf), "+%.1f dB", value);
            break;
        case kParamReverbLevel:
        case kParamReverbRoom:
        case kParamReverbDamp:
            snprintf(buf, sizeof(buf), "%.2f", value);
            break;
        case kParamReverbWidth:
            snprintf(buf, sizeof(buf), "%.1f", value);
            break;
        case kParamChorusLevel:
            snprintf(buf, sizeof(buf), "%.2f", value);
            break;
        case kParamChorusVoices:
            snprintf(buf, sizeof(buf), "%d", iv);
            break;
        case kParamChorusSpeed:
            snprintf(buf, sizeof(buf), "%.2f Hz", value);
            break;
        case kParamChorusDepth:
            snprintf(buf, sizeof(buf), "%.1f ms", value);
            break;
        case kParamPolyphony:
            snprintf(buf, sizeof(buf), "%d voices", iv);
            break;
        case kParamBendRange:
            snprintf(buf, sizeof(buf), "%d st", iv);
            break;
        case kParamTranspose:
            snprintf(buf, sizeof(buf), "%+d st", iv);
            break;
        case kParamFineTune:
            snprintf(buf, sizeof(buf), "%+.1f ct", value);
            break;
        case kParamInterpolation:
            return kInterpNames[iv < 0 ? 0 : iv >= kNumInterpModes ? kNumInterpModes - 1 : iv];
        case kParamRenderInterpolation:
            return kRenderInterpNames[iv < 0 ? 0 : iv > kNumInterpModes ? kNumInterpModes : iv];
        case kParamReverbEngine:
            return kReverbEngineNames[iv < 0 ? 0 : iv >= kNumReverbEngines ? kNumReverbEngines - 1 : iv];
        case kParamPortamentoTimeMode:
            return kPortaTimeModeNames[iv < 0 ? 0 : iv > 2 ? 2 : iv];
        case kParamLimiterCeiling:
            snprintf(buf, sizeof(buf), "%.1f dB", value);
            break;
        case kParamLimiterRelease:
            snprintf(buf, sizeof(buf), "%.0f ms", value);
            break;
        case kParamMidiMode:
            return kMidiModeNames[iv ? 1 : 0];
        case kParamVoiceMode:
            return kVoiceModeNames[iv < 0 ? 0 : iv > 2 ? 2 : iv];
        case kParamPortamento:
            return iv == 0 ? std::string("Off") : std::to_string(iv);
        default:
            snprintf(buf, sizeof(buf), "%.3f", value);
            break;
    }
    return buf;
}
