/**
 * @file
 * The <C>MidiEventConverter</C> module implements the handling of
 * MIDI events by feeding them to an underlying FluidSynth library.
 *
 * @author Dr. Thomas Tensi
 * @date   2022-04
 */

/*=========*/
/* IMPORTS */
/*=========*/

#include "Logging.h"
#include "FluidSynthSoundFont.h"
#include "MidiEventConverter.h"
#include "MidiPresetIdentification.h"

/*--------------------*/

using Audio::AudioDataPoint;
using Audio::AudioDataPointList;
using Libraries::FluidSynth::FluidSynthSoundFont;
using MIDI::MidiEventConverter;
using MIDI::MidiEventKind;
using MIDI::MidiPresetIdentification;

/** abbreviation for StringUtil */
using STR = BaseModules::StringUtil;

/*--------------------*/
/* prototypes         */
/*--------------------*/

static String
_findSuitablePreset (IN FluidSynthSoundFont& soundFont);

static Boolean _setPreset (INOUT FluidSynthSynthesizer& synthesizer,
                           IN Natural bankNumber,
                           IN Natural programNumber);

/*====================*/
/* PRIVATE FEATURES    */
/*====================*/

/** the number of the drums midi channel */
static const Natural _drumsMidiChannel = 10;

/** the separator for the interpolation method value for realtime and
 * file rendering */
static const String _interpolationMethodSeparator = ";";

/*====================*/

namespace MIDI {

    /*--------------------*/

    /**
     * the internal data of a fluidsynth MIDI processor: a fluidsynth
     * library, settings and synthesizer object and the dictionary of
     * settings and some preallocated internal audio data point buffer
     */
    struct _MidiEventConverterDescriptor {

        /** a fluidsynth synthesizer object */
        FluidSynthSynthesizer* synthesizer;

        /** the dictionary of settings with fluidsynth keys */
        Dictionary settingsDictionary;

        /** the underlying buffer size of the fluidsynth
         * synthesizer */
        Natural synthesizerBufferSize;

        /** a flag to tell whether sound font is loaded */
        Boolean soundFontIsOkay;

        /** a flag to tell whether the synthesizer has to work in
         * realtime */
        Boolean processingIsInRealtime;

        /** the name of the currently active preset (either set in the
         * settings or via a program change) */
        String presetName;

        /*--------------------*/
        /* con-/destruction   */
        /*--------------------*/

        /** Sets up the processor descriptor.
         *
         * @param[in] audioIsSuppressedForBadSettings  tells that no audio
         *                                         is produced when settings
         *                                         are inconsistent
         */
        _MidiEventConverterDescriptor
             (IN Boolean audioIsSuppressedForBadSettings);

        /*--------------------*/

        /**
         * Constructs new descriptor from <C>otherDescriptor</C>
         * (NOT AVAILABLE!)
         *
         * @param[in] otherDescriptor  descriptor to be copied
         */
        _MidiEventConverterDescriptor
            (IN _MidiEventConverterDescriptor& otherDescriptor) = delete;

        /*--------------------*/

        /** Destroys the processor descriptor */
        ~_MidiEventConverterDescriptor ();

        /*--------------------*/
        /* assignment         */
        /*--------------------*/

        /**
         * Assigns current descriptor from <C>otherDescriptor</C>
         * (NOT AVAILABLE!)
         *
         * @param[in] otherDescriptor  descriptor to be assigned
         */
        _MidiEventConverterDescriptor& operator=
            (IN _MidiEventConverterDescriptor& otherDescriptor) = delete;

        /*--------------------*/
        /* property access    */
        /*--------------------*/

        /**
         * Returns the default mapping from keys to values for the
         * fluidsynth settings.
         *
         * @return  map of default values for all keys relevant for
         *          initialization
         */
        static const Dictionary& settingsKeyToDefaultValueMap ();
        
        /*--------------------*/
        /* property change    */
        /*--------------------*/

