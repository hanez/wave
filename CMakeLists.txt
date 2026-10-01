cmake_minimum_required(VERSION 3.25)

# Set this before project() so compiler checks and dependencies use Ventura too.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
        "Minimum supported macOS version" FORCE)
endif()

project(WaveEmulationSample VERSION 0.1.11 LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep Windows downloads self-contained, including the standalone executable.
# Apply the same CRT choice to JUCE, Musashi, plugins and tests.
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "MSVC runtime library")
endif()

option(WAVE_BUILD_TESTS "Build deterministic DSP and firmware tests" ON)

# Build universal macOS bundles. Distribution signing is an explicit opt-in.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING
        "macOS target architectures" FORCE)
endif()
option(WAVE_SIGN_RELEASE_ARTIFACTS
    "Developer ID-sign macOS Release plugin and app bundles" OFF)
set(WAVE_CODESIGN_IDENTITY
    ""
    CACHE STRING "Developer ID Application identity for macOS releases")

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
include(cmake/PrepareJuce.cmake)
wave_prepare_juce("${juce_SOURCE_DIR}")

FetchContent_Declare(
    Musashi
    GIT_REPOSITORY https://github.com/kstenerud/Musashi.git
    GIT_TAG 313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Musashi)

include(cmake/PrepareMusashi.cmake)
set(WAVE_MUSASHI_DIR "${CMAKE_CURRENT_BINARY_DIR}/wave-musashi")
wave_prepare_musashi("${musashi_SOURCE_DIR}" "${WAVE_MUSASHI_DIR}")

add_executable(WaveM68kGenerator EXCLUDE_FROM_ALL
    ${musashi_SOURCE_DIR}/m68kmake.c)

set(MUSASHI_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/musashi-generated)
add_custom_command(
    OUTPUT
        ${MUSASHI_GENERATED_DIR}/m68kops.c
        ${MUSASHI_GENERATED_DIR}/m68kops.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSASHI_GENERATED_DIR}
    COMMAND WaveM68kGenerator ${MUSASHI_GENERATED_DIR} ${musashi_SOURCE_DIR}/m68k_in.c
    DEPENDS
        WaveM68kGenerator
        ${musashi_SOURCE_DIR}/m68k_in.c
    VERBATIM)

add_library(WaveM68k STATIC
    ${WAVE_MUSASHI_DIR}/m68kcpu.c
    ${WAVE_MUSASHI_DIR}/softfloat/softfloat.c
    ${MUSASHI_GENERATED_DIR}/m68kops.c)
target_include_directories(WaveM68k PUBLIC
    ${WAVE_MUSASHI_DIR}
    ${MUSASHI_GENERATED_DIR})
