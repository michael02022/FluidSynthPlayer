/**
 * @file
 * The <C>FluidSynthSynthesizer</C> class implements an object-oriented
 * wrapper around an underlying fluidsynth synthesizer object with
 * dynamic library access.
 *
 * @author Dr. Thomas Tensi
 * @date   2022-04
 */

/*=========*/
/* IMPORTS */
/*=========*/

#include "DynamicLibrary.h"
#include "FluidSynthSynthesizer.h"
#include "Logging.h"
#include "MyArray.h"

/*--------------------*/

using BaseTypes::Containers::Array_clear;
using BaseTypes::Containers::Array_convert;
using Libraries::DynamicLibrary;
using Libraries::FluidSynth::FluidSynthSynthesizer;

/*-----------------------------------------*/
/* TYPES AND FUNCTIONS FOR DYNAMIC BINDING */
/*-----------------------------------------*/

/** synthesizer creation function type in library  */
typedef Object (*_FSSynthesizer_CreationProc)(Object);

/** synthesizer destruction function type in library */
typedef void (*_FSSynthesizer_DestructionProc)(Object);

/** MIDI bank selection function type in library */
typedef int (*_FSSynthesizer_BankChangeProc)(Object, int, int);

/** MIDI control change function type in library */
typedef int (*_FSSynthesizer_ControlChangeProc)(Object, int, int, int);

/** MIDI channel pressure function type in library */
typedef int (*_FSSynthesizer_ChannelPressureProc)(Object, int, int);

/** MIDI note off function type in library */
typedef int (*_FSSynthesizer_NoteOffProc)(Object, int, int);

/** MIDI note on function type in library */
typedef int (*_FSSynthesizer_NoteOnProc)(Object, int, int, int);

/** MIDI pitch bend function type in library */
typedef int (*_FSSynthesizer_PitchBendProc)(Object, int, int);

/** MIDI polyphonic key pressure function type in library */
typedef int (*_FSSynthesizer_PolyphonicKeyPressureProc)(Object, int, int, int);

/** audio buffer processing function type in library */
typedef int (*_FSSynthesizer_ProcessProc)(Object, int, int,
                                         Object, int, Object);

/** MIDI program change function type in library */
typedef int (*_FSSynthesizer_ProgChangeProc)(Object, int, int);

/** sound font load function type in library */
typedef int (*_FSSynthesizer_SFLoadProc)(Object, const char*, int);

/** internal size function type in library */
typedef int (*_FSSynthesizer_SizeProc)(Object);

/** SPECIAL FUNCTION: setting function type for interpolation method
  * in library */
typedef int (*_FSSynthesizer_SetInterpolationMethodProc)(Object, int, int);
 
/*--------------------*/

namespace Libraries::FluidSynth {

    struct _FSSynthesizerDescriptor {

        /** the underlying synthesizer object */
        Object synthesizer = nullptr;

        /** the associated fluidsynth library */
        FluidSynth* associatedLibrary = nullptr;

        /** synthesizer creation function in library  */
        _FSSynthesizer_CreationProc synthesizerMake;

        /** synthesizer destruction function in library */
        _FSSynthesizer_DestructionProc synthesizerDestroy;

        /** the internal buffer size (raster) in library */
        _FSSynthesizer_SizeProc synthesizerInternalBufferSize;

        /** the number of audio channel pairs in library */
        _FSSynthesizer_SizeProc synthesizerAudioChannelPairCount;

        /** MIDI bank select function type in library */
        _FSSynthesizer_BankChangeProc synthesizerHandleBankChange;

        /** MIDI channel pressure function in library */
        _FSSynthesizer_ChannelPressureProc synthesizerHandleChannelPressure;

        /** MIDI control change function type in library */
        _FSSynthesizer_ControlChangeProc
            synthesizerHandleControlChange;

        /** MIDI note off function in library */
        _FSSynthesizer_NoteOffProc synthesizerHandleNoteOff;

        /** MIDI note on function in library */
        _FSSynthesizer_NoteOnProc synthesizerHandleNoteOn;

        /** MIDI pitch bend processing function in library */
        _FSSynthesizer_PitchBendProc synthesizerHandlePitchBend;

        /** MIDI polyphonic pressure function in library */
        _FSSynthesizer_PolyphonicKeyPressureProc
            synthesizerHandlePolyphonicPressure;

