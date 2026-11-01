/**
 * @file
 * The <C>FluidSynthSoundFont</C> class implements an object-oriented
 * wrapper around an underlying fluidsynth SoundFont object with
 * dynamic library access.
 *
 * @author Dr. Thomas Tensi
 * @date   2024-03
 */

/*=========*/
/* IMPORTS */
/*=========*/

#include "DynamicLibrary.h"
#include "FluidSynthSoundFont.h"
#include "Logging.h"
#include "OperatingSystem.h"

/*--------------------*/

using BaseModules::OperatingSystem;
using Libraries::DynamicLibrary;
using Libraries::FluidSynth::FluidSynthSoundFont;

/** abbreviation for StringUtil */
using STR = BaseModules::StringUtil;

/*-----------------------------------------*/
/* TYPES AND FUNCTIONS FOR DYNAMIC BINDING */
/*-----------------------------------------*/

/** SoundFont construction function type in library  */
typedef Object (*_FSSoundFont_ConstructionProc)(Object, int);

/** sound font preset enumeration reset function type in library */
typedef void (*_FSSoundFont_EnumerationResetProc)(Object);

/** sound font preset enumeration successor function type in library */
typedef Object (*_FSSoundFont_EnumerationSuccessorProc)(Object);

/** sound font object name query type in library */
typedef const char* (*_FSSoundFont_ObjectNameQueryProc)(Object);

/** sound font preset bank number query type in library */
typedef int (*_FSSoundFont_PresetBankNumQueryProc)(Object);

/** sound font preset program number query type in library */
typedef int (*_FSSoundFont_PresetProgNumQueryProc)(Object);

/** sound font preset query by bank and program type in library */
typedef Object (*_FSSoundFont_PresetQueryProc)(Object, int, int);

/*--------------------*/

namespace Libraries::FluidSynth {

    /** the associated descriptor object type */
    struct _FSSoundFontDescriptor {

        /** the fluidsynth sound font object */
        Object soundFont;

        /** SoundFont creation function in library  */
        _FSSoundFont_ConstructionProc soundFontMake;

        /** SoundFont name query in library */
        _FSSoundFont_ObjectNameQueryProc soundFontName;

        /** SoundFont preset enumeration reset function in library */
        _FSSoundFont_EnumerationSuccessorProc
            soundFontNextPresetInEnumeration;

        /** SoundFont preset bank number query in library */
        _FSSoundFont_PresetBankNumQueryProc soundFontPresetBankNum;

        /** SoundFont preset name query in library */
        _FSSoundFont_ObjectNameQueryProc soundFontPresetName;

        /** SoundFont preset program number query in library */
        _FSSoundFont_PresetProgNumQueryProc soundFontPresetProgNum;

        /** SoundFont preset query in library */
        _FSSoundFont_PresetQueryProc soundFontPresetQuery;

        /** SoundFont preset enumeration reset function in library */
        _FSSoundFont_EnumerationResetProc soundFontResetPresetEnumeration;

        /*--------------------*/
        /* con-/destruction   */
        /*--------------------*/

        /**
         * Connects functions dynamically to functions from
         * <C>library</C> using <C>synthesizer</C> object and puts
         * soundfont at level <C>levelFromTop</C> on SoundFont stack
         *
         * @param[in] library       underlying fluidsynth library
         * @param[in] synthesizer   the fluidsynth synthesizer object
         * @param[in] levelFromTop  the distance to the top of stack
         *                          for this SoundFont
         * @return soundfont object
         */
        _FSSoundFontDescriptor (IN DynamicLibrary* library,
                                IN FluidSynthSynthesizer* synthesizer,
                                IN Natural levelFromTop);

        /*--------------------*/

        /**
         * Finalizes descriptor
         */
        ~_FSSoundFontDescriptor ();

        /*--------------------*/
        /* measurement        */
        /*--------------------*/

        /**
         * Checks and returns whether all SoundFont-related functions
         * are available
         *
         * @return  information about full function availability
         */
        Boolean isFullyInitialized () const;
        
    };

}

/*====================*/

using Libraries::FluidSynth::_FSSoundFontDescriptor;

