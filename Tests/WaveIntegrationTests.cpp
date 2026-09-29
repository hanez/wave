#include "PluginProcessor.h"
#include "PanelWiring.h"
#include "Firmware/DosFloppyImage.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireFinite(const juce::AudioBuffer<float>& audio)
{
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            require(std::isfinite(audio.getSample(channel, sample)),
                    "Plug-in processing produced non-finite audio");
}

void testAutomaticBootAudioAndState()
{
    auto processorStorage = std::make_unique<WaveEmulationAudioProcessor>();
    auto& processor = *processorStorage;
    const auto& report = processor.getFirmwareReport();
    require(report.hasBothImages(),
            "Private master and voice firmware did not load automatically");
    require(report.authenticity
                == wave::firmware::Bundle::Authenticity::verifiedOs1700,
            "Automatically loaded firmware is not the authenticated OS 1.700 pair");
    require(processor.getMasterFirmwareRuntime().isLoaded(),
            "Embedded master firmware did not start its 68000 runtime");
    require(processor.getVoiceBoardCount() == 3,
            "Processor does not expose all three 16-voice cards");
    for (int board = 0; board < processor.getVoiceBoardCount(); ++board)
    {
        const auto& runtime = processor.getVoiceFirmwareRuntime(board);
        require(runtime.isLoaded() && runtime.getBoardIndex() == board
                    && runtime.reachedServiceLoop(),
                "A WDV voice-card runtime did not boot in its physical slot");
    }
    require(processor.getMasterFirmwareRuntime().completedVoiceBoardLoaderHandoff(),
            "Automatic startup did not complete the genuine WDV acknowledgement handoff");
    require(processor.getMasterFirmwareRuntime().lcdVideoWriteCount() > 0,
            "Genuine master firmware did not draw into LCD video memory");
    require(processor.getMasterFirmwareRuntime().installedSyntheticInitialisationRecords(),
            "Automatic startup did not install synthetic INIT.SND and INIT.PFM records");
    require(processor.getMasterFirmwareRuntime().loadedSyntheticInitialisationFiles(),
            "Automatic startup did not route INIT.SND and INIT.PFM through the OS loader");
    require(processor.getMasterFirmwareRuntime().programCounter() < 0xb700
                || processor.getMasterFirmwareRuntime().programCounter() >= 0xbd00,
            "Automatic startup returned to the WDV loader wait loop");
    uint64_t lcdHash = 1469598103934665603ull;
    for (const auto byte : processor.getMasterFirmwareRuntime().lcdVideoSnapshot())
    {
        lcdHash ^= byte;
        lcdHash *= 1099511628211ull;
    }
    require(lcdHash == 0x1e0358e60013a4a3ull,
            "Genuine OS 1.700 LCD framebuffer did not reach the factory performance screen");
    const auto localLong = [&processor](uint32_t address) {
        const auto& runtime = processor.getMasterFirmwareRuntime();
        return (static_cast<uint32_t>(runtime.localByte(address)) << 24u)
               | (static_cast<uint32_t>(runtime.localByte(address + 1u)) << 16u)
               | (static_cast<uint32_t>(runtime.localByte(address + 2u)) << 8u)
               | runtime.localByte(address + 3u);
    };
    require(localLong(0x4de14u) >= wave::firmware::MasterFirmwareRuntime::imageBase
                && localLong(0x4de14u) < 0x00050000u
                && localLong(0x4de18u) >= wave::firmware::MasterFirmwareRuntime::imageBase
                && localLong(0x4de18u) < 0x00050000u,
            "Automatic startup skipped the genuine OS MIDI callback registration");
    require(processor.getWavetableBank().isExternalRomLoaded()
                && processor.getWavetableBank().importedTableCount() == 30,
            "Reconstructed PPG Wave 2.3 V6 bank did not load automatically");
    require(processor.getFactorySetReport().validLayout
                && processor.getFactorySetReport().validSounds == 256
                && processor.getFactorySetReport().validPerformances == 256,
            "Embedded Wave factory SET did not load its two native banks");
    constexpr uint32_t storedSoundBank = 0x18000u;
    constexpr uint32_t soundSize = 0x100u;
    const auto nativeSoundName = [&](uint32_t soundIndex) {
        std::string name;
        for (uint32_t i = 0; i < 16; ++i)
            name.push_back(static_cast<char>(
                processor.getMasterFirmwareRuntime().sharedProgramByte(
                    storedSoundBank + soundIndex * soundSize + 240u + i)));
        return name;
    };
    require(nativeSoundName(0) == "sitar           "
                && nativeSoundName(1) == "DROOPOLYFLANGE  "
                && nativeSoundName(128) == "WoodOrgan    WMF",
            "Firmware Sound browser bank does not contain distinct SET names");
    require(processor.getNumPrograms() == 256
                && processor.getProgramName(0) == "drooSyn 1 oo DN"
                && processor.getProgramName(128) == "WoodOrgan    WMF",
            "Host program list does not expose the native factory performances");
    for (int fader = 0; fader < 8; ++fader)
        require(processor.getPanelFaderValue(fader) == 0.0f,
                "Performance fader did not start at the bottom of its track");
    processor.setPanelFader(0, 32, 0.75f);
    juce::MemoryBlock restoredState;
    processor.getStateInformation(restoredState);
    processor.setStateInformation(restoredState.getData(),
                                  static_cast<int>(restoredState.getSize()));
    require(std::abs(processor.getPanelFaderValue(0) - 0.75f) < 1.0e-5f,
            "Restored state discarded the Control-X performance fader position");
    require((processor.getPerformanceFadersTouchedMask() & 0x01u) != 0u,
            "Restored state discarded the active Control-X fader routing");
    processor.setCurrentProgram(1);
    require((processor.getPerformanceFadersTouchedMask() & 0x01u) != 0u,
            "Changing patch silently disabled the visible Control-X fader");
    processor.setCurrentProgram(0);
    for (int fader = 1; fader < 8; ++fader)
        require(processor.getPanelFaderValue(fader) == 0.0f,
                "Restored state changed an untouched Performance fader");
    processor.setPanelFader(0, 32, 0.0f);
    const auto* factoryPan = processor.parameters.getRawParameterValue(wave::parameters::pan);
    require(factoryPan != nullptr && factoryPan->load() > 0.95f,
            "Factory A001 did not expose its selected right-hand instrument pan");
    const auto sharedName = [&processor](uint32_t offset) {
        juce::String result;
        for (uint32_t index = 0; index < 16; ++index)
            result += static_cast<juce::juce_wchar>(
                processor.getMasterFirmwareRuntime().sharedProgramByte(offset + index));
        return result.trimEnd();
    };
    require(sharedName(0x5300 + 240) == "DROOPOLYFLANGE"
                && sharedName(0x5400 + 32) == "drooSyn 1 oo DN",
            "Factory A001 records did not pass through the genuine INIT file loader");

    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    processor.processBlock(audio, midi);
    requireFinite(audio);
    require(processor.getActiveVoiceCount() == 2,
            "Factory A001 did not allocate both Performance instruments");
    require(audio.getMagnitude(0, 0, audio.getNumSamples()) > 1.0e-3f
                && audio.getMagnitude(1, 0, audio.getNumSamples()) > 1.0e-3f
                && processor.getOutputPeak() > 1.0e-3f,
            "Authenticated startup path produced no playable audio");
    const auto* waveTime1 = processor.parameters.getRawParameterValue(
        wave::parameters::waveEnvelopeTime[0]);
    const auto* waveLevel1 = processor.parameters.getRawParameterValue(
        wave::parameters::waveEnvelopeLevel[0]);
    const auto* waveLoop = processor.parameters.getRawParameterValue(
        wave::parameters::waveEnvelopeLoopEnabled);
    require(waveTime1 != nullptr && waveLevel1 != nullptr && waveLoop != nullptr
                && juce::roundToInt(waveTime1->load()) == 37
                && juce::roundToInt(waveLevel1->load()) == 127
                && waveLoop->load() >= 0.5f,
            "Factory A001 did not load its DROOPOLYFLANGE Wave envelope");
    const auto lcdBeforeFader = processor.getMasterFirmwareRuntime().lcdVideoSnapshot();
    processor.setPanelFader(7, 59, 0.25f);
    for (int block = 0; block < 24; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor.processBlock(audio, noMidi);
    }
    const auto lcdAfterFader = processor.getMasterFirmwareRuntime().lcdVideoSnapshot();
    require(lcdBeforeFader != lcdAfterFader,
            "Performance fader movement did not update the genuine firmware LCD");
    const auto originalFaderValue = processor.getPanelFaderValue(7);
    const auto originalWaveLevel = waveLevel1->load();
    processor.beginPanelFaderGesture(7);
    processor.setPanelFader(7, 59, 0.25f);
    processor.endPanelFaderGesture(7);
    require(std::abs(processor.getPanelFaderValue(7) - 0.25f) < 1.0e-5f
                && std::abs(waveLevel1->load() - originalWaveLevel) < 1.0e-6f,
            "Performance fader 8 is not independent from the Wave-envelope editor");
    processor.setPanelFader(7, 59, originalFaderValue);
    const auto* lfo1Rate = processor.parameters.getRawParameterValue(
        wave::parameters::lfoRate[0]);
    const auto* lfo1Shape = processor.parameters.getRawParameterValue(
        wave::parameters::lfoShape[0]);
    const auto* pitchLfoSource = processor.parameters.getRawParameterValue(
        wave::parameters::modulationSource[wave::parameters::osc1PitchMod2]);
    const auto* pitchLfoAmount = processor.parameters.getRawParameterValue(
        wave::parameters::modulationAmount[wave::parameters::osc1PitchMod2]);
    require(lfo1Rate != nullptr && lfo1Shape != nullptr && pitchLfoSource != nullptr
                && pitchLfoAmount != nullptr && juce::roundToInt(lfo1Rate->load()) == 77
                && juce::roundToInt(lfo1Shape->load()) == 0
                && juce::roundToInt(pitchLfoSource->load()) == 0
                && juce::roundToInt(pitchLfoAmount->load()) == 12,
            "Factory A001 did not load its genuine LFO 1 pitch route");
    auto minimumWavePosition = processor.getFirstActiveWavePosition();
    auto maximumWavePosition = minimumWavePosition;
    auto minimumLfo = processor.getFirstActiveLfoValue(0);
    auto maximumLfo = minimumLfo;
    auto minimumPitchMod = processor.getFirstActivePitchModulation();
    auto maximumPitchMod = minimumPitchMod;
    for (int block = 0; block < 420; ++block)
    {
        audio.clear();
        juce::MidiBuffer heldNote;
        processor.processBlock(audio, heldNote);
        const auto position = processor.getFirstActiveWavePosition();
        minimumWavePosition = juce::jmin(minimumWavePosition, position);
        maximumWavePosition = juce::jmax(maximumWavePosition, position);
        const auto lfo = processor.getFirstActiveLfoValue(0);
        minimumLfo = juce::jmin(minimumLfo, lfo);
        maximumLfo = juce::jmax(maximumLfo, lfo);
        const auto pitchMod = processor.getFirstActivePitchModulation();
        minimumPitchMod = juce::jmin(minimumPitchMod, pitchMod);
        maximumPitchMod = juce::jmax(maximumPitchMod, pitchMod);
    }
    require(maximumWavePosition - minimumWavePosition > 12.0f
                && maximumWavePosition - minimumWavePosition < 17.0f,
            "Factory A001 wavetable travel did not follow the WDV amount curve");
    require(maximumLfo - minimumLfo > 1.8f,
            "Factory A001 LFO 1 did not traverse its bipolar range");
    require(maximumPitchMod - minimumPitchMod > 0.03f
                && maximumPitchMod - minimumPitchMod < 0.055f,
            "Factory A001 LFO pitch depth did not follow the WDV amount curve");

    require(processor.selectPerformanceInstrument(0),
            "Factory A001 first Instrument could not be selected for Control X testing");
    // Let the firmware Sound-record boundary settle before exercising direct
    // host automation. Selection itself must not be mistaken for automation.
    for (int block = 0; block < 192
                        && processor.getMasterFirmwareRuntime()
                               .currentPerformanceInstrument() != 0;
         ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor.processBlock(audio, noMidi);
    }
    require(processor.getMasterFirmwareRuntime().currentPerformanceInstrument() == 0,
            "Programmatic Instrument selection did not reach OS 1.700");
    const auto setPlainValue = [&processor](const char* id, float value) {
        auto* parameter = processor.parameters.getParameter(id);
        require(parameter != nullptr, "A Control X regression parameter is missing");
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    setPlainValue(wave::parameters::position, 31.0f);
    setPlainValue(wave::parameters::scan, 0.0f);
    setPlainValue(
        wave::parameters::modulationSource[wave::parameters::wave1Mod1], 0.0f);
    setPlainValue(
        wave::parameters::modulationControl[wave::parameters::wave1Mod1], 34.0f);
    setPlainValue(
        wave::parameters::modulationAmount[wave::parameters::wave1Mod1], 63.0f);

    juce::MidiBuffer restart;
    restart.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    restart.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 1);
    audio.clear();
    processor.processBlock(audio, restart);
    const auto controlDownPosition = processor.getFirstActiveWavePosition();
    juce::MidiBuffer noControlMidi;
    for (int block = 0; block < 32; ++block)
    {
        audio.clear();
        processor.processBlock(audio, noControlMidi);
        require(std::abs(processor.getFirstActiveWavePosition() - controlDownPosition)
                    < 0.01f,
                "Control-X-gated Mod 1 route moved with its Performance fader down");
    }

    // A001 maps fader 1 to MIDI CC 1 and maps Control X to the same CC. The
    // fader must therefore open the LFO 1 x Control X modulation route.
    processor.setPanelFader(0, 40, 1.0f);
    auto controlUpMinimum = processor.getFirstActiveWavePosition();
    auto controlUpMaximum = controlUpMinimum;
    for (int block = 0; block < 420; ++block)
    {
        audio.clear();
        processor.processBlock(audio, noControlMidi);
        const auto wavePosition = processor.getFirstActiveWavePosition();
        controlUpMinimum = juce::jmin(controlUpMinimum, wavePosition);
        controlUpMaximum = juce::jmax(controlUpMaximum, wavePosition);
    }
    require(controlUpMaximum - controlUpMinimum > 50.0f,
            "Performance Control X did not make the maximum Mod 1 Amount audible");

    const auto lcdRowsHash = [&processor](int firstRow, int lastRow) {
        const auto snapshot = processor.getMasterFirmwareRuntime().lcdVideoSnapshot();
        const auto pageOffset = static_cast<size_t>(
                                    processor.getMasterFirmwareRuntime().lcdDisplayPage() & 0x03u)
                                * 0x1000u;
        uint64_t hash = 1469598103934665603ull;
        for (auto row = firstRow; row <= lastRow; ++row)
            for (size_t column = 0; column < 60u; ++column)
            {
                hash ^= snapshot[pageOffset + static_cast<size_t>(row) * 64u + column];
                hash *= 1099511628211ull;
            }
        return hash;
    };
    const auto oldPerformanceBarHash = lcdRowsHash(24, 43);
    const auto oldLcdWrites = processor.getMasterFirmwareRuntime().lcdVideoWriteCount();

    processor.setCurrentProgram(1);
    require(processor.getCurrentProgram() == 1
                && processor.getProgramName(1) == "DIRTYFUZZZ  DN",
            "Factory performance selection did not update the current program");
    for (int block = 0; block < 32; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor.processBlock(audio, noMidi);
    }
    require(sharedName(0x5300 + 240) == "WAVEFUZZ DN"
                && sharedName(0x5400 + 32) == "DIRTYFUZZZ  DN",
            "The audio-thread program transaction did not update the firmware edit buffers");
    require(processor.getMasterFirmwareRuntime().localByte(0x54b40u) == 0x00u
                && processor.getMasterFirmwareRuntime().localByte(0x54b41u) == 0x01u,
            "Factory selection did not enter the genuine OS program-change path");
    // The firmware's current-screen callback clears and reconstructs the full
    // display. Rows whose final pixels happen to be identical still generate
    // writes, which is precisely what prevents stale cell contents surviving.
    require(lcdRowsHash(24, 43) != oldPerformanceBarHash
                && processor.getMasterFirmwareRuntime().lcdVideoWriteCount()
                       > oldLcdWrites + 1000u,
            "Genuine OS program change did not perform a full current-screen repaint");

    const auto plusButton = wave::panel::matrixIndexForDiagnosticCode(72);
    processor.setPanelButton(plusButton, true);
    for (int block = 0; block < 8; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor.processBlock(audio, none);
    }
    processor.setPanelButton(plusButton, false);
    // Run well beyond OS 1.700's first and subsequent auto-repeat delays. A
    // single physical click must remain one Performance step indefinitely.
    for (int block = 0; block < 768; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor.processBlock(audio, none);
    }
    require(processor.getCurrentProgram() == 2
                && sharedName(0x5400 + 32) == processor.getProgramName(2)
                && processor.getMasterFirmwareRuntime().localByte(0x54b40u) == 0x00u
                && processor.getMasterFirmwareRuntime().localByte(0x54b41u) == 0x02u,
            "Panel increment changed the sound without changing the firmware name record");

    const auto filterEditButton = wave::panel::matrixIndexForDiagnosticCode(60);
    processor.setPanelButton(filterEditButton, true);
    for (int block = 0; block < 8; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor.processBlock(audio, none);
    }
    processor.setPanelButton(filterEditButton, false);
    processor.setPanelButton(plusButton, true);
    for (int block = 0; block < 8; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor.processBlock(audio, none);
    }
    processor.setPanelButton(plusButton, false);
    for (int block = 0; block < 24; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor.processBlock(audio, none);
    }
    require(processor.getCurrentProgram() == 2
                && sharedName(0x5400 + 32) == processor.getProgramName(2),
            "Edit-page plus button incorrectly changed the underlying patch");

    require(processor.selectFactoryPerformance(0, 99)
                && processor.getCurrentProgram() == 98,
            "Numeric performance selection did not select A099");
    processor.selectFactoryBank(1);
    require(processor.getCurrentProgram() == 226,
                "Bank selection did not preserve performance 099");
    require(processor.selectFactoryPerformance(1, 128)
                && processor.getCurrentProgram() == 255,
            "Hundreds performance selection did not select B128");
    processor.stepFactoryPerformance(1);
    require(processor.getCurrentProgram() == 0,
                "Performance increment did not wrap from B128 to A001");
    processor.stepFactoryPerformance(-1);
    require(processor.getCurrentProgram() == 255,
                "Performance decrement did not wrap from A001 to B128");
    require(!processor.selectFactoryPerformance(0, 0)
                && !processor.selectFactoryPerformance(1, 129)
                && processor.getCurrentProgram() == 255,
            "Invalid numeric performance selection was accepted");
    processor.setCurrentProgram(1);
    processor.allSoundOffFromUi();
    audio.clear();
    juce::MidiBuffer flushUiMidi;
    processor.processBlock(audio, flushUiMidi);
    audio.clear();
    juce::MidiBuffer selectedProgramMidi;
    selectedProgramMidi.addEvent(juce::MidiMessage::noteOn(1, 64, 0.9f), 0);
    processor.processBlock(audio, selectedProgramMidi);
    require(audio.getMagnitude(0, 0, audio.getNumSamples()) > 1.0e-3f,
            "Selected factory performance produced no audio");

    // Program-selection events above legitimately schedule an LCD redraw in
    // the genuine OS.  Let that event queue settle before taking the baseline
    // used to detect later framebuffer corruption.
    for (int block = 0; block < 64; ++block)
    {
        audio.clear();
        juce::MidiBuffer settleMidi;
        processor.processBlock(audio, settleMidi);
    }
    const auto stableMasterCycles = processor.getMasterFirmwareRuntime().emulatedCycleCount();
    const auto stableLcdWrites = processor.getMasterFirmwareRuntime().lcdVideoWriteCount();
    uint64_t stableLcdHash = 1469598103934665603ull;
    for (const auto byte : processor.getMasterFirmwareRuntime().lcdVideoSnapshot())
    {
        stableLcdHash ^= byte;
        stableLcdHash *= 1099511628211ull;
    }
    for (int block = 0; block < 250; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor.processBlock(audio, noMidi);
        requireFinite(audio);
    }
    uint64_t advancedLcdHash = 1469598103934665603ull;
    for (const auto byte : processor.getMasterFirmwareRuntime().lcdVideoSnapshot())
    {
        advancedLcdHash ^= byte;
        advancedLcdHash *= 1099511628211ull;
    }
    require(processor.getMasterFirmwareRuntime().emulatedCycleCount() > stableMasterCycles
                && processor.getMasterFirmwareRuntime().programCounter()
                       >= wave::firmware::MasterFirmwareRuntime::imageBase
                && processor.getMasterFirmwareRuntime().programCounter()
                       < wave::firmware::MasterFirmwareRuntime::localRamSize,
            "Host audio clock did not keep the genuine master OS running safely");
    require(processor.getMasterFirmwareRuntime().unmappedReadCount() == 0
                && processor.getMasterFirmwareRuntime().lcdVideoWriteCount() == stableLcdWrites
                && advancedLcdHash == stableLcdHash,
            "Continuous master OS execution corrupted the operational LCD state");

    processor.releaseResources();
    processor.prepareToPlay(96000.0, 512);
    processor.noteOnFromUi(67, 0.8f);
    auto keyboardLeftPeak = 0.0f;
    auto keyboardRightPeak = 0.0f;
    for (int block = 0; block < 4; ++block)
    {
        audio.clear();
        juce::MidiBuffer noHostMidi;
        processor.processBlock(audio, noHostMidi);
        keyboardLeftPeak = juce::jmax(
            keyboardLeftPeak, audio.getMagnitude(0, 0, audio.getNumSamples()));
        keyboardRightPeak = juce::jmax(
            keyboardRightPeak, audio.getMagnitude(1, 0, audio.getNumSamples()));
    }
    require(processor.getActiveVoiceCount() == 2,
            "Computer-keyboard MIDI did not allocate both Performance instruments");
    require(keyboardLeftPeak > 1.0e-3f && keyboardRightPeak > 1.0e-3f,
            "Computer-keyboard MIDI queue did not produce playable audio");
    processor.allSoundOffFromUi();

    auto* output = processor.parameters.getParameter(wave::parameters::output);
    require(output != nullptr, "Output parameter is missing");
    output->setValueNotifyingHost(0.23f);
    processor.parameters.state.setProperty(
        "firmwareDirectory", "/definitely/missing/wave-firmware", nullptr);
    juce::MemoryBlock savedState;
    processor.getStateInformation(savedState);

    auto restoredStorage = std::make_unique<WaveEmulationAudioProcessor>();
    auto& restored = *restoredStorage;
    restored.setStateInformation(savedState.getData(),
                                 static_cast<int>(savedState.getSize()));
    require(restored.getFirmwareReport().hasBothImages()
                && restored.getWavetableBank().isExternalRomLoaded(),
            "Session recall lost the automatic firmware or PPG ROM fallback");
    require(restored.getCurrentProgram() == 1
                && restored.getProgramName(1) == "DIRTYFUZZZ  DN",
            "Session recall lost the selected factory performance");
    const auto* restoredOutput = restored.parameters.getParameter(wave::parameters::output);
    require(restoredOutput != nullptr
                && std::abs(restoredOutput->getValue() - 0.23f) < 1.0e-6f,
            "Session recall did not restore synth parameters");
}