        /** MIDI program change function in library */
        _FSSynthesizer_ProgChangeProc synthesizerHandleProgramChange;

        /** sound font load function in library */
        _FSSynthesizer_SFLoadProc synthesizerLoadSoundFont;

        /** audio buffer processing function in library */
        _FSSynthesizer_ProcessProc synthesizerProcess;

        /** interpolation method set function in library */
        _FSSynthesizer_SetInterpolationMethodProc
            synthesizerSetInterpolationMethod;

        /*--------------------*/
        /* con-/destruction   */
        /*--------------------*/

        /**
         * Connects functions dynamically to functions from
         * <C>fsLibrary</C> and stores <C>fsSettings</C> object.
         *
         * @param[in] fsLibrary   associated fluidsynth library
         * @param[in] fsSettings  associated fluidsynth settings
         *                        object
         */
        _FSSynthesizerDescriptor (IN FluidSynth* fsLibrary,
                                  IN FluidSynthSettings* fsSettings);

        /*--------------------*/

        /**
         * Finalizes descriptor
         */
        ~_FSSynthesizerDescriptor ();

    };
}

/*====================*/

using Libraries::FluidSynth::_FSSynthesizerDescriptor;

/*--------*/
/* MACROS */
/*--------*/

/** error message for an undefined descriptor */
static const String _errorMessageForUndefinedDescriptor =
    "synthesizer object must be defined";

/** error message for a bad interpolation method */
static const String _errorMessageForBadInterpolationMethodCode =
    "interpolation method code must be 0, 1, 4, 5, 6 or 7";

/** reports that function with <C>name</C> is not dynamically defined */
#define _reportBadFunction(name) \
    Logging_traceError("synthesizer '" name "' function undefined")

/** simple macro for dynamic binding of a function */
#define GPA(signature, name) \
    reinterpret_cast<signature>(library->getFunctionByName(name))

/*==========================*/
/* _FSSynthesizerDescriptor */
/*==========================*/

_FSSynthesizerDescriptor::
_FSSynthesizerDescriptor (IN FluidSynth* fsLibrary,
                          IN FluidSynthSettings* fsSettings)
{
    Logging_trace(">>");

    if (fsLibrary == nullptr) {
        Logging_traceError("fluidsynth library not loaded");
    } else {
        associatedLibrary = const_cast<FluidSynth*>(fsLibrary);
        const DynamicLibrary* library =
            reinterpret_cast<DynamicLibrary*>(fsLibrary->underlyingObject());

        synthesizerMake =
            GPA(_FSSynthesizer_CreationProc, "new_fluid_synth");
        synthesizerDestroy =
            GPA(_FSSynthesizer_DestructionProc, "delete_fluid_synth");

        synthesizerInternalBufferSize =
            GPA(_FSSynthesizer_SizeProc,
                "fluid_synth_get_internal_bufsize");
        synthesizerAudioChannelPairCount =
            GPA(_FSSynthesizer_SizeProc,
                "fluid_synth_count_audio_channels");
        synthesizerHandleBankChange =
            GPA(_FSSynthesizer_BankChangeProc, "fluid_synth_bank_select");
        synthesizerHandleChannelPressure = 
            GPA(_FSSynthesizer_ChannelPressureProc,
                "fluid_synth_channel_pressure");
        synthesizerHandleControlChange = 
            GPA(_FSSynthesizer_ControlChangeProc, "fluid_synth_cc");
        synthesizerHandleNoteOff =
            GPA(_FSSynthesizer_NoteOffProc, "fluid_synth_noteoff");
        synthesizerHandleNoteOn =
            GPA(_FSSynthesizer_NoteOnProc, "fluid_synth_noteon");
        synthesizerHandlePitchBend =
            GPA(_FSSynthesizer_PitchBendProc, "fluid_synth_pitch_bend");
        synthesizerHandlePolyphonicPressure = 
            GPA(_FSSynthesizer_PolyphonicKeyPressureProc,
                "fluid_synth_key_pressure");
        synthesizerHandleProgramChange =
            GPA(_FSSynthesizer_ProgChangeProc,
                "fluid_synth_program_change");
        synthesizerLoadSoundFont =
            GPA(_FSSynthesizer_SFLoadProc, "fluid_synth_sfload");
        synthesizerProcess =
            GPA(_FSSynthesizer_ProcessProc, "fluid_synth_process");
        synthesizerSetInterpolationMethod =
            GPA(_FSSynthesizer_SetInterpolationMethodProc,
                "fluid_synth_set_interp_method");
        
        if (synthesizerMake == nullptr) {
            _reportBadFunction("make");
        } else {
            synthesizer = synthesizerMake(fsSettings->underlyingObject());
        }
    }

    Logging_trace("<<");
}

