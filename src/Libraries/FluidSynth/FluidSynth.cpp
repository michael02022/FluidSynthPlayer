/**
 * @file
 * The <C>FluidSynth</C> class implements the access to the fluidsynth
 * library via a dynamic loading of it.
 *
 * @author Dr. Thomas Tensi
 * @date   2022-04
 */

/*=========*/
/* IMPORTS */
/*=========*/

#include "Logging.h"
#include "Dictionary.h"

#include "FluidSynth.h"
#include "DynamicLibrary.h"

/*--------------------*/

using Libraries::DynamicLibrary;
using Libraries::FluidSynth::FluidSynth;

/*-----------*/
/* constants */
/*-----------*/

/** the library name for the fluidsynth dynamic library */
#if defined(_WINDOWS)
    #define _libraryName "libfluidsynth-3.dll"
#elif defined(APPLE)
    #define _libraryName "@loader_path/libfluidsynth.3.dylib"
#else
    #define _libraryName "libfluidsynth.so.3"
#endif

/*-----------------------------------------*/
/* TYPES AND FUNCTIONS FOR DYNAMIC BINDING */
/*-----------------------------------------*/

/** version function type in library */
typedef char* (*FSynth_VersionProc)(void);

/*--------------------*/

namespace Libraries::FluidSynth {

    /** the associated descriptor object type */
    struct _FluidSynthDescriptor {

        /** the fluidsynth library */
        DynamicLibrary* library;

        /** the fluid synth version proc */
        FSynth_VersionProc fluidSynthVersion;

        /*--------------------*/
        /* con-/destruction   */
        /*--------------------*/

        /**
         * Connects functions dynamically to functions from 
         * <C>fsLibrary</C>
         *
         * @param[in] fsLibrary  underlying fluidsynth library
         */
        _FluidSynthDescriptor (IN DynamicLibrary* fsLibrary);

        /*--------------------*/

        /**
         * Finalizes descriptor
         */
        ~_FluidSynthDescriptor ();

    };
    
}

/*====================*/

using Libraries::FluidSynth::_FluidSynthDescriptor;

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
/* _FluidSynthDescriptor */
/*=======================*/

_FluidSynthDescriptor::_FluidSynthDescriptor (IN DynamicLibrary* fsLibrary)
{
    Logging_trace(">>");

    library = const_cast<DynamicLibrary*>(fsLibrary);

    if (library == nullptr) {
        Logging_traceError("fluidsynth library not loaded");
    } else {
        fluidSynthVersion =
            GPA(FSynth_VersionProc, "fluid_version_str");
    }
    
    Logging_trace("<<");
}

/*--------------------*/

_FluidSynthDescriptor::~_FluidSynthDescriptor ()
{
    Logging_trace(">>");

    if (library != nullptr) {
        delete library;
        library = nullptr;
    }

    Logging_trace("<<");
}

/*====================*/
/* PUBLIC FEATURES    */
/*====================*/

FluidSynth::FluidSynth ()
    : _descriptor{nullptr}
{
    Logging_trace(">>");

    DynamicLibrary* library = new DynamicLibrary(_libraryName);
    _descriptor = new _FluidSynthDescriptor(library);
    Boolean libraryIsLoaded = library->isLoaded();
    const String message = (libraryIsLoaded
                            ? "library loaded"
                            : "could not load library");

    Logging_trace1("<<: %1", message);
}

/*--------------------*/

FluidSynth::~FluidSynth ()
{
    Logging_trace(">>");
    delete reinterpret_cast<_FluidSynthDescriptor*>(_descriptor);
    Logging_trace("<<");
}

/*--------------------*/

Boolean FluidSynth::isLoaded () const
{
    Logging_trace(">>");
    _FluidSynthDescriptor& descriptor =
        TOREFERENCE<_FluidSynthDescriptor>(_descriptor);
    const DynamicLibrary* library = descriptor.library;
    Boolean result = library->isLoaded();
    Logging_trace1("<<: %1", TOSTRING(result));
    return result;
}

/*--------------------*/

String FluidSynth::version () const
{
    Logging_trace(">>");

    String result = "???";

    if (_descriptor != nullptr) {
        _FluidSynthDescriptor& descriptor =
            TOREFERENCE<_FluidSynthDescriptor>(_descriptor);
        FSynth_VersionProc fluidSynthVersion =
            descriptor.fluidSynthVersion;

        if (fluidSynthVersion == nullptr) {
            reportBadFunction("version");
        } else {
            result = String{fluidSynthVersion()};
        }
    }

    Logging_trace1("<<: %1", result);
    return result;
}

/*--------------------*/

Object FluidSynth::underlyingObject () const
{
    Logging_trace(">>");
    _FluidSynthDescriptor& descriptor =
        TOREFERENCE<_FluidSynthDescriptor>(_descriptor);
    DynamicLibrary* result = descriptor.library;
    Logging_trace("<<");
    return result;
}