void testFactoryA092ChoirVibratoUsesCallerScale()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(91); // A092: Choir 2.
    require(processor->getProgramName(91).startsWith("Choir 2"),
            "A092 factory fixture is not Choir 2");

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noMidi;
    for (int block = 0; block < 96; ++block)
    {
        audio.clear();
        processor->processBlock(audio, noMidi);
    }

    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    audio.clear();
    processor->processBlock(audio, noteOn);

    std::array<float, 2> minimum {
        processor->getFirstActivePitchModulation(0),
        processor->getFirstActivePitchModulation(1)
    };
    auto maximum = minimum;
    for (int block = 0; block < 600; ++block)
    {
        audio.clear();
        processor->processBlock(audio, noMidi);
        for (int oscillator = 0; oscillator < 2; ++oscillator)
        {
            const auto value
                = processor->getFirstActivePitchModulation(oscillator);
            minimum[static_cast<size_t>(oscillator)] = juce::jmin(
                minimum[static_cast<size_t>(oscillator)], value);
            maximum[static_cast<size_t>(oscillator)] = juce::jmax(
                maximum[static_cast<size_t>(oscillator)], value);
        }
    }

    const auto oscillator1Range = maximum[0] - minimum[0];
    const auto oscillator2Range = maximum[1] - minimum[1];
    require(oscillator1Range > 0.35f && oscillator1Range < 0.55f
                && oscillator2Range > 0.42f && oscillator2Range < 0.65f,
            "A092 omitted the WDV oscillator caller's final pitch-depth scaling");
}

void testFactoryB057UsesMeasuredVcaAttackOne()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(184); // B057: Sarahvoice.
    require(processor->getProgramName(184).startsWithIgnoreCase("Sarahvoice"),
            "B057 factory fixture is not Sarahvoice");

    const auto* attack
        = processor->parameters.getRawParameterValue(wave::parameters::attack);
    const auto expected
        = wave::dsp::WdvEnvelope::amplifierAttackTimeConstantForRate(1);
    require(attack != nullptr && std::abs(attack->load() - expected) < 1.0e-5f,
            "B057 did not decode its native VCA attack-one setting");

    juce::AudioBuffer<float> sampleBuffer(2, 1);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    processor->processBlock(sampleBuffer, noteOn);

    auto tenPercentSample = -1;
    auto ninetyPercentSample = -1;
    for (auto sample = 0; sample < 1024 && ninetyPercentSample < 0; ++sample)
    {
        sampleBuffer.clear();
        juce::MidiBuffer noMidi;
        processor->processBlock(sampleBuffer, noMidi);
        for (const auto& voice : processor->getVoiceStates())
        {
            if (!voice.active || voice.triggerNote != 60)
                continue;
            const auto vcaGain = voice.vcaControl * voice.vcaControl
                                 * (3.0f - 2.0f * voice.vcaControl);
            if (tenPercentSample < 0 && vcaGain >= 0.1f)
                tenPercentSample = sample;
            if (vcaGain >= 0.9f)
                ninetyPercentSample = sample;
            break;
        }
    }

    const auto riseMilliseconds
        = static_cast<float>(ninetyPercentSample - tenPercentSample)
          * 1000.0f / 48000.0f;
    require(tenPercentSample >= 0 && ninetyPercentSample >= tenPercentSample
                && std::abs(riseMilliseconds - 5.0f) < 0.3f,
            "B057 VCA attack does not reach the measured hardware rise time");
}

void testA030UsesItsSetUserWavetable()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->setCurrentProgram(29); // A030, Sitar.

    const auto* wavetable
        = processor->parameters.getRawParameterValue(wave::parameters::wavetable);
    require(wavetable != nullptr && juce::roundToInt(wavetable->load()) == 96,
            "Factory A030 was clamped away from its TSITAR3 user Wavetable");
    require(processor->getWavetableBank().hasWaveSetUserTables(),
            "Factory SET user Wavetables are absent from the audio engine");

    const auto& bank = processor->getWavetableBank();
    auto differsFromFormerClamp = false;
    for (int wave = 0; wave < wave::dsp::WavetableBank::wavesPerTable; ++wave)
        for (int sample = 0; sample < wave::dsp::WavetableBank::samplesPerWave; ++sample)
            if (bank.rawSample(95, wave, sample) != bank.rawSample(63, wave, sample))
            {
                differsFromFormerClamp = true;
                break;
            }
    require(differsFromFormerClamp,
            "A030 still resolves to the former table-64 clamp instead of TSITAR3");
}

void testA044UsesNineteenTwentySpeechTable()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->setCurrentProgram(43); // A044, NINETEENTWENTY.

    const auto* wavetable
        = processor->parameters.getRawParameterValue(wave::parameters::wavetable);
    require(wavetable != nullptr && juce::roundToInt(wavetable->load()) == 52,
            "Factory A044 does not select Wave table 52 (19/20)");

    const auto& bank = processor->getWavetableBank();
    auto differsFromProceduralNeighbour = false;
    for (int wave = 0; wave < 61 && !differsFromProceduralNeighbour; ++wave)
        for (int sample = 0; sample < 64; ++sample)
            if (bank.rawSample(51, wave, sample) != bank.rawSample(52, wave, sample))
            {
                differsFromProceduralNeighbour = true;
                break;
            }
    require(differsFromProceduralNeighbour,
            "A044 still resolves to a generic procedural wavetable");
}

void testFactoryPerformanceLayering()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(41); // A042 -> three distinct full-range Sounds.

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    processor->processBlock(audio, midi);
    requireFinite(audio);
    require(processor->getActiveVoiceCount() == 3,
            "Factory A042 did not allocate all three active Performance instruments");

    auto leftPeak = audio.getMagnitude(0, 0, audio.getNumSamples());
    auto rightPeak = audio.getMagnitude(1, 0, audio.getNumSamples());
    for (int block = 0; block < 8; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor->processBlock(audio, noMidi);
        requireFinite(audio);
        leftPeak = juce::jmax(leftPeak,
                              audio.getMagnitude(0, 0, audio.getNumSamples()));
        rightPeak = juce::jmax(rightPeak,
                               audio.getMagnitude(1, 0, audio.getNumSamples()));
    }
    require(leftPeak > 1.0e-4f && rightPeak > 1.0e-4f,
            "Factory Performance layer panning did not reach both output channels");
}

void testRapidPerformanceLcdRefreshIsAtomic()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto settle = [&] (int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };

    processor->setCurrentProgram(11); // A012
    settle(64);
    const auto referenceSelected
        = (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(0x54b40u)) << 8)
          | processor->getMasterFirmwareRuntime().localByte(0x54b41u);
    const auto referencePage = processor->getMasterFirmwareRuntime().lcdDisplayPage();

    // Interrupt two in-flight OS program changes with a third. The final
    // display must be byte-identical to a clean draw of the same Performance,
    // with no instrument cells retained from either intermediate record.
    processor->setCurrentProgram(12);
    settle(1);
    processor->setCurrentProgram(13);
    settle(1);
    processor->setCurrentProgram(11);
    settle(64);
    const auto finalPage = processor->getMasterFirmwareRuntime().lcdDisplayPage();
    const auto finalFrame = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    const auto finalSelected
        = (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(0x54b40u)) << 8)
          | processor->getMasterFirmwareRuntime().localByte(0x54b41u);
    const auto finalOffset = static_cast<size_t>(finalPage & 0x03u) * 0x1000u;
    require(referenceSelected == 11 && finalSelected == 11
                && referencePage < 4 && finalPage < 4
                && std::any_of(finalFrame.begin() + static_cast<std::ptrdiff_t>(finalOffset),
                               finalFrame.begin() + static_cast<std::ptrdiff_t>(finalOffset + 0x1000u),
                               [](uint8_t byte) { return byte != 0; }),
            "Rapid Performance selection did not settle on the requested firmware frame");
}

void testFirmwarePerformanceStepButtonsRefreshLcd()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };

    processBlocks(64);
    auto before = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    const auto plus = wave::panel::matrixIndexForDiagnosticCode(72);
    for (int expected = 1; expected <= 4; ++expected)
    {
        processor->setPanelButton(plus, true);
        processBlocks(8);
        processor->setPanelButton(plus, false);
        processBlocks(64);

        require(processor->getCurrentProgram() == expected
                    && processor->getMasterFirmwareRuntime().localByte(0x54b40u)
                           == 0x00u
                    && processor->getMasterFirmwareRuntime().localByte(0x54b41u)
                           == static_cast<uint8_t>(expected),
                "Repeated physical Plus stopped selecting firmware Performances");
        const auto after
            = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        require(after != before,
                "Repeated physical Plus stopped refreshing the firmware LCD");
        before = after;
    }
}

void testStoreButtonReachesFirmwareMenu()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };

    processBlocks(48);
    const auto before = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    const auto programBefore = processor->getCurrentProgram();
    const std::array octavesBefore {
        processor->getFirmwareOscillatorOctave(0),
        processor->getFirmwareOscillatorOctave(1)
    };

    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(57), true); // Store.
    processBlocks(8);
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(57), false);
    processBlocks(24);

    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() != before,
            "Physical Store did not open its genuine firmware menu");
    require(processor->getPanelSelectedMode() == 57
                && processor->getPanelLed(87)
                && !processor->getPanelLed(51),
            "Store page LED went out when its momentary button was released");
    require(processor->getCurrentProgram() == programBefore
                && std::array {
                       processor->getFirmwareOscillatorOctave(0),
                       processor->getFirmwareOscillatorOctave(1)
                   } == octavesBefore,
            "Store was misrouted into a sound or octave control");
}

void testStoreRequesterStepButtonsChooseDestination()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };
    const auto click = [&](int diagnosticCode, int settleBlocks = 48)
    {
        const auto button
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        require(processor->setPanelButton(button, true),
                "Firmware rejected a required Store-menu button");
        processBlocks(8);
        processor->setPanelButton(button, false);
        processBlocks(settleBlocks);
    };

    processBlocks(64);
    click(33); // Leave the Performance instrument-button context.
    click(57); // Store.
    click(25); // Perf. display softkey.
    require(processor->isFirmwareRequesterActive(),
            "Store Performance did not open its genuine firmware requester");
    const auto programBefore = processor->getCurrentProgram();
    const auto requesterBefore
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    const auto destinationBefore
        = (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
               0x4b1b2u))
           << 8)
          | processor->getMasterFirmwareRuntime().localByte(0x4b1b3u);
    click(72); // Next destination slot (startup is already at A001).
    require(processor->isFirmwareRequesterActive(),
            "Store destination stepping unexpectedly closed the requester");
    require(processor->getCurrentProgram() == programBefore,
            "Store destination stepping changed the playing Performance");
    const auto destinationAfter
        = (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
               0x4b1b2u))
           << 8)
          | processor->getMasterFirmwareRuntime().localByte(0x4b1b3u);
    require(destinationAfter == destinationBefore + 1,
            "Store Plus did not advance the firmware destination location");
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot()
                != requesterBefore,
            "Store Plus did not update the firmware destination field");

    click(69); // Previous destination slot.
    const auto destinationRestored
        = (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
               0x4b1b2u))
           << 8)
          | processor->getMasterFirmwareRuntime().localByte(0x4b1b3u);
    require(processor->isFirmwareRequesterActive()
                && destinationRestored == destinationBefore
                && processor->getCurrentProgram() == programBefore,
            "Store Minus did not restore the preceding firmware destination");
    require(processor->getPanelLed(87),
            "Store LED did not remain lit throughout its requester");
    const auto& runtime = processor->getMasterFirmwareRuntime();
    const auto cursorBefore = runtime.localByte(0x57005u);
    click(23); // The naming dialog accepts Page Right.
    require(runtime.localByte(0x57005u) == cursorBefore + 1,
            "Performance name Page Right did not advance the firmware cursor");
    click(21); // And Page Left, while retaining the Store requester.
    require(runtime.localByte(0x57005u) == cursorBefore,
            "Performance name Page Left did not restore the firmware cursor");
    require(processor->isFirmwareRequesterActive(),
            "Page cursor keys closed the Store requester");
    click(71); // Cancel / ESC leaves Store.
    click(33);
    click(57);
    click(25);
    require(processor->isFirmwareRequesterActive(),
            "A second Store requester did not open");
    const auto reopenedCursor = runtime.localByte(0x57005u);
    click(23);
    require(processor->isFirmwareRequesterActive(),
            "Page Right closed the second Store requester");
    require(runtime.localByte(0x57005u) == reopenedCursor + 1,
            "Page Right did not move the cursor in the reopened Store requester");
    click(71); // Leave the second requester without writing again.
    require(processor->getPanelSelectedMode() == 39
                && processor->getPanelLed(51)
                && !processor->getPanelLed(87),
            "Store LED remained lit after leaving the Store page");
}

void testRepeatedSoundStoreCursor()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int count) {
        for (int block = 0; block < count; ++block) {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int code) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(code);
        require(processor->setPanelButton(button, true), "Sound Store rejected a button");
        process(8);
        processor->setPanelButton(button, false);
        process(48);
    };
    process(64);
    click(33);
    click(57);
    click(79); // Sound opens the Instrument chooser.
    for (int pass = 0; pass < 3; ++pass) {
        click(pass % 2 == 0 ? 22 : 25);
        require(processor->isFirmwareRequesterActive(), "Sound Store requester did not open");
        const auto& runtime = processor->getMasterFirmwareRuntime();
        const auto cursor = runtime.localByte(0x57005u);
        click(23);
        require(runtime.localByte(0x57005u) == cursor + 1,
                "Sound name Page Right did not advance the actual firmware cursor");
        click(21);
        require(runtime.localByte(0x57005u) == cursor,
                "Sound name Page Left did not restore the actual firmware cursor");
        const auto character = runtime.localByte(0x56000u + cursor);
        for (int step = 0; step < 8 && runtime.localByte(0x56000u + cursor) == character; ++step)
        {
            processor->turnPanelEncoder(8, 1);
            process(48);
        }
        require(runtime.localByte(0x56000u + cursor) != character,
                "Data dial did not edit the selected Sound name character");
        click(70);
        require(!processor->isFirmwareRequesterActive(),
                "One OK press did not close the Sound naming requester");
        require(processor->getPanelSelectedMode() == 57 && processor->getPanelLed(87),
                "Sound Store OK retired Store while the firmware still owns the chooser");
    }
    click(71);
    require(processor->getPanelSelectedMode() == 39 && !processor->getPanelLed(87),
            "Cancel did not leave the Sound Store chooser");
}

void testPerformanceBrowserHasDistinctStoredRecords()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    process(64);
    const auto& firmware = processor->getMasterFirmwareRuntime();
    const auto verifyNames = [&]() {
        for (int program = 0; program < 256; ++program)
        {
            const auto offset = 0x28000u + static_cast<uint32_t>(program) * 512u;
            require(firmware.sharedProgramByte(offset + 48u) == 0x55u,
                    "Firmware browser Performance bank contains an uninitialised record");
            juce::String name;
            for (uint32_t character = 0; character < 16; ++character)
                name += juce::String::charToString(static_cast<juce::juce_wchar>(
                    firmware.sharedProgramByte(offset + 32u + character)));
            require(name.trim() == processor->getProgramName(program).trim(),
                    "Firmware browser Performance name differs from its stored program");
        }
    };
    verifyNames();
    for (int pass = 0; pass < 3; ++pass)
        for (const int code : {36, 39})
        {
            const auto matrix = wave::panel::matrixIndexForDiagnosticCode(code);
            require(processor->setPanelButton(matrix, true), "Mode switch rejected");
            process(8);
            processor->setPanelButton(matrix, false);
            process(96);
            verifyNames();
        }
}

void testStoreCancelRestoresNumericPerformancePreview()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int diagnosticCode, int settleBlocks = 64) {
        const auto matrix
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        require(processor->setPanelButton(matrix, true),
                "Firmware rejected a numeric-preview regression contact");
        process(8);
        processor->setPanelButton(matrix, false);
        process(settleBlocks);
    };

    process(64);
    click(47); // '?' opens the numerical Performance-entry preview.
    const auto cleanPreview
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    click(71); // Leave the preview.

    click(33); // Global Edit permits entry to Store in the tested OS flow.
    click(57); // Store.
    click(25); // Perf. display softkey opens Store Performance requester.
    require(processor->isFirmwareRequesterActive(),
            "Store Performance requester did not open for preview regression");
    click(71); // Cancel Store.
    require(processor->getPanelSelectedMode() == 39,
            "Store Cancel did not return the engine to Performance");

    click(47); // Open the same numerical entry preview again.
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot()
                == cleanPreview,
            "Store Cancel left every numerical preview slot on one stale name");
}

void testKeyboardControllerShiftReachesFirmware()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noMidi;
    for (int block = 0; block < 48; ++block)
    {
        audio.clear();
        processor->processBlock(audio, noMidi);
    }
    processor->setKeyboardControllerButton(0x52u, true);
    const auto& runtime = processor->getMasterFirmwareRuntime();
    require(runtime.localByte(0x55cf4u) == 0xffu
                && runtime.localByte(0x55d00u) == 0x52u,
            "Keyboard-controller Shift did not enter the genuine firmware input ring");
    for (int block = 0; block < 8; ++block)
    {
        audio.clear();
        processor->processBlock(audio, noMidi);
    }
    const auto consumedPointer
        = (static_cast<uint32_t>(runtime.localByte(0x55ce4u)) << 24u)
          | (static_cast<uint32_t>(runtime.localByte(0x55ce5u)) << 16u)
          | (static_cast<uint32_t>(runtime.localByte(0x55ce6u)) << 8u)
          | runtime.localByte(0x55ce7u);
    require(consumedPointer > 0x55d00u && consumedPointer < 0x55e00u,
            "OS 1.700 did not consume the keyboard-controller Shift command");
    processor->setKeyboardControllerButton(0x52u, false);
}

void testKeyboardOctaveButtonsDriveLocalKeyboardRange()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks, juce::MidiBuffer* supplied = nullptr) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, supplied != nullptr ? *supplied : none);
            supplied = nullptr;
        }
    };
    const auto click = [&](int diagnosticCode) {
        const auto matrix
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        require(processor->setPanelButton(matrix, true),
                "Engine rejected a keyboard Octave contact");
        process(8);
        processor->setPanelButton(matrix, false);
        process(64);
    };

    process(64);
    click(12); // Octave Up.
    require(processor->getKeyboardOctaveShift() == 1
                && processor->getMasterFirmwareRuntime().panelLed(58)
                && processor->getPanelLed(58),
            "Octave Up did not follow firmware LED/output serial 58");

    juce::MemoryBlock savedState;
    processor->getStateInformation(savedState);
    auto restored = std::make_unique<WaveEmulationAudioProcessor>();
    restored->setStateInformation(savedState.getData(),
                                  static_cast<int>(savedState.getSize()));
    restored->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> restoredAudio(2, 512);
    for (int block = 0; block < 128; ++block)
    {
        restoredAudio.clear();
        juce::MidiBuffer none;
        restored->processBlock(restoredAudio, none);
    }
    require(restored->getKeyboardOctaveShift() == 1
                && restored->getMasterFirmwareRuntime().panelLed(58)
                && restored->getPanelLed(58),
            "Host state did not restore the firmware keyboard octave position");

    processor->noteOnFromUi(60, 0.9f);
    process(4);
    require(processor->getFirstActiveMidiNote() == 72,
            "Wave local keyboard was not shifted one octave up");

    click(73); // Opposite direction returns the keyboard to its centre range.
    require(processor->getKeyboardOctaveShift() == 0
                && !processor->getPanelLed(58)
                && !processor->getPanelLed(27),
            "Opposite Octave button did not restore the centre range");
    processor->noteOffFromUi(60);
    process(4);
    require(processor->getHeldVoiceCount() == 0,
            "Changing octave while holding a key stranded its note-off");

    processor->releaseResources();
    processor->prepareToPlay(48000.0, 512);
    click(73); // Octave Down.
    require(processor->getKeyboardOctaveShift() == -1
                && processor->getMasterFirmwareRuntime().panelLed(27)
                && processor->getPanelLed(27),
            "Octave Down did not follow firmware LED/output serial 27");

    processor->setMidiInputActsAsLocalKeyboard(true);
    juce::MidiBuffer localMidi;
    localMidi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    process(1, &localMidi);
    require(processor->getFirstActiveMidiNote() == 48,
            "MIDI configured as the Wave local keyboard ignored Octave Down");

    processor->releaseResources();
    processor->prepareToPlay(48000.0, 512);
    processor->setMidiInputActsAsLocalKeyboard(false);
    juce::MidiBuffer externalMidi;
    externalMidi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    process(1, &externalMidi);
    require(processor->getFirstActiveMidiNote() == 60,
            "Keyboard Octave buttons incorrectly transposed external MIDI");
}

void testLowerKeyboardAssignableButtonsDriveFirmware()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    process(96);
    for (const auto [button, led] : std::array<std::pair<int, int>, 2> {
             std::pair { 3, 16 }, std::pair { 4, 35 } })
    {
        const auto before = processor->getPanelLed(led);
        processor->setPanelButton(button, true);
        process(48);
        processor->setPanelButton(button, false);
        process(48);
        require(processor->getPanelLed(led) != before,
                "A lower assignable button did not reach its firmware mode/LED output");
    }
}

void testGlideEditUsesFirmwareLamp()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    process(160);
    const auto before = processor->getPanelLed(75);
    processor->setPanelButton(11, true);
    process(16);
    processor->setPanelButton(11, false);
    process(160);
    require(processor->getMasterFirmwareRuntime().panelLed(75) != before
                && processor->getPanelLed(75) != before,
            "Glide Edit did not drive its genuine firmware lamp output");
}