/*--------*/
/* MACROS */
/*--------*/

/** error message for an undefined descriptor */
static const String _errorMessageForUndefinedDescriptor =
    "SoundFont descriptor object must be defined";

/** error message for an undefined sound font */
static const String _errorMessageForUndefinedSoundFont =
    "SoundFont object must be defined";

/** reports that function with <C>name</C> is not dynamically defined */
#define _reportBadFunction(name) \
    Logging_traceError("SoundFont '" name "' function undefined")

/** simple macro for dynamic binding of a function */
#define GPA(signature, name) \
    reinterpret_cast<signature>(library->getFunctionByName(name))

/*================================*/
/* _FSSoundFontDescriptor */
/*================================*/

_FSSoundFontDescriptor::
_FSSoundFontDescriptor (IN DynamicLibrary* library,
                        IN FluidSynthSynthesizer* synthesizer,
                        IN Natural levelFromTop)
{
    Logging_trace(">>");

    if (library == nullptr) {
        Logging_traceError("fluidsynth library not loaded");
    } else {
        soundFontMake =
            GPA(_FSSoundFont_ConstructionProc, "fluid_synth_get_sfont");
        soundFontName =
            GPA(_FSSoundFont_ObjectNameQueryProc, "fluid_sfont_get_name");
        soundFontNextPresetInEnumeration = 
            GPA(_FSSoundFont_EnumerationSuccessorProc,
                "fluid_sfont_iteration_next");
        soundFontPresetBankNum =
            GPA(_FSSoundFont_PresetBankNumQueryProc,
                "fluid_preset_get_banknum");
        soundFontPresetName =
            GPA(_FSSoundFont_ObjectNameQueryProc, "fluid_preset_get_name");
        soundFontPresetProgNum =
            GPA(_FSSoundFont_PresetProgNumQueryProc, "fluid_preset_get_num");
        soundFontPresetQuery =
            GPA(_FSSoundFont_PresetQueryProc, "fluid_sfont_get_preset");
        soundFontResetPresetEnumeration =
            GPA(_FSSoundFont_EnumerationResetProc,
                "fluid_sfont_iteration_start");

        if (soundFontMake == nullptr) {
            _reportBadFunction("make");
        } else {
            soundFont = soundFontMake(synthesizer->underlyingObject(),
                                      int(levelFromTop));
        }
    }
    
    Logging_trace("<<");
}

/*--------------------*/

_FSSoundFontDescriptor::~_FSSoundFontDescriptor ()
{
    Logging_trace(">>");
    soundFont = nullptr;
    Logging_trace("<<");
}

/*--------------------*/

Boolean
_FSSoundFontDescriptor::isFullyInitialized () const
{
    Logging_trace(">>");

    Boolean result = true;

    if (soundFont == nullptr) {
        Logging_traceError(_errorMessageForUndefinedSoundFont);
        result = false;
    }
    
    if (soundFontResetPresetEnumeration == nullptr) {
        _reportBadFunction("resetPresetEnumeration");
        result = false;
    }

    if (soundFontNextPresetInEnumeration ==  nullptr) {
        _reportBadFunction("nextPresetInEnumeration");
        result = false;
    }

    if (soundFontPresetBankNum == nullptr) {
        _reportBadFunction("presetBankNum");
        result = false;
    }

    if (soundFontPresetName == nullptr) {
        _reportBadFunction("presetName");
        result = false;
    }

    if (soundFontPresetProgNum == nullptr) {
        _reportBadFunction("presetProgNum");
        result = false;
    }

    Logging_trace1("<<: %1", TOSTRING(result));
    return result;
}

/*====================*/
/* PRIVATE FEATURES   */
/*====================*/

/*====================*/
/* PUBLIC FEATURES    */
/*====================*/

/*--------------------*/
/* con-/destruction   */
/*--------------------*/