        /**
         * Handles bank and program change depending on for given
         * string <C>value</C> on synthesizer. <C>value</C> contains
         * bank and program number separated by a colon.  The bank
         * number comes first followed by a colon and finally the
         * program number.  All MIDI channels are affected, if the
         * bank is missing, bank number 0 is assumed; an empty bank is
         * ignored and an empty program is erroneous.
         *
         * @param[in] value           string value for bank and/or
         *                            program
         * @param[in] changeIsForced  tells whether program change
         *                            must be done unconditionally
         * @return  information whether program change handling has
         *          been successful
         */
        Boolean handlePresetChange (IN String& value,
                                    IN Boolean changeIsForced = false);

        /*--------------------*/

        /**
         * Handles change of SoundFont for given string <C>value</C>
         * on synthesizer where value contains SoundFont path.
         * 
         * @param[in] value  string value for sound font path
         * @return  information whether sound font change handling has
         *          been successful
         */
        Boolean handleSoundFontChange (IN String& value);

        /*--------------------*/

        /**
         * Handles MIDI event <C>event</C> for synthesizer
         *
         * @param[in] event  midi event to be processed
         * @return  information whether midi event handling has been
         *          successful
         */
        Boolean processMidiEvent (IN MidiEvent& event);

        /*--------------------*/

        /**
         * Processes MIDI events in MIDI event list <C>eventList</C>
         * at or after <C>eventIndex</C> scheduled at
         * <C>referenceTimeInDataPoints</C> using Fluidsynth
         * synthesizer
         *
         * @param[in]    referenceTimeInDataPoints  reference time for
         *                                          midi events
         * @param[in]    eventList                  list of midi events
         * @param[inout] eventIndex                 pointer to current
         *                                          position in MIDI
         *                                          event list
         */
        void processMidiEventList (IN Natural referenceTimeInDataPoints,
                                  IN MidiEventList& eventList,
                                  INOUT Natural& eventIndex);

        /*--------------------*/

        /**
         * Resets all settings to default.
         */
        void resetSettings ();

        /*--------------------*/

        /**
         * Sets setting in descriptor given by <C>key</C> to new
         * value given by <C>value</C>
         *
         * @param[in] key    settings name
         * @param[in] value  value for key as string 
         * @return  information whether set operation has been successful
         */
        Boolean setSetting (IN String& key,
                            IN String& value);

        /*--------------------*/

        private:

            /** flag for suppressing audio output for bad
                settings */
            Boolean _audioIsSuppressedForBadSettings;
        
            /** tells that current program number is valid */
            Boolean _hasValidPreset;
        
            /** a fluidsynth library object */
            FluidSynth* _library;

            /** a fluidsynth settings object */
            FluidSynthSettings* _settings;

    };

}

/*====================*/

using MIDI::_MidiEventConverterDescriptor;

/*===================*/
/* support functions */
/*===================*/

/**
 * Calculates the interpolation method code from
 * <C>interpolationMethod</C>.
 *
 * @param[in] interpolationMethod  string for interpolation method
 *                                 (may be empty)
 * @param[in] isRealtime           tells whether the realtime or
 *                                 file render setting shall be used
 * @return  numeric method code
 */
static Natural
_interpolationMethodCodeFromSettings (IN String& interpolationMethod,
                                      IN Boolean isInRealtime)
{
    /* default interpolation method is 4 for realtime, 7 for
     * file rendering */
    Natural result = (isInRealtime ? 4 : 7);

    if (interpolationMethod > "") {
        /* split string at separator */
        const StringList valueList =
            StringList::makeBySplit(interpolationMethod,
                                    _interpolationMethodSeparator);

        if (valueList.length() > 0) {
            const Natural index =
                (valueList.length() == 1 || isInRealtime ? 0 : 1);
            const String value = valueList.at(index);

            if (STR::isNatural(value)) {
                result = STR::toNatural(value);
            }
        }
    }

    return result;
}

/*===============================*/
/* _MidiEventConverterDescriptor */
/*===============================*/

/*--------------------*/
/* con-/destruction   */
/*--------------------*/