void testPluginMidiFollowsKeyboardOctaveButtons()
{
    juce::AudioProcessor::setTypeOfNextNewPlugin(
        juce::AudioProcessor::wrapperType_VST3);
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    juce::AudioProcessor::setTypeOfNextNewPlugin(
        juce::AudioProcessor::wrapperType_Undefined);
    processor->prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> audio(2, 128);
    const auto process = [&](int blocks, juce::MidiBuffer* supplied = nullptr) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, supplied != nullptr ? *supplied : none);
            supplied = nullptr;
        }
    };

    process(128);
    const auto octaveUp
        = wave::panel::matrixIndexForDiagnosticCode(12);
    require(processor->setPanelButton(octaveUp, true),
            "Plugin wrapper rejected Octave Up");
    processor->setPanelButton(octaveUp, false); // Tap between host callbacks.
    process(128);
    require(processor->getKeyboardOctaveShift() == 1,
            "A brief plugin Octave Up tap did not reach the keyboard firmware");

    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    process(1, &noteOn);
    require(processor->getFirstActiveMidiNote() == 72,
            "Plugin MIDI bypassed the Wave keyboard Octave Up state");

    const auto octaveDown
        = wave::panel::matrixIndexForDiagnosticCode(73);
    require(processor->setPanelButton(octaveDown, true),
            "Plugin wrapper rejected Octave Down");
    processor->setPanelButton(octaveDown, false);
    process(128);
    require(processor->getKeyboardOctaveShift() == 0,
            "Plugin Octave Down did not return the keybed to its centre range");

    juce::MidiBuffer noteOff;
    noteOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    process(8, &noteOff);
    require(processor->getHeldVoiceCount() == 0,
            "Changing plugin octave while holding a note stranded its note-off");
}

void testShiftDisplaySevenOpensFirmwareServiceMenu()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noMidi;
    const auto processBlocks = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            processor->processBlock(audio, noMidi);
        }
    };
    processBlocks(48);
    processor->setPanelButton(57, true); // Store.
    processBlocks(8);
    processor->setPanelButton(57, false);
    processBlocks(24);
    const auto storeScreen = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();

    processor->setKeyboardControllerButton(0x52u, true);
    processBlocks(8);
    require(processor->getMasterFirmwareRuntime().localByte(0x59869u) != 0,
            "Firmware Shift virtual input did not assert the Store modifier");
    processor->setPanelButton(29, true); // Seventh button above the display.
    processBlocks(8);
    processor->setPanelButton(29, false);
    processBlocks(32);
    processor->setKeyboardControllerButton(0x52u, false);
    processBlocks(8);
    const auto readPointer = [&processor](uint32_t address) {
        const auto& runtime = processor->getMasterFirmwareRuntime();
        return (static_cast<uint32_t>(runtime.localByte(address)) << 24u)
               | (static_cast<uint32_t>(runtime.localByte(address + 1u)) << 16u)
               | (static_cast<uint32_t>(runtime.localByte(address + 2u)) << 8u)
               | runtime.localByte(address + 3u);
    };
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() != storeScreen,
            "Shift plus display button 7 did not open the firmware service menu");
    require(readPointer(0x5673cu) == 0x0001a492u,
            "Shift plus display button 7 did not install Wave Maintenance");
    require(processor->getMasterFirmwareRuntime().localByte(0x59869u) == 0,
            "Releasing keyboard-controller Shift left the firmware modifier held");
}

void testFirmwareVcfCalibrationTableFeedsAllVoices()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    const auto& runtime = processor->getMasterFirmwareRuntime();
    std::array<uint16_t, wave::dsp::WaldorfEngine::voiceCount> initialCodes{};
    for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
    {
        const auto code = runtime.filterCalibrationCode(voice);
        initialCodes[static_cast<size_t>(voice)] = code;
        require(code <= 0x0fffu,
                "A firmware VCF calibration word is not 12-bit");
    }
    require(initialCodes
                == wave::dsp::WaldorfEngine::installedFilterCalibrationCodes,
            "The installed three-card VCF trim table was not seeded into firmware");

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noMidi;
    for (int block = 0; block < 64; ++block)
    {
        audio.clear();
        processor->processBlock(audio, noMidi);
    }
    for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
        require(runtime.filterCalibrationCode(voice)
                    == initialCodes[static_cast<size_t>(voice)],
                "Firmware execution destabilised a seeded VCF trim word");

    // The OS uses the surrounding shared-work SRAM for unrelated operations.
    // Reproduce a transient overwrite after the recognised disk phase has
    // ended and verify that it cannot become hardware calibration state.
    auto& writableRuntime
        = const_cast<wave::firmware::MasterFirmwareRuntime&>(runtime);
    for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
        writableRuntime.setFilterCalibrationCode(
            voice, static_cast<uint16_t>(
                       (static_cast<unsigned int>(voice) * 173u + 0x0123u)
                       & 0x0fffu));
    audio.clear();
    processor->processBlock(audio, noMidi);
    const auto voiceStates = processor->getVoiceStates();
    for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
    {
        const auto expected
            = wave::dsp::WaldorfEngine::installedFilterCalibrationCodes[
                static_cast<size_t>(voice)];
        require(voiceStates[static_cast<size_t>(voice)].cutoffTrim == expected,
                "Shared-work scratch data escaped into a voice-card VCF trim");
    }

}

void testFactoryA001VoiceCardsTrackTheSameFilter()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->selectFactoryPerformance(0, 1);
    const auto sound = wave::parameters::readSnapshot(processor->parameters);
    const auto& mod1 = sound.modulationRoutes[wave::parameters::filterMod1];
    const auto& mod2 = sound.modulationRoutes[wave::parameters::filterMod2];
    require(mod1.amount == 0.0f && mod2.amount == 0.0f,
            "A001 unexpectedly has an active direct filter modulation route");
    std::array<std::array<double, wave::dsp::WaldorfEngine::voiceBoardCount>, 2>
        cardCutoff{};
    auto minimumMeanCutoff = std::numeric_limits<double>::max();
    auto maximumMeanCutoff = 0.0;
    for (int layer = 0; layer < 2; ++layer)
    {
        for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
        {
            const auto probe = processor->probeCurrentLayerVoice(
                voice, layer, 60, 0.8f, 24000, 1);
            require(probe.maximumCutoffHz > 0.0f,
                    "A001 is missing one of its two enabled stereo layers");
            const auto card = static_cast<size_t>(
                voice / wave::dsp::WaldorfEngine::voicesPerBoard);
            cardCutoff[static_cast<size_t>(layer)][card] += probe.meanCutoffHz;
            minimumMeanCutoff = std::min(minimumMeanCutoff, probe.meanCutoffHz);
            maximumMeanCutoff = std::max(maximumMeanCutoff, probe.meanCutoffHz);
        }
    }
    for (size_t layer = 0; layer < cardCutoff.size(); ++layer)
    {
        for (size_t card = 0; card < cardCutoff[layer].size(); ++card)
            cardCutoff[layer][card] /= wave::dsp::WaldorfEngine::voicesPerBoard;
    }
    require(maximumMeanCutoff / minimumMeanCutoff < 1.04,
            "A001 exceeds the installed per-voice VCF trim spread");
    for (size_t layer = 0; layer < cardCutoff.size(); ++layer)
        require(*std::max_element(cardCutoff[layer].begin(), cardCutoff[layer].end())
                    / *std::min_element(cardCutoff[layer].begin(), cardCutoff[layer].end())
                    < 1.02,
                "A001 has an excessive mean VCF offset between voice cards");
}

void testFactoryA001HeldCutoffUsesFirmwareEnvelopeScale()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(96000.0, 256);
    processor->selectFactoryPerformance(0, 1);
    const auto sound = wave::parameters::readSnapshot(processor->parameters);
    const auto probe = processor->probeCurrentLayerVoice(
        18, 0, 69, 0.8f, 192000, 19);
    const auto combinedDepth = juce::jlimit(
        -64.0f, 32767.0f / 512.0f,
        sound.filterEnvelopeSemitones
            + sound.filterVelocitySemitones * 0.8f);
    const auto expectedStep
        = wave::parameters::cutoffStepForFrequency(sound.cutoffHz)
          + 2.0f * combinedDepth * sound.filterSustainLevel;
    const auto actualStep
        = wave::parameters::cutoffStepForFrequency(probe.finalCutoffHz);
    require(std::abs(actualStep - expectedStep) < 0.1f,
            "A001 held cutoff did not use the WDV's linear envelope-depth arithmetic");
}

void testFactoryA001OverlappingChordNoteOffKeepsRetriggeredVoices()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->selectFactoryPerformance(0, 1);

    const auto playChord = [&processor](std::array<int, 3> previous,
                                        std::array<int, 3> next,
                                        bool hasPrevious) {
        juce::AudioBuffer<float> audio(2, 512);
        audio.clear();
        juce::MidiBuffer transition;
        for (size_t index = 0; index < next.size(); ++index)
            transition.addEvent(
                juce::MidiMessage::noteOn(1, next[index], 100.0f / 127.0f),
                static_cast<int>(index));
        if (hasPrevious)
            for (size_t index = 0; index < previous.size(); ++index)
                transition.addEvent(
                    juce::MidiMessage::noteOff(1, previous[index]),
                    static_cast<int>(next.size() + index));
        processor->processBlock(audio, transition);
        require(processor->getHeldVoiceCount() == 6,
                "A001 released retriggered notes shared by successive chords");
    };

    constexpr std::array first { 60, 64, 67 };
    constexpr std::array second { 59, 64, 67 };
    constexpr std::array third { 58, 64, 67 };
    playChord({}, first, false);
    playChord(first, second, true);
    playChord(second, third, true);

    juce::AudioBuffer<float> heldAudio(2, 24000);
    heldAudio.clear();
    juce::MidiBuffer noMidi;
    processor->processBlock(heldAudio, noMidi);
    require(processor->getHeldVoiceCount() == 6,
            "A001 lost voices while the third chord remained held");
}

void testFactoryA001CapturedChordSequenceKeepsEveryVcaOpen()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->setMidiInputActsAsLocalKeyboard(true);
    processor->prepareToPlay(96000.0, 256);
    processor->selectFactoryPerformance(0, 1);

    constexpr std::array<std::array<std::pair<int, int>, 3>, 4> chords {{
        {{{67, 42}, {72, 62}, {76, 50}}},
        {{{71, 41}, {65, 49}, {74, 54}}},
        {{{69, 54}, {64, 58}, {72, 59}}},
        {{{67, 76}, {71, 66}, {62, 72}}}
    }};
    juce::AudioBuffer<float> audio(2, 256);
    const auto process = [&](juce::MidiBuffer midi) {
        audio.clear();
        processor->processBlock(audio, midi);
    };

    for (size_t chordIndex = 0; chordIndex < chords.size(); ++chordIndex)
    {
        juce::MidiBuffer on;
        for (const auto [note, velocity] : chords[chordIndex])
            on.addEvent(juce::MidiMessage::noteOn(
                            1, note, static_cast<float>(velocity) / 127.0f), 0);
        process(on);
        for (int block = 0; block < (chordIndex == 3 ? 1500 : 78); ++block)
        {
            juce::MidiBuffer pressure;
            if (chordIndex == 3 && block < 12)
                pressure.addEvent(juce::MidiMessage::channelPressureChange(
                                      1, juce::jmin(11, block + 1)), 0);
            process(pressure);
        }

        if (chordIndex != 3)
        {
            juce::MidiBuffer off;
            for (const auto [note, velocity] : chords[chordIndex])
            {
                juce::ignoreUnused(velocity);
                off.addEvent(juce::MidiMessage::noteOff(1, note), 0);
            }
            process(off);
            for (int block = 0; block < 78; ++block)
                process({});
        }
    }

    const auto states = processor->getVoiceStates();
    auto minimumVca = 1.0f;
    auto maximumVca = 0.0f;
    auto held = 0;
    for (const auto& state : states)
    {
        if (!state.active || !state.keyDown)
            continue;
        ++held;
        minimumVca = juce::jmin(minimumVca, state.vcaControl);
        maximumVca = juce::jmax(maximumVca, state.vcaControl);
    }
    require(held == 6, "Captured A001 sequence did not retain six held layered voices");
    require(minimumVca > 0.1f && minimumVca / maximumVca > 0.5f,
            "Captured A001 sequence collapsed five held voice VCAs");
}

void testDiskSetSerialSelectionUpdatesDsp(bool checkColdStart = true)
{
    constexpr size_t soundBankOffset = 0x12e7cu;
    constexpr size_t performanceBankOffset = 0x22e7cu;
    constexpr size_t soundSize = 256u;
    constexpr size_t performanceSize = 512u;
    const auto configuredSet = juce::SystemStats::getEnvironmentVariable(
        "WAVE_FACTORY_SET", {});
    const auto factorySetFile
        = configuredSet.isNotEmpty()
              ? juce::File(configuredSet)
              : juce::File::getCurrentWorkingDirectory().getChildFile("wave.set");
    juce::MemoryBlock setup;
    require(factorySetFile.loadFileAsData(setup),
            "Could not load the source SET for disk/DSP regression");
    auto* bytes = static_cast<uint8_t*>(setup.getData());
    const auto performanceOffset = performanceBankOffset + performanceSize; // A002.
    const auto* performance = bytes + performanceOffset;
    auto instrument = juce::jlimit(0, 7, static_cast<int>(performance[24]));
    const auto isActive = [performance](int candidate)
    {
        const auto offset = static_cast<size_t>(64 + candidate * 32);
        return candidate >= 0 && candidate < 8 && performance[offset + 3] != 0
               && performance[offset + 13] == 0;
    };
    if (!isActive(instrument))
        for (instrument = 0; instrument < 8 && !isActive(instrument); ++instrument)
        {
        }
    require(instrument < 8, "Test SET A002 contains no active Instrument");
    const auto instrumentOffset = static_cast<size_t>(64 + instrument * 32);
    const auto soundIndex = static_cast<size_t>(performance[instrumentOffset + 1]) * 128u
                            + static_cast<size_t>(performance[instrumentOffset]);
    const auto soundOffset = soundBankOffset + soundIndex * soundSize;
    bytes[soundOffset + 78u] = 3u;  // An unmistakable filter mode and cutoff.
    bytes[soundOffset + 79u] = 17u;
    constexpr std::array<uint8_t, 16> diskName {
        'D', 'I', 'S', 'K', ' ', 'F', 'I', 'L',
        'T', 'E', 'R', ' ', 'T', 'E', 'S', 'T'
    };
    std::copy(diskName.begin(), diskName.end(), bytes + performanceOffset + 32u);

    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("wave-disk-dsp-test", {}, true);
    require(directory.createDirectory().wasOk(),
            "Could not create disk/DSP regression directory");
    const auto setFile = directory.getChildFile("FILTER.SET");
    const auto firstImageFile = directory.getChildFile("FIRST.IMG");
    const auto imageFile = directory.getChildFile("FILTER.IMG");
    require(setFile.replaceWithData(setup.getData(), setup.getSize())
                && wave::firmware::DosFloppyImage::createWithWaveSetup(
                       firstImageFile, setFile).wasOk()
                && firstImageFile.copyFileTo(imageFile),
            "Could not create disk/DSP regression image");

    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    require(processor->mountDiskImage(firstImageFile).wasOk(),
            "Initial Wave disk could not be mounted before media swap");
    juce::AudioBuffer<float> audio(2, 512);
    for (int block = 0; block < 16; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor->processBlock(audio, noMidi);
    }
    require(processor->mountDiskImage(imageFile).wasOk()
                && processor->getProgramName(1) == "DISK FILTER TEST",
            "Swapped Wave SET did not become the engine's candidate bank");
    for (int block = 0; block < 64; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor->processBlock(audio, noMidi);
    }

    // A mounted SET is accepted from the firmware's Option/Disk requester
    // with OK, not Cancel. That successful exit must restore the Performance
    // serial-panel context or +/- is subsequently rejected as an exclusive
    // Option-page action and the visible Performance screen appears frozen.
    const auto clickAndSettle = [&](int diagnosticCode) {
        const auto matrix
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        require(processor->setPanelButton(matrix, true),
                "Mounted-SET panel press was rejected");
        for (int block = 0; block < 8; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
        processor->setPanelButton(matrix, false);
        for (int block = 0; block < 64; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };
    clickAndSettle(58); // Disk / Load menu.
    clickAndSettle(70); // Open its next step; the SET has not transferred.
    require(processor->getPanelSelectedMode() == 58,
            "First Disk OK prematurely closed the Total Recall workspace");
    clickAndSettle(71); // Leave Disk before testing Option/Import separately.
    require(processor->mountDiskImage(imageFile).wasOk(),
            "Could not remount SET after cancelling Disk selection");
    clickAndSettle(34); // Option / Import workspace.
    require(processor->getPanelSelectedMode() == 34,
            "Mounted SET did not enter the Option/Import workspace");
    clickAndSettle(70); // Confirm the SET load.
    for (int block = 0;
         block < 256 && processor->isPanelModeDisplayTransitionActive();
         ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor->processBlock(audio, noMidi);
    }
    require(processor->getPanelSelectedMode() == 39
                && !processor->isPanelModeDisplayTransitionActive(),
            "Successful SET import left Performance mode or its LCD transaction frozen");
    constexpr uint32_t storedSoundBank = 0x18000u;
    constexpr uint32_t bankBSound1Name = storedSoundBank + 128u * 256u + 240u;
    require(processor->getMasterFirmwareRuntime().sharedProgramByte(
                bankBSound1Name) == static_cast<uint8_t>('W')
                && processor->getMasterFirmwareRuntime().sharedProgramByte(
                       bankBSound1Name + 256u) == static_cast<uint8_t>('S'),
            "Imported SET did not populate distinct firmware Sound names");

    const auto lcdWritesBeforeStep
        = processor->getMasterFirmwareRuntime().lcdVideoWriteCount();
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(72), true); // Plus serial code.
    for (int block = 0; block < 8; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor->processBlock(audio, noMidi);
    }
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(72), false);
    const auto* filterMode = processor->parameters.getRawParameterValue(
        wave::parameters::filterMode);
    const auto* cutoff = processor->parameters.getRawParameterValue(
        wave::parameters::cutoff);
    require(processor->getCurrentProgram() == 1 && filterMode != nullptr
                && juce::roundToInt(filterMode->load()) == 3 && cutoff != nullptr
                && std::abs(cutoff->load()
                            - wave::parameters::cutoffFrequencyForStep(17.0f))
                       < 0.01f
                && processor->getMasterFirmwareRuntime().lcdVideoWriteCount()
                       > lcdWritesBeforeStep,
            "Serial Plus changed the disk-set name without decoding its DSP patch");

    for (int block = 0; block < 64; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor->processBlock(audio, noMidi);
    }
    require(processor->getMasterFirmwareRuntime().localByte(0x54b40u) == 0x00u
                && processor->getMasterFirmwareRuntime().localByte(0x54b41u) == 0x01u,
            "Disk-set DSP selection diverged from the firmware-selected index");
    for (int expected = 2; expected <= 4; ++expected)
    {
        const auto writesBefore
            = processor->getMasterFirmwareRuntime().lcdVideoWriteCount();
        clickAndSettle(72);
        require(processor->getCurrentProgram() == expected
                    && processor->getMasterFirmwareRuntime().localByte(0x54b40u)
                           == 0x00u
                    && processor->getMasterFirmwareRuntime().localByte(0x54b41u)
                           == static_cast<uint8_t>(expected)
                    && processor->getMasterFirmwareRuntime().lcdVideoWriteCount()
                           > writesBefore,
                "Repeated Plus froze after importing a replacement disk SET");
    }
    for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
        require(processor->getMasterFirmwareRuntime().filterCalibrationCode(voice)
                    == wave::dsp::WaldorfEngine::installedFilterCalibrationCodes[
                        static_cast<size_t>(voice)],
                "Loading a SET overwrote the hardware VCF calibration table");

    juce::MemoryBlock persistentState;
    processor->getStateInformation(persistentState);
    require(imageFile.deleteFile(),
            "Could not remove the source disk before the self-contained recall test");
    auto restored = std::make_unique<WaveEmulationAudioProcessor>();
    restored->setStateInformation(persistentState.getData(),
                                  static_cast<int>(persistentState.getSize()));
    require(restored->hasMountedDiskImage()
                && restored->getMountedDiskImageFile().existsAsFile()
                && restored->getMountedDiskImageFile().getFileName()
                       == imageFile.getFileName()
                && restored->getNumPrograms() == wave::presets::WaveFactorySet::programCount
                && restored->getCurrentProgram() == 4
                && restored->getProgramName(1) == "DISK FILTER TEST",
            "Persistent state depended on the missing source disk or SET");
    const auto restoredHostStateDirectory
        = restored->getMountedDiskImageFile().getParentDirectory();
    require(restored->getDiskImageChooserDirectory() == directory,
            "Mount chooser exposed the private host-state cache directory");
    restored->setStateInformation(persistentState.getData(),
                                  static_cast<int>(persistentState.getSize()));
    require(restored->getMountedDiskImageFile().getParentDirectory()
                == restoredHostStateDirectory,
            "Repeated host recall leaked another UUID disk directory");
    restored.reset();
    require(!restoredHostStateDirectory.exists(),
            "Destroying a plugin instance retained its private restored disk directory");

    if (!checkColdStart)
    {
        require(directory.deleteRecursively(),
                "Could not remove disk/DSP regression directory");
        return;
    }
    processor->resetToColdStart();
    require(!processor->hasMountedDiskImage()
                && processor->getNumPrograms() == 1
                && processor->getProgramName(0) == "Initial Wave"
                && processor->getSelectedPerformanceInstrument() == -1,
            "Cold start retained a mounted disk, sound set, or active layer");
    for (int layer = 0; layer < 8; ++layer)
        require(!processor->isPerformanceInstrumentActive(layer),
                "Cold start retained a playable Performance layer");
    juce::MidiBuffer coldStartMidi;
    coldStartMidi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    audio.clear();
    processor->processBlock(audio, coldStartMidi);
    require(processor->getActiveVoiceCount() == 0,
            "Cold start allocated a voice without a loaded sound set");
    require(directory.deleteRecursively(),
            "Could not remove disk/DSP regression directory");
}

