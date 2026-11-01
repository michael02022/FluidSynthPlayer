/**
 * @file
 * The <C>FluidSynthSettings</C> class implements an object-oriented
 * wrapper around an underlying fluidsynth settings object with
 * dynamic library access.
 *
 * @author Dr. Thomas Tensi
 * @date   2022-04
 */

/*=========*/
/* IMPORTS */
/*=========*/

#include "Logging.h"

#include "DynamicLibrary.h"
#include "FluidSynthSettings.h"

/*--------------------*/

using Libraries::DynamicLibrary;
using Libraries::FluidSynth::FluidSynthSettings;

/** abbreviation for StringUtil */
using STR = BaseModules::StringUtil;

/*-----------------------------------------*/
/* TYPES AND FUNCTIONS FOR DYNAMIC BINDING */
/*-----------------------------------------*/

/** settings creation function type in library */
typedef Object (*_FSSettings_CreationProc)(void);
 
/** settings destruction function type in library */
typedef void (*_FSSettings_DestructionProc)(Object);
 
/** setting function type for int value in library */
typedef int (*_FSSettings_SetIntProc)(Object, const char*, int);
 
/** setting function type for float value in library */
typedef int (*_FSSettings_SetNumProc)(Object, const char*, double);
 
/** setting function type for string value in library */
typedef int (*_FSSettings_SetStrProc)(Object, const char*, const char*);

/*--------------------*/

namespace Libraries::FluidSynth {

    /** the associated descriptor object type */
    struct _FSSettingsDescriptor {

        /** the underlying fluidsynth settings object */
        Object settings;

        /** settings creation function in library */
        _FSSettings_CreationProc settingsMake;

        /** settings destruction function in library */
        _FSSettings_DestructionProc settingsDestroy;

        /** setting function for int value in library */
        _FSSettings_SetIntProc settingsSetInt;

        /** setting function for float value in library */
        _FSSettings_SetNumProc settingsSetNum;

        /** setting function for string value in library */
        _FSSettings_SetStrProc settingsSetStr;

        /*--------------------*/
        /* con-/destruction   */
        /*--------------------*/

        /**
         * Connects functions dynamically to functions from 
         * <C>library</C>
         *
         * @param[in] library  underlying fluidsynth library
         */
        _FSSettingsDescriptor (IN DynamicLibrary* library);

        /*--------------------*/

        /**
         * Finalizes descriptor
         */
        ~_FSSettingsDescriptor ();

    };

}

/*====================*/

using Libraries::FluidSynth::_FSSettingsDescriptor;

/*--------*/
/* MACROS */
/*--------*/

/** reports that function with <C>name</C> is not dynamically defined */
#define reportBadFunction(name) \
    Logging_traceError("settings '" name "' function undefined")

/** simple macro for dynamic binding of a function */
#define GPA(signature, name) \
    reinterpret_cast<signature>(library->getFunctionByName(name))

/*=======================*/
/* _FSSettingsDescriptor */
/*=======================*/

_FSSettingsDescriptor::
_FSSettingsDescriptor (IN DynamicLibrary* library)
{
    Logging_trace(">>");

    if (library == nullptr) {
        Logging_traceError("fluidsynth library not loaded");
    } else {
        settingsMake =
            GPA(_FSSettings_CreationProc, "new_fluid_settings");
        settingsDestroy =
            GPA(_FSSettings_DestructionProc, "delete_fluid_settings");
        settingsSetInt =
            GPA(_FSSettings_SetIntProc, "fluid_settings_setint");
        settingsSetNum =
            GPA(_FSSettings_SetNumProc, "fluid_settings_setnum");
        settingsSetStr =
            GPA(_FSSettings_SetStrProc, "fluid_settings_setstr");

        if (settingsMake == nullptr) {
            reportBadFunction("make");
        } else {
            settings = settingsMake();
        }
    }
    
    Logging_trace("<<");
}

/*--------------------*/

_FSSettingsDescriptor::~_FSSettingsDescriptor ()
{
    Logging_trace(">>");

    if (settings != nullptr) {
        if (settingsDestroy == nullptr) {
            reportBadFunction("destroy");
        } else {
            settingsDestroy(settings);
            settings = nullptr;
        }
    }

    Logging_trace("<<");
}

/*====================*/
/* PRIVATE FEATURES    */
/*====================*/

/**
 * the list of settings that require a restart of the synthesizer
 */
