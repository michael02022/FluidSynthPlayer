/**
 * @file
 * The <C>FluidSynthSettings</C> class specifies an object-oriented
 * wrapper around an underlying fluidsynth settings object.
 *
 * @author Dr. Thomas Tensi
 * @date   2022-04
 */

/*====================*/

#pragma once

/*=========*/
/* IMPORTS */
/*=========*/

#include "FluidSynth.h"
#include "Dictionary.h"

/*--------------------*/

using BaseTypes::Containers::Dictionary;
using Libraries::FluidSynth::FluidSynth;

/*====================*/

namespace Libraries::FluidSynth {

    /**
     * An object-oriented wrapper around an underlying fluidsynth
     * settings object
     */
    struct FluidSynthSettings {

        /*--------------------*/
        /* con-/destruction   */
        /*--------------------*/

        /**
         * Constructor for settings using an underlying fluidsynth
         * library object.
         *
         * @param [in] library  the fluidsynth library object 
         */
        FluidSynthSettings (IN FluidSynth* library);

        /*--------------------*/

        /** Settings destructor */
        ~FluidSynthSettings ();

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
        static const Dictionary& keyToDefaultValueMap ();
        
        /*--------------------*/

        /**
         * Returns the default mapping from keys to values for the
         * fluidsynth settings.
         *
         * @return  map of default values for all keys relevant for
         *          initialization
         */
        static const StringList& restartingKeysList (); 

        /*--------------------*/

        /**
         * Returns the underlying settings object from the FluidSynth
         * library.
         *
         * @return  underlying fluidsynth settings object
         */
        Object underlyingObject () const;

        /*--------------------*/
        /* change             */
        /*--------------------*/

        /**
         * Sets setting given by <C>key</C> to new value given by
         * <C>value</C>; the value type is adjusted internally.
         *
         * @param[in] key    settings name
         * @param[in] value  value for key as string
         * @return  information whether setting was successful
         */
        Boolean set (IN String& key, IN String& value);

        /*--------------------*/

        private:

            /** descriptor object with internal data */
            Object _descriptor;

    };

}
