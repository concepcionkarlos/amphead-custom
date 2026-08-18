# AmpHead Custom - CMake build
#
# The Projucer project in CopilotTone.jucer stays the source of truth for local
# macOS development. This exists alongside it so the plugin can be built on
# Windows and macOS from one command, which is what continuous integration
# needs: no Projucer step, no IDE, no per-platform project files to keep in sync.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#
# JUCE is fetched at a pinned version rather than vendored, so the repository
# stays small and the build is reproducible.

cmake_minimum_required(VERSION 3.22)
project(AmpHeadCustom VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Universal binary on macOS so one build covers Apple Silicon and Intel.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.13
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# AU is macOS only; everything else builds everywhere.
set(AMPHEAD_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND AMPHEAD_FORMATS AU)
endif()

juce_add_plugin(AmpHeadCustom
    COMPANY_NAME                "JCConcepcion"
    PLUGIN_MANUFACTURER_CODE    Jcca
    PLUGIN_CODE                 Amph
    FORMATS                     ${AMPHEAD_FORMATS}
    PRODUCT_NAME                "AmpHead Custom"
    BUNDLE_ID                   com.jcconcepcion.ampheadcustom
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Distortion
    AU_MAIN_TYPE                kAudioUnitType_Effect
    # The amp reads audio from the input, so the standalone build needs the mic
    # permission string on macOS or it is refused at launch.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "AmpHead Custom needs audio input to process your guitar."
)

# Keeps #include <JuceHeader.h> working, the same way the Projucer build does.
juce_generate_juce_header(AmpHeadCustom)

target_sources(AmpHeadCustom PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(AmpHeadCustom PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(AmpHeadCustom PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
