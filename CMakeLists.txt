# CLAP build of FluidSynthPlayer: a JUCE-free SoundFont player plugin with a
# Dear ImGui GUI over OpenGL2 - raw X11/GLX on Linux (same GUI stack as the
# sibling projects SfzDrummer/SoloSampler), Win32/WGL on Windows - statically
# linked against the vendored FluidSynth 2.6.2 (external/fluidsynth) - no
# dependency on whatever libfluidsynth version the system has.
#
# Platforms: Linux (GCC/Clang) and Windows (MSVC or MinGW-w64).
#
# The original JUCE-based VST3/AU plugin and the command-line converter keep
# their own build in buildSetup/CMakeLists.txt; this file only builds the
# CLAP plugin and its test host.
cmake_minimum_required(VERSION 3.24) # signalsmith (FluidSynth 2.6 reverb/limiter) needs it
project(FluidSynthPlayer VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_property(FSP_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT CMAKE_BUILD_TYPE AND NOT FSP_MULTI_CONFIG)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Windows: link the C/C++ runtime statically (MSVC: /MT, MinGW: libgcc,
# libstdc++, winpthread), so the .clap doesn't need DLLs next to the *host*
# executable - Windows looks for a plugin's dependent DLLs there, not next
# to the plugin. Must be set before any target is created.
option(FSP_STATIC_RUNTIME "Windows: link the C/C++ runtime statically" ON)
if(WIN32 AND FSP_STATIC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT WIN32)
  find_package(X11 REQUIRED)
endif()
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------- fonts
#
# Icon font (Font Awesome 4, see external/fontawesome/VENDORED_FROM.txt) and
# UI fonts (Roboto, see external/roboto/VENDORED_FROM.txt), embedded in the
# binary so the .clap stays one self-contained file. cmake/EmbedFile.cmake
# generates the same symbols `xxd -i` would, without needing xxd.
set(FSP_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(FSP_EMBEDDED_FONTS_CPP)
function(fsp_embed_file input symbol)
  set(out ${FSP_GENERATED_DIR}/${symbol}_data.cpp)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FSP_GENERATED_DIR}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${out} -DNAME=${symbol}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${input} ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding ${symbol}"
    VERBATIM
  )
  set(FSP_EMBEDDED_FONTS_CPP ${FSP_EMBEDDED_FONTS_CPP} ${out} PARENT_SCOPE)
endfunction()
fsp_embed_file(${CMAKE_SOURCE_DIR}/external/fontawesome/fontawesome-webfont.ttf fontawesome_webfont_ttf)
fsp_embed_file(${CMAKE_SOURCE_DIR}/external/roboto/Roboto-Bold.ttf Roboto_Bold_ttf)
fsp_embed_file(${CMAKE_SOURCE_DIR}/external/roboto/Roboto-Medium.ttf Roboto_Medium_ttf)

# ---------------------------------------------------------------- libsndfile
#
# Needed by FluidSynth to decode SF3 (Ogg/Vorbis compressed) SoundFonts.
# Looked up here (same way FluidSynth does: CMake config package, else
# pkg-config) so the plugin knows whether it got it. Without it everything
# but SF3 still works - common on Windows, where it isn't a system library:
# install it e.g. with vcpkg (static triplet, see README) or MSYS2.
option(FSP_WITH_SNDFILE "SF3 support through libsndfile (if found)" ON)
set(FSP_HAVE_SNDFILE OFF)
if(FSP_WITH_SNDFILE)
  list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/external/fluidsynth/cmake_admin)
  # FluidSynth's pkg-config based Find modules (SndFileLegacy -> Ogg,
  # Vorbis, FLAC, ...) call helpers defined here.
  include(PkgConfigHelpers)
  find_package(SndFile QUIET)
  if(NOT TARGET SndFile::sndfile)
    find_package(SndFileLegacy QUIET)
    if(TARGET SndFile::sndfile)
      # FluidSynth repeats this lookup, and the legacy module only reports
      # success when it creates the target itself - hand FluidSynth the one
      # found here through a find_package() redirect (cleared every run).
      include(CMakePackageConfigHelpers)
      file(WRITE ${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/sndfile-config.cmake "")
      write_basic_package_version_file(
        ${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/sndfile-config-version.cmake
        VERSION ${SndFile_VERSION} COMPATIBILITY AnyNewerVersion)
    endif()
  endif()
  if(TARGET SndFile::sndfile)
    set(FSP_HAVE_SNDFILE ON)
  endif()
endif()
if(FSP_HAVE_SNDFILE)
  message(STATUS "libsndfile found: SF3 SoundFonts supported")
  if(WIN32)
    # A DLL dependency of the plugin is searched next to the host's .exe,
    # not next to the .clap: prefer a static libsndfile on Windows.
    get_target_property(_sndfile_loc SndFile::sndfile LOCATION)
    if(_sndfile_loc MATCHES "\\.dll(\\.a)?$" OR _sndfile_loc MATCHES "[/\\\\]bin[/\\\\]")
      message(WARNING "libsndfile is a DLL (${_sndfile_loc}): the plugin will only load if "
                      "libsndfile and its codec DLLs are found by the host (next to its .exe or "
                      "in PATH). Use a static libsndfile (e.g. vcpkg x64-windows-static) to avoid it.")
    endif()
  endif()
else()
  message(WARNING "libsndfile not found (or FSP_WITH_SNDFILE=OFF): building WITHOUT SF3 "
                  "support - SF2 and DLS SoundFonts still work.")
endif()
# Read by scripts/package-windows.ps1 to describe the packaged build; with
# vcpkg it also points at the license files of libsndfile and its codecs
# (<dir>/<port>/copyright), linked into the plugin.
set(_fsp_build_info "sf3=${FSP_HAVE_SNDFILE}\n")
if(FSP_HAVE_SNDFILE AND DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  string(APPEND _fsp_build_info "vcpkg_share=${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share\n")
endif()
file(WRITE ${CMAKE_BINARY_DIR}/fsp_build_info.txt "${_fsp_build_info}")

# ---------------------------------------------------------------- FluidSynth
#
# Vendored FluidSynth 2.6.2 (see external/fluidsynth/VENDORED_FROM.txt),
# built as a static library with everything a plugin doesn't need switched
# off: no audio/MIDI drivers (the host does the I/O), no shell/network, no
# OpenMP (keeps libgomp out of the host process). libsndfile stays on for
# SF3 (Ogg/Vorbis compressed) SoundFonts when available; Signalsmith
# provides 2.6's limiter and extra reverb engines; native DLS support is
# built in.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(opt enable-alsa enable-aufile enable-dbus enable-ipv6 enable-jack enable-ladspa
            enable-midishare enable-network enable-oss enable-sdl3 enable-pulseaudio
            enable-pipewire enable-readline enable-systemd enable-openmp enable-portaudio
            enable-dsound enable-wasapi enable-waveout enable-winmidi)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt enable-threads enable-native-dls enable-signalsmith)
  set(${opt} ON CACHE BOOL "" FORCE)
endforeach()
set(enable-libsndfile ${FSP_HAVE_SNDFILE} CACHE BOOL "" FORCE)
set(osal cpp11 CACHE STRING "" FORCE)
# FluidSynth's signalsmith lookup is relative to the *top-level* source dir;
# point it at the vendored copy, and provide the three header-only targets
# it would otherwise FetchContent from GitHub at configure time.
set(SIGNALSMITH_AUDIO_BASICS_PATH ${CMAKE_SOURCE_DIR}/external/fluidsynth/signalsmith-audio-basics)
add_subdirectory(external/signalsmith/linear EXCLUDE_FROM_ALL)
add_subdirectory(external/signalsmith/dsp EXCLUDE_FROM_ALL)
add_subdirectory(external/signalsmith/hilbert-iir EXCLUDE_FROM_ALL)
add_subdirectory(external/fluidsynth EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- plugin

if(WIN32)
  set(FSP_PLATFORM_SOURCES
    src/clap/gui/gui_window_win32.cpp
    src/clap/gui/file_dialog_win32.cpp
  )
else()
  set(FSP_PLATFORM_SOURCES
    src/clap/gui/gui_window_x11.cpp
    src/clap/gui/file_dialog_zenity.cpp
  )
endif()

# All of the plugin's code, as a static library: each plugin format below is
# just src/clap/clap_entry.cpp (the exported clap_entry) linked against it.
add_library(fluidsynthplayer_impl STATIC
  src/clap/plugin.cpp
  src/clap/params.cpp
  src/clap/platform.cpp
  src/clap/engine/FluidEngine.cpp
  src/clap/state/SoundFontInfo.cpp
  src/clap/gui/editor_ui.cpp
  src/clap/gui/imgui_setup.cpp
  ${FSP_PLATFORM_SOURCES}
  external/imgui/imgui.cpp
  external/imgui/imgui_draw.cpp
  external/imgui/imgui_tables.cpp
  external/imgui/imgui_widgets.cpp
  external/imgui/backends/imgui_impl_opengl2.cpp
  ${FSP_EMBEDDED_FONTS_CPP}
)

target_include_directories(fluidsynthplayer_impl PRIVATE
  src/clap
  external/imgui
  external/fontawesome
)
target_include_directories(fluidsynthplayer_impl PUBLIC external/clap/include)

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
target_compile_definitions(fluidsynthplayer_impl PRIVATE
  FSP_PLUGIN_VERSION="${PROJECT_VERSION}"
  FSP_UPSTREAM_VERSION="${FSP_UPSTREAM_VERSION}"
)

target_link_libraries(fluidsynthplayer_impl PUBLIC
  libfluidsynth
  OpenGL::GL
  Threads::Threads
)

if(FSP_HAVE_SNDFILE)
  target_compile_definitions(fluidsynthplayer_impl PRIVATE FSP_HAVE_SNDFILE)
  target_link_libraries(fluidsynthplayer_impl PUBLIC SndFile::sndfile)
endif()

if(WIN32)
  target_compile_definitions(fluidsynthplayer_impl PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    # imgui.cpp's Win32 clipboard/IME code uses the ANSI/Unicode-neutral
    # API names; everything of ours calls the explicit ...W functions.
    UNICODE _UNICODE)
  target_link_libraries(fluidsynthplayer_impl PUBLIC
    user32 gdi32 shell32 comdlg32 ole32 uuid imm32)
  if(MSVC)
    target_compile_options(fluidsynthplayer_impl PRIVATE /utf-8 /bigobj)
  endif()
else()
  target_link_libraries(fluidsynthplayer_impl PUBLIC X11::X11)
endif()

# Settings every plugin format's module shares.
function(fsp_plugin_module target)
  target_sources(${target} PRIVATE src/clap/clap_entry.cpp)
  target_link_libraries(${target} PRIVATE fluidsynthplayer_impl)
  if(MINGW AND FSP_STATIC_RUNTIME)
    # libgcc, libstdc++ and winpthread inside the plugin (see above); the
    # Windows system libraries stay DLL imports.
    target_link_options(${target} PRIVATE -static)
  endif()
endfunction()

# ---------------------------------------------------------------- CLAP

add_library(fluidsynthplayer MODULE)
fsp_plugin_module(fluidsynthplayer)
set_target_properties(fluidsynthplayer PROPERTIES
  PREFIX ""
  SUFFIX ".clap"
  OUTPUT_NAME "FluidSynthPlayer"
)

# ---------------------------------------------------------------- VST3
#
# FluidSynthPlayer.vst3 from the very same code through clap-wrapper
# (external/clap-wrapper, with patches/clap-wrapper applied) and the VST3 SDK
# subset in external/vst3sdk - nothing is downloaded. Built by `all` next to
# the CLAP; target fluidsynthplayer_vst3.
option(FSP_BUILD_VST3 "Also build the VST3 (clap-wrapper)" ON)
if(FSP_BUILD_VST3)
  set(CLAP_SDK_ROOT ${CMAKE_SOURCE_DIR}/external/clap)
  set(VST3_SDK_ROOT ${CMAKE_SOURCE_DIR}/external/vst3sdk)
  set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
  set(CLAP_WRAPPER_BUILD_AUV2 OFF CACHE BOOL "" FORCE)
  add_subdirectory(external/clap-wrapper EXCLUDE_FROM_ALL)

  add_library(fluidsynthplayer_vst3 MODULE)
  fsp_plugin_module(fluidsynthplayer_vst3)
  target_add_vst3_wrapper(TARGET fluidsynthplayer_vst3
    OUTPUT_NAME "FluidSynthPlayer"
    BUNDLE_IDENTIFIER "com.michael02022.fluidsynthplayer.vst3"
    BUNDLE_VERSION "${PROJECT_VERSION}"
  )
  # The binary inside the bundle must be named exactly like the bundle
  # (Contents/x86_64-win/FluidSynthPlayer.vst3) or hosts skip it; MinGW's
  # default "lib" module prefix would break that, and clap-wrapper only
  # clears it on Linux.
  set_target_properties(fluidsynthplayer_vst3 PROPERTIES PREFIX "")
endif()

# ---------------------------------------------------------------- test host
#
# Standalone test host: loads the built .clap and drives it without a DAW
# (audio rendering, params, state round trip, optional GUI window).
add_executable(mini_host test/clap/mini_host.c)
set_target_properties(mini_host PROPERTIES C_STANDARD 11) # designated initializers (MSVC)
target_include_directories(mini_host PRIVATE external/clap/include)
if(WIN32)
  target_link_libraries(mini_host PRIVATE user32)
  if(MINGW AND FSP_STATIC_RUNTIME)
    target_link_options(mini_host PRIVATE -static)
  endif()
else()
  target_link_libraries(mini_host PRIVATE ${CMAKE_DL_LIBS} m)
endif()
