cmake_minimum_required(VERSION 3.22)

# These must be set BEFORE project(), or CMake ignores them.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum iOS version")
elseif(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build for Apple Silicon and Intel")
endif()

project(FeedbackDestroyer VERSION 4.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Which plugin formats to build. The default (VST3 only) is exactly what the main build has always produced.
# Extra formats are opt-in so they cannot break the main build:
#   -DFD_BUILD_AU=ON          Audio Unit (macOS)
#   -DFD_BUILD_STANDALONE=ON  Standalone app (macOS / Windows / Linux / iOS)
#   -DFD_BUILD_AUV3=ON        AUv3 (iOS only, configure with -DCMAKE_SYSTEM_NAME=iOS -G Xcode)
option(FD_BUILD_AU "Build the Audio Unit (macOS)" OFF)
option(FD_BUILD_STANDALONE "Build the standalone app" OFF)
option(FD_BUILD_AUV3 "Build the AUv3 (iOS)" OFF)

set(FD_FORMATS)
set(FD_EXTRA_ARGS)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(FD_BUILD_AUV3)
        list(APPEND FD_FORMATS AUv3)
    endif()
else()
    list(APPEND FD_FORMATS VST3)
    if(APPLE AND FD_BUILD_AU)
        list(APPEND FD_FORMATS AU)
    endif()
endif()

if(FD_BUILD_STANDALONE)
    list(APPEND FD_FORMATS Standalone)
    list(APPEND FD_EXTRA_ARGS MICROPHONE_PERMISSION_ENABLED TRUE
         MICROPHONE_PERMISSION_TEXT "Freqd Out listens to your input to find feedback.")
endif()

juce_add_plugin(FeedbackDestroyer
    COMPANY_NAME "SJAM Audio"
    BUNDLE_ID com.sjamaudio.freqdout
    PLUGIN_MANUFACTURER_CODE Sjam
    PLUGIN_CODE Fqdo
    FORMATS ${FD_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    PRODUCT_NAME "Freqd Out v4.0"
    DESCRIPTION "Adaptive feedback suppression and notch filtering"
    VST3_CATEGORIES "Fx"
    ${FD_EXTRA_ARGS})

juce_generate_juce_header(FeedbackDestroyer)

target_sources(FeedbackDestroyer PRIVATE
    Source/Biquad.h
    Source/Analyzer.h
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp
    Source/PluginEditor.h
    Source/PluginEditor.cpp)

target_compile_definitions(FeedbackDestroyer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(FeedbackDestroyer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