void testSamePerformanceReselectionLeavesModalScreen()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };
    const auto visiblePage = [&]()
    {
        const auto snapshot = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        const auto offset = static_cast<size_t>(
                                processor->getMasterFirmwareRuntime().lcdDisplayPage()
                                & 0x03u)
                            * 0x1000u;
        return std::vector<uint8_t>(
            snapshot.begin() + static_cast<std::ptrdiff_t>(offset),
            snapshot.begin() + static_cast<std::ptrdiff_t>(offset + 0x1000u));
    };

    processBlocks(64);
    const auto a001 = visiblePage();
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(58), true); // Disk.
    processBlocks(8);
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(58), false);
    processBlocks(48);
    const auto diskPage = visiblePage();
    require(diskPage != a001, "Disk did not open its genuine firmware page");

    processor->setCurrentProgram(0); // Reselect A001, not a different number.
    processBlocks(64);
    require(visiblePage() != diskPage
                && processor->getPanelSelectedMode() == 39
                && processor->getMasterFirmwareRuntime().localByte(0x54b40u) == 0
                && processor->getMasterFirmwareRuntime().localByte(0x54b41u) == 0,
            "Reselecting A001 left the previous firmware page active");

    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(72), true); // Plus.
    processBlocks(8);
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(72), false);
    processBlocks(64);
    require(processor->getCurrentProgram() == 1
                && processor->getPanelSelectedMode() == 39
                && visiblePage() != diskPage,
            "Performance selected from a modal page left -/+ routed to that old page");
}

void testPerformanceInstrumentSelection()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };
    processor->setCurrentProgram(41); // A042: three distinct active Instruments.
    processBlocks(96);
    const auto original = processor->getSelectedPerformanceInstrument();
    require(original >= 0 && original < 8
                && processor->isPerformanceInstrumentActive(original),
            "Factory Performance did not expose its editable Instrument");

    auto alternate = -1;
    auto inactive = -1;
    for (int instrument = 0; instrument < 8; ++instrument)
    {
        if (processor->isPerformanceInstrumentActive(instrument)
            && instrument != original && alternate < 0)
            alternate = instrument;
        if (!processor->isPerformanceInstrumentActive(instrument) && inactive < 0)
            inactive = instrument;
    }
    require(alternate >= 0 && inactive >= 0,
            "A042 does not provide the expected active/inactive Instrument test cases");

    // Instrument selection repopulates the shared panel parameters. Readers
    // must continue to see either the old or new layer throughout that update,
    // never the internal transaction itself as an unselected Performance.
    std::atomic<bool> stopObserving { false };
    std::atomic<bool> observedNoSelection { false };
    std::thread selectionObserver([&] {
        while (!stopObserving.load(std::memory_order_acquire))
            if (processor->getSelectedPerformanceInstrument() < 0)
                observedNoSelection.store(true, std::memory_order_release);
    });
    auto selectionSwitchesSucceeded = true;
    for (int repetition = 0; repetition < 8; ++repetition)
    {
        selectionSwitchesSucceeded
            &= processor->selectPerformanceInstrument(alternate);
        selectionSwitchesSucceeded
            &= processor->selectPerformanceInstrument(original);
    }
    stopObserving.store(true, std::memory_order_release);
    selectionObserver.join();
    require(selectionSwitchesSucceeded,
            "Instrument switching failed during selection invariant testing");
    require(!observedNoSelection.load(std::memory_order_acquire),
            "Instrument switching temporarily exposed no editable layer");
    processBlocks(256);
    require(processor->getMasterFirmwareRuntime().currentPerformanceInstrument()
                == original,
            "Instrument invariant test left the firmware on another voice");
    processBlocks(4);

    auto* resonanceParameter = processor->parameters.getParameter(wave::parameters::resonance);
    require(resonanceParameter != nullptr, "Resonance parameter is missing");
    resonanceParameter->setValueNotifyingHost(
        resonanceParameter->convertTo0to1(0.987f));
    processor->setPanelPotValue(wave::parameters::resonance, 0.987f);
    const auto physicalResonance = processor->getMasterFirmwareRuntime()
                                       .panelAnalogByte(62);
    auto* filterModAmount = processor->parameters.getParameter(
        wave::parameters::modulationAmount[wave::parameters::filterMod1]);
    require(filterModAmount != nullptr, "Filter Mod 1 amount parameter is missing");
    filterModAmount->setValueNotifyingHost(filterModAmount->convertTo0to1(27.0f));
    processor->setPanelPotValue(
        wave::parameters::modulationAmount[wave::parameters::filterMod1],
        filterModAmount->getValue());
    const auto physicalFilterMod = processor->getMasterFirmwareRuntime()
                                       .panelAnalogByte(47);
    // A hardware potentiometer edit becomes Sound state only after OS 1.700
    // has scanned the ADC. Do not make the selection helper stand in for that
    // firmware time boundary.
    processBlocks(48);
    const auto confirmedOriginalResonance
        = static_cast<float>(processor->getMasterFirmwareRuntime()
                                 .currentSoundRecordByte(80) & 0x7fu)
          / 127.0f;
    const auto confirmedOriginalFilterMod
        = static_cast<float>(processor->getMasterFirmwareRuntime()
                                 .currentSoundRecordByte(87) & 0x7fu)
          - 64.0f;

    require(processor->selectPerformanceInstrument(alternate)
                && processor->getSelectedPerformanceInstrument() == alternate,
            "An active Instrument button did not change the editable layer");
    const auto alternateResonance = processor->parameters.getRawParameterValue(
        wave::parameters::resonance);
    require(alternateResonance != nullptr
                && std::abs(alternateResonance->load() - 0.987f) > 0.05f,
            "Selecting an Instrument did not load its own panel parameters");
    const auto storedAlternateResonance = alternateResonance->load();
    const auto alternateFilterMod = processor->parameters.getRawParameterValue(
        wave::parameters::modulationAmount[wave::parameters::filterMod1]);
    require(alternateFilterMod != nullptr
                && std::abs(alternateFilterMod->load() - 27.0f) > 1.0f,
            "Selecting an Instrument retained the previous voice's modulation knob");
    const auto storedAlternateFilterMod = alternateFilterMod->load();
    require(processor->getMasterFirmwareRuntime().panelAnalogByte(62)
                == physicalResonance,
            "Instrument selection moved a physical potentiometer");

    require(processor->selectPerformanceInstrument(original),
            "Could not return to the original editable Instrument");
    const auto restoredResonance = processor->parameters.getRawParameterValue(
        wave::parameters::resonance);
    require(restoredResonance != nullptr
                && std::abs(restoredResonance->load()
                            - confirmedOriginalResonance) < 0.002f,
            "Instrument edits were lost when switching away and back");
    const auto restoredFilterMod = processor->parameters.getRawParameterValue(
        wave::parameters::modulationAmount[wave::parameters::filterMod1]);
    require(restoredFilterMod != nullptr
                && std::abs(restoredFilterMod->load()
                            - confirmedOriginalFilterMod) < 0.002f,
            "A modulation knob edit was lost when switching away and back");
    require(processor->getMasterFirmwareRuntime().panelAnalogByte(62)
                == physicalResonance,
            "Returning to an Instrument moved a physical potentiometer");

    // Exercise the complete panel -> firmware -> live-Sound path. Merely
    // selecting an Instrument must not rewrite that voice with values from the
    // previously visible edit page, even after the firmware has had time to
    // finish its asynchronous selection.
    constexpr std::array<int, 8> instrumentCodes {
        22, 25, 26, 27, 79, 28, 29, 30
    };
    const auto clickInstrument = [&](int instrument) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(
            instrumentCodes[static_cast<size_t>(instrument)]);
        processor->setPanelButton(button, true);
        processBlocks(32);
        processor->setPanelButton(button, false);
        processBlocks(160);
    };
    clickInstrument(alternate);
    require(processor->getSelectedPerformanceInstrument() == alternate
                && std::abs(alternateResonance->load()
                            - storedAlternateResonance) < 0.002f
                && std::abs(alternateFilterMod->load()
                            - storedAlternateFilterMod) < 0.002f,
            "Firmware Instrument selection changed the destination voice's Sound");
    require(processor->getMasterFirmwareRuntime().currentPerformanceInstrument()
                == alternate,
            "Firmware did not settle on the selected Instrument");
    clickInstrument(original);
    require(processor->getSelectedPerformanceInstrument() == original
                && std::abs(restoredResonance->load()
                            - confirmedOriginalResonance) < 0.002f
                && std::abs(restoredFilterMod->load()
                            - confirmedOriginalFilterMod) < 0.002f,
            "Switching voices changed a previously edited Sound");
    require(processor->getMasterFirmwareRuntime().panelAnalogByte(62)
                == physicalResonance
                && processor->getMasterFirmwareRuntime().panelAnalogByte(47)
                       == physicalFilterMod,
            "Firmware Instrument selection automated a physical potentiometer");

    require(!processor->selectPerformanceInstrument(inactive)
                && processor->getSelectedPerformanceInstrument() == original,
            "An inactive Performance Instrument became spuriously editable");
}

void testSelectedInstrumentSoundRecordOverridesOldPanelSnapshot()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(41); // A042: several active Instruments.
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count, juce::MidiBuffer* first = nullptr) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, block == 0 && first != nullptr
                                               ? *first : none);
        }
    };
    processBlocks(96);

    const auto selected = processor->getSelectedPerformanceInstrument();
    require(selected >= 0
                && processor->getMasterFirmwareRuntime()
                       .currentPerformanceInstrument() == selected,
            "Firmware did not settle on the editable Instrument");

    // Deliberately disagree with the panel mirror. This reproduces selecting
    // a voice while the physical controls still occupy the previous voice's
    // positions: APVTS says almost-closed, but the firmware's live Sound says
    // a high, unmodulated cutoff.
    const auto setHost = [&](const char* id, float value) {
        auto* parameter = processor->parameters.getParameter(id);
        require(parameter != nullptr, "Required panel parameter is missing");
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    setHost(wave::parameters::cutoff,
            wave::parameters::cutoffFrequencyForStep(12.0f));
    setHost(wave::parameters::filterEnv, 0.0f);
    setHost(wave::parameters::filterVelocity, 0.0f);
    setHost(wave::parameters::filterKeytrack, 0.0f);
    processBlocks(1); // Establish this as the old, already-consumed panel state.

    auto& firmware = const_cast<wave::firmware::MasterFirmwareRuntime&>(
        processor->getMasterFirmwareRuntime());
    require(firmware.writeCurrentSoundRecordByte(79u, 100u)
                && firmware.writeCurrentSoundRecordByte(80u, 0u)
                && firmware.writeCurrentSoundRecordByte(81u, 64u)
                && firmware.writeCurrentSoundRecordByte(82u, 64u)
                && firmware.writeCurrentSoundRecordByte(83u, 64u)
                && firmware.writeCurrentSoundRecordByte(87u, 64u)
                && firmware.writeCurrentSoundRecordByte(89u, 64u),
            "Could not prepare the firmware Sound-record regression case");

    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
    processBlocks(8, &noteOn);
    const auto voices = processor->getVoiceStates();
    const auto voice = std::find_if(
        voices.begin(), voices.end(), [selected](const auto& candidate) {
            return candidate.active && candidate.layer == selected;
        });
    require(voice != voices.end(),
            "The selected Instrument did not receive the regression note");
    require(voice->cutoffHz > 1000.0f,
            "Old panel values replaced the selected firmware Sound record");
}

void testOutgoingInstrumentIsSavedFromFirmwareNotPanelMirror()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(41); // A042: several active Instruments.
    juce::AudioBuffer<float> audio(2, 512);
    for (int block = 0; block < 96; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor->processBlock(audio, none);
    }

    const auto original = processor->getSelectedPerformanceInstrument();
    auto alternate = -1;
    for (int instrument = 0; instrument < 8; ++instrument)
        if (instrument != original
            && processor->isPerformanceInstrumentActive(instrument))
        {
            alternate = instrument;
            break;
        }
    require(original >= 0 && alternate >= 0,
            "A042 did not expose two Instruments for the save-boundary test");

    const auto before = processor->probeCurrentLayerVoice(
        7, original, 60, 0.8f, 24000, 42);
    require(before.meanCutoffHz > 0.0,
            "The outgoing Instrument could not be probed");

    // Reproduce delayed/stale panel feedback at the exact selection edge.
    // These values have not travelled through the firmware and therefore must
    // never be used to save the outgoing Instrument.
    const auto setMirror = [&](const char* id, float value) {
        auto* parameter = processor->parameters.getParameter(id);
        require(parameter != nullptr, "A stale-panel test parameter is missing");
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    setMirror(wave::parameters::cutoff,
              wave::parameters::cutoffFrequencyForStep(4.0f));
    setMirror(wave::parameters::filterEnv, -64.0f);
    setMirror(wave::parameters::filterVelocity, -64.0f);
    require(processor->selectPerformanceInstrument(alternate),
            "Could not switch away from the firmware-confirmed Instrument");

    const auto after = processor->probeCurrentLayerVoice(
        7, original, 60, 0.8f, 24000, 42);
    require(std::abs(after.meanCutoffHz - before.meanCutoffHz)
                / before.meanCutoffHz < 0.005,
            "The outgoing Instrument was saved from stale panel parameters");
}

void requireInstrumentSelectionDoesNotChangeSound(int program,
                                                  const char* performanceName)
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(program);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    processBlocks(128);
    const auto baselineSnapshot = processor->getCurrentPerformanceSnapshot();

    std::array<wave::dsp::WaldorfEngine::VoiceProbe, 8> baseline {};
    std::vector<int> active;
    for (int instrument = 0; instrument < 8; ++instrument)
        if (processor->isPerformanceInstrumentActive(instrument))
        {
            active.push_back(instrument);
            baseline[static_cast<size_t>(instrument)]
                = processor->probeCurrentLayerVoice(
                    7, instrument, 60, 0.8f, 24000, 42);
        }
    require(active.size() >= 2,
            "Selection-transparency Performance did not expose two Instruments");

    constexpr std::array<int, 8> instrumentCodes {
        22, 25, 26, 27, 79, 28, 29, 30
    };
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const auto selected : active)
        {
            if (selected == processor->getSelectedPerformanceInstrument())
                continue;
            if (pass == 0)
            {
                const auto button = wave::panel::matrixIndexForDiagnosticCode(
                    instrumentCodes[static_cast<size_t>(selected)]);
                require(button >= 0, "Regression Instrument has no panel serial");
                processor->setPanelButton(button, true);
                processBlocks(32);
                processor->setPanelButton(button, false);
                processBlocks(160);
            }
            else
            {
                require(processor->selectPerformanceInstrument(selected),
                        "Could not select an active regression Instrument");
                processBlocks(192);
            }
            require(processor->getSelectedPerformanceInstrument() == selected,
                    "Display softbutton did not select its regression Instrument");
            const auto selectedSnapshot = processor->getCurrentPerformanceSnapshot();
            for (const auto instrument : active)
            {
                const auto index = static_cast<size_t>(instrument);
                require(std::memcmp(
                            &selectedSnapshot.layers[index],
                            &baselineSnapshot.layers[index],
                            sizeof(wave::dsp::WaldorfEngine::PerformanceLayer)) == 0,
                        "Instrument selection mutated a stored Performance layer");
                const auto after = processor->probeCurrentLayerVoice(
                    7, instrument, 60, 0.8f, 24000, 42);
                const auto& before = baseline[index];
                require(std::abs(after.rmsOutput - before.rmsOutput)
                            <= std::max(1.0e-8, before.rmsOutput * 1.0e-4)
                            && std::abs(after.meanCutoffHz - before.meanCutoffHz)
                                   <= std::max(1.0e-5,
                                               before.meanCutoffHz * 1.0e-5),
                        performanceName);
            }
        }
    }
}

void testFactoryStereoInstrumentSelectionDoesNotChangeSound()
{
    requireInstrumentSelectionDoesNotChangeSound(
        44, "Selecting an A045 Instrument changed its stereo sound");
    requireInstrumentSelectionDoesNotChangeSound(
        50, "Selecting an A051 Instrument changed its stereo sound");
}

void testA001DuplicateSoundAssignmentsHavePrivateInstrumentEdits()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(0); // A001: I1 and I2 both assign Sound a002.
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto clickInstrument = [&](int diagnosticCode) {
        const auto button
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        processBlocks(32);
        processor->setPanelButton(button, false);
        processBlocks(160);
    };

    processBlocks(96);
    require(processor->getMasterFirmwareRuntime().currentPerformanceInstrument() == 1,
            "A001 did not start with Instrument 2 selected");
    const auto factoryCutoff
        = processor->getMasterFirmwareRuntime().currentSoundRecordByte(79) & 0x7fu;
    require(factoryCutoff > 0,
            "A001 did not provide a nonzero cutoff for the aliasing regression");

    auto& firmware = const_cast<wave::firmware::MasterFirmwareRuntime&>(
        processor->getMasterFirmwareRuntime());
    require(firmware.writeCurrentSoundRecordByte(79u, 0u),
            "Could not turn A001 Instrument 2 cutoff down");
    processBlocks(4);

    clickInstrument(22); // Instrument 1.
    require(firmware.currentPerformanceInstrument() == 0,
            "A001 Instrument 1 serial selection did not complete");
    require((firmware.currentSoundRecordByte(79) & 0x7fu) == factoryCutoff,
            "Editing A001 Instrument 2 also changed Instrument 1");

    clickInstrument(25); // Instrument 2.
    require(firmware.currentPerformanceInstrument() == 1,
            "A001 Instrument 2 serial reselection did not complete");
    require((firmware.currentSoundRecordByte(79) & 0x7fu) == 0u,
            "A001 Instrument 2 lost its private cutoff edit");

    juce::MemoryBlock hostState;
    processor->getStateInformation(hostState);
    auto restored = std::make_unique<WaveEmulationAudioProcessor>();
    restored->setStateInformation(hostState.getData(),
                                  static_cast<int>(hostState.getSize()));
    restored->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> restoredAudio(2, 512);
    const auto processRestored = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            restoredAudio.clear();
            juce::MidiBuffer none;
            restored->processBlock(restoredAudio, none);
        }
    };
    processRestored(192);
    require(restored->getCurrentProgram() == 0
                && restored->getSelectedPerformanceInstrument() == 1
                && restored->getMasterFirmwareRuntime()
                           .currentPerformanceInstrument() == 1
                && (restored->getMasterFirmwareRuntime()
                        .currentSoundRecordByte(79) & 0x7fu) == 0u,
            "Host recall lost the selected Instrument or its firmware edit buffer");

    const auto restoredInstrumentOne
        = wave::panel::matrixIndexForDiagnosticCode(22);
    restored->setPanelButton(restoredInstrumentOne, true);
    processRestored(32);
    restored->setPanelButton(restoredInstrumentOne, false);
    processRestored(160);
    require(restored->getMasterFirmwareRuntime().currentPerformanceInstrument() == 0
                && (restored->getMasterFirmwareRuntime()
                        .currentSoundRecordByte(79) & 0x7fu) == factoryCutoff,
            "Host recall collapsed two independently edited Instrument buffers");
}

void testInstrumentButtonsDoNotRewriteLayerOctaves()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(41); // A042: three active Instruments.
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&] (int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };
    constexpr std::array<int, 8> instrumentDiagnosticCodes {
        22, 25, 26, 27, 79, 28, 29, 30
    };
    std::array<std::array<int, 2>, 8> originalOctaves{};
    std::array<bool, 8> activeInstruments{};

    for (int instrument = 0; instrument < 8; ++instrument)
    {
        if (!processor->isPerformanceInstrumentActive(instrument))
            continue;
        activeInstruments[static_cast<size_t>(instrument)] = true;
        require(processor->pressPerformanceInstrumentButton(instrument),
                "An active Instrument could not be selected for the octave test");
        const std::array octaveBefore {
            processor->getFirmwareOscillatorOctave(0),
            processor->getFirmwareOscillatorOctave(1)
        };
        originalOctaves[static_cast<size_t>(instrument)] = octaveBefore;

        const auto matrix = wave::panel::matrixIndexForDiagnosticCode(
            instrumentDiagnosticCodes[static_cast<size_t>(instrument)]);
        processor->setPanelButton(matrix, true);
        processBlocks(8);
        processor->setPanelButton(matrix, false);
        processBlocks(24);

        const std::array octaveAfter {
            processor->getFirmwareOscillatorOctave(0),
            processor->getFirmwareOscillatorOctave(1)
        };
        require(octaveAfter == octaveBefore,
                "Selecting an Instrument rewrote its oscillator Octave values");
    }

    // The first pass switches away from every layer except the final one. A
    // second pass catches octave loss in layer serialisation, rather than only
    // checking the immediate button event on the destination layer.
    for (int instrument = 0; instrument < 8; ++instrument)
    {
        if (!activeInstruments[static_cast<size_t>(instrument)])
            continue;
        require(processor->pressPerformanceInstrumentButton(instrument),
                "An active Instrument could not be revisited for the octave test");
        const std::array restoredOctaves {
            processor->getFirmwareOscillatorOctave(0),
            processor->getFirmwareOscillatorOctave(1)
        };
        require(restoredOctaves == originalOctaves[static_cast<size_t>(instrument)],
                "Switching away and back changed an Instrument's oscillator Octaves");
    }
}

void testInstrumentEditLayerSelection()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(41); // A042: three active Instruments.
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };
    processBlocks(96);

    const auto original = processor->getSelectedPerformanceInstrument();
    auto alternate = -1;
    for (int instrument = 0; instrument < 8; ++instrument)
        if (instrument != original
            && processor->isPerformanceInstrumentActive(instrument))
        {
            alternate = instrument;
            break;
        }
    require(original >= 0 && alternate >= 0,
            "A042 did not expose two Instruments for edit-page selection");

    const auto pressAndRelease = [&](int diagnosticCode) {
        const auto matrix
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(matrix, true);
        processBlocks(8);
        processor->setPanelButton(matrix, false);
        processBlocks(40);
    };
    pressAndRelease(36); // Instrument Edit.
    require(processor->getPanelLed(22),
            "Instrument Edit did not become the active operating mode");

    constexpr std::array<int, 8> instrumentCodes {
        22, 25, 26, 27, 79, 28, 29, 30
    };
    for (int repetition = 0; repetition < 6; ++repetition)
    {
        const auto expected = (repetition & 1) == 0 ? alternate : original;
        const auto layerButton = wave::panel::matrixIndexForDiagnosticCode(
            instrumentCodes[static_cast<size_t>(expected)]);
        processor->setPanelButton(layerButton, true);
        processBlocks(8);
        processor->setPanelButton(layerButton, false);
        processBlocks(160);
        require(processor->getSelectedPerformanceInstrument() == expected
                    && processor->getMasterFirmwareRuntime()
                           .currentPerformanceInstrument() == expected,
                "Repeated Instrument Edit selection lost firmware synchronization");
    }
}