static const StringList _restartingKeysList =
    StringList::fromList({"synth.interpolation-method",
                          "synth.limiter.active",
                          "synth.limiter.attack",
                          "synth.limiter.hold",
                          "synth.limiter.link-channels",
                          "synth.limiter.output-limit",
                          "synth.limiter.release",
                          "synth.limiter.smoothing-stages",
                          "synth.reverb.engine",
                          "synth.sample-rate",              
                          "synth.verbose"});

/*--------------------*/

/**
 * a mapping from a setting name from FluidSynth to the associated
 * type (encoded as a single letter)
*/
static const Dictionary _settingsKeyToKindMap =
    Dictionary::makeFromString(
        "synth.chorus.active               -> B,"
        "synth.chorus.depth                -> F,"
        "synth.chorus.level                -> F,"
        "synth.chorus.nr                   -> I,"
        "synth.chorus.speed                -> F,"
        "synth.default-soundfont           -> S,"
        "synth.dynamic-sample-loading      -> B,"
        "synth.gain                        -> F,"
        "synth.interpolation-method        -> I,"
        "synth.limiter.active              -> B,"
        "synth.limiter.attack              -> F,"
        "synth.limiter.hold                -> F,"
        "synth.limiter.link-channels       -> F,"
        "synth.limiter.output-limit        -> F,"
        "synth.limiter.release             -> F,"
        "synth.limiter.smoothing-stages    -> I,"
        "synth.midi-bank-select            -> S,"
        "synth.min-note-length             -> I,"
        "synth.overflow.age                -> F,"
        "synth.overflow.important          -> F,"
        "synth.overflow.important-channels -> S,"
        "synth.overflow.percussion         -> F,"
        "synth.overflow.released           -> F,"
        "synth.overflow.sustained          -> F,"
        "synth.overflow.volume             -> F,"
        "synth.polyphony                   -> I,"
        "synth.portamento-time             -> S,"
        "synth.reverb.active               -> B,"
        "synth.reverb.damp                 -> F,"
        "synth.reverb.engine               -> S,"
        "synth.reverb.level                -> F,"
        "synth.reverb.room-size            -> F,"
        "synth.reverb.width                -> F,"
        "synth.sample-rate                 -> F,"
        "synth.threadsafe-api              -> B,"
        "synth.verbose                     -> B"
    );

/*--------------------*/

/**
 * a mapping from a setting name from FluidSynth to the associated
 * default value as string
*/
static const Dictionary _settingsKeyToDefaultValueMap =
    Dictionary::makeFromString(
        "synth.chorus.active               -> true,"
        "synth.chorus.depth                -> 4.25,"
        "synth.chorus.level                -> 0.6,"
        "synth.chorus.nr                   -> 3,"
        "synth.chorus.speed                -> 0.2,"
        "synth.dynamic-sample-loading      -> false,"
        "synth.gain                        -> 0.2,"
        "synth.interpolation-method        -> 4;7,"
        "synth.limiter.active              -> false,"
        "synth.limiter.attack              -> 5,"
        "synth.limiter.hold                -> 15,"
        "synth.limiter.link-channels       -> 0.5,"
        "synth.limiter.output-limit        -> 0.99999,"
        "synth.limiter.release             -> 40,"
        "synth.limiter.smoothing-stages    -> 1,"
        "synth.midi-bank-select            -> gs,"
        "synth.min-note-length             -> 10,"
        "synth.overflow.age                -> 1000,"
        "synth.overflow.important          -> 5000,"
        "synth.overflow.percussion         -> 4000,"
        "synth.overflow.released           -> -2000,"
        "synth.overflow.sustained          -> -1000,"
        "synth.overflow.volume             -> 500,"
        "synth.polyphony                   -> 256,"
        "synth.portamento-time             -> auto,"
        "synth.reverb.active               -> true,"
        "synth.reverb.damp                 -> 0.2,"
        "synth.reverb.engine               -> dat,"
        "synth.reverb.level                -> 0.7,"
        "synth.reverb.room-size            -> 0.5,"
        "synth.reverb.width                -> 0.8,"
        /* sample rate is always set by the plugin */
        /* "synth.sample-rate              -> 44100," */
        "synth.threadsafe-api              -> true,"
        "synth.verbose                     -> false"
    );

/*====================*/
/* PUBLIC FEATURES    */
/*====================*/

/*--------------------*/
/* con-/destruction   */
/*--------------------*/