/*--------------------*/

_FSSynthesizerDescriptor::~_FSSynthesizerDescriptor ()
{
    Logging_trace(">>");

    if (synthesizer != nullptr) {
        if (synthesizerDestroy == nullptr) {
            _reportBadFunction("destroy");
        } else {
            synthesizerDestroy(synthesizer);
        }
    }

    Logging_trace("<<");
}

/*--------------------*/

/*====================*/
/* PUBLIC FEATURES    */
/*====================*/

FluidSynthSynthesizer::FluidSynthSynthesizer (IN FluidSynth* library,
                                              IN FluidSynthSettings* settings)
    : _descriptor{nullptr}
{
    Logging_trace(">>");

    if (!library->isLoaded() || settings == nullptr) {
        Logging_traceError("library or settings object is undefined");
    } else {
        _descriptor = new _FSSynthesizerDescriptor(library, settings);
    }
    
    Logging_trace("<<");
}

/*--------------------*/

FluidSynthSynthesizer::~FluidSynthSynthesizer ()
{
    Logging_trace(">>");
    delete reinterpret_cast<_FSSynthesizerDescriptor*>(_descriptor);
    Logging_trace("<<");
}

/*--------------------*/
/* property access    */
/*--------------------*/

Natural FluidSynthSynthesizer::internalBufferSize () const
{
    Logging_trace(">>");
    Natural result = 0;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_SizeProc synthesizerInternalBufferSize =
            descriptor.synthesizerInternalBufferSize;

        if (synthesizerInternalBufferSize == nullptr) {
            _reportBadFunction("internalBufferSize");
        } else {
            result =
                synthesizerInternalBufferSize(descriptor.synthesizer);
        }
    }
    
    Logging_trace1("<<: %1", TOSTRING(result));
    return result;
}

/*--------------------*/

FluidSynth* FluidSynthSynthesizer::library () const
{
    FluidSynth* result = nullptr;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        result = descriptor.associatedLibrary;
    }

    return result;
}

/*--------------------*/

Object FluidSynthSynthesizer::underlyingObject () const
{
    Object result = nullptr;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        result = descriptor.synthesizer;
    }

    return result;
}
        
/*--------------------*/
/* change             */
/*--------------------*/