void testInstrumentSourceKeepsOtherLayers()
{
    const auto configuredSet = juce::SystemStats::getEnvironmentVariable(
        "WAVE_FACTORY_SET", {});
    const auto sourceSet = configuredSet.isNotEmpty()
        ? juce::File(configuredSet)
        : juce::File::getCurrentWorkingDirectory().getChildFile("wave.set");
    juce::MemoryBlock setup;
    require(sourceSet.loadFileAsData(setup), "Source test could not read SET");
    auto* bytes = static_cast<uint8_t*>(setup.getData());
    constexpr size_t performanceOffset = 0x22e7cu + 41u * 512u;
    constexpr size_t layerTable = performanceOffset + 64u;
    std::array<uint8_t, 32> firstLayer {};
    std::copy_n(bytes + layerTable, firstLayer.size(), firstLayer.begin());
    for (int layer = 0; layer < 3; ++layer)
    {
        auto* destination = bytes + layerTable + static_cast<size_t>(layer) * 32u;
        std::copy(firstLayer.begin(), firstLayer.end(), destination);
        destination[3] = 3; // Keys and MIDI; all three share the same Sound.
        destination[13] = 0;
    }
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("wave-source-test", {}, true);
    require(directory.createDirectory().wasOk(), "Source test directory failed");
    const auto setFile = directory.getChildFile("SOURCE.SET");
    const auto imageFile = directory.getChildFile("SOURCE.IMG");
    require(setFile.replaceWithData(setup.getData(), setup.getSize())
                && wave::firmware::DosFloppyImage::createWithWaveSetup(
                       imageFile, setFile).wasOk(),
            "Source test disk creation failed");
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    require(processor->mountDiskImage(imageFile).wasOk(),
            "Source test disk mount failed");
    processor->setCurrentProgram(41); // A042 has three active Instruments.
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer midi;
            processor->processBlock(audio, midi);
        }
    };
    processBlocks(96);
    for (int layer = 0; layer < 3; ++layer)
        require(processor->isPerformanceInstrumentActive(layer),
                "Source test did not initialise three active layers");
    const auto& firmware = processor->getMasterFirmwareRuntime();
    const auto record = firmware.currentPerformanceRecordOffset();
    require(record.has_value(), "Source test has no Performance record");
    std::array<int, 8> originalSources {};
    for (int layer = 0; layer < 8; ++layer)
        originalSources[static_cast<size_t>(layer)]
            = firmware.sharedProgramByte(*record + 64u
                                         + static_cast<uint32_t>(layer) * 32u + 3u) & 0x7f;
    const auto click = [&](int code) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(code);
        processor->setPanelButton(button, true);
        processBlocks(8);
        processor->setPanelButton(button, false);
        processBlocks(96);
    };
    click(36); // Instrument Edit.
    click(26); // Instrument 3.
    require(firmware.currentPerformanceInstrument() == 2,
            "Source test did not select Instrument 3");
    for (int layer = 0; layer < 8; ++layer)
    {
        const auto selectedRecord = firmware.currentPerformanceRecordOffset();
        require(selectedRecord.has_value(), "Selection lost Performance record");
        const auto source = firmware.sharedProgramByte(
                                *selectedRecord + 64u
                                + static_cast<uint32_t>(layer) * 32u + 3u) & 0x7f;
        require(source == originalSources[static_cast<size_t>(layer)],
                "Selecting Instrument 3 changed another Instrument Source");
    }
    constexpr auto sourceChannel = wave::panel::performanceFaderAdcChannels[7];
    for (const auto position : { 0.0f, 1.0f, 0.0f, 1.0f })
    {
        processor->setPanelFader(7, sourceChannel, position, false);
        processBlocks(96);
        const auto liveRecord = firmware.currentPerformanceRecordOffset();
        require(liveRecord.has_value(), "Source edit lost its Performance record");
        require(firmware.currentInstrumentEditTarget() == 2
                    && processor->getSelectedPerformanceInstrument() == 2,
                "Source change moved the Instrument Edit target");
        require((firmware.sharedProgramByte(*liveRecord + 64u + 2u * 32u + 3u) & 0x7f)
                    == (position == 0.0f ? 0 : 3)
                    && processor->isPerformanceInstrumentActive(2) == (position > 0.0f),
                "Source change did not update Instrument 3 in firmware and engine");
        for (int layer = 0; layer < 8; ++layer)
        {
            if (layer == 2)
                continue;
            const auto source = firmware.sharedProgramByte(
                                    *liveRecord + 64u
                                    + static_cast<uint32_t>(layer) * 32u + 3u) & 0x7f;
            if (source != originalSources[static_cast<size_t>(layer)])
                throw std::runtime_error("Changing Instrument 3 Source changed native Source for Instrument "
                                         + std::to_string(layer + 1) + ": "
                                         + std::to_string(originalSources[static_cast<size_t>(layer)])
                                         + " -> " + std::to_string(source)
                                         + " at fader " + std::to_string(position));
            require(processor->isPerformanceInstrumentActive(layer)
                        == (source != 0),
                    "Instrument Source engine state disagreed with native record");
        }
    }
    require(directory.deleteRecursively(), "Source test cleanup failed");
}

void testPerformanceMuteAndSolo()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->setCurrentProgram(41); // A042: three active Instruments.
    std::array<int, 3> active { -1, -1, -1 };
    auto activeCount = 0;
    for (int instrument = 0; instrument < 8 && activeCount < 3; ++instrument)
        if (processor->isPerformanceInstrumentActive(instrument))
            active[static_cast<size_t>(activeCount++)] = instrument;
    require(activeCount == 3, "A042 did not expose three Mute/Solo targets");

    processor->togglePerformanceMuteMode();
    require(processor->getInstrumentButtonMode()
                == WaveEmulationAudioProcessor::InstrumentButtonMode::mute,
            "Mute function button did not latch");
    require(processor->pressPerformanceInstrumentButton(active[0])
                && processor->isPerformanceInstrumentMuted(active[0])
                && !processor->isPerformanceInstrumentAudible(active[0])
                && processor->isPerformanceInstrumentAudible(active[1]),
            "Mute mode did not silence its targeted Instrument");
    const auto mutedGreenLed = 96 + active[0] * 2;
    const auto mutedRedLed = mutedGreenLed + 1;
    const auto audibleGreenLed = 96 + active[1] * 2;
    const auto audibleRedLed = audibleGreenLed + 1;
    require(!processor->getPanelLed(mutedGreenLed)
                && processor->getPanelLed(mutedRedLed)
                && processor->getPanelLed(audibleGreenLed)
                && !processor->getPanelLed(audibleRedLed),
            "Instrument bi-colour LEDs asserted both outputs in Mute mode");

    processor->prepareToPlay(48000.0, 512);
    processor->setMidiInputActsAsLocalKeyboard(true);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    processor->processBlock(audio, on);
    require(processor->getActiveVoiceCount() == 3,
            "A muted Instrument stopped consuming its genuine voice allocation");

    processor->togglePerformanceMuteMode();
    require(processor->getInstrumentButtonMode()
                    == WaveEmulationAudioProcessor::InstrumentButtonMode::normal
                && !processor->isPerformanceInstrumentMuted(active[0])
                && processor->isPerformanceInstrumentAudible(active[0]),
            "Leaving Mute mode did not clear the mute status");

    processor->togglePerformanceSoloMode();
    require(processor->getInstrumentButtonMode()
                == WaveEmulationAudioProcessor::InstrumentButtonMode::solo,
            "Solo function button did not latch");
    require(processor->pressPerformanceInstrumentButton(active[1])
                && processor->isPerformanceInstrumentAudible(active[1])
                && !processor->isPerformanceInstrumentAudible(active[0])
                && !processor->isPerformanceInstrumentAudible(active[2]),
            "Solo mode did not isolate its targeted Instrument");
    require(processor->pressPerformanceInstrumentButton(active[2])
                && processor->isPerformanceInstrumentAudible(active[2])
                && !processor->isPerformanceInstrumentAudible(active[1]),
            "Selecting another Solo target did not replace the previous Solo");
    processor->togglePerformanceSoloMode();
    require(processor->getInstrumentButtonMode()
                    == WaveEmulationAudioProcessor::InstrumentButtonMode::normal
                && processor->isPerformanceInstrumentAudible(active[0])
                && processor->isPerformanceInstrumentAudible(active[1])
                && processor->isPerformanceInstrumentAudible(active[2]),
            "Leaving Solo mode did not restore all Instruments");
}

void testA013ComparatorAutoPan()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(12); // A013 -> 2.3 Sweep oo WMF.

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    juce::MidiBuffer noMidi;
    auto strongestLeft = 0.0f;
    auto strongestRight = 0.0f;
    for (int block = 0; block < 190; ++block)
    {
        audio.clear();
        processor->processBlock(audio, block == 0 ? noteOn : noMidi);
        requireFinite(audio);
        const auto left = audio.getRMSLevel(0, 0, audio.getNumSamples());
        const auto right = audio.getRMSLevel(1, 0, audio.getNumSamples());
        strongestLeft = juce::jmax(strongestLeft, left / (right + 1.0e-7f));
        strongestRight = juce::jmax(strongestRight, right / (left + 1.0e-7f));
    }

    require(strongestLeft > 2.0f && strongestRight > 2.0f,
            "Factory A013 Comparator/S&H pan modulation remained on one channel");
}

void testFactoryVcaReleaseTail()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(0); // A001, release byte 47 in both layers.
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noMidi;
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    processor->processBlock(audio, noteOn);
    for (int block = 0; block < 10; ++block)
    {
        audio.clear();
        processor->processBlock(audio, noMidi);
    }

    juce::MidiBuffer noteOff;
    noteOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    audio.clear();
    processor->processBlock(audio, noteOff);
    require(processor->getActiveVoiceCount() == 2
                && audio.getMagnitude(0, 0, audio.getNumSamples()) > 1.0e-6f,
            "Factory A001 VCA envelope cut off instead of entering release");

    for (int block = 0; block < 20; ++block)
    {
        audio.clear();
        processor->processBlock(audio, noMidi);
    }
    require(processor->getActiveVoiceCount() == 2,
            "Factory A001 firmware release rate produced no audible tail");

    for (int block = 0; block < 300; ++block)
    {
        audio.clear();
        processor->processBlock(audio, noMidi);
    }
    require(processor->getActiveVoiceCount() == 0,
            "Factory A001 VCA release did not eventually close its voices");
}

void testLegacyStartupStateReloadsExactFactoryPerformance()
{
    auto savedProcessor = std::make_unique<WaveEmulationAudioProcessor>();
    savedProcessor->setCurrentProgram(0); // A001: one left and one right layer.
    auto* pan = savedProcessor->parameters.getParameter(wave::parameters::pan);
    require(pan != nullptr, "Factory pan parameter is unavailable");
    pan->setValueNotifyingHost(pan->convertTo0to1(-1.0f));

    // Simulate standalone state written before complete layered-Performance
    // restoration was versioned. Its selected-element pan is stale and left.
    auto legacyState = savedProcessor->parameters.copyState();
    legacyState.setProperty("factoryProgram", 0, nullptr);
    juce::MemoryBlock legacyBinary;
    if (const auto xml = legacyState.createXml())
        juce::AudioProcessor::copyXmlToBinary(*xml, legacyBinary);

    auto restored = std::make_unique<WaveEmulationAudioProcessor>();
    restored->setStateInformation(legacyBinary.getData(),
                                  static_cast<int>(legacyBinary.getSize()));
    const auto* restoredPan = restored->parameters.getRawParameterValue(
        wave::parameters::pan);
    require(restoredPan != nullptr && restoredPan->load() > 0.95f,
            "Legacy startup state overrode A001's factory right-hand layer pan");

    restored->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    juce::MidiBuffer noMidi;
    auto leftPeak = 0.0f;
    auto rightPeak = 0.0f;
    for (int block = 0; block < 8; ++block)
    {
        audio.clear();
        restored->processBlock(audio, block == 0 ? noteOn : noMidi);
        leftPeak = juce::jmax(leftPeak,
                              audio.getMagnitude(0, 0, audio.getNumSamples()));
        rightPeak = juce::jmax(rightPeak,
                               audio.getMagnitude(1, 0, audio.getNumSamples()));
    }
    require(leftPeak > 1.0e-4f && rightPeak > 1.0e-4f,
            "Migrated startup Performance did not play through both channels");
}

void testFactoryMultimodeFilterLoading()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);

    processor->setCurrentProgram(4); // A005 -> WaveStrings, Dual filter.
    const auto* mode = processor->parameters.getRawParameterValue(wave::parameters::filterMode);
    const auto* highpass = processor->parameters.getRawParameterValue(
        wave::parameters::highpassCutoff);
    const auto expectedHighpass = 20.0f * std::pow(2.0f, 50.0f / 12.0f);
    require(mode != nullptr && highpass != nullptr
                && juce::roundToInt(mode->load()) == 3
                && std::abs(highpass->load() - expectedHighpass) < 0.1f,
            "Factory Dual-filter mode or independent high-pass cutoff was not decoded");

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    processor->processBlock(audio, midi);
    requireFinite(audio);
    auto filterPeak = juce::jmax(audio.getMagnitude(0, 0, audio.getNumSamples()),
                                 audio.getMagnitude(1, 0, audio.getNumSamples()));
    for (int block = 0; block < 8; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor->processBlock(audio, noMidi);
        requireFinite(audio);
        filterPeak = juce::jmax(filterPeak,
                                audio.getMagnitude(0, 0, audio.getNumSamples()),
                                audio.getMagnitude(1, 0, audio.getNumSamples()));
    }
    require(filterPeak > 1.0e-5f,
            "Factory Dual-filter performance produced no audio");

    processor->allSoundOffFromUi();
    processor->setCurrentProgram(18); // A019 -> XTChoir, Band-pass filter.
    require(juce::roundToInt(mode->load()) == 2,
            "Factory Band-pass filter mode was not decoded");
}

void testPanelEditButtonFirmwareRouting()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };

    processBlocks(48);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), false);
    processBlocks(24);

    constexpr std::array editDiagnosticCodes {
        8, 17, 86, 11, 60, 67, 9, 18, 59, 66, 10, 19, 61
    };
    for (const auto diagnosticCode : editDiagnosticCodes)
    {
        const std::array octaveBefore {
            processor->getFirmwareOscillatorOctave(0),
            processor->getFirmwareOscillatorOctave(1)
        };
        const auto before = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        const auto pageBefore = processor->getMasterFirmwareRuntime().lcdDisplayPage();
        const auto button
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        processBlocks(8);
        processor->setPanelButton(button, false);
        for (int block = 0;
             block < 192 && processor->isPanelModeDisplayTransitionActive();
             ++block)
            processBlocks(1);
        processBlocks(8);
        const auto after = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        const auto pageAfter = processor->getMasterFirmwareRuntime().lcdDisplayPage();
        const std::array octaveAfter {
            processor->getFirmwareOscillatorOctave(0),
            processor->getFirmwareOscillatorOctave(1)
        };
        require(octaveBefore == octaveAfter,
                "An Edit switch changed an oscillator Octave selector");
        auto changed = 0;
        const auto beforeOffset = static_cast<size_t>(pageBefore & 3u) * 0x1000u;
        const auto afterOffset = static_cast<size_t>(pageAfter & 3u) * 0x1000u;
        for (size_t row = 0; row < 64; ++row)
            for (size_t column = 0; column < 60; ++column)
                changed += before[beforeOffset + row * 64 + column]
                           != after[afterOffset + row * 64 + column];
        if (changed <= 100)
            throw std::runtime_error(
                "Physical Edit switch " + std::to_string(diagnosticCode)
                + " did not reach its genuine firmware page");
    }
}

void testArtworkControlsMatchCompleteWiringMap()
{
    constexpr auto panelHorizontalOffset = 66.0f;
    constexpr auto upperPanelBottom = 619.0f;
    const auto svgFile = juce::File(__FILE__).getParentDirectory()
                             .getParentDirectory()
                             .getChildFile("media/WaldorfWaveUI.svg");
    const auto svg = svgFile.loadFileAsString();
    const auto document = juce::XmlDocument::parse(svg);
    require(document != nullptr, "The supplied Waldorf Wave SVG could not be parsed");

    auto switchCount = 0;
    auto keyboardButtonCount = 0;
    auto lowerKeyboardSwitchCount = 0;
    auto faderCount = 0;
    std::vector<juce::Point<float>> ledCentres;
    std::vector<juce::Point<float>> lowerKeyboardLedCentres;
    for (auto* element : document->getChildIterator())
    {
        if (element->hasTagName("circle"))
        {
            const auto radius = static_cast<float>(element->getDoubleAttribute("r"));
            if (std::abs(radius - 14.5f) < 0.1f)
            {
                const auto x = static_cast<float>(element->getDoubleAttribute("cx"));
                const auto y = static_cast<float>(element->getDoubleAttribute("cy"));
                if (y < upperPanelBottom)
                {
                    require(wave::panel::switchAt(x - panelHorizontalOffset, y) != nullptr,
                            "An upper-panel SVG button is absent from the wiring map");
                    ++switchCount;
                }
                else if (wave::panel::keyboardPanelSwitchAt(x, y) != nullptr)
                    ++lowerKeyboardSwitchCount;
                else if (wave::panel::keyboardControllerButtonAt(x, y) != nullptr)
                    ++keyboardButtonCount;
            }
            if (std::abs(radius - 3.0f) < 0.1f
                && element->getStringAttribute("fill") == "#6C0455")
            {
                const juce::Point<float> centre {
                    static_cast<float>(element->getDoubleAttribute("cx")),
                    static_cast<float>(element->getDoubleAttribute("cy"))
                };
                if (std::none_of(ledCentres.begin(), ledCentres.end(),
                                 [centre](const auto& existing) {
                                     return existing.getDistanceFrom(centre) < 0.1f;
                                 }))
                {
                    if (centre.y < upperPanelBottom)
                    {
                        require(wave::panel::ledAt(
                                    centre.x - panelHorizontalOffset, centre.y) != nullptr,
                                "An upper-panel SVG LED is absent from the serial output map");
                        ledCentres.push_back(centre);
                    }
                    else
                    {
                        require(wave::panel::keyboardPanelLedAt(
                                    centre.x, centre.y) != nullptr,
                                "A lower keyboard-panel SVG LED is absent from the serial output map");
                        lowerKeyboardLedCentres.push_back(centre);
                    }
                }
            }
            continue;
        }

        if (!element->hasTagName("rect"))
            continue;
        const auto width = static_cast<float>(element->getDoubleAttribute("width"));
        const auto height = static_cast<float>(element->getDoubleAttribute("height"));
        const auto radius = static_cast<float>(element->getDoubleAttribute("rx"));
        if (std::abs(width - 28.0f) < 0.1f
            && std::abs(height - 169.0f) < 0.1f)
        {
            ++faderCount;
            continue;
        }
        const auto isEditPill = std::abs(width - 61.0f) < 0.1f
                                && std::abs(height - 30.0f) < 0.1f
                                && std::abs(radius - 15.0f) < 0.1f;
        const auto isStorePill = std::abs(width - 60.0f) < 0.1f
                                 && std::abs(height - 29.0f) < 0.1f
                                 && std::abs(radius - 14.5f) < 0.1f
                                 && element->getStringAttribute("fill") == "#DA2E2E";
        if (!isEditPill && !isStorePill)
            continue;

        const auto x = static_cast<float>(element->getDoubleAttribute("x"));
        const auto y = static_cast<float>(element->getDoubleAttribute("y"));
        auto centre = juce::Point<float> { x + width * 0.5f, y + height * 0.5f };
        auto transformText = element->getStringAttribute("transform");
        if (transformText.startsWith("rotate(") && transformText.endsWithChar(')'))
        {
            transformText = transformText.substring(7, transformText.length() - 1);
            const auto values = juce::StringArray::fromTokens(transformText, " ,", "");
            require(values.size() >= 3, "An SVG Edit-button transform is malformed");
            centre = centre.transformedBy(juce::AffineTransform::rotation(
                juce::degreesToRadians(static_cast<float>(values[0].getDoubleValue())),
                static_cast<float>(values[1].getDoubleValue()),
                static_cast<float>(values[2].getDoubleValue())));
        }
        if (centre.y < upperPanelBottom)
        {
            require(wave::panel::switchAt(
                        centre.x - panelHorizontalOffset, centre.y) != nullptr,
                    "An upper-panel pill button is absent from the wiring map");
            ++switchCount;
        }
        else if (wave::panel::keyboardPanelSwitchAt(centre.x, centre.y) != nullptr)
            ++lowerKeyboardSwitchCount;
        else if (wave::panel::keyboardControllerButtonAt(centre.x, centre.y) != nullptr)
            ++keyboardButtonCount;
    }

    require(switchCount == static_cast<int>(wave::panel::visibleSwitches.size()),
            "The SVG and complete button wiring table have different control counts");
    require(keyboardButtonCount
                == static_cast<int>(wave::panel::keyboardControllerButtons.size()),
            "The SVG and keyboard-controller button table have different control counts");
    require(lowerKeyboardSwitchCount
                == static_cast<int>(wave::panel::keyboardPanelSwitches.size()),
            "The SVG and lower keyboard-panel switch table have different control counts");
    require(faderCount == static_cast<int>(wave::panel::performanceFaderAdcChannels.size()),
            "The SVG and performance-fader multiplexer table have different counts");
    require(ledCentres.size() == wave::panel::visibleLeds.size(),
            "The SVG and LED serial-output table have different marker counts");
    require(lowerKeyboardLedCentres.size()
                == wave::panel::keyboardPanelLeds.size(),
            "The SVG and lower keyboard-panel LED table have different marker counts");

    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    const auto& runtime = processor->getMasterFirmwareRuntime();
    for (const auto& control : wave::panel::visibleSwitches)
    {
        const auto target = wave::panel::physicalMatrixIndex(control);
        processor->setPanelButton(target, true);
        const auto oneShot = control.diagnosticCode == 69
                             || control.diagnosticCode == 72
                             || control.diagnosticCode == 21
                             || control.diagnosticCode == 23;
        constexpr std::array displayButtons { 22, 25, 26, 27,
                                              79, 28, 29, 30 };
        const auto dualPurposeDisplayButton
            = std::find(displayButtons.begin(), displayButtons.end(),
                        control.diagnosticCode) != displayButtons.end();
        constexpr std::array routedModeButtons { 38, 33, 35, 34,
                                                  32, 37, 36, 39 };
        const auto discreteModeButton
            = std::find(routedModeButtons.begin(), routedModeButtons.end(),
                        control.diagnosticCode) != routedModeButtons.end();
        const auto discreteEditButton = std::any_of(
            wave::panel::editIndicators.begin(),
            wave::panel::editIndicators.end(),
            [&control](const auto& indicator) {
                return indicator.buttonDiagnosticCode == control.diagnosticCode;
            });
        for (const auto& observed : wave::panel::visibleSwitches)
        {
            const auto serial = wave::panel::physicalMatrixIndex(observed);
            const auto requested = runtime.panelButtonRequestedDown(serial);
            require(serial == target
                        ? (oneShot || dualPurposeDisplayButton
                           || discreteModeButton || discreteEditButton
                           || control.diagnosticCode == 31
                           || control.diagnosticCode == 11
                           || requested)
                        : !requested,
                    "A UI button asserted more than its one mapped matrix input");
        }
        processor->setPanelButton(target, false);
        require(!runtime.panelButtonRequestedDown(target),
                "A UI button left its matrix input asserted after release");
    }

    for (size_t index = 0; index < wave::panel::keyboardControllerButtons.size(); ++index)
        for (size_t other = index + 1;
             other < wave::panel::keyboardControllerButtons.size(); ++other)
            require(wave::panel::keyboardControllerButtons[index].asciiCode
                        != wave::panel::keyboardControllerButtons[other].asciiCode,
                    "Two keyboard controls share one serial command byte");

    constexpr auto potPosition = 0.23f;
    const auto expectedRaw = static_cast<uint8_t>(
        juce::roundToInt((1.0f - potPosition) * 255.0f));
    for (const auto& pot : wave::panel::visiblePots)
    {
        processor->setPanelPotValue(pot.parameterId, potPosition);
        require(runtime.panelAnalogByte(pot.adcChannel) == expectedRaw,
                "A UI pot did not reach its one mapped analogue channel");
    }
}