_MidiEventConverterDescriptor
::_MidiEventConverterDescriptor (IN Boolean audioIsSuppressedForBadSettings)
{
    Logging_trace(">>");

    _library  = new FluidSynth();
    _settings = new FluidSynthSettings(_library);
    _audioIsSuppressedForBadSettings = audioIsSuppressedForBadSettings;
    synthesizer = new FluidSynthSynthesizer(_library, _settings);
    synthesizerBufferSize = (!_library->isLoaded() ? 0
                             : synthesizer->internalBufferSize());
    processingIsInRealtime = false;
    _hasValidPreset = false;

    Logging_trace("<<");
}

/*--------------------*/
    
_MidiEventConverterDescriptor::~_MidiEventConverterDescriptor ()
{
    Logging_trace(">>");
    delete synthesizer;
    delete _settings;
    delete _library;
    Logging_trace("<<");
}

/*--------------------*/
/* property queries   */
/*--------------------*/

const Dictionary&
_MidiEventConverterDescriptor::settingsKeyToDefaultValueMap ()
{
    return FluidSynthSettings::keyToDefaultValueMap();
}

/*--------------------*/
/* property change    */
/*--------------------*/

Boolean _MidiEventConverterDescriptor::handlePresetChange
                      (IN String& value,
                       IN Boolean changeIsForced)
{
    Logging_trace2(">>: value = %1, changeIsForced = %2",
                   value, TOSTRING(changeIsForced));

    Boolean isOkay;

    if (_hasValidPreset && !changeIsForced) {
        Logging_trace("--: skip program change when preset is valid");
        isOkay = true;
    } else {
        Natural bankNumber;
        Natural programNumber;
        MidiPresetIdentification presetIdentification{value};
        isOkay = presetIdentification.split(bankNumber, programNumber);

        const String errorMessageTemplate =
            "'%1' is not a valid program number";

        if (!isOkay) {
            Logging_traceError1(errorMessageTemplate, value);
        } else {
            FluidSynthSoundFont soundFont{synthesizer};

            if (!soundFont.hasPreset(bankNumber, programNumber)) {
                /* program does not exist in current SoundFont => skip */
                Logging_traceError1(errorMessageTemplate, value);
                isOkay = false;
            } else {
                isOkay = _setPreset(*synthesizer, bankNumber, programNumber);

                if (isOkay) {
                    presetName =
                        soundFont.presetName(bankNumber, programNumber);
                }
            }
        }

        _hasValidPreset = isOkay;
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean _MidiEventConverterDescriptor::handleSoundFontChange
            (IN String& value)
{
    Logging_trace1(">>: %1", value);

    soundFontIsOkay = synthesizer->loadSoundFont(value);
    Boolean isOkay = soundFontIsOkay;
    _hasValidPreset = false;

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean _MidiEventConverterDescriptor::processMidiEvent (IN MidiEvent& event)
{
    Logging_trace1(">>: %1", event.toString());

    const MidiEventKind eventKind = event.kind();
    Boolean isOkay = false;

    if (eventKind != MidiEventKind::meta
        && eventKind != MidiEventKind::systemExclusive) {
        const Natural midiChannel = event.channel();

        if (eventKind == MidiEventKind::channelPressure) {
            const Natural value = Natural(event.getDataByte(1));
            isOkay = synthesizer->handleChannelPressure(midiChannel, value);
        } else if (eventKind == MidiEventKind::controlChange) {
            const Natural controller = Natural(event.getDataByte(1));
            const Natural value      = Natural(event.getDataByte(2));
            const Boolean isBankChange =
                (controller == 0 || controller == 32);

            if (_hasValidPreset && isBankChange) {
                Logging_trace("--: skipped bank change when preset is valid");
                isOkay = true;
            } else {
                /* transmit control change when there is no valid
                 * preset or this is just another control change*/
                isOkay = synthesizer->handleControlChange(midiChannel,
                                                          controller, value);
            }
        } else if (eventKind == MidiEventKind::noteOff) {
            const Natural key = Natural(event.getDataByte(1));
            isOkay = synthesizer->handleNoteOff(midiChannel, key);
        } else if (eventKind == MidiEventKind::noteOn) {
            const Natural key          = Natural(event.getDataByte(1));
            const Natural midiVelocity = Natural(event.getDataByte(2));
            isOkay = synthesizer->handleNoteOn(midiChannel,
                                               key, midiVelocity);
        } else if (eventKind == MidiEventKind::pitchBend) {
            const Natural value = (Natural(event.getDataByte(1))
                                   + Natural(event.getDataByte(2) * 128));
            isOkay = synthesizer->handlePitchBend(midiChannel, value);
        } else if (eventKind == MidiEventKind::polyphonicKeyPressure) {
            const Natural key   = Natural(event.getDataByte(1));
            const Natural value = Natural(event.getDataByte(2));
            isOkay = synthesizer->handlePolyphonicKeyPressure(midiChannel,
                                                              key, value);
        } else if (eventKind == MidiEventKind::programChange) {
            const Natural programNumber = Natural(event.getDataByte(1));

            if (_hasValidPreset) {
                /* ignore */
                isOkay = true;
            } else {
                isOkay = synthesizer->handleProgramChange(midiChannel,
                                                          programNumber);
            }
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}
        
/*--------------------*/

void _MidiEventConverterDescriptor::processMidiEventList
         (IN Natural referenceTimeInDataPoints,
          IN MidiEventList& eventList,
          INOUT Natural& eventIndex)
{
    Logging_trace2(">>: referenceTimeInDataPoints = %1, eventIndex = %2",
                   TOSTRING(referenceTimeInDataPoints),
                   TOSTRING(eventIndex));

    const Natural eventListLength = eventList.size();

    while (eventIndex < eventListLength) {
        const MidiEvent& event = eventList[eventIndex];
        const Integer eventTimeInDataPoints = event.time();

        if (eventTimeInDataPoints > referenceTimeInDataPoints) {
            /* this event and the following are not yet relevant */
            break;
        } else {
            /* this event has to be processed */
            processMidiEvent(event);
            eventIndex++;
        }
    }

    Logging_trace1("<<: new event index = %1",
                   TOSTRING(eventIndex));
}

/*--------------------*/

void _MidiEventConverterDescriptor::resetSettings ()
{
    Logging_trace(">>");

    settingsDictionary.clear();
    _hasValidPreset = false;
    //TT settings.reset();
    
    Logging_trace("<<");
}

/*--------------------*/

/** the alias name of the MIDI preset */
static const String _Settings_Key_alternativePreset = "program";

/** the (artificial) key for the interpolation method */
static const String _Settings_Key_interpolationMethod =
    "synth.interpolation-method";

/** the name of the key for the MIDI preset with bank and program */
static const String _Settings_Key_preset = "preset";

/** the name of the key for the soundfont */
static const String _Settings_Key_soundFont = "soundfont";

/** the required prefix for the settings (except for soundfont and
 * preset) */
static const String _Settings_requiredKeyPrefix = "synth.";

/** the list of all keys requiring special handling */
static const StringList _Settings_specialKeysList =
    StringList::fromList({_Settings_Key_interpolationMethod});

/*--------------------*/

Boolean
_MidiEventConverterDescriptor::setSetting (IN String& key,
                                           IN String& value)
{
    Logging_trace2(">>: key = %1, value = %2", key, value);

    const StringList& restartingKeysList =
        FluidSynthSettings::restartingKeysList();       
    enum class UpdateLevel
        { error = -1, std = 0, preset = 1, soundFont = 2, restart = 3 };

    /* replace 'program' by 'preset' as key */
    const String& effectiveKey =
        (key == _Settings_Key_alternativePreset
         ? _Settings_Key_preset : key);
    Boolean isOkay = true;
    UpdateLevel updateLevel = UpdateLevel::std;

    if (effectiveKey == _Settings_Key_soundFont) {
        updateLevel = UpdateLevel::soundFont;
    } else if (effectiveKey == _Settings_Key_preset) {
        updateLevel = UpdateLevel::preset;
    } else if (!STR::startsWith(effectiveKey,
                                _Settings_requiredKeyPrefix)) {
        updateLevel = UpdateLevel::error;
        isOkay = false;
    } else {
        if (!_Settings_specialKeysList.contains(effectiveKey)) {
            isOkay = _settings->set(effectiveKey, value);
        }

        if (isOkay && restartingKeysList.contains(effectiveKey)) {
            updateLevel = UpdateLevel::restart;
        }
    }

    if (updateLevel != UpdateLevel::error) {
        settingsDictionary.set(effectiveKey, value);
    }

    Logging_trace1("--: updateLevel = %1",
                   TOSTRING(Natural{size_t(updateLevel)}));

    if (isOkay && updateLevel == UpdateLevel::restart) {
        /* restart synthesizer when synthesizer related setting is
         * activated */
        const FluidSynth* library = synthesizer->library();
        delete synthesizer;
        synthesizer = new FluidSynthSynthesizer(library, _settings);
        synthesizerBufferSize = (!library->isLoaded() ? 0
                                 : synthesizer->internalBufferSize());
    }

    if (isOkay && updateLevel >= UpdateLevel::soundFont) {
        if (settingsDictionary.contains(_Settings_Key_soundFont)) {
            const String soundFontValue =
                settingsDictionary.at(_Settings_Key_soundFont);
            isOkay = handleSoundFontChange(soundFontValue);
        }
    }

    if (isOkay
        && (updateLevel == UpdateLevel::restart
            || updateLevel == UpdateLevel::preset)) {
        if (settingsDictionary.contains(_Settings_Key_preset)) {
            const String presetIdentification =
                settingsDictionary.at(_Settings_Key_preset);
            isOkay = handlePresetChange(presetIdentification, true);
            _hasValidPreset = true;
        }

        if (!isOkay) {
            /* remove invalid entry for preset to signal error */
            settingsDictionary.remove(_Settings_Key_preset);
        }
    }

    if (isOkay && updateLevel > UpdateLevel::std) {
        /* restore specific settings for synthesizer */
        const String interpolationMethod =
            settingsDictionary.atWithDefault(_Settings_Key_interpolationMethod,
                                             "");
        const Natural interpolationMethodCode =
            _interpolationMethodCodeFromSettings(interpolationMethod,
                                                 processingIsInRealtime);
        isOkay =
            synthesizer->setInterpolationMethod(interpolationMethodCode);
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*====================*/

/*--------------------*/
/* internal routines  */
/*--------------------*/

/**
 * Clear <C>dataPointListVector</C>.
 *
 * @param[in] dataPointListVector  audio buffer to be set to 0
 * @param[in] frameCount           number of data points to set
 */
static void
_clearDataPointListVector
    (INOUT AudioDataPointListVector& dataPointListVector,
     IN Natural frameCount)
{
    const Natural channelCount = dataPointListVector.length();

    for (Natural channel = 0;  channel < channelCount;  channel++) {
        AudioDataPointList& dataPointList = dataPointListVector[channel];
        
        for (Natural j = 0;  j < frameCount;  j++) {
            dataPointList[j] = 0.0;
        }
    }
}
    
/*--------------------*/

/**
 * Finds and returns smallest preset number in <C>soundFont</C>.
 *
 * @param[in] soundFont  sound font to be analyzed
 * @return bank and program number of smallest numbered preset
 */
static String _findSuitablePreset (IN FluidSynthSoundFont& soundFont)
{
    Logging_trace(">>");

    Natural bankNumber = 0;
    Natural programNumber = 0;
    Boolean isFound = false;

    while (!isFound && bankNumber < 16384) {
        while (!isFound && programNumber < 128) {
            if (soundFont.hasPreset(bankNumber, programNumber)) {
                isFound = true;
                break;
            }

            programNumber++;
        }

        if (isFound) {
            break;
        } else {
            bankNumber++;
        }
    }

    if (!isFound) {
        bankNumber    = 0;
        programNumber = 0;
    }

    String result =
        STR::expand("%1:%2", TOSTRING(bankNumber), TOSTRING(programNumber));

    Logging_trace1("<<: %1", result);
    return result;
}

/*--------------------*/

/**
 * Sets preset on all channels of <C>synthesizer</C> to
 * <C>bankNumber</C> and <C>programNumber</C>
 *
 * @param[inout] synthesizer    current synthesizer
 * @param[in]    bankNumber     bank number to be set
 * @param[in]    programNumber  program number to be set
 * @return  information whether preset change has been successful
 */
static Boolean _setPreset (INOUT FluidSynthSynthesizer& synthesizer,
                           IN Natural bankNumber,
                           IN Natural programNumber)
{
    Logging_trace2(">>: bankNumber = %1, programNumber = %2",
                   TOSTRING(bankNumber), TOSTRING(programNumber));

    Boolean isOkay = true;

    for (Natural midiChannel = 0;  midiChannel < 16;  midiChannel++) {
        isOkay = synthesizer.handleBankChange(midiChannel, bankNumber);
        isOkay = (isOkay
                  && synthesizer.handleProgramChange(midiChannel,
                                                     programNumber));

        if (!isOkay) {
            break;
        }
    }
    

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*====================*/
/* PUBLIC FEATURES    */
/*====================*/

/*--------------------*/
/* setup              */
/*--------------------*/

MidiEventConverter::MidiEventConverter
                        (IN Boolean audioIsSuppressedForBadSettings)
    : _descriptor{nullptr}
{
    Logging_trace(">>");
    _descriptor =
        new _MidiEventConverterDescriptor(audioIsSuppressedForBadSettings);
    Logging_trace("<<");
}

/*--------------------*/

MidiEventConverter::~MidiEventConverter ()
{
    Logging_trace(">>");

    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    delete &descriptor;

    Logging_trace("<<");
}

/*--------------------*/
/* property queries   */
/*--------------------*/

Boolean MidiEventConverter::getSetting (IN String& key,
                                        OUT String& value) const
{
    Logging_trace1(">>: %1", key);

    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    const Dictionary& settings = descriptor.settingsDictionary;
    const Boolean isOkay = settings.contains(key);

    if (isOkay) {
        value = settings.at(key);
    }

    Logging_trace2("<<: isOkay = %1, value = %2",
                   TOSTRING(isOkay), value);
    return isOkay;
}

/*--------------------*/

Boolean MidiEventConverter::isCorrectlyInitialized () const
{
    Logging_trace(">>");
    const _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    const FluidSynth* library = descriptor.synthesizer->library();
    Boolean result = (library->isLoaded());
    Logging_trace1("<<: %1", TOSTRING(result));
    return result;
}

/*--------------------*/

String MidiEventConverter::fsLibraryVersion () const
{
    Logging_trace(">>");

    const _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    const FluidSynth* library = descriptor.synthesizer->library();
    String result = library->version();

    Logging_trace1("<< %1", result);
    return result;
}

/*--------------------*/

StringList MidiEventConverter::presetList () const
{
    Logging_trace(">>");
    const _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);

    FluidSynthSoundFont soundFont{descriptor.synthesizer};
    StringList result = soundFont.presetList(); 
    
    Logging_trace1("<< %1", result.toString());
    return result;
}

/*--------------------*/

String MidiEventConverter::presetName () const
{
    Logging_trace(">>");

    const _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    const String result = descriptor.presetName;

    Logging_trace1("<< %1", result);
    return result;
}

/*--------------------*/

const Dictionary&
MidiEventConverter::settingsKeyToDefaultValueMap ()
{
    return _MidiEventConverterDescriptor::settingsKeyToDefaultValueMap();
}

/*--------------------*/

String MidiEventConverter::soundFontName () const
{
    Logging_trace(">>");

    const _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    FluidSynthSoundFont soundFont{descriptor.synthesizer};
    const String result = soundFont.name();

    Logging_trace1("<< %1", result);
    return result;
}

/*--------------------*/

Natural MidiEventConverter::synthesizerBufferSize () const
{
    Logging_trace(">>");

    const _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    Natural result = descriptor.synthesizerBufferSize;

    Logging_trace1("<<: %1", TOSTRING(result));
    return result;
}

/*--------------------*/
/* property change    */
/*--------------------*/

void MidiEventConverter::resetSettings ()
{
    Logging_trace(">>");

    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    descriptor.resetSettings();
    
    Logging_trace("<<");
}

/*--------------------*/

Boolean MidiEventConverter::setSetting (IN String& key,
                                        IN String& value)
{
    Logging_trace2(">>: key = %1, value = %2", key, value);
    
    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    Boolean isOkay = descriptor.setSetting(key, value);
   
    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/

Boolean MidiEventConverter::setSettings (IN Dictionary& dictionary)
{
    Logging_trace1(">>: %1", dictionary.toString());

    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    Boolean isOkay = true;

    for (const auto& [key, value]: dictionary) {
        isOkay = descriptor.setSetting(key, value);

        if (!isOkay) {
            break;
        }
    }

    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}

/*--------------------*/
/* event handling     */
/*--------------------*/

void
MidiEventConverter::prepareToPlay
                        (IN Real dataPointRate,
                         IN Natural maximumExpectedDataPointsPerBlock,
                         IN Boolean isRealtime)
{
    Logging_trace3(">>: dataPointRate = %1, dataPointsPerBlock = %2,"
                   " isRealtime = %3",
                   TOSTRING(dataPointRate),
                   TOSTRING(maximumExpectedDataPointsPerBlock),
                   TOSTRING(isRealtime));

    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    descriptor.processingIsInRealtime = isRealtime;
    setSetting("synth.sample-rate", TOSTRING(dataPointRate));

    Logging_trace("<<");
}

/*--------------------*/

void MidiEventConverter::releaseResources ()
{
    Logging_trace(">>");
    Logging_trace("<<");
}

/*--------------------*/

void
MidiEventConverter::processBlock
                        (IN MidiEventList& midiEventList,
                         INOUT AudioDataPointListVector& audioBuffer,
                         IN Natural frameCount)
{
    Logging_trace2(">>: midiEventList = %1, frameCount = %2",
                   midiEventList.toString(),
                   TOSTRING(frameCount));

    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);

    FluidSynthSynthesizer* synthesizer = descriptor.synthesizer;

    /* calculate the times (in data points) */
    Natural currentTimeInDataPoints = 0;
    const Natural endTimeInDataPoints = frameCount;
    Natural eventIndex = 0;

    while (currentTimeInDataPoints < endTimeInDataPoints) {
        /* set count of data points to be processed to remaining
         * buffer length */
        Natural intervalDurationInDataPoints =
            endTimeInDataPoints - currentTimeInDataPoints;

        descriptor.processMidiEventList(currentTimeInDataPoints,
                                        midiEventList,
                                        eventIndex);

        if (eventIndex < midiEventList.size()) {
            /* there are still events open */
            const MidiEvent& event = midiEventList[eventIndex];
            const Natural durationToNextEventInDataPoints =
                Integer::toNatural(event.time() - currentTimeInDataPoints);
            intervalDurationInDataPoints =
                Natural::minimum(intervalDurationInDataPoints,
                                 durationToNextEventInDataPoints);
        }

        synthesizer->process(audioBuffer,
                             currentTimeInDataPoints,
                             intervalDurationInDataPoints);
        currentTimeInDataPoints += intervalDurationInDataPoints;
    }

    Logging_trace("<<");
}

/*--------------------*/

Boolean MidiEventConverter::processMidiEvent (IN MidiEvent& event)
{
    Logging_trace1(">>: %1", event.toString());

    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    Boolean result = descriptor.processMidiEvent(event);
    
    Logging_trace1("<<: %1", TOSTRING(result));
    return result;
}

/*--------------------*/
/* persistence        */
/*--------------------*/

String MidiEventConverter::getStateInformation ()
{
    Logging_trace(">>");

    /* the state of the Midi instrument is just the serialized
       settings dictionary */
    const _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    const String result = descriptor.settingsDictionary.toString();

    Logging_trace1("<<: %1", result);
    return result;
}

/*--------------------*/

void MidiEventConverter::setStateInformation (IN String& st)
{
    Logging_trace(">>");

    /* restores state of audio processor from <C>st</C> */
    _MidiEventConverterDescriptor& descriptor =
        TOREFERENCE<_MidiEventConverterDescriptor>(_descriptor);
    Dictionary d = Dictionary::makeFromString(st);
    descriptor.resetSettings();
    setSettings(d);

    Logging_trace("<<");
}
