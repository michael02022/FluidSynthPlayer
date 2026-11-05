// Declarations for the Font Awesome .ttf embedded in the binary (see
// external/fontawesome/VENDORED_FROM.txt). The symbols are defined in
// <build>/generated/fontawesome_webfont_ttf_data.cpp, generated from
// external/fontawesome/fontawesome-webfont.ttf by cmake/EmbedFile.cmake
// (same symbols as `xxd -i`, see CMakeLists.txt) -- there's no source .cpp
// for this .hpp, it just declares what the build generates.
#pragma once

extern unsigned char fontawesome_webfont_ttf[];
extern unsigned int fontawesome_webfont_ttf_len;

// Roboto (Apache 2.0, see external/roboto/VENDORED_FROM.txt), embedded the
// same way (<build>/generated/Roboto_*_ttf_data.cpp).
extern unsigned char Roboto_Bold_ttf[];
extern unsigned int Roboto_Bold_ttf_len;
extern unsigned char Roboto_Medium_ttf[];
extern unsigned int Roboto_Medium_ttf_len;