void testKnobModeSelectUsesFirmwarePanelPath()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };

    processBlocks(64);
    const auto& firmware = processor->getMasterFirmwareRuntime();
    constexpr std::array<int, 4> modeLeds { 6, 70, 71, 7 };
    const auto activeMode = [&]
    {
        auto active = -1;
        for (size_t index = 0; index < modeLeds.size(); ++index)
            if (processor->getPanelLed(modeLeds[index]))
            {
                require(firmware.panelLed(modeLeds[index]),
                        "The UI invented a Knob Mode LED outside firmware output");
                require(active < 0, "Firmware asserted multiple Knob Mode LEDs");
                active = static_cast<int>(index);
            }
        return active;
    };
    require(activeMode() == 3, "Firmware did not launch in Relative Knob Mode");

    auto* cutoffParameter = processor->parameters.getParameter(wave::parameters::cutoff);
    require(cutoffParameter != nullptr, "Cutoff parameter is missing");
    const auto physicalCutoffBefore = cutoffParameter->getValue();
    auto previousCutoff = static_cast<int>(firmware.currentSoundRecordByte(79));
    auto observedChanges = 0;
    auto largestStep = 0;
    for (int step = 1; step <= 80; ++step)
    {
        processor->setPanelPotValue(
            wave::parameters::cutoff,
            juce::jlimit(0.0f, 1.0f,
                         physicalCutoffBefore + static_cast<float>(step) / 254.0f));
        processBlocks(8);
        const auto currentCutoff
            = static_cast<int>(firmware.currentSoundRecordByte(79));
        if (currentCutoff != previousCutoff)
        {
            if (observedChanges > 0)
                largestStep = juce::jmax(largestStep,
                                         std::abs(currentCutoff - previousCutoff));
            ++observedChanges;
            previousCutoff = currentCutoff;
        }
    }
    require(observedChanges >= 4 && largestStep <= 3,
            "Relative Cutoff movement was not delivered as a smooth firmware sweep");

    const auto* wiring = wave::panel::switchAt(786.0f, 555.0f);
    require(wiring != nullptr, "Knob Mode Select has no panel wiring entry");
    const auto select = wave::panel::physicalMatrixIndex(*wiring);
    processor->setPanelButton(select, true);
    require(firmware.panelButtonRequestedDown(select)
                && firmware.panelButtonPressed(select),
            "Knob Mode Select did not reach its canonical firmware input");
    processBlocks(8);
    processor->setPanelButton(select, false);
    require(!firmware.panelButtonRequestedDown(select),
            "Knob Mode Select remained requested after release");
    processBlocks(32);
    require(!firmware.panelButtonPressed(select),
            "Knob Mode Select did not release after firmware debounce");
    require(activeMode() == 2, "Firmware did not change from Relative to Snap");

    const auto pressSelect = [&]
    {
        processor->setPanelButton(select, true);
        processBlocks(8);
        processor->setPanelButton(select, false);
        processBlocks(32);
    };
    pressSelect();
    require(activeMode() == 1, "Firmware did not change from Snap to Jump");
    pressSelect();
    require(activeMode() == 0, "Firmware did not change from Jump to Knobs Off");

    const auto beforeOffMovement = firmware.currentSoundRecordByte(2);
    processor->setPanelPotValue(wave::parameters::oscillatorDetune[0], 0.9f);
    processBlocks(32);
    require(firmware.currentSoundRecordByte(2) == beforeOffMovement,
            "Knobs Off allowed a panel pot to edit the firmware Sound");

    pressSelect();
    require(activeMode() == 3, "Firmware did not return to Relative Knob Mode");
    processor->setPanelPotValue(wave::parameters::oscillatorDetune[0], 0.8f);
    processBlocks(32);
    require(firmware.currentSoundRecordByte(2) != beforeOffMovement,
            "Relative Knob Mode did not apply physical pot movement");
}

void testEditPageFadersDriveFirmwareValues()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };

    processBlocks(48);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), false);
    processBlocks(24);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(17), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(17), false);
    processBlocks(40);
    constexpr auto displayFaderAdcChannels
        = wave::panel::performanceFaderAdcChannels;
    for (size_t fader = 0; fader < displayFaderAdcChannels.size(); ++fader)
    {
        const auto before = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        const auto rawBefore = processor->getMasterFirmwareRuntime().panelAnalogByte(
            displayFaderAdcChannels[fader]);
        const auto target = rawBefore < 128u ? 0.9f : 0.1f;
        processor->beginPanelFaderGesture(static_cast<int>(fader), false);
        processor->setPanelFader(static_cast<int>(fader),
                                 displayFaderAdcChannels[fader], target, false);
        processor->endPanelFaderGesture(static_cast<int>(fader), false);
        processBlocks(36);
        const auto after = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        // Wave 1 Edit leaves its first display-fader slot unassigned; the
        // remaining seven columns must be driven by the firmware scanner.
        require(fader == 0 || before != after,
                "An assigned Edit-page fader did not update its genuine firmware LCD value");
        require(processor->getPanelFaderValue(static_cast<int>(fader)) == 0.0f,
                "Edit-page movement incorrectly changed a Performance fader assignment");
        processor->setPanelFader(static_cast<int>(fader),
                                 displayFaderAdcChannels[fader], 0.0f, false);
        processBlocks(36);
    }

    constexpr std::array sectionButtons { 8, 17, 60, 66, 19, 61 };
    auto sectionValue = 0.23f;
    for (const auto button : sectionButtons)
    {
        processor->setPanelButton(
            wave::panel::matrixIndexForDiagnosticCode(button), true);
        processBlocks(8);
        processor->setPanelButton(
            wave::panel::matrixIndexForDiagnosticCode(button), false);
        processBlocks(32);
        const auto before = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        // ADC 42 is display fader 6 and is active on each of these edit pages.
        processor->setPanelFader(5, displayFaderAdcChannels[5], sectionValue, false);
        processBlocks(36);
        const auto after = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        require(before != after,
                "An Edit section did not refresh its displayed value after fader movement");
        sectionValue = sectionValue < 0.5f ? 0.73f : 0.23f;
    }

    // The Lowpass Filter page labels display fader 6 as the Mod 1 amount.
    // Reproduce the reported state exactly: Performance remains the selected
    // blue operating mode underneath the Filter Edit overlay, and verify the
    // firmware commits the fader into the selected Instrument's live record.
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(39), true);
    processBlocks(8);
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(39), false);
    processBlocks(24);
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(60), true);
    processBlocks(8);
    processor->setPanelButton(
        wave::panel::matrixIndexForDiagnosticCode(60), false);
    processBlocks(32);
    processor->beginPanelFaderGesture(5, false);
    processor->setPanelFader(5, displayFaderAdcChannels[5], 0.08f, false);
    processor->endPanelFaderGesture(5, false);
    processBlocks(48);
    const auto liveRecord = processor->getMasterFirmwareRuntime()
                                .currentSoundRecordOffset();
    require(liveRecord.has_value(), "Firmware did not resolve the live Sound record");
    const auto lowAmount = processor->getMasterFirmwareRuntime()
                               .currentSoundRecordByte(87) & 0x7fu;
    processor->beginPanelFaderGesture(5, false);
    processor->setPanelFader(5, displayFaderAdcChannels[5], 0.92f, false);
    processor->endPanelFaderGesture(5, false);
    processBlocks(48);
    const auto highAmount = processor->getMasterFirmwareRuntime()
                                .currentSoundRecordByte(87) & 0x7fu;
    require(highAmount > lowAmount + 80u,
            "Firmware did not route Filter-page fader 6 to live Sound byte 87");
}

void testEditPageRefreshesAfterFirmwarePerformanceChange()
{
    const auto makeProcessor = []
    {
        auto processor = std::make_unique<WaveEmulationAudioProcessor>();
        processor->prepareToPlay(48000.0, 512);
        return processor;
    };
    const auto processBlocks = [](WaveEmulationAudioProcessor& processor,
                                  juce::AudioBuffer<float>& audio, int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor.processBlock(audio, none);
        }
    };
    const auto press = [&processBlocks](WaveEmulationAudioProcessor& processor,
                                        juce::AudioBuffer<float>& audio,
                                        int diagnosticCode)
    {
        const auto matrix = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor.setPanelButton(matrix, true);
        processBlocks(processor, audio, 8);
        processor.setPanelButton(matrix, false);
        processBlocks(processor, audio, 40);
    };
    const auto openFilterPage = [&press](WaveEmulationAudioProcessor& processor,
                                         juce::AudioBuffer<float>& audio)
    {
        press(processor, audio, 36); // Instrument Edit.
        press(processor, audio, 60); // Filter Edit.
    };

    auto reference = makeProcessor();
    juce::AudioBuffer<float> referenceAudio(2, 512);
    reference->setCurrentProgram(1); // A002.
    processBlocks(*reference, referenceAudio, 96);
    openFilterPage(*reference, referenceAudio);
    const auto expected
        = reference->getMasterFirmwareRuntime().lcdVideoSnapshot();

    auto changedInPlace = makeProcessor();
    juce::AudioBuffer<float> changedAudio(2, 512);
    changedInPlace->setCurrentProgram(0); // A001.
    processBlocks(*changedInPlace, changedAudio, 96);
    openFilterPage(*changedInPlace, changedAudio);
    juce::MidiBuffer programChange;
    programChange.addEvent(juce::MidiMessage::programChange(1, 1), 0);
    changedAudio.clear();
    changedInPlace->processBlock(changedAudio, programChange);
    processBlocks(*changedInPlace, changedAudio, 96);
    const auto firmwareSelected
        = (static_cast<int>(changedInPlace->getMasterFirmwareRuntime().localByte(
               0x54b40u)) << 8)
          | static_cast<int>(changedInPlace->getMasterFirmwareRuntime().localByte(
              0x54b41u));
    require(firmwareSelected == 1,
            "MIDI Program Change did not select A002 while an Edit page was open");
    const auto actual
        = changedInPlace->getMasterFirmwareRuntime().lcdVideoSnapshot();

    require(actual == expected,
            "A firmware Performance change left stale values on an open Edit page");
}

void testOscillatorOctaveButtonsDriveFirmwareSoundRecord()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };

    processBlocks(48);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), false);
    processBlocks(24);
    constexpr std::array octaveDiagnosticCodes { 0, 1 };
    for (size_t oscillator = 0; oscillator < octaveDiagnosticCodes.size(); ++oscillator)
    {
        const auto matrix = wave::panel::matrixIndexForDiagnosticCode(
            octaveDiagnosticCodes[oscillator]);
        for (int step = 0; step < 5; ++step)
        {
            const auto before = processor->getFirmwareOscillatorOctave(
                static_cast<int>(oscillator));
            processor->setPanelButton(matrix, true);
            processBlocks(8);
            processor->setPanelButton(matrix, false);
            processBlocks(48);
            const auto after = processor->getFirmwareOscillatorOctave(
                static_cast<int>(oscillator));
            require(after != before,
                    "An oscillator Octave switch did not change the genuine firmware sound record");

            auto illuminated = 0;
            for (size_t position = 0;
                 position
                     < wave::panel::oscillatorOctaveLedSerialCodes[oscillator].size();
                 ++position)
            {
                const auto selected = after == 2 - static_cast<int>(position);
                const auto lit = processor->getPanelLed(
                    wave::panel::oscillatorOctaveLedSerialCodes[oscillator][position]);
                illuminated += lit ? 1 : 0;
                require(lit == selected,
                        "An oscillator Octave bank retained its stored LED");
            }
            require(illuminated == 1,
                    "An oscillator Octave bank illuminated more than one position");
        }
    }
}

void testEditSectionKnobsDriveFirmwareValues()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };

    processBlocks(64);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), false);
    processBlocks(24);

    struct SectionKnob
    {
        int editButton;
        const char* parameterId;
        int adcChannel;
    };
    constexpr std::array sectionKnobs {
        SectionKnob { 8, wave::parameters::oscillatorDetune[0], 24 },
        SectionKnob { 17, wave::parameters::wavePhase[0], 32 },
        SectionKnob { 9, wave::parameters::oscillatorDetune[1], 18 },
        SectionKnob { 18, wave::parameters::wavePhase[1], 9 },
        SectionKnob { 60, wave::parameters::cutoff, 63 },
        SectionKnob { 59, wave::parameters::filterAttack, 61 },
        SectionKnob { 66, wave::parameters::attack, 41 },
        SectionKnob {
            61,
            wave::parameters::modulationAmount[wave::parameters::panMod1],
            53
        }
    };

    for (const auto& section : sectionKnobs)
    {
        processor->setPanelButton(
            wave::panel::matrixIndexForDiagnosticCode(section.editButton), true);
        processBlocks(8);
        processor->setPanelButton(
            wave::panel::matrixIndexForDiagnosticCode(section.editButton), false);
        processBlocks(32);
        auto* parameter = processor->parameters.getParameter(section.parameterId);
        require(parameter != nullptr, "An edit-section knob parameter is missing");
        parameter->setValueNotifyingHost(0.12f);
        processor->setPanelPotValue(section.parameterId, 0.12f);
        processBlocks(36);
        const auto low = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        const auto lowRaw = processor->getMasterFirmwareRuntime().panelAnalogByte(
            section.adcChannel);
        parameter->setValueNotifyingHost(0.88f);
        processor->setPanelPotValue(section.parameterId, 0.88f);
        processBlocks(36);
        const auto high = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        const auto highRaw = processor->getMasterFirmwareRuntime().panelAnalogByte(
            section.adcChannel);
        require(low != high,
                "An edit-section knob did not update its genuine firmware LCD value");
        require(lowRaw > highRaw,
                "A clockwise panel knob was not converted to the Wave's reversed ADC polarity");
    }
}

void testEndlessPanelDialDrivesFirmwareValue()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    processBlocks(64);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(36), false);
    processBlocks(24);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(19), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(19), false);
    processBlocks(32);
    const auto before = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    processor->turnPanelEncoder(0, 5);
    processBlocks(36);
    if (processor->getMasterFirmwareRuntime().lcdVideoSnapshot() == before)
    {
        // The loaded value can already be at the clockwise limit. Exercise
        // the opposite quadrature direction before declaring the serial path
        // inactive.
        processor->turnPanelEncoder(0, -5);
        processBlocks(36);
    }
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() != before,
            "An endless panel dial did not reach the genuine firmware edit page");
}

void testGlideEditPageDrivesNativeSoundAndDsp()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int diagnosticCode) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        process(8);
        processor->setPanelButton(button, false);
        for (int block = 0;
             block < 192 && processor->isPanelModeDisplayTransitionActive();
             ++block)
            process(1);
        process(8);
    };

    process(96);
    click(36); // Instrument Edit.
    click(11); // Glide Edit.
    auto& firmware = const_cast<wave::firmware::MasterFirmwareRuntime&>(
        processor->getMasterFirmwareRuntime());
    processor->setPanelFader(0, wave::panel::performanceFaderAdcChannels[0],
                             0.9f, false);
    processor->setPanelFader(1, wave::panel::performanceFaderAdcChannels[1],
                             0.9f, false);
    processor->setPanelFader(4, wave::panel::performanceFaderAdcChannels[4],
                             0.9f, false);
    processor->setPanelFader(5, wave::panel::performanceFaderAdcChannels[5],
                             0.9f, false);
    process(64);
    require((firmware.currentSoundRecordByte(233) & 0x7fu) == 6
                && (firmware.currentSoundRecordByte(235) & 0x7fu) == 1
                && (firmware.currentSoundRecordByte(236) & 0x7fu) > 30
                && (firmware.currentSoundRecordByte(237) & 0x7fu) > 100,
            "The Glide page faders did not update their native Sound fields");

    if ((firmware.currentSoundRecordByte(238u) & 0x01u) != 0u)
        click(6);
    click(6); // Physical Glide On/Off switch on the keyboard assembly.
    require((firmware.currentSoundRecordByte(238u) & 0x01u) != 0u,
            "The physical Glide On/Off switch did not update native Sound state");

    const auto rateBefore = firmware.currentSoundRecordByte(234u) & 0x7fu;
    const auto targetRate = rateBefore == 50u ? 51u : 50u;
    processor->setPanelPotValue(wave::parameters::glideRate,
                                static_cast<float>(targetRate) / 127.0f);
    require((firmware.currentSoundRecordByte(234u) & 0x7fu) == rateBefore,
            "The UI mutated firmware state outside the audio-thread timeline");
    process(16);
    require((firmware.currentSoundRecordByte(234u) & 0x7fu) == targetRate,
            "The physical Glide Rate pot did not update native Sound state");

    // Select the firmware's non-modulated Rate setting through the two real
    // contextual faders as well. The audible test must not inject any of the
    // six Glide Sound bytes behind the serial/control path.
    processor->setPanelFader(4, wave::panel::performanceFaderAdcChannels[4],
                             1.0f, false);
    processor->setPanelFader(5, wave::panel::performanceFaderAdcChannels[5],
                             64.0f / 127.0f, false);
    process(64);
    require((firmware.currentSoundRecordByte(236u) & 0x7fu) == 38u
                && (firmware.currentSoundRecordByte(237u) & 0x7fu) == 64u
                && ((firmware.currentSoundRecordByte(238u) & 0x01u) != 0u),
            "The Glide modulation faders did not select an unmodulated Rate");
    process(16);
    processor->noteOnFromUi(60, 0.9f);
    process(2);
    // The first page fader selected Fingered Gliss, so overlap the two notes
    // to engage it.
    processor->noteOnFromUi(72, 0.9f);
    process(1);
    auto foundGlidingVoice = false;
    for (const auto& voice : processor->getVoiceStates())
        if (voice.active && voice.triggerNote == 72
            && voice.glidePitch > 59.0f && voice.glidePitch < 65.0f)
            foundGlidingVoice = true;
    require(foundGlidingVoice,
            "Native Glide page values did not reach the voice-card pitch path");
}

void testPhysicalGlideControlsWorkOutsideEditPage()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int diagnosticCode) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        process(8);
        processor->setPanelButton(button, false);
        process(32);
    };

    process(96); // Remain on the normal Performance page.
    const auto& firmware = processor->getMasterFirmwareRuntime();
    const auto enabledBefore
        = (firmware.currentSoundRecordByte(238u) & 0x01u) != 0u;
    click(6);
    require(((firmware.currentSoundRecordByte(238u) & 0x01u) != 0u)
                != enabledBefore,
            "The physical Glide switch only worked while Glide Edit was open");
    if ((firmware.currentSoundRecordByte(238u) & 0x01u) == 0u)
        click(6);
    require((firmware.currentSoundRecordByte(238u) & 0x01u) != 0u,
            "The physical Glide switch could not be enabled outside its edit page");

    const auto rateBefore = firmware.currentSoundRecordByte(234u) & 0x7fu;
    const auto targetRate = rateBefore == 91u ? 90u : 91u;
    processor->setPanelPotValue(wave::parameters::glideRate,
                                static_cast<float>(targetRate) / 127.0f);
    process(24);
    require((firmware.currentSoundRecordByte(234u) & 0x7fu) == targetRate,
            "The physical Glide Rate pot only worked while Glide Edit was open");

    // The firmware record is not enough: confirm its closed-page feedback is
    // the value actually rendered by the engine before the message-thread
    // parameter mirror has had an opportunity to run.
    processor->noteOnFromUi(60, 0.9f);
    process(2);
    processor->noteOffFromUi(60);
    process(1);
    processor->noteOnFromUi(72, 0.9f);
    process(1);
    auto foundGlidingVoice = false;
    for (const auto& voice : processor->getVoiceStates())
        if (voice.active && voice.keyDown && voice.triggerNote == 72
            && voice.glidePitch > 59.0f && voice.glidePitch < 71.9f)
            foundGlidingVoice = true;
    require(foundGlidingVoice,
            "Closed-page physical Glide controls did not reach the audio engine");
}

void testBriefGlideSwitchClicksSurviveFirmwareScan()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto tap = [&](int diagnosticCode) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        processor->setPanelButton(button, false);
        process(128);
    };
    const auto glideEnabled = [&] {
        return (processor->getMasterFirmwareRuntime()
                    .currentSoundRecordByte(238u)
                & 0x01u) != 0u;
    };

    process(96);
    auto before = glideEnabled();
    tap(6);
    require(glideEnabled() != before,
            "A brief Performance-page Glide click disappeared between firmware scans");
    const auto afterPerformanceTap = glideEnabled();
    process(256);
    require(glideEnabled() == afterPerformanceTap,
            "One brief Performance-page Glide click repeated after release");

    const auto clickPage = [&](int diagnosticCode) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        process(8);
        processor->setPanelButton(button, false);
        for (int block = 0;
             block < 192 && processor->isPanelModeDisplayTransitionActive();
             ++block)
            process(1);
        process(8);
    };
    clickPage(36); // Instrument Edit.
    clickPage(11); // Glide Edit.
    before = glideEnabled();
    tap(6);
    require(glideEnabled() != before,
            "A brief Glide-page Glide click disappeared between firmware scans");
    const auto afterEditTap = glideEnabled();
    process(256);
    require(glideEnabled() == afterEditTap,
            "One brief Glide-page Glide click repeated after release");
}

