cmake_minimum_required(VERSION 3.22)

# These two must be set BEFORE project(), or CMake ignores them.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build for Apple Silicon and Intel")

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

juce_add_plugin(FeedbackDestroyer
    COMPANY_NAME "SJAM Audio"
    BUNDLE_ID com.sjamaudio.freqdout
    PLUGIN_MANUFACTURER_CODE Sjam
    PLUGIN_CODE Fqdo
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    PRODUCT_NAME "Freqd Out v4.0"
    DESCRIPTION "Adaptive feedback suppression and notch filtering"
    VST3_CATEGORIES "Fx")

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
