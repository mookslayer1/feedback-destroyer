cmake_minimum_required(VERSION 3.22)

# Universal binary: SuperRack Performer on Apple Silicon needs arm64 for third-party VST3.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for macOS")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(FeedbackDestroyer VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6          # bump to the latest 8.x release if you like
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(FeedbackDestroyer
    COMPANY_NAME "YourName"
    PLUGIN_MANUFACTURER_CODE Fbdy
    PLUGIN_CODE Fbd1
    FORMATS VST3
    PRODUCT_NAME "Feedback Destroyer"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx
    COPY_PLUGIN_AFTER_BUILD TRUE)   # installs to ~/Library/Audio/Plug-Ins/VST3 on macOS

target_sources(FeedbackDestroyer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(FeedbackDestroyer PRIVATE Source)

target_compile_definitions(FeedbackDestroyer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(FeedbackDestroyer
    PRIVATE juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

# Plain C++ unit tests for the DSP core (no JUCE): `ctest` or ./build/core_tests
option(FD_BUILD_TESTS "Build the DSP core tests" ON)
if(FD_BUILD_TESTS)
    enable_testing()
    add_executable(core_tests tests/core_tests.cpp)
    target_include_directories(core_tests PRIVATE Source/Core)
    add_test(NAME core_tests COMMAND core_tests)
endif()