void testPlusTapAfterDataDialAdvancesOneStep()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int diagnosticCode)
    {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        processBlocks(8);
        processor->setPanelButton(button, false);
        processBlocks(32);
    };

    processBlocks(64);
    click(36); // Instrument Edit: the data dial initially edits Wavetable.
    const auto& firmware = processor->getMasterFirmwareRuntime();
    const auto before = static_cast<int>(firmware.currentSoundRecordByte(25) & 0x7fu);
    processor->turnPanelEncoder(8, 1);
    processBlocks(32);
    const auto afterDial = static_cast<int>(firmware.currentSoundRecordByte(25) & 0x7fu);
    require(afterDial == juce::jmin(63, before + 1),
            "The Wavetable/Data dial did not advance the selected field by one");

    const auto plus = wave::panel::matrixIndexForDiagnosticCode(72);
    processor->setPanelButton(plus, true);
    processor->setPanelButton(plus, false);
    processBlocks(32);
    const auto afterPlus = static_cast<int>(firmware.currentSoundRecordByte(25) & 0x7fu);
    require(afterPlus == juce::jmin(63, afterDial + 1),
            "A Plus tap after the data dial advanced more than one step");

    auto expected = afterPlus;
    for (auto tap = 0; tap < 10; ++tap)
    {
        processor->setPanelButton(plus, true);
        processor->setPanelButton(plus, false);
        expected = juce::jmin(63, expected + 1);
        auto latency = 0;
        while (static_cast<int>(firmware.currentSoundRecordByte(25) & 0x7fu)
                   != expected
               && latency < 64)
        {
            processBlocks(1);
            ++latency;
        }
        require(static_cast<int>(firmware.currentSoundRecordByte(25) & 0x7fu)
                    == expected,
                "An edit-page Plus tap was lost");
    }

    for (auto tap = 0; tap < 10; ++tap)
    {
        processor->setPanelButton(plus, true);
        processor->setPanelButton(plus, false);
    }
    processBlocks(128);
    expected = juce::jmin(63, expected + 10);
    require(static_cast<int>(firmware.currentSoundRecordByte(25) & 0x7fu)
                == expected,
            "Rapid edit-page Plus taps were lost");
}

void testWaveLinkUsesFirmwareLatch()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int diagnosticCode) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        process(8);
        processor->setPanelButton(button, false);
        process(48);
    };
    process(64);
    const auto& firmware = processor->getMasterFirmwareRuntime();
    const auto before = (firmware.currentSoundRecordByte(23u) & 0x01u) != 0u;
    click(15);
    const auto after = (firmware.currentSoundRecordByte(23u) & 0x01u) != 0u;
    require(after != before,
            "Wave Link did not toggle native Sound-record byte 23");
    require(firmware.panelLed(63) == after && processor->getPanelLed(63) == after,
            "Wave Link LED did not retain the firmware's latched state");

    click(15);
    require(((firmware.currentSoundRecordByte(23u) & 0x01u) != 0u) == before,
            "Wave Link did not toggle back through serial code 15");
}

void testProgramStateDoesNotMasqueradeAsPanelMovement()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };

    processBlocks(64);
    const auto initialRaw = processor->getMasterFirmwareRuntime().panelAnalogByte(63);
    auto* cutoff = processor->parameters.getParameter(wave::parameters::cutoff);
    require(cutoff != nullptr, "Cutoff parameter is missing");
    cutoff->setValueNotifyingHost(0.88f);
    processBlocks(36);
    require(processor->getMasterFirmwareRuntime().panelAnalogByte(63) == initialRaw,
            "A program/automation change was incorrectly replayed as physical panel motion");

    processor->setPanelPotValue(wave::parameters::cutoff, 0.88f);
    require(processor->getMasterFirmwareRuntime().panelAnalogByte(63) < initialRaw,
            "A real Cutoff gesture did not reach the reversed physical ADC input");
}

void testHostStatePreservesRelativePanelPotPositions()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](WaveEmulationAudioProcessor& target, int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            target.processBlock(audio, none);
        }
    };

    process(*processor, 64);
    constexpr auto physicalCutoff = 0.173f;
    processor->setPanelPotValue(wave::parameters::cutoff, physicalCutoff);
    const auto savedAdc = processor->getMasterFirmwareRuntime().panelAnalogByte(63);

    auto* cutoff = processor->parameters.getParameter(wave::parameters::cutoff);
    require(cutoff != nullptr, "Cutoff parameter is missing");
    cutoff->setValueNotifyingHost(0.88f);
    process(*processor, 36);
    require(processor->getMasterFirmwareRuntime().panelAnalogByte(63) == savedAdc,
            "Host automation moved the physical Cutoff pot before state capture");
    const auto savedSoundCutoff
        = static_cast<int>(processor->getMasterFirmwareRuntime()
                               .currentSoundRecordByte(79) & 0x7fu);

    juce::MemoryBlock state;
    processor->getStateInformation(state);
    auto restored = std::make_unique<WaveEmulationAudioProcessor>();
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    restored->prepareToPlay(48000.0, 512);
    process(*restored, 64);

    require(restored->getMasterFirmwareRuntime().panelAnalogByte(63) == savedAdc,
            "Host recall replaced the physical Cutoff position with the sound value");
    require(static_cast<int>(restored->getMasterFirmwareRuntime()
                                 .currentSoundRecordByte(79) & 0x7fu)
                == savedSoundCutoff,
            "Restoring a physical/sound mismatch edited the Cutoff before user input");
    const auto restoredPosition
        = restored->getPanelPotValue(wave::parameters::cutoff);
    require(restoredPosition.has_value()
                && std::abs(*restoredPosition - physicalCutoff) <= 1.0f / 255.0f,
            "Host recall did not restore the independent Relative-mode pot baseline");

    auto previousCutoff = savedSoundCutoff;
    auto changes = 0;
    auto largestStep = 0;
    constexpr std::array<int, 4> knobModeLeds { 6, 70, 71, 7 };
    auto restoredKnobMode = -1;
    for (size_t index = 0; index < knobModeLeds.size(); ++index)
        if (restored->getMasterFirmwareRuntime().panelLed(knobModeLeds[index]))
            restoredKnobMode = static_cast<int>(index);
    require(restoredKnobMode == 3,
            "Host recall did not retain firmware Relative Knob Mode");
    for (int step = 1; step <= 20; ++step)
    {
        restored->setPanelPotValue(
            wave::parameters::cutoff,
            juce::jlimit(0.0f, 1.0f,
                         *restoredPosition + static_cast<float>(step) / 255.0f));
        process(*restored, 8);
        const auto current = static_cast<int>(
            restored->getMasterFirmwareRuntime().currentSoundRecordByte(79) & 0x7fu);
        if (current == previousCutoff)
            continue;
        largestStep = juce::jmax(largestStep, std::abs(current - previousCutoff));
        previousCutoff = current;
        ++changes;
    }
    require(changes >= 4 && largestStep <= 6,
            "The first restored Cutoff gesture jumped instead of moving relatively");
}

void testProgramRecallRejectsPreviousHostPanelEcho()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };

    process(64);
    auto* cutoff = processor->parameters.getParameter(wave::parameters::cutoff);
    require(cutoff != nullptr, "Cutoff parameter is missing");
    constexpr auto precedingHostValue = 0.0f;
    cutoff->setValueNotifyingHost(precedingHostValue);
    process(24);
    require((processor->getMasterFirmwareRuntime().currentSoundRecordByte(79)
             & 0x7fu) == 0u,
            "Test setup did not commit the preceding host Cutoff value");

    processor->setCurrentProgram(1);
    process(64);
    const auto recalledCutoff = static_cast<int>(
        processor->getMasterFirmwareRuntime().currentSoundRecordByte(79) & 0x7fu);
    require(recalledCutoff != 0,
            "The target factory Sound does not distinguish the stale-host test");

    // This is the late parameter acknowledgement emitted by Pro Tools/AAX for
    // the program we just left. It must not edit the firmware-selected Sound.
    cutoff->setValueNotifyingHost(precedingHostValue);
    process(24);
    const auto afterStaleEcho = static_cast<int>(
        processor->getMasterFirmwareRuntime().currentSoundRecordByte(79) & 0x7fu);
    if (afterStaleEcho != recalledCutoff)
        throw std::runtime_error(
            "A preceding AAX panel value overwrote the recalled Sound: "
            + std::to_string(recalledCutoff) + " -> "
            + std::to_string(afterStaleEcho));

    // A genuinely new automation value remains valid during the same window.
    const auto newHostValue = recalledCutoff < 64 ? 1.0f : 0.5f;
    cutoff->setValueNotifyingHost(newHostValue);
    process(24);
    require(static_cast<int>(
                processor->getMasterFirmwareRuntime().currentSoundRecordByte(79)
                & 0x7fu) != recalledCutoff,
            "The program-recall guard blocked new host automation");
}

void testRecallInitModalAcceptsDisplaySoftKeys()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto processBlocks = [&](int count)
    {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer noMidi;
            processor->processBlock(audio, noMidi);
        }
    };

    processBlocks(64);
    const auto performanceScreen
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(40), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(40), false);
    processBlocks(48);
    const auto recallScreen
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    require(recallScreen != performanceScreen,
            "Recall / Init did not open its genuine firmware modal page");

    const auto writesBeforeSoftKey
        = processor->getMasterFirmwareRuntime().lcdVideoWriteCount();
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(28), true);
    processBlocks(8);
    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(28), false);
    processBlocks(48);
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() != recallScreen
                && processor->getMasterFirmwareRuntime().lcdVideoWriteCount()
                       > writesBeforeSoftKey,
            "Recall / Init stopped accepting the dual-purpose display soft keys");
}

void testFirmwareModifierSelectorDrivesDsp()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks, juce::MidiBuffer* first = nullptr)
    {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, block == 0 && first != nullptr ? *first : none);
        }
    };
    const auto click = [&](int diagnosticCode)
    {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        process(8);
        processor->setPanelButton(button, false);
        for (int block = 0;
             block < 192 && processor->isPanelModeDisplayTransitionActive();
             ++block)
            process(1);
        process(8);
    };
    const auto tap = [&](int diagnosticCode)
    {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        processor->setPanelButton(button, false);
        process(32);
    };

    process(96);
    click(39); // Performance
    click(60); // Filter Edit
    processor->setPanelFader(5, wave::panel::performanceFaderAdcChannels[5],
                             0.92f, false);
    process(48);

    const auto& firmware = processor->getMasterFirmwareRuntime();
    require(firmware.currentSoundRecordOffset().has_value()
                && (firmware.currentSoundRecordByte(85) & 0x7fu) == 0
                && (firmware.currentSoundRecordByte(87) & 0x7fu) > 110,
            "Filter-page fixture did not establish LFO 1 at a high amount");

    // Move to Maximum using only the real OS 1.700 +/- handler.
    for (int clicks = 0;
         (firmware.currentSoundRecordByte(86) & 0x7fu) != 38 && clicks < 48;
         ++clicks)
        tap(72);
    require((firmware.currentSoundRecordByte(86) & 0x7fu) == 38,
            "Firmware +/- could not select Maximum for Filter Mod 1");

    const auto setPlainValue = [&processor](const char* id, float value)
    {
        auto* parameter = processor->parameters.getParameter(id);
        require(parameter != nullptr, "Filter modulation test parameter is missing");
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    setPlainValue(wave::parameters::cutoff, 30.0f);
    setPlainValue(wave::parameters::filterEnv, 0.0f);
    setPlainValue(wave::parameters::filterVelocity, 0.0f);
    setPlainValue(wave::parameters::filterKeytrack, 0.0f);
    setPlainValue(wave::parameters::lfoRate[0], 100.0f);

    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    noteOn.addEvent(juce::MidiMessage::controllerEvent(1, 1, 0), 0);
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    const auto restartNote = [&]
    {
        auto restart = noteOn;
        process(1, &restart);
        process(8);
    };
    restartNote();
    const auto measureCutoffTravel = [&]
    {
        auto minimum = std::numeric_limits<float>::max();
        auto maximum = 0.0f;
        for (int block = 0; block < 420; ++block)
        {
            process(1);
            for (const auto& voice : processor->getVoiceStates())
                if (voice.active)
                {
                    minimum = juce::jmin(minimum, voice.cutoffHz);
                    maximum = juce::jmax(maximum, voice.cutoffHz);
                }
        }
        return maximum - minimum;
    };
    const auto maximumTravel = measureCutoffTravel();
    require(maximumTravel > 100.0f,
            "LFO 1 x Maximum selected on the firmware screen was not audible");

    // Turning the physical amount knob must update the same live Sound record
    // that the firmware selector edits. Let it run for well over one second to
    // catch the former record/APVTS feedback loop that made knobs drift back.
    setPlainValue(wave::parameters::modulationAmount[wave::parameters::filterMod1],
                  31.0f);
    process(160);
    const auto stableAmount = processor->parameters.getRawParameterValue(
        wave::parameters::modulationAmount[wave::parameters::filterMod1]);
    require(stableAmount != nullptr
                && std::abs(stableAmount->load() - 31.0f) < 0.001f
                && (firmware.currentSoundRecordByte(87) & 0x7fu) == 95,
            "A physical modulation knob fought the firmware edit record");

    tap(69);
    require((firmware.currentSoundRecordByte(86) & 0x7fu) != 38,
            "Firmware Minus did not change the Filter Mod 1 modifier");

    for (int clicks = 0;
         (firmware.currentSoundRecordByte(86) & 0x7fu) != 38 && clicks < 48;
         ++clicks)
        tap(72);
    require((firmware.currentSoundRecordByte(86) & 0x7fu) == 38,
            "Firmware Plus could not restore Maximum for Filter Mod 1");
}

void testEditedPerformanceFaderRoutingDrivesDsp()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(255); // MULTI INIT: only Instrument 1 is active.

    juce::AudioBuffer<float> audio(2, 512);
    for (int block = 0; block < 96; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor->processBlock(audio, none);
    }

    auto& firmware = const_cast<wave::firmware::MasterFirmwareRuntime&>(
        processor->getMasterFirmwareRuntime());
    std::array<uint8_t, wave::presets::WaveFactorySet::soundSize> sound{};
    std::array<uint8_t, wave::presets::WaveFactorySet::performanceSize> performance{};
    for (size_t byte = 0; byte < sound.size(); ++byte)
        sound[byte] = firmware.sharedProgramByte(0x5300u + static_cast<uint32_t>(byte));
    for (size_t byte = 0; byte < performance.size(); ++byte)
        performance[byte]
            = firmware.sharedProgramByte(0x5400u + static_cast<uint32_t>(byte));

    // Reproduce an edit made on the genuine Performance page: fader 6 now
    // targets active Instrument 1, sends CC 74, and CC 74 is Control X.
    performance[18] = 0;
    performance[19] = 73;
    performance[28] = 74;
    sound[34] = 0;  // Wave 1 Mod 1 source: LFO 1.
    sound[35] = 34; // Wave 1 Mod 1 modifier: Control X.
    require(firmware.installEditRecords(sound, performance),
            "Could not install edited firmware Performance routing fixture");

    const auto setPlainValue = [&processor](const char* id, float value) {
        auto* parameter = processor->parameters.getParameter(id);
        require(parameter != nullptr, "Edited-fader routing parameter is missing");
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    setPlainValue(wave::parameters::position, 31.0f);
    setPlainValue(wave::parameters::scan, 0.0f);
    setPlainValue(wave::parameters::waveEnvelopeVelocity[0], 0.0f);
    setPlainValue(wave::parameters::waveKeytrack[0], 0.0f);
    setPlainValue(wave::parameters::lfoRate[0], 100.0f);
    setPlainValue(
        wave::parameters::modulationAmount[wave::parameters::lfo1LevelMod], 0.0f);
    setPlainValue(
        wave::parameters::modulationAmount[wave::parameters::wave1Mod1], 63.0f);

    processor->setPanelFader(5, wave::panel::performanceFaderAdcChannels[5], 0.0f);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    processor->processBlock(audio, noteOn);
    auto downMinimum = processor->getFirstActiveWavePosition();
    auto downMaximum = downMinimum;
    for (int block = 0; block < 48; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor->processBlock(audio, none);
        const auto position = processor->getFirstActiveWavePosition();
        downMinimum = juce::jmin(downMinimum, position);
        downMaximum = juce::jmax(downMaximum, position);
    }
    require(downMaximum - downMinimum < 0.01f,
            "Control-X route moved while edited Performance fader 6 was down");

    processor->setPanelFader(5, wave::panel::performanceFaderAdcChannels[5], 1.0f);
    auto upMinimum = processor->getFirstActiveWavePosition();
    auto upMaximum = upMinimum;
    for (int block = 0; block < 160; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor->processBlock(audio, none);
        const auto position = processor->getFirstActiveWavePosition();
        upMinimum = juce::jmin(upMinimum, position);
        upMaximum = juce::jmax(upMaximum, position);
    }
    require(upMaximum - upMinimum > 40.0f,
            "DSP ignored the live firmware assignment for Performance fader 6");

    // Reassign the same physical fader to Modwheel.  A later CC1 from an
    // external keyboard must take ownership; the stationary fader must not
    // stamp its old zero value back over the controller on the next block.
    performance[19] = 0; // Stored controller 0 is MIDI CC1.
    sound[26] = 0;       // Wave 1 base position.
    sound[30] = 64;      // No static wave scan.
    sound[31] = 64;      // No velocity offset.
    sound[32] = 64;      // No keytrack offset.
    sound[34] = 22;      // Wave 1 Mod 1 source: Modwheel.
    sound[35] = 38;      // Modifier: Maximum.
    sound[36] = 127;     // Full positive modulation amount.
    require(firmware.installEditRecords(sound, performance),
            "Could not install the MIDI Modwheel routing fixture");
    setPlainValue(wave::parameters::position, 0.0f);
    setPlainValue(
        wave::parameters::modulationSource[wave::parameters::wave1Mod1], 22.0f);
    setPlainValue(
        wave::parameters::modulationControl[wave::parameters::wave1Mod1], 38.0f);
    setPlainValue(
        wave::parameters::modulationAmount[wave::parameters::wave1Mod1], 63.0f);
    processor->setPanelFader(5, wave::panel::performanceFaderAdcChannels[5], 0.0f);

    juce::MidiBuffer externalWheel;
    externalWheel.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    externalWheel.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 1);
    externalWheel.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 2);
    audio.clear();
    processor->processBlock(audio, externalWheel);
    require(processor->getModWheelForUi() > 0.99f,
            "External MIDI CC1 did not reach the processor controller state");
    for (int block = 0; block < 4; ++block)
    {
        audio.clear();
        juce::MidiBuffer settle;
        processor->processBlock(audio, settle);
    }
    const auto* wheelSource = processor->parameters.getRawParameterValue(
        wave::parameters::modulationSource[wave::parameters::wave1Mod1]);
    const auto* wheelAmount = processor->parameters.getRawParameterValue(
        wave::parameters::modulationAmount[wave::parameters::wave1Mod1]);
    require(wheelSource != nullptr && juce::roundToInt(wheelSource->load()) == 22,
            "MIDI Modwheel routing fixture did not reach the selected Sound");
    require(wheelAmount != nullptr && wheelAmount->load() > 62.0f,
            "MIDI Modwheel amount fixture did not reach the selected Sound");
    require(processor->getFirstActiveWavePosition() > 60.0f,
            "External MIDI CC1 was masked by a stationary Performance fader");

    audio.clear();
    juce::MidiBuffer none;
    processor->processBlock(audio, none);
    require(processor->getFirstActiveWavePosition() > 60.0f,
            "External MIDI CC1 ownership lasted for only one audio block");

    processor->setPanelFader(5, wave::panel::performanceFaderAdcChannels[5], 0.0f);
    audio.clear();
    processor->processBlock(audio, none);
    require(processor->getFirstActiveWavePosition() < 1.0f,
            "Moving the assigned Performance fader did not retake Modwheel control");
}

void testB019FilterEnvelopeAudibility()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(146); // B019: Chung Bass FB.
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 48, 0.9f), 0);
    auto peak = 0.0f;
    double energy = 0.0;
    for (int block = 0; block < 120; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor->processBlock(audio, block == 0 ? on : none);
        requireFinite(audio);
        peak = juce::jmax(peak, audio.getMagnitude(0, 0, 512),
                          audio.getMagnitude(1, 0, 512));
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto rms = audio.getRMSLevel(channel, 0, 512);
            energy += rms * rms;
        }
    }
    const auto rms = std::sqrt(energy / 240.0);
    require(peak > 0.07f && rms > 0.006,
            "B019's firmware-rate filter transient became barely audible");
}

void testB004StereoLayerBalance()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(131); // B004: Vox Bells, left/right/centre layers.

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        juce::MidiBuffer none;
        processor->processBlock(audio, block == 0 ? on : none);
        requireFinite(audio);
        const auto left = audio.getRMSLevel(0, 0, audio.getNumSamples());
        const auto right = audio.getRMSLevel(1, 0, audio.getNumSamples());
        leftEnergy += left * left;
        rightEnergy += right * right;
    }

    const auto leftRms = std::sqrt(leftEnergy / 80.0);
    const auto rightRms = std::sqrt(rightEnergy / 80.0);
    require(leftRms > 1.0e-4 && rightRms > 1.0e-4,
            "B004 lost one side of its factory stereo layer stack");
    require(rightRms / leftRms > 0.25 && leftRms / rightRms > 0.25,
            "B004 factory stereo layer stack became severely unbalanced");
}

void testA004LocalKeyboardRouting()
{
    auto externalMidiProcessor = std::make_unique<WaveEmulationAudioProcessor>();
    externalMidiProcessor->prepareToPlay(48000.0, 512);
    externalMidiProcessor->setCurrentProgram(3); // A004: channels 1/2, hard left/right.
    juce::AudioBuffer<float> externalAudio(2, 512);
    juce::MidiBuffer externalNote;
    externalNote.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    externalMidiProcessor->processBlock(externalAudio, externalNote);
    require(externalMidiProcessor->getActiveVoiceCount() == 1,
            "A004 external MIDI channel 1 did not retain its genuine single-Instrument routing");

    auto localKeyboardProcessor = std::make_unique<WaveEmulationAudioProcessor>();
    localKeyboardProcessor->setMidiInputActsAsLocalKeyboard(true);
    localKeyboardProcessor->prepareToPlay(48000.0, 512);
    localKeyboardProcessor->setCurrentProgram(3);
    juce::AudioBuffer<float> localAudio(2, 512);
    juce::MidiBuffer controllerNote;
    controllerNote.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    auto leftPeak = 0.0f;
    auto rightPeak = 0.0f;
    for (int block = 0; block < 8; ++block)
    {
        localAudio.clear();
        juce::MidiBuffer none;
        localKeyboardProcessor->processBlock(
            localAudio, block == 0 ? controllerNote : none);
        requireFinite(localAudio);
        leftPeak = juce::jmax(leftPeak,
                              localAudio.getMagnitude(0, 0, localAudio.getNumSamples()));
        rightPeak = juce::jmax(rightPeak,
                               localAudio.getMagnitude(1, 0, localAudio.getNumSamples()));
    }
    require(localKeyboardProcessor->getActiveVoiceCount() == 2,
            "A004 local-keyboard routing did not allocate both Keyboard+MIDI Instruments");
    require(leftPeak > 1.0e-3f && rightPeak > 1.0e-3f,
            "A004 local-keyboard routing did not restore its two-sided factory sound");
}

