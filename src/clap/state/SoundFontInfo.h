// Reads the RIFF "LIST INFO" chunk of a SoundFont (SF2/SF3) or DLS file:
// name, author, copyright, comments... FluidSynth's public API doesn't
// expose any of it, so this is a tiny standalone RIFF walker that only looks
// at top-level chunks and never touches the (potentially huge) sample data.
#pragma once

#include <string>

#include "shared.hpp"

// Returns false (and leaves `out` mostly empty) if the file can't be opened
// or isn't a RIFF file; a RIFF file without an INFO list still returns true
// with just fileSize filled in.
bool readSoundFontInfo(const std::string& path, SoundFontInfo& out);

// Scans the SoundFont's instrument modulators (pdta/imod) for the SF2
// default modulators redefined with a zero amount - which per the spec
// replaces (disables) them - and returns the StandardController bits.
// A controller is only reported if *every* such redefinition in the file
// is zero, so deliberate non-zero customizations are never undone.
uint32_t scanDisabledControllers(const std::string& path);

// True for the extensions the plugin offers to load (.sf2, .sf3, .dls) -
// used by the file browser filter and the drag & drop handler. FluidSynth
// itself has the last word when actually loading.
bool isSoundFontFile(const std::string& path);