target_compile_features(WaveM68k PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(WaveM68k PRIVATE "WAVE_M68K_THREAD_LOCAL=__declspec(thread)")
else()
    target_compile_definitions(WaveM68k PRIVATE WAVE_M68K_THREAD_LOCAL=_Thread_local)
endif()
target_compile_definitions(WaveM68k PRIVATE
    M68K_EMULATE_010=0
    M68K_EMULATE_EC020=0
    M68K_EMULATE_020=0
    M68K_EMULATE_030=0
    M68K_EMULATE_040=0
    M68K_EMULATE_ADDRESS_ERROR=1
    M68K_INSTRUCTION_HOOK=1)

set(WAVE_ASSET_SOURCES
    media/WaldorfWaveUI_NOLOGO.svg
    media/Slider.svg
    media/Knob1.svg
    media/Knob_red1.svg
    media/Knob_red_large1.svg
    media/button_edit.svg
    media/button_round.svg
    media/button_vertical.svg
    media/LED_OFF.svg
    media/LED_ON_GREEN.svg
    media/LED_ON_RED.svg
    media/LED_ON_YELLOW.svg)
option(WAVE_EMBED_PRIVATE_ASSETS "Embed locally supplied private firmware and banks" OFF)
set(WAVE_PRIVATE_MASTER_ROM Firmware/wave_sys1_700/w2sys.bin)
set(WAVE_PRIVATE_VOICE_ROM Firmware/wave_sys1_700/wdv.sys)
set(WAVE_PRIVATE_PPG_ROM Firmware/wave_sys1_700/ppg-wave-2.3-v6-wavetables.rom)
set(WAVE_PRIVATE_FACTORY_SET wave.set)
if(WAVE_EMBED_PRIVATE_ASSETS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${WAVE_PRIVATE_MASTER_ROM}"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${WAVE_PRIVATE_VOICE_ROM}"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${WAVE_PRIVATE_PPG_ROM}")
    list(APPEND WAVE_ASSET_SOURCES
        ${WAVE_PRIVATE_MASTER_ROM}
        ${WAVE_PRIVATE_VOICE_ROM}
        ${WAVE_PRIVATE_PPG_ROM})
    set(WAVE_HAS_PRIVATE_ROMS 1)
else()
    set(WAVE_HAS_PRIVATE_ROMS 0)
endif()
if(WAVE_EMBED_PRIVATE_ASSETS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${WAVE_PRIVATE_FACTORY_SET}")
    list(APPEND WAVE_ASSET_SOURCES ${WAVE_PRIVATE_FACTORY_SET})
    set(WAVE_HAS_FACTORY_SET 1)
else()
    set(WAVE_HAS_FACTORY_SET 0)
endif()

set(WAVE_HAS_PRIVATE_UPPER_TABLES 0)
if(WAVE_EMBED_PRIVATE_ASSETS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Firmware/private/FactoryUpperWavetables.h")
    set(WAVE_HAS_PRIVATE_UPPER_TABLES 1)
endif()

juce_add_binary_data(WaveAssets
    NAMESPACE WaveAssets
    SOURCES ${WAVE_ASSET_SOURCES})

juce_add_binary_data(WavePpgAssets
    NAMESPACE WavePpgAssets
    HEADER_NAME PpgData.h
    SOURCES data/ppg-v6-wavetables.bin)

set(WAVE_CORE_SOURCES
    Source/Dsp/DspMath.cpp
    Source/Dsp/DspMath.h
    Source/Dsp/Cem3387.cpp
    Source/Dsp/Cem3387.h
    Source/Dsp/ReferenceComparator.cpp
    Source/Dsp/ReferenceComparator.h
    Source/Dsp/PpgWaveRom.cpp
    Source/Dsp/PpgWaveRom.h
    Source/Dsp/WaldorfAsic.cpp
    Source/Dsp/WaldorfAsic.h
    Source/Dsp/WaldorfEngine.cpp
    Source/Dsp/WaldorfEngine.h
    Source/Dsp/WaveOutputStage.cpp
    Source/Dsp/WaveOutputStage.h
    Source/Dsp/WaveEnvelope.cpp
    Source/Dsp/WaveEnvelope.h
    Source/Dsp/WdvEnvelope.cpp
    Source/Dsp/WdvEnvelope.h
    Source/Dsp/WaveLfo.cpp
    Source/Dsp/WaveLfo.h
    Source/Firmware/FirmwareBundle.cpp
    Source/Firmware/FirmwareBundle.h
    Source/Firmware/Dp8473.cpp
    Source/Firmware/Dp8473.h
    Source/Firmware/DosFloppyImage.cpp
    Source/Firmware/DosFloppyImage.h
    Source/Firmware/M68000.cpp
    Source/Firmware/M68000.h
    Source/Firmware/MasterFirmwareRuntime.cpp
    Source/Firmware/MasterFirmwareRuntime.h
    Source/Firmware/SharedFirmwareMemory.h
    Source/Firmware/Via6522.cpp
    Source/Firmware/Via6522.h
    Source/Firmware/VoiceFirmwareRuntime.cpp
    Source/Firmware/VoiceFirmwareRuntime.h
    Source/Firmware/VoiceBoardProtocol.cpp
    Source/Firmware/VoiceBoardProtocol.h
    Source/Presets/WaveFactorySet.cpp
    Source/Presets/WaveFactorySet.h
    Source/UI/WaveLcdModel.cpp
    Source/UI/WaveLcdModel.h
    Source/WaveParameters.cpp
    Source/WaveParameters.h)

add_library(WaveCore STATIC ${WAVE_CORE_SOURCES})
target_include_directories(WaveCore PUBLIC Source)
target_link_libraries(WaveCore
    PUBLIC
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_cryptography
        juce::juce_data_structures
        juce::juce_dsp
        WaveM68k
    PRIVATE
        WavePpgAssets)
target_compile_definitions(WaveCore
    PUBLIC
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)

target_compile_definitions(WaveCore PUBLIC
    WAVE_HAS_PRIVATE_UPPER_TABLES=${WAVE_HAS_PRIVATE_UPPER_TABLES})
if(WAVE_HAS_PRIVATE_UPPER_TABLES)
    target_include_directories(WaveCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Firmware/private")
endif()

add_executable(DecodePpgWavetables EXCLUDE_FROM_ALL
    Tools/DecodePpgWavetables.cpp Source/Dsp/PpgWaveRom.cpp)
target_include_directories(DecodePpgWavetables PRIVATE Source)

add_executable(CreateWaveDisk EXCLUDE_FROM_ALL Tools/CreateWaveDisk.cpp)
target_link_libraries(CreateWaveDisk PRIVATE WaveCore juce::juce_recommended_config_flags)

if(MSVC)
    target_compile_options(WaveCore PRIVATE /W4 /permissive-)
else()
    target_compile_options(WaveCore PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(APPLE)
    set(WAVE_PLUGIN_FORMATS AU VST3 AAX Standalone)
else()
    set(WAVE_PLUGIN_FORMATS VST3 Standalone)
endif()

# JUCE's manifest helper is executed directly after linking and cannot run on
# the build host during Windows cross-compilation. VST3 hosts can scan the
# bundle without this optional JSON manifest.
set(WAVE_VST3_AUTO_MANIFEST TRUE)
if(CMAKE_CROSSCOMPILING)
    set(WAVE_VST3_AUTO_MANIFEST FALSE)
endif()

juce_add_plugin(WaveEmulation
    COMPANY_NAME "DJW"
    BUNDLE_ID "com.DJW.WaveEmulation"
    PLUGIN_MANUFACTURER_CODE Djwh
    PLUGIN_CODE WvSm
    FORMATS ${WAVE_PLUGIN_FORMATS}
    PRODUCT_NAME "Wave Emulation"
    DESCRIPTION "Firmware-aware Waldorf Wave hardware modelling research sample"
    AAX_CATEGORY AAX_ePlugInCategory_SWGenerators
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_AUTO_MANIFEST ${WAVE_VST3_AUTO_MANIFEST}
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(WaveEmulation PRIVATE
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/UI/WaveLcdComponent.cpp
    Source/UI/WaveLcdComponent.h)
target_link_libraries(WaveEmulation
    PRIVATE
        WaveCore
        WaveAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
target_compile_definitions(WaveEmulation
    PUBLIC
        JUCE_REPORT_APP_USAGE=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        WAVE_HAS_PRIVATE_ROMS=${WAVE_HAS_PRIVATE_ROMS})
target_compile_definitions(WaveEmulation
    PUBLIC WAVE_HAS_FACTORY_SET=${WAVE_HAS_FACTORY_SET})

target_sources(WaveEmulation_Standalone PRIVATE Source/StandaloneApp.cpp)
target_compile_definitions(WaveEmulation_Standalone PRIVATE
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)

if(APPLE AND WAVE_SIGN_RELEASE_ARTIFACTS)
    find_program(WAVE_CODESIGN_EXECUTABLE codesign REQUIRED)
    foreach(WAVE_BUNDLE_TARGET
            WaveEmulation_Standalone
            WaveEmulation_AU
            WaveEmulation_VST3
            WaveEmulation_AAX)
        add_custom_command(TARGET ${WAVE_BUNDLE_TARGET} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DBUILD_CONFIG=$<CONFIG>"
                "-DCODESIGN_EXECUTABLE=${WAVE_CODESIGN_EXECUTABLE}"
                "-DCODESIGN_IDENTITY=${WAVE_CODESIGN_IDENTITY}"
                "-DBUNDLE_PATH=$<TARGET_BUNDLE_DIR:${WAVE_BUNDLE_TARGET}>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignAppleBundle.cmake"
            COMMENT "Developer ID signing ${WAVE_BUNDLE_TARGET}"
            VERBATIM)
    endforeach()
endif()

if(WAVE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()