Boolean
FluidSynthSynthesizer::handleBankChange (IN Natural channel,
                                         IN Natural bankNumber)
{
    Logging_trace2(">>: channel = %1, bankNumber = %2",
                   TOSTRING(channel), TOSTRING(bankNumber));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_BankChangeProc synthesizerHandleBankChange =
            descriptor.synthesizerHandleBankChange;

        if (synthesizerHandleBankChange == nullptr) {
            _reportBadFunction("bankChange");
        } else {
            Integer operationResult =
                synthesizerHandleBankChange(descriptor.synthesizer,
                                            int(channel),
                                            int(bankNumber));
            isOkay = (operationResult == 0);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean
FluidSynthSynthesizer::handleChannelPressure (IN Natural channel,
                                              IN Natural value)
{
    Logging_trace2(">>: channel = %1, value = %2",
                   TOSTRING(channel), TOSTRING(value));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_ChannelPressureProc synthesizerHandleChannelPressure =
            descriptor.synthesizerHandleChannelPressure;

        if (synthesizerHandleChannelPressure == nullptr) {
            _reportBadFunction("channelPressure");
        } else {
            Integer operationResult =
                synthesizerHandleChannelPressure(descriptor.synthesizer,
                                                 int(channel),
                                                 int(value));
            isOkay = (operationResult == 0);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean FluidSynthSynthesizer::handleControlChange (IN Natural channel,
                                                    IN Natural controller,
                                                    IN Natural value)
{
    Logging_trace3(">>: channel = %1, controller = %2, value = %3",
                   TOSTRING(channel), TOSTRING(controller), TOSTRING(value));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_ControlChangeProc synthesizerHandleControlChange =
            descriptor.synthesizerHandleControlChange;

        if (synthesizerHandleControlChange == nullptr) {
            _reportBadFunction("controlChange");
        } else {
            Integer operationResult =
                synthesizerHandleControlChange(descriptor.synthesizer,
                                               int(channel),
                                               int(controller),
                                               int(value));
            isOkay = (operationResult == 0);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean FluidSynthSynthesizer::handleNoteOff (IN Natural channel,
                                              IN Natural note)
{
    Logging_trace2(">>: channel = %1, note = %2",
                   TOSTRING(channel), TOSTRING(note));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_NoteOffProc synthesizerHandleNoteOff =
            descriptor.synthesizerHandleNoteOff;
        
        if (synthesizerHandleNoteOff == nullptr) {
            _reportBadFunction("noteoff");
        } else {
            Integer operationResult =
                synthesizerHandleNoteOff(descriptor.synthesizer,
                                         int(channel),
                                         int(note));
            isOkay = (operationResult == 0);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean FluidSynthSynthesizer::handleNoteOn (IN Natural channel,
                                             IN Natural note,
                                             IN Natural velocity)
{
    Logging_trace3(">>: channel = %1, note = %2, velocity = %3",
                   TOSTRING(channel), TOSTRING(note),
                   TOSTRING(velocity));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_NoteOnProc synthesizerHandleNoteOn =
            descriptor.synthesizerHandleNoteOn;

        if (synthesizerHandleNoteOn == nullptr) {
            _reportBadFunction("noteon");
        } else {
            Integer operationResult =
                synthesizerHandleNoteOn(descriptor.synthesizer,
                                        int(channel),
                                        int(note),
                                        int(velocity));
            isOkay = (operationResult == 0);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean FluidSynthSynthesizer::handlePitchBend (IN Natural channel,
                                                IN Natural value)
{
    Logging_trace2(">>: channel = %1, value = %2",
                   TOSTRING(channel), TOSTRING(value));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_PitchBendProc synthesizerHandlePitchBend =
            descriptor.synthesizerHandlePitchBend;

        if (synthesizerHandlePitchBend == nullptr) {
            _reportBadFunction("pitchBend");
        } else {
            Integer operationResult =
                synthesizerHandlePitchBend(descriptor.synthesizer,
                                           int(channel),
                                           int(value));
            isOkay = (operationResult == 0);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean
FluidSynthSynthesizer::handlePolyphonicKeyPressure (IN Natural channel,
                                                    IN Natural key,
                                                    IN Natural value)
{
    Logging_trace3(">>: channel = %1, key = %2, value = %3",
                   TOSTRING(channel), TOSTRING(key), TOSTRING(value));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_PolyphonicKeyPressureProc
            synthesizerHandlePolyphonicPressure =
                descriptor.synthesizerHandlePolyphonicPressure;
        
        if (synthesizerHandlePolyphonicPressure == nullptr) {
            _reportBadFunction("polyphonicKeyPressure");
        } else {
            Integer operationResult =
                synthesizerHandlePolyphonicPressure(descriptor.synthesizer,
                                                    int(channel),
                                                    int(key),
                                                    int(value));
            isOkay = (operationResult == 0);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean
FluidSynthSynthesizer::handleProgramChange (IN Natural channel,
                                            IN Natural programNumber)
{
    Logging_trace2(">>: channel = %1, programNumber = %2",
                   TOSTRING(channel), TOSTRING(programNumber));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_ProgChangeProc synthesizerHandleProgramChange =
            descriptor.synthesizerHandleProgramChange;

         if (synthesizerHandleProgramChange == nullptr) {
             _reportBadFunction("programChange");
        } else {
             Integer operationResult =
                 synthesizerHandleProgramChange(descriptor.synthesizer,
                                                int(channel),
                                                int(programNumber));
             isOkay = (operationResult == 0);
         }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean FluidSynthSynthesizer::loadSoundFont (IN String& soundFontPath)
{
    Logging_trace1(">>: %1", soundFontPath);

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_SFLoadProc synthesizerLoadSoundFont =
            descriptor.synthesizerLoadSoundFont;

        if (synthesizerLoadSoundFont == nullptr) {
            _reportBadFunction("loadSoundFont");
        } else {
            int soundFontId =
                synthesizerLoadSoundFont(descriptor.synthesizer,
                                         soundFontPath.c_str(), 1);

            if (soundFontId < 0) {
                Logging_trace("--: loading of the sound font failed!");
            } else {
                Logging_trace("--: sound font loaded");
                isOkay = true;
            }
        }
    }

    Logging_trace1("<<", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean
FluidSynthSynthesizer::process
    (INOUT AudioDataPointListVector& dataPointBuffer,
     IN Natural position,
     IN Natural dataPointCount)
{
    Natural channelCount = dataPointBuffer.length();
    Logging_trace3(">>: channelCount = %1, position = %2,"
                   " dataPointCount = %3",
                   TOSTRING(channelCount),
                   TOSTRING(position),
                   TOSTRING(dataPointCount));

    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_ProcessProc synthesizerProcess =
            descriptor.synthesizerProcess;
        _FSSynthesizer_SizeProc synthesizerAudioChannelPairCount =
            descriptor.synthesizerAudioChannelPairCount;

        if (synthesizerProcess == nullptr) {
            _reportBadFunction("process");
        } else if (synthesizerAudioChannelPairCount == nullptr) {
            _reportBadFunction("audioChannelPairCount");
        } else {
            int i;

            /* count of channels that FluidSynth can handle */
            const Natural fluidSynthChannelCount =
                2 * synthesizerAudioChannelPairCount(descriptor.synthesizer);
            int effectiveChannelCount =
                int(Natural::minimum(channelCount + channelCount % 2,
                                     fluidSynthChannelCount));

            /* provide a float buffer for FluidSynth with
             * <dataPointCount> frames */
            float** floatDataPointBuffer =
                static_cast<float**>(Array_createLocal(float*,
                                                    effectiveChannelCount));

            for (i = 0;  i < effectiveChannelCount;  i++) {
                floatDataPointBuffer[i] = 
                    static_cast<float*>(Array_createLocal(float,
                                                          dataPointCount));
                Array_clear(floatDataPointBuffer[i], dataPointCount, 0.0f);
            };

            /* mix dry audio and effects into data point buffer */
            Integer operationResult =
                synthesizerProcess(descriptor.synthesizer,
                                   int(dataPointCount),
                                   effectiveChannelCount,
                                   floatDataPointBuffer,
                                   effectiveChannelCount,
                                   floatDataPointBuffer);

            effectiveChannelCount =
                int(Natural::minimum(channelCount, effectiveChannelCount));

            /* copy the channel data from FluidSynth into <dataPointBuffer> */
            for (i = 0;  i < effectiveChannelCount;  i++) {
                Array_convert(dataPointBuffer[i].asArray(position),
                              floatDataPointBuffer[i], dataPointCount);
            }

            /* clear remaining channels (if any) */
            while (i < int(channelCount)) {
                Array_clear(dataPointBuffer[i++].asArray(position),
                            dataPointCount, AudioDataPoint{0.0});
            }

            isOkay = (operationResult == 0);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean
FluidSynthSynthesizer::setInterpolationMethod (IN Natural methodCode)
{
    Logging_trace1(">>: %1", TOSTRING(methodCode));
    
    Boolean isOkay = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSynthesizerDescriptor& descriptor =
            TOREFERENCE<_FSSynthesizerDescriptor>(_descriptor);
        _FSSynthesizer_SetInterpolationMethodProc
            synthesizerSetInterpolationMethod =
                descriptor.synthesizerSetInterpolationMethod;

        if (synthesizerSetInterpolationMethod == nullptr) {
            _reportBadFunction("setInterpolationMethod");
        } else if (!(methodCode == 0 || methodCode == 1
                     || methodCode >= 4 && methodCode <= 7)) {
            Logging_traceError(_errorMessageForBadInterpolationMethodCode);
        } else {
            isOkay = true;

            for (Natural channel = 0;  channel < 16;  channel++) {
                Integer operationResult =
                    synthesizerSetInterpolationMethod(descriptor.synthesizer,
                                                      int(channel),
                                                      int(methodCode));
                isOkay = isOkay && (operationResult == 0);
            }
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}
