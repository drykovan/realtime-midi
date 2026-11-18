cmake_minimum_required(VERSION 3.22)
project(RealtimeMIDI VERSION 0.0.1 LANGUAGES CXX)

include(CTest)
add_library(core STATIC src/core/NoteSequence.cpp src/core/PitchDetector.cpp)
target_include_directories(core PUBLIC src/core)
target_compile_features(core PUBLIC cxx_std_17)
set_target_properties(core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    add_executable(CoreSmoke tests/CoreSmoke.cpp)
    target_link_libraries(CoreSmoke PRIVATE core)
    add_test(NAME core-scheduler COMMAND CoreSmoke)
endif()

option(REALTIME_MIDI_BUILD_VST3 "Build the JUCE VST3 adapter" ON)
if(NOT REALTIME_MIDI_BUILD_VST3)
    return()
endif()

enable_language(C)
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ea77444750de45e036e0279bb0291144bf8e3688 # 8.0.16-security
)
FetchContent_MakeAvailable(JUCE)

option(REALTIME_MIDI_ENABLE_IAC "Mirror detected notes to macOS IAC Driver Bus 1" OFF)

juce_add_plugin(RealtimeMIDI
    PRODUCT_NAME "Realtime MIDI"
    COMPANY_NAME "Realtime MIDI"
    BUNDLE_ID com.drykovan.realtimemidi
    PLUGIN_MANUFACTURER_CODE Dryk
    PLUGIN_CODE RtMd
    IS_SYNTH FALSE
    IS_MIDI_EFFECT FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT TRUE
    VST3_CATEGORIES Fx
    FORMATS VST3
)
set(REALTIME_MIDI_DEV_BUILD "" CACHE STRING "Local Nota UTC build stamp")
if(REALTIME_MIDI_DEV_BUILD AND NOT REALTIME_MIDI_DEV_BUILD MATCHES "^[0-9]+T[0-9]+[.][0-9]+Z$")
    message(FATAL_ERROR "REALTIME_MIDI_DEV_BUILD must be a UTC timestamp")
endif()
configure_file(.dev/nota/DevBuild.h.in generated/DevBuild.h)
target_include_directories(RealtimeMIDI PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_sources(RealtimeMIDI PRIVATE src/PluginProcessor.cpp)
target_compile_definitions(RealtimeMIDI PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(RealtimeMIDI
    PRIVATE core juce::juce_audio_processors
    PUBLIC juce::juce_recommended_config_flags
)

if(APPLE AND REALTIME_MIDI_ENABLE_IAC)
    target_compile_definitions(RealtimeMIDI PRIVATE REALTIME_MIDI_ENABLE_IAC=1)
    target_link_libraries(RealtimeMIDI PRIVATE "-framework CoreMIDI" "-framework CoreFoundation")
endif()

if(BUILD_TESTING)
    add_executable(RealtimeMIDISmoke tests/Smoke.cpp)
    target_compile_features(RealtimeMIDISmoke PRIVATE cxx_std_17)
    target_include_directories(RealtimeMIDISmoke PRIVATE "${JUCE_SOURCE_DIR}/modules")
    target_link_libraries(RealtimeMIDISmoke PRIVATE RealtimeMIDI)
    add_test(NAME processor-smoke COMMAND RealtimeMIDISmoke)
endif()
