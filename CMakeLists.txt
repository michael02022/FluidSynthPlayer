# CLAP build of FluidSynthPlayer: a JUCE-free SoundFont player plugin with a
# Dear ImGui GUI over raw X11/OpenGL2 (same GUI stack as the sibling
# projects SfzDrummer/SoloSampler), statically linked against the vendored
# FluidSynth 2.6.2 (external/fluidsynth) - no dependency on whatever
# libfluidsynth version the system has.
#
# The original JUCE-based VST3/AU plugin and the command-line converter keep
# their own build in buildSetup/CMakeLists.txt; this file only builds the
# CLAP plugin and its test host.
cmake_minimum_required(VERSION 3.24) # signalsmith (FluidSynth 2.6 reverb/limiter) needs it
project(FluidSynthPlayer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
find_package(X11 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Icon font (Font Awesome 4, see external/fontawesome/VENDORED_FROM.txt)
# embedded in the binary via xxd -i, so the .clap stays one self-contained
# file.
find_program(XXD_EXECUTABLE xxd REQUIRED)
set(FONTAWESOME_DIR ${CMAKE_SOURCE_DIR}/external/fontawesome)
set(FONTAWESOME_DATA_CPP ${CMAKE_BINARY_DIR}/generated/fontawesome_data.cpp)
add_custom_command(
  OUTPUT ${FONTAWESOME_DATA_CPP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
  COMMAND ${XXD_EXECUTABLE} -i fontawesome-webfont.ttf ${FONTAWESOME_DATA_CPP}
  WORKING_DIRECTORY ${FONTAWESOME_DIR}
  DEPENDS ${FONTAWESOME_DIR}/fontawesome-webfont.ttf
  COMMENT "Embedding fontawesome-webfont.ttf (xxd -i)"
  VERBATIM
)

# UI fonts (Roboto, see external/roboto/VENDORED_FROM.txt), embedded the
# same way.
set(ROBOTO_DIR ${CMAKE_SOURCE_DIR}/external/roboto)
set(ROBOTO_DATA_CPP)
foreach(face Bold Medium)
  set(out ${CMAKE_BINARY_DIR}/generated/roboto_${face}_data.cpp)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${XXD_EXECUTABLE} -i Roboto-${face}.ttf ${out}
    WORKING_DIRECTORY ${ROBOTO_DIR}
    DEPENDS ${ROBOTO_DIR}/Roboto-${face}.ttf
    COMMENT "Embedding Roboto-${face}.ttf (xxd -i)"
    VERBATIM
  )
  list(APPEND ROBOTO_DATA_CPP ${out})
endforeach()

# ---------------------------------------------------------------- FluidSynth
#
# Vendored FluidSynth 2.6.2 (see external/fluidsynth/VENDORED_FROM.txt),
# built as a static library with everything a plugin doesn't need switched
# off: no audio/MIDI drivers (the host does the I/O), no shell/network, no
# OpenMP (keeps libgomp out of the host process). libsndfile stays on for
# SF3 (Ogg/Vorbis compressed) SoundFonts; Signalsmith provides 2.6's limiter
# and extra reverb engines; native DLS support is built in.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(opt enable-alsa enable-aufile enable-dbus enable-ipv6 enable-jack enable-ladspa
            enable-midishare enable-network enable-oss enable-sdl3 enable-pulseaudio
            enable-pipewire enable-readline enable-systemd enable-openmp enable-portaudio)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt enable-libsndfile enable-threads enable-native-dls enable-signalsmith)
  set(${opt} ON CACHE BOOL "" FORCE)
endforeach()
set(osal cpp11 CACHE STRING "" FORCE)
# FluidSynth's signalsmith lookup is relative to the *top-level* source dir;
# point it at the vendored copy, and provide the three header-only targets
# it would otherwise FetchContent from GitHub at configure time.
set(SIGNALSMITH_AUDIO_BASICS_PATH ${CMAKE_SOURCE_DIR}/external/fluidsynth/signalsmith-audio-basics)
add_subdirectory(external/signalsmith/linear EXCLUDE_FROM_ALL)
add_subdirectory(external/signalsmith/dsp EXCLUDE_FROM_ALL)
add_subdirectory(external/signalsmith/hilbert-iir EXCLUDE_FROM_ALL)
add_subdirectory(external/fluidsynth EXCLUDE_FROM_ALL)

add_library(fluidsynthplayer MODULE
  src/clap/plugin.cpp
  src/clap/params.cpp
  src/clap/engine/FluidEngine.cpp
  src/clap/state/SoundFontInfo.cpp
  src/clap/gui/gui_window.cpp
  src/clap/gui/editor_ui.cpp
  src/clap/gui/zenity_dialog.cpp
  external/imgui/imgui.cpp
  external/imgui/imgui_draw.cpp
  external/imgui/imgui_tables.cpp
  external/imgui/imgui_widgets.cpp
  external/imgui/backends/imgui_impl_opengl2.cpp
  ${FONTAWESOME_DATA_CPP}
  ${ROBOTO_DATA_CPP}
)

target_include_directories(fluidsynthplayer PRIVATE
  src/clap
  external/clap/include
  external/imgui
  external/fontawesome
)

# Versions shown in the plugin's info (CLAP descriptor + Options > Info):
# this CLAP plugin's own (project() above) and the upstream FluidSynthPlugin
# release this fork is based on, read from its BuildInformation::version()
# so it follows every upstream merge automatically.
file(STRINGS ${CMAKE_SOURCE_DIR}/src/Main/BuildInformation.cpp FSP_UPSTREAM_VERSION_LINE
     REGEX "return \"[0-9]+\\.[0-9]+(\\.[0-9]+)?\";")
string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" FSP_UPSTREAM_VERSION "${FSP_UPSTREAM_VERSION_LINE}")
if(NOT FSP_UPSTREAM_VERSION)
  set(FSP_UPSTREAM_VERSION "unknown")
endif()
message(STATUS "FluidSynthPlayer ${PROJECT_VERSION}, based on FluidSynthPlugin ${FSP_UPSTREAM_VERSION}")
target_compile_definitions(fluidsynthplayer PRIVATE
  FSP_PLUGIN_VERSION="${PROJECT_VERSION}"
  FSP_UPSTREAM_VERSION="${FSP_UPSTREAM_VERSION}"
)

target_link_libraries(fluidsynthplayer PRIVATE
  libfluidsynth
  X11::X11
  OpenGL::GL
  Threads::Threads
)

set_target_properties(fluidsynthplayer PROPERTIES
  PREFIX ""
  SUFFIX ".clap"
  OUTPUT_NAME "FluidSynthPlayer"
)

# Standalone test host: dlopen's the built .clap and drives it without a DAW
# (audio rendering, params, state round trip, optional GUI window).
add_executable(mini_host test/clap/mini_host.c)
target_include_directories(mini_host PRIVATE external/clap/include)
target_link_libraries(mini_host PRIVATE ${CMAKE_DL_LIBS} m)