void testBriefPanelClicksSurviveFirmwareScan()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto& runtime = processor->getMasterFirmwareRuntime();
    for (const auto button : { 70, 71 }) // OK and Cancel/ESC contacts.
    {
        processor->setPanelButton(button, true);
        processor->setPanelButton(button, false);
        require(!runtime.panelButtonRequestedDown(button),
                "A brief OK/Cancel click left the UI request held");
        require(runtime.panelButtonPressed(button),
                "A brief OK/Cancel click vanished before a firmware scan");
        process(4);
        require(!runtime.panelButtonPressed(button),
                "A stretched OK/Cancel contact did not release after debounce");
    }
}

void testEditSectionLedsAreMutuallyExclusive()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto selectEdit = [&](int diagnosticCode) {
        const auto button
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        processor->setPanelButton(button, false);
        process(24);
    };
    const auto requireOnly = [&](int selectedButton) {
        for (const auto& indicator : wave::panel::editIndicators)
            require(processor->getPanelLed(indicator.ledSerialCode)
                        == (indicator.buttonDiagnosticCode == selectedButton),
                    "More than one Edit-section LED was illuminated");
    };

    selectEdit(60); // Filter Edit.
    requireOnly(60);
    selectEdit(86); // Oscillator Mixer Edit.
    requireOnly(86);
    selectEdit(59); // Filter-envelope Edit.
    requireOnly(59);
    selectEdit(59); // Pressing the active Edit switch closes the overlay.
    requireOnly(-1);

    for (const auto modifierEdit : { 62, 64, 65, 63 })
    {
        selectEdit(modifierEdit);
        requireOnly(modifierEdit);
        process(48);
        requireOnly(modifierEdit);
        selectEdit(modifierEdit);
        requireOnly(-1);
    }
}

void testGroupEditUsesFirmwareSerialPageAndLed()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> audio(2, 128);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int diagnosticCode) {
        const auto matrix
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        require(processor->setPanelButton(matrix, true),
                "Engine rejected a Group Edit test contact");
        processor->setPanelButton(matrix, false);
    };
    const auto finishDisplayTransaction = [&] {
        auto blocks = 0;
        while (processor->isPanelModeDisplayTransitionActive() && blocks < 384)
        {
            process(1);
            ++blocks;
        }
        require(!processor->isPanelModeDisplayTransitionActive(),
                "Group Edit firmware LCD transaction did not finish");
    };

    process(96);
    click(36); // Instrument Edit is one of the two genuine Group Edit owners.
    finishDisplayTransaction();
    process(96); // Let the Instrument page's selected-sound transaction settle.

    const auto instrumentPage
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    click(31); // Group Edit.
    require(processor->isPanelModeDisplayTransitionActive(),
            "Group Edit was not queued as a firmware page transaction");
    finishDisplayTransaction();
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot()
                != instrumentPage,
            "Group Edit did not let OS 1.700 draw its native page");
    require(processor->getMasterFirmwareRuntime().panelLed(54)
                && processor->getPanelLed(54),
            "Group Edit did not latch its genuine serial-54 LED");

    click(31); // The same physical switch exits Group Edit.
    finishDisplayTransaction();
    require(!processor->getMasterFirmwareRuntime().panelLed(54)
                && !processor->getPanelLed(54),
            "Group Edit LED remained latched after leaving its page");
}

void testInstrumentModeRoundTripsKeepSelectionAndSolo()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(41);
    juce::AudioBuffer<float> audio(2, 512);
    int expectedDuringTransition = -1;
    const auto process = [&](int count) {
        for (int block = 0; block < count; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
            if (expectedDuringTransition >= 0)
                require(processor->getSelectedPerformanceInstrument() == expectedDuringTransition,
                        "A mode transition temporarily selected a different Instrument");
        }
    };
    const auto click = [&](int code) {
        if (code == 36 || code == 39)
            expectedDuringTransition = processor->getSelectedPerformanceInstrument();
        const auto matrix = wave::panel::matrixIndexForDiagnosticCode(code);
        require(processor->setPanelButton(matrix, true), "Mode-cycle button rejected");
        process(8);
        processor->setPanelButton(matrix, false);
        process(96);
        expectedDuringTransition = -1;
    };
    process(128);
    click(25); // Select Instrument 2.
    require(processor->getSelectedPerformanceInstrument() == 1,
            "Mode-cycle setup did not select Instrument 2");
    const auto& firmware = processor->getMasterFirmwareRuntime();
    std::array<uint8_t, 192> originalSound {};
    for (size_t byte = 0; byte < originalSound.size(); ++byte)
        originalSound[byte] = firmware.currentSoundRecordByte(static_cast<uint32_t>(byte));
    const auto baselineVoice = processor->probeCurrentLayerVoice(7, 1, 60, 0.8f, 24000, 42);
    for (int round = 0; round < 3; ++round)
    {
        click(78); // Enter Solo in Performance.
        require(processor->getPanelLed(59), "Solo LED did not light in Performance");
        click(25); // Solo Instrument 2.
        click(36);
        click(26); // Select Instrument 3 for editing while Solo remains active.
        require(processor->getSelectedPerformanceInstrument() == 2
                    && firmware.currentInstrumentEditTarget() == 2,
                "Solo prevented Instrument Edit from selecting another layer");
        click(25); // Return to the original Sound before comparing its record.
        click(78); // Leave Solo from Instrument Edit.
        require(!processor->getPanelLed(59)
                    && processor->getInstrumentButtonMode()
                           == WaveEmulationAudioProcessor::InstrumentButtonMode::normal,
                "Solo did not switch off after entering Instrument Edit");
        require(processor->getSelectedPerformanceInstrument() == 1
                    && firmware.currentInstrumentEditTarget() == 1
                    && processor->isPerformanceInstrumentActive(1),
                "Mode cycling lost the selected active Instrument");
        click(39);
        require(processor->getSelectedPerformanceInstrument() == 1
                    && firmware.currentPerformanceInstrument() == 1,
                "Returning to Performance lost the selected Instrument");
        for (size_t byte = 0; byte < originalSound.size(); ++byte)
            require(firmware.currentSoundRecordByte(static_cast<uint32_t>(byte))
                        == originalSound[byte],
                    "Mode cycling changed the selected Sound record");
        const auto voice = processor->probeCurrentLayerVoice(7, 1, 60, 0.8f, 24000, 42);
        require(std::abs(voice.rmsOutput - baselineVoice.rmsOutput)
                        <= std::max(1.0e-8, baselineVoice.rmsOutput * 1.0e-4)
                    && std::abs(voice.meanCutoffHz - baselineVoice.meanCutoffHz)
                        <= std::max(1.0e-5, baselineVoice.meanCutoffHz * 1.0e-5),
                "Mode cycling changed the selected layer's rendered Sound");
    }
    click(36);
    bool sawOrange = false;
    bool sawOff = false;
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 900.0;
    while (juce::Time::getMillisecondCounterHiRes() < deadline
           && !(sawOrange && sawOff))
    {
        process(1);
        const auto green = processor->getPanelLed(98);
        const auto red = processor->getPanelLed(99);
        sawOrange = sawOrange || (green && red);
        sawOff = sawOff || (!green && !red);
        juce::Thread::sleep(1);
    }
    require(sawOrange && sawOff,
            "Selected Instrument LED stopped flashing after mode cycling");
}

void testSelectedInstrumentSoftkeyFlashesOrangeOff()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(41); // A042: multiple active Instruments.

    const auto selected = processor->getSelectedPerformanceInstrument();
    require(selected >= 0 && selected < 8,
            "No selected Instrument was available for the softkey LED test");
    const auto greenLed = 96 + selected * 2;
    const auto redLed = greenLed + 1;

    while ((juce::Time::getMillisecondCounter() / 350u) % 2u != 0u)
        juce::Thread::sleep(1);
    require(!processor->getPanelLed(greenLed)
                && !processor->getPanelLed(redLed),
            "Selected softkey retained green during its off phase");

    while ((juce::Time::getMillisecondCounter() / 350u) % 2u == 0u)
        juce::Thread::sleep(1);
    require(processor->getPanelLed(greenLed)
                && processor->getPanelLed(redLed),
            "Selected softkey did not illuminate both dies for orange");
}

void testModeLcdRedrawIsOneDisplayTransaction()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> audio(2, 128);
    const auto processOne = [&] {
        audio.clear();
        juce::MidiBuffer none;
        processor->processBlock(audio, none);
    };
    for (int block = 0; block < 640; ++block)
        processOne();

    for (const auto diagnosticCode : { 37, 36 })
    {
        const auto before
            = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        processor->setPanelButton(diagnosticCode, true);
        processor->setPanelButton(diagnosticCode, false);
        require(processor->isPanelModeDisplayTransitionActive(),
                "Mode-page LCD transaction did not start with its panel event");

        const auto maximumDisplayBlocks = diagnosticCode == 37 ? 64 : 96;
        auto blocks = 0;
        while (processor->isPanelModeDisplayTransitionActive()
               && blocks < maximumDisplayBlocks)
        {
            processOne();
            ++blocks;
        }
        require(!processor->isPanelModeDisplayTransitionActive(),
                "Mode-page LCD transaction retained an unnecessary display delay");
        const auto releasedFrame
            = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        const auto releasedPage
            = processor->getMasterFirmwareRuntime().lcdDisplayPage();
        if (releasedFrame == before)
            throw std::runtime_error(
                std::string(diagnosticCode == 37 ? "External" : "Instrument")
                + " Edit LCD transaction finished without a firmware redraw");
        // Cover delayed mode-button retry/debounce cycles as well as the
        // immediate raster pass (about 2.7 seconds at this block size).
        for (int block = 0; block < 1024; ++block)
            processOne();
        if (processor->getMasterFirmwareRuntime().lcdVideoSnapshot()
                != releasedFrame
            || processor->getMasterFirmwareRuntime().lcdDisplayPage()
                   != releasedPage)
            throw std::runtime_error(
                std::string(diagnosticCode == 37 ? "External" : "Instrument")
                + " Edit released an LCD frame before firmware finished drawing");
    }
}

void testSequencerKeyDoesNotTrapPerformanceMode()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    process(64);
    const auto& runtime = processor->getMasterFirmwareRuntime();
    const auto performanceScreen = runtime.lcdVideoSnapshot();

    const auto sequencer
        = wave::panel::matrixIndexForDiagnosticCode(35);
    processor->setPanelButton(sequencer, true);
    processor->setPanelButton(sequencer, false);
    process(8);
    require(runtime.lcdVideoSnapshot() == performanceScreen,
            "OS 1.700 unexpectedly entered a resident Sequencer page");

    const auto plus = wave::panel::matrixIndexForDiagnosticCode(72);
    processor->setPanelButton(plus, true);
    processor->setPanelButton(plus, false);
    process(96);
    processor->setPanelButton(plus, false);
    require(processor->getCurrentProgram() == 1
                && runtime.lcdVideoSnapshot() != performanceScreen,
            "The reserved Sequencer key trapped subsequent Performance controls");
}

void testDiskLoadStepDoesNotRepeat()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block) {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int code) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(code);
        require(processor->setPanelButton(button, true), "Disk Load rejected a panel contact");
        process(8);
        processor->setPanelButton(button, false);
        process(8);
    };
    process(64);
    click(58); // Disk.
    click(22); // Open the Load choices.
    process(64);
    const auto& runtime = processor->getMasterFirmwareRuntime();
    const auto initial = runtime.lcdVideoSnapshot();
    click(72);
    process(64);
    const auto advanced = runtime.lcdVideoSnapshot();
    require(advanced != initial, "Load + did not move the selection");
    process(500);
    require(runtime.lcdVideoSnapshot() == advanced,
            "Load selection kept scrolling after releasing +");
    click(69);
    process(64);
    require(runtime.lcdVideoSnapshot() == initial,
            "Load +/- did not move by exactly one menu entry");
    process(500);
    require(runtime.lcdVideoSnapshot() == initial,
            "Load selection kept scrolling after releasing -");
}

void testDiskFormatNameCursor()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    const juce::TemporaryFile temporaryDisk(".img");
    const auto disk = temporaryDisk.getFile();
    require(wave::firmware::DosFloppyImage::createEmpty(disk).wasOk(), "Test disk creation failed");
    require(processor->mountDiskImage(disk).wasOk(), "Test disk mount failed");
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block) {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int code) {
        const auto button = wave::panel::matrixIndexForDiagnosticCode(code);
        require(processor->setPanelButton(button, true), "Disk naming rejected a panel contact");
        process(8);
        processor->setPanelButton(button, false);
        process(64);
    };
    process(64);
    click(58); // Disk.
    click(79); // Format.
    click(70); // Choose the format operation.
    click(70); // Confirm the test medium to reach its name editor.
    process(3000); // Let the emulated floppy finish formatting the temporary medium.
    const auto& runtime = processor->getMasterFirmwareRuntime();
    require(processor->isFirmwareRequesterActive(), "Format name requester did not open");
    const auto cursor = runtime.localByte(0x5705fu);
    const auto character = runtime.localByte(0x56f30u + cursor);
    for (int step = 0; step < 8 && runtime.localByte(0x56f30u + cursor) == character; ++step)
    {
        processor->turnPanelEncoder(8, 1);
        process(48);
    }
    require(runtime.localByte(0x56f30u + cursor) != character,
            "Data dial did not edit the selected disk-name character");
    // The DOS editor skips padding on Right; enter a character first so
    // this checks movement by one actual name position.
    click(23);
    require(runtime.localByte(0x5705fu) == cursor + 1,
            "Disk name Page Right did not advance the firmware cursor");
    click(21);
    require(runtime.localByte(0x5705fu) == cursor,
            "Disk name Page Left did not restore the firmware cursor");
    click(71); // Cancel naming the temporary medium.
    processor.reset();
}

void testDiskCancelReturnsPerformanceModeLed()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int diagnosticCode) {
        const auto matrix
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        require(processor->setPanelButton(matrix, true),
                "Engine rejected a Disk-mode panel contact");
        process(8);
        processor->setPanelButton(matrix, false);
        process(48);
    };

    process(64);
    require(processor->getPanelLed(51),
            "Performance mode was not active before opening Disk");
    click(58); // Disk.
    require(processor->getPanelSelectedMode() == 58
                && processor->getPanelLed(52)
                && !processor->getPanelLed(51),
            "Disk page LED went out when its momentary button was released");
    click(71); // Cancel / ESC.

    require(processor->getPanelSelectedMode() == 39
                && processor->getPanelLed(51)
                && !processor->getPanelLed(74)
                && !processor->getPanelLed(52),
            "Disk Cancel did not restore the exclusive Performance mode LED");
}

void testExclusiveModesRejectOtherModeLeds()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    const auto process = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            juce::MidiBuffer none;
            processor->processBlock(audio, none);
        }
    };
    const auto click = [&](int diagnosticCode) {
        const auto matrix
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        const auto accepted = processor->setPanelButton(matrix, true);
        process(8);
        processor->setPanelButton(matrix, false);
        process(48);
        return accepted;
    };

    process(64);
    constexpr std::array modeCodes {
        38, 33, 35, 34, 32, 37, 36, 39, 58, 57
    };
    constexpr std::array modeLeds {
        64, 69, 21, 74, 26, 10, 22, 51, 52, 87
    };
    constexpr std::array exclusiveModes { 34, 32, 58, 57 };
    constexpr std::array exclusiveLeds { 74, 26, 52, 87 };
    for (size_t exclusive = 0; exclusive < exclusiveModes.size(); ++exclusive)
    {
        require(click(exclusiveModes[exclusive]),
                "Engine rejected entry into an exclusive mode");
        require(processor->getPanelSelectedMode() == exclusiveModes[exclusive]
                    && processor->getPanelLed(exclusiveLeds[exclusive]),
                "Exclusive mode did not own its operating-mode selection");

        for (size_t target = 0; target < modeCodes.size(); ++target)
        {
            if (modeCodes[target] == exclusiveModes[exclusive])
                continue;
            (void) click(modeCodes[target]);
            require(processor->getPanelSelectedMode() == exclusiveModes[exclusive]
                        && processor->getPanelLed(exclusiveLeds[exclusive])
                        && !processor->getPanelLed(modeLeds[target]),
                    "A blocked mode button displaced an exclusive mode LED");
        }

        require(click(71), "Engine rejected the exclusive mode's Cancel exit");
        require(processor->getPanelSelectedMode() == 39
                    && processor->getPanelLed(51)
                    && !processor->getPanelLed(exclusiveLeds[exclusive]),
                "Exclusive mode Cancel did not restore Performance mode");
    }
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string_view(argv[1]) == "--disk-step")
        {
            testDiskLoadStepDoesNotRepeat();
            std::cout << "Disk Load step regression passed\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--disk-name")
        {
            testDiskFormatNameCursor();
            std::cout << "Disk naming regression passed\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--program-names")
        {
            testPerformanceBrowserHasDistinctStoredRecords();
            std::cout << "Performance browser names regression passed\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--mode-selection")
        {
            testInstrumentModeRoundTripsKeepSelectionAndSolo();
            testInstrumentEditLayerSelection();
            testPerformanceMuteAndSolo();
            testSelectedInstrumentSoftkeyFlashesOrangeOff();
            std::cout << "Mode selection regression passed\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--disk-total-recall")
        {
            testDiskSetSerialSelectionUpdatesDsp(false);
            std::cout << "Disk Total Recall regression passed\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--instrument-source")
        {
            testInstrumentSourceKeepsOtherLayers();
            testInstrumentEditLayerSelection();
            std::cout << "Instrument Source regression passed\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--store-name")
        {
            testRepeatedSoundStoreCursor();
            testStoreRequesterStepButtonsChooseDestination();
            testStoreCancelRestoresNumericPerformancePreview();
            std::cout << "Store Page-key routing regression passed\n";
            return 0;
        }
        testAutomaticBootAudioAndState();
        testFactoryA092ChoirVibratoUsesCallerScale();
        testFactoryB057UsesMeasuredVcaAttackOne();
        testA030UsesItsSetUserWavetable();
        testA044UsesNineteenTwentySpeechTable();
        testLegacyStartupStateReloadsExactFactoryPerformance();
        testFactoryVcaReleaseTail();
        testFactoryPerformanceLayering();
        testPerformanceBrowserHasDistinctStoredRecords();
        testRapidPerformanceLcdRefreshIsAtomic();
        testFirmwarePerformanceStepButtonsRefreshLcd();
        testStoreButtonReachesFirmwareMenu();
        testStoreRequesterStepButtonsChooseDestination();
        testRepeatedSoundStoreCursor();
        testStoreCancelRestoresNumericPerformancePreview();
        testKeyboardControllerShiftReachesFirmware();
        testLowerKeyboardAssignableButtonsDriveFirmware();
        testGlideEditUsesFirmwareLamp();
        testKeyboardOctaveButtonsDriveLocalKeyboardRange();
        testPluginMidiFollowsKeyboardOctaveButtons();
        testShiftDisplaySevenOpensFirmwareServiceMenu();
        testFirmwareVcfCalibrationTableFeedsAllVoices();
        testFactoryA001VoiceCardsTrackTheSameFilter();
        testFactoryA001HeldCutoffUsesFirmwareEnvelopeScale();
        testFactoryA001OverlappingChordNoteOffKeepsRetriggeredVoices();
        testFactoryA001CapturedChordSequenceKeepsEveryVcaOpen();
        testDiskSetSerialSelectionUpdatesDsp();
        testSamePerformanceReselectionLeavesModalScreen();
        testPerformanceInstrumentSelection();
        testSelectedInstrumentSoundRecordOverridesOldPanelSnapshot();
        testOutgoingInstrumentIsSavedFromFirmwareNotPanelMirror();
        testFactoryStereoInstrumentSelectionDoesNotChangeSound();
        testA001DuplicateSoundAssignmentsHavePrivateInstrumentEdits();
        testInstrumentButtonsDoNotRewriteLayerOctaves();
        testInstrumentEditLayerSelection();
        testInstrumentSourceKeepsOtherLayers();
        testPerformanceMuteAndSolo();
        testA013ComparatorAutoPan();
        testFactoryMultimodeFilterLoading();
        testPanelEditButtonFirmwareRouting();
        testArtworkControlsMatchCompleteWiringMap();
        testKnobModeSelectUsesFirmwarePanelPath();
        testOscillatorOctaveButtonsDriveFirmwareSoundRecord();
        testEditPageFadersDriveFirmwareValues();
        testEditPageRefreshesAfterFirmwarePerformanceChange();
        testEditSectionKnobsDriveFirmwareValues();
        testEndlessPanelDialDrivesFirmwareValue();
        testPhysicalGlideControlsWorkOutsideEditPage();
        testBriefGlideSwitchClicksSurviveFirmwareScan();
        testGlideEditPageDrivesNativeSoundAndDsp();
        testWaveLinkUsesFirmwareLatch();
        testPlusTapAfterDataDialAdvancesOneStep();
        testProgramStateDoesNotMasqueradeAsPanelMovement();
        testProgramRecallRejectsPreviousHostPanelEcho();
        testHostStatePreservesRelativePanelPotPositions();
        testRecallInitModalAcceptsDisplaySoftKeys();
        testFirmwareModifierSelectorDrivesDsp();
        testEditedPerformanceFaderRoutingDrivesDsp();
        testB019FilterEnvelopeAudibility();
        testB004StereoLayerBalance();
        testA004LocalKeyboardRouting();
        testBriefPanelClicksSurviveFirmwareScan();
        testEditSectionLedsAreMutuallyExclusive();
        testGroupEditUsesFirmwareSerialPageAndLed();
        testSelectedInstrumentSoftkeyFlashesOrangeOff();
        testInstrumentModeRoundTripsKeepSelectionAndSolo();
        testModeLcdRedrawIsOneDisplayTransaction();
        testSequencerKeyDoesNotTrapPerformanceMode();
        testDiskLoadStepDoesNotRepeat();
        testDiskFormatNameCursor();
        testDiskCancelReturnsPerformanceModeLed();
        testExclusiveModesRejectOtherModeLeds();
        std::cout << "WaveIntegrationTests: all checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WaveIntegrationTests: " << error.what() << '\n';
        return 1;
    }
}