FluidSynthSoundFont::FluidSynthSoundFont
                         (IN FluidSynthSynthesizer* synthesizer,
                          IN Natural levelFromTop)
    : _descriptor{nullptr}
{
    Logging_trace(">>");

    const FluidSynth* library =
        (synthesizer == nullptr ? nullptr : synthesizer->library());

    if (synthesizer == nullptr || library == nullptr || !library->isLoaded()) {
        Logging_traceError("library or synthesizer object is undefined");
    } else {
        DynamicLibrary* fsLibrary =
            reinterpret_cast<DynamicLibrary*>(library->underlyingObject());
        _descriptor =
            new _FSSoundFontDescriptor(fsLibrary,
                                               synthesizer, levelFromTop);
    }
    
    Logging_trace("<<");
}

/*--------------------*/

FluidSynthSoundFont::~FluidSynthSoundFont ()
{
    Logging_trace(">>");
    delete reinterpret_cast<_FSSoundFontDescriptor*>(_descriptor);
    Logging_trace("<<");
}

/*--------------------*/
/* queries            */
/*--------------------*/

Boolean FluidSynthSoundFont::hasPreset (IN Natural bankNumber,
                                        IN Natural programNumber) const
{
    Logging_trace2(">>: bank = %1, program = %2",
                   TOSTRING(bankNumber), TOSTRING(programNumber));

    Boolean result = false;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSoundFontDescriptor& descriptor =
            TOREFERENCE<_FSSoundFontDescriptor>(_descriptor);
        Object soundFont = descriptor.soundFont;
        
        if (descriptor.isFullyInitialized()) {
            const Object preset =
                descriptor.soundFontPresetQuery(soundFont,
                                                int(bankNumber),
                                                int(programNumber));
            result = (preset != nullptr);
        }
    }

    Logging_trace1("<<: %1", TOSTRING(result));
    return result;
}
        
/*--------------------*/

String FluidSynthSoundFont::name () const
{
    Logging_trace(">>");

    String result;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSoundFontDescriptor& descriptor =
            TOREFERENCE<_FSSoundFontDescriptor>(_descriptor);
        Object soundFont = descriptor.soundFont;
        
        if (descriptor.isFullyInitialized()) {
            result = String{descriptor.soundFontName(soundFont)};
            result = OperatingSystem::basename(result, false);
        }
    }

    Logging_trace1("<<: %1", result);
    return result;
}

/*--------------------*/

StringList FluidSynthSoundFont::presetList () const
{
    Logging_trace(">>");

    StringList result;
    
    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSoundFontDescriptor& descriptor =
            TOREFERENCE<_FSSoundFontDescriptor>(_descriptor);
        Object soundFont = descriptor.soundFont;
        
        if (descriptor.isFullyInitialized()) {
            descriptor.soundFontResetPresetEnumeration(soundFont);

            while (true) {
                Object preset =
                    descriptor.soundFontNextPresetInEnumeration(soundFont);

                if (preset == nullptr) {
                    break;
                }

                Natural bankNumber    =
                    descriptor.soundFontPresetBankNum(preset);
                Natural programNumber =
                    descriptor.soundFontPresetProgNum(preset);
                String name           =
                    descriptor.soundFontPresetName(preset);
                String line = STR::expand("%1\t%2\t%3",
                                          TOSTRING(bankNumber),
                                          TOSTRING(programNumber),
                                          name);
                result.append(line);
            }
        }
    }

    Logging_trace1("<<: %1", result.toString());
    return result;
}

/*--------------------*/

String FluidSynthSoundFont::presetName (IN Natural bankNumber,
                                        IN Natural programNumber) const
{
    Logging_trace2(">>: bank = %1, program = %2",
                   TOSTRING(bankNumber), TOSTRING(programNumber));

    const String undefined = "???";
    String result = undefined;

    if (_descriptor == nullptr) {
        Logging_traceError(_errorMessageForUndefinedDescriptor);
    } else {
        _FSSoundFontDescriptor& descriptor =
            TOREFERENCE<_FSSoundFontDescriptor>(_descriptor);
        Object soundFont = descriptor.soundFont;
        
        if (descriptor.isFullyInitialized()) {
            Object preset =
                descriptor.soundFontPresetQuery(soundFont,
                                                int(bankNumber),
                                                int(programNumber));

            if (preset != nullptr) {
                result = descriptor.soundFontPresetName(preset);
            }
        }
    }
    
    Logging_trace1("<<: %1", result);
    return result;
}