FluidSynthSettings::FluidSynthSettings (IN FluidSynth* fsLibrary)
    : _descriptor{nullptr}
{
    Logging_trace(">>");

    Logging_trace1("--: settingsToKindMap = %1",
                   _settingsKeyToKindMap.toString());

    if (fsLibrary == nullptr || !fsLibrary->isLoaded()) {
        Logging_traceError("library is undefined");
    } else {
        const DynamicLibrary* dynamicLibrary =
            reinterpret_cast<DynamicLibrary*>(fsLibrary->underlyingObject());
        _descriptor = new _FSSettingsDescriptor(dynamicLibrary);
    }

    Logging_trace("<<");
}

/*--------------------*/

FluidSynthSettings::~FluidSynthSettings ()
{
    Logging_trace(">>");
    delete reinterpret_cast<_FSSettingsDescriptor*>(_descriptor);
    Logging_trace("<<");
}

/*--------------------*/
/* property access    */
/*--------------------*/

const Dictionary& FluidSynthSettings::keyToDefaultValueMap ()
{
    return _settingsKeyToDefaultValueMap;
}

/*--------------------*/

const StringList& FluidSynthSettings::restartingKeysList ()
{
    return _restartingKeysList;
}

/*--------------------*/

Object FluidSynthSettings::underlyingObject () const
{
    _FSSettingsDescriptor& descriptor =
        TOREFERENCE<_FSSettingsDescriptor>(_descriptor);
    return descriptor.settings;
}

/*--------------------*/
/* change             */
/*--------------------*/

Boolean FluidSynthSettings::set (IN String& key, IN String& value)
{
    Logging_trace2(">>: key = %1, value = %2", key, value);

    Boolean isOkay = false;

    if (!_settingsKeyToKindMap.contains(key)) {
        Logging_traceError("key not found --> skipped");
    } else {
        const _FSSettingsDescriptor& descriptor =
            TOREFERENCE<_FSSettingsDescriptor>(_descriptor);
        const String kind = _settingsKeyToKindMap.at(key);
        const char* keyAsCharArray = key.c_str();
        Object settingsObject = descriptor.settings;

        if (settingsObject == nullptr) {
            Logging_traceError("settings object must be defined");
        } else {
            Integer operationResult;

            if (key == "synth.interpolation-method") {
                /* special handling of interpolation method (which
                   is not an official parameter */
                Integer intValue = STR::toInteger(value);
                Logging_trace2("--: kind = %1, value = %2",
                               kind, TOSTRING(intValue));
                isOkay = (intValue == 0 || intValue == 1
                          || intValue >= 4 && intValue <= 7);
            } else if (kind == "B" || kind == "I") {
                _FSSettings_SetIntProc settingsSetInt =
                    descriptor.settingsSetInt;
                Integer intValue;

                if (kind == "B") {
                    intValue = (value == "true" || value == "1"
                                ? 1 : 0);
                } else {
                    intValue = STR::toInteger(value);
                }

                Logging_trace2("--: kind = %1, value = %2",
                               kind, TOSTRING(intValue));

                if (settingsSetInt == nullptr) {
                    reportBadFunction("setInt");
                } else {
                    operationResult = settingsSetInt(settingsObject,
                                                     keyAsCharArray,
                                                     int(intValue));
                    isOkay = (operationResult == 0);
                }
            } if (kind == "F") {
                _FSSettings_SetNumProc settingsSetNum =
                    descriptor.settingsSetNum;
                Real numValue = STR::toReal(value);
                Logging_trace2("--: kind = %1, value = %2",
                               kind, TOSTRING(numValue));

                if (settingsSetNum == nullptr) {
                    reportBadFunction("setNum");
                } else {
                    operationResult = settingsSetNum(settingsObject,
                                                     keyAsCharArray,
                                                     float(numValue));
                    isOkay = (operationResult == 0);
                }
            } if (kind == "S") {
                _FSSettings_SetStrProc settingsSetStr =
                    descriptor.settingsSetStr;
                Logging_trace2("--: kind = %1, value = %2",
                               kind, value);

                if (settingsSetStr == nullptr) {
                    reportBadFunction("setStr");
                } else {
                    operationResult = settingsSetStr(settingsObject,
                                                     keyAsCharArray,
                                                     value.c_str());
                    isOkay = (operationResult == 0);
                }
            }
        } 
    }
    
    Logging_trace1("<<: %1", TOSTRING(isOkay));
    return isOkay;
}
