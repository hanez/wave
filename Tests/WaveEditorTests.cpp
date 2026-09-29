#include "PluginEditor.h"
#include "PanelWiring.h"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <typeinfo>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void processBlocks(WaveEmulationAudioProcessor& processor,
                   juce::AudioBuffer<float>& audio, int count)
{
    for (int block = 0; block < count; ++block)
    {
        audio.clear();
        juce::MidiBuffer noMidi;
        processor.processBlock(audio, noMidi);
    }
}

void clickPanelControl(WaveEmulationAudioProcessorEditor& editor,
                       WaveEmulationAudioProcessor& processor,
                       juce::AudioBuffer<float>& audio,
                       float panelX, float panelY, int heldBlocks = 8,
                       std::function<void()> afterMouseDown = {})
{
    constexpr auto panelHorizontalOffset = 66.0f;
    const auto position = juce::Point<float> {
        panelX + panelHorizontalOffset, panelY
    };
    const auto* target = editor.getComponentAt(position.roundToInt());
    if (target != &editor)
    {
        const auto description
            = "A child component intercepted a panel-switch click at "
              + std::to_string(position.x) + "," + std::to_string(position.y)
              + ": " + (target != nullptr ? typeid(*target).name() : "null");
        throw std::runtime_error(description);
    }

    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto time = juce::Time::getCurrentTime();
    const juce::MouseEvent down {
        source, position,
        juce::ModifierKeys { juce::ModifierKeys::leftButtonModifier },
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor,
        time, position, time, 1, false
    };
    editor.mouseDown(down);
    if (afterMouseDown)
        afterMouseDown();
    if (heldBlocks > 0)
        processBlocks(processor, audio, heldBlocks);

    const juce::MouseEvent up {
        source, position,
        juce::ModifierKeys { juce::ModifierKeys::leftButtonModifier },
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor,
        juce::Time::getCurrentTime(), position, time, 1, false
    };
    editor.mouseUp(up);
}

void dragEditorControl(WaveEmulationAudioProcessorEditor& editor,
                       juce::Point<float> start, juce::Point<float> end,
                       const std::function<void()>& whileHeld)
{
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto downTime = juce::Time::getCurrentTime();
    const juce::MouseEvent down {
        source, start,
        juce::ModifierKeys { juce::ModifierKeys::leftButtonModifier },
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor,
        downTime, start, downTime, 1, false
    };
    editor.mouseDown(down);
    const juce::MouseEvent drag {
        source, end,
        juce::ModifierKeys { juce::ModifierKeys::leftButtonModifier },
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor,
        juce::Time::getCurrentTime(), start, downTime, 1, true
    };
    editor.mouseDrag(drag);
    whileHeld();
    const juce::MouseEvent up {
        source, end, juce::ModifierKeys {},
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor,
        juce::Time::getCurrentTime(), start, downTime, 1, true
    };
    editor.mouseUp(up);
}

void testWindowShortcutsPreservePanelLayout()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    auto* lcd = static_cast<juce::Component*>(nullptr);
    for (int index = 0; index < editor.getNumChildComponents(); ++index)
    {
        auto* child = editor.getChildComponent(index);
        if (dynamic_cast<wave::ui::WaveLcdComponent*>(child) != nullptr)
            lcd = child;
    }
    require(lcd != nullptr, "Could not find the firmware LCD in the editor");
    const auto fullLcdBounds = lcd->getBounds();
    const auto pressCommand = [&](int keyCode) {
        return editor.keyPressed(juce::KeyPress(
            keyCode, juce::ModifierKeys { juce::ModifierKeys::commandModifier }, 0));
    };

    require(editor.getWidth() == 2338 && editor.getHeight() == 1042,
            "Editor did not open at its default scale");
    require(pressCommand('k') && editor.getWidth() == 2338
                && editor.getHeight() == 619 && lcd->getBounds() == fullLcdBounds,
            "Hide Keyboard distorted or moved the upper panel");
    require(std::abs(editor.getConstrainer()->getFixedAspectRatio()
                         - 2338.0 / 619.0) < 0.001,
            "Hidden keyboard kept the full-height resize aspect ratio");
    require(pressCommand('+') && editor.getWidth() == 2572
                && editor.getHeight() == 681,
            "Zoom In did not resize the cropped panel");
    require(pressCommand('-') && editor.getWidth() == 2338
                && editor.getHeight() == 619,
            "Zoom Out did not restore the cropped panel size");
    require(pressCommand('k') && editor.getWidth() == 2338
                && editor.getHeight() == 1042
                && lcd->getBounds() == fullLcdBounds,
            "Show Keyboard did not restore the full editor");
    require(pressCommand('-') && editor.getWidth() == 2104,
            "Zoom Out did not shrink the full editor");
    require(pressCommand('0') && editor.getWidth() == 2338
                && editor.getHeight() == 1042,
            "Actual Size did not restore the default editor size");
}

void testLowerPerformanceWheelsFollowHardwareRules()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);

    dragEditorControl(editor, { 110.5f, 879.5f }, { 110.5f, 830.0f }, [&] {
        require(processor->getPitchWheelForUi() > 0.8f,
                "The visible Pitch Bend wheel did not send pitch-wheel data");
    });
    require(std::abs(processor->getPitchWheelForUi() - 8192.0f / 16383.0f)
                < 0.001f,
            "Pitch Bend did not return to its spring centre");

    dragEditorControl(editor, { 169.5f, 879.5f }, { 169.5f, 830.0f }, [&] {
        require(processor->getModWheelForUi() > 0.35f,
                "The visible Mod wheel did not send controller data");
    });
    require(processor->getModWheelForUi() > 0.35f,
            "The Mod wheel did not retain its physical position");

    dragEditorControl(editor, { 228.5f, 879.5f }, { 228.5f, 930.0f }, [&] {
        require(processor->getFreeWheelForUi() < 0.2f,
                "The visible Free wheel did not send its bipolar controller data");
    });
    require(processor->getFreeWheelForUi() < 0.2f,
            "The freely assignable bipolar wheel did not retain its position");
}

void testRepeatedInstrumentEditLayerClicks()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(41); // A042: three active Instruments.
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 96);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    clickPanelControl(editor, *processor, audio,
                      1402.5f, 494.0f); // Instrument Edit.
    processBlocks(*processor, audio, 48);

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
            "A042 did not expose two selectable Instruments");

    constexpr std::array<float, 8> instrumentX {
        903.0f, 959.0f, 1014.0f, 1070.0f,
        1125.0f, 1181.0f, 1236.0f, 1292.0f
    };
    for (int repetition = 0; repetition < 12; ++repetition)
    {
        const auto expected = (repetition & 1) == 0 ? alternate : original;
        clickPanelControl(editor, *processor, audio,
                          instrumentX[static_cast<size_t>(expected)], 199.0f,
                          repetition % 3 == 0 ? 0 : 8);
        processBlocks(*processor, audio, 160);
        if (processor->getSelectedPerformanceInstrument() != expected)
            throw std::runtime_error(
                "A repeated editor click did not select its Instrument: expected "
                + std::to_string(expected) + ", repetition "
                + std::to_string(repetition) + ", original "
                + std::to_string(original) + ", alternate "
                + std::to_string(alternate) + ", host "
                + std::to_string(processor->getSelectedPerformanceInstrument())
                + ", firmware "
                + std::to_string(processor->getMasterFirmwareRuntime()
                                     .currentPerformanceInstrument().value_or(-1))
                + ", active action "
                + std::to_string(
                    (processor->getMasterFirmwareRuntime().localByte(0x58fe4u)
                     << 8u)
                    | processor->getMasterFirmwareRuntime().localByte(0x58fe5u))
                + ", mode action "
                + std::to_string(processor->getMasterFirmwareRuntime().localByte(
                    0x2850eu + 36u)));
        const auto firmwareInstrument = processor->getMasterFirmwareRuntime()
                                            .currentPerformanceInstrument();
        if (firmwareInstrument != expected)
        {
            const auto diagnosticCode = std::array<int, 8> {
                22, 25, 26, 27, 79, 28, 29, 30
            }[static_cast<size_t>(expected)];
            const auto matrix
                = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
            throw std::runtime_error(
                "The editor and firmware layer selections diverged at repetition "
                + std::to_string(repetition) + ": expected "
                + std::to_string(expected) + ", firmware "
                + std::to_string(firmwareInstrument.value_or(-1))
                + ", requested "
                + std::to_string(processor->getMasterFirmwareRuntime()
                                     .panelButtonRequestedDown(matrix))
                + ", contact "
                + std::to_string(processor->getMasterFirmwareRuntime()
                                     .panelButtonPressed(matrix)));
        }
    }
}

void testLayerProcessingPromptAcceptsCancel()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 640);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    clickPanelControl(editor, *processor, audio,
                      1402.5f, 494.0f); // Instrument Edit.
    processBlocks(*processor, audio, 400);
    for (int page = 0; page < 3; ++page)
    {
        clickPanelControl(editor, *processor, audio,
                          942.0f, 553.0f); // Page right to Zoning.
        processBlocks(*processor, audio, 160);
    }
    const auto zoning
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    clickPanelControl(editor, *processor, audio,
                      903.0f, 199.0f, 32); // Layer display button.
    processBlocks(*processor, audio, 160);
    const auto prompt
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    require(prompt != zoning,
            "The Layer display key did not open its firmware requester");
    require(processor->isFirmwareRequesterActive(),
            "The engine did not recognise the genuine Layer requester");

    clickPanelControl(editor, *processor, audio,
                      1402.5f, 553.0f); // Performance mode is modal-blocked.
    processBlocks(*processor, audio, 80);
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() == prompt,
            "A mode button escaped from an OK/Cancel-only requester");
    require(processor->getPanelLed(22) && !processor->getPanelLed(51),
            "A blocked Performance press changed the mode LEDs");

    clickPanelControl(editor, *processor, audio,
                      1078.0f, 553.0f, 0); // Cancel / ESC.
    processBlocks(*processor, audio, 240);
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() == zoning,
            "Cancel did not dismiss the two-Instrument Layer requester");
    require(!processor->isFirmwareRequesterActive(),
            "The firmware requester lock remained active after Cancel");

    constexpr std::array<float, 8> softKeyX {
        903.0f, 959.0f, 1014.0f, 1070.0f,
        1125.0f, 1181.0f, 1236.0f, 1292.0f
    };
    for (size_t key = 1; key < 6; ++key)
    {
        clickPanelControl(editor, *processor, audio,
                          softKeyX[key], 199.0f, 32);
        processBlocks(*processor, audio, 160);
        require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() != zoning,
                "A visible Instrument Zoning softkey was not selectable");
        clickPanelControl(editor, *processor, audio,
                          1078.0f, 553.0f, 0);
        processBlocks(*processor, audio, 240);
        require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() == zoning,
                "Cancel did not return from an Instrument Zoning softkey");
    }

    clickPanelControl(editor, *processor, audio,
                      softKeyX[7], 199.0f, 32); // Detail.
    processBlocks(*processor, audio, 160);
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() != zoning,
            "The Instrument Zoning Detail softkey was not selectable");
}

void testFirmwareWavetableStepReachesAudioParameter()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 64);

    const auto click = [&](int diagnosticCode) {
        const auto button
            = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
        processor->setPanelButton(button, true);
        processBlocks(*processor, audio, 8);
        processor->setPanelButton(button, false);
        processBlocks(*processor, audio, 48);
    };

    click(36); // Instrument Edit.
    click(8);  // Leave a nested edit selector active.
    processor->turnPanelEncoder(8, 1); // The large Wavetable/Data dial.
    processBlocks(*processor, audio, 32);
    const auto* audioValue
        = processor->parameters.getRawParameterValue(wave::parameters::wavetable);
    require(audioValue != nullptr, "The Wavetable audio parameter is missing");
    const auto before = juce::roundToInt(audioValue->load());
    click(72); // Plus.
    require(juce::roundToInt(audioValue->load()) == juce::jmin(64, before + 1),
            "A Wavetable Plus press changed the LCD but not the audio parameter");

    processor->setPanelPotValue(wave::parameters::position, 0.5f);
    const auto afterWavetableStep = juce::roundToInt(audioValue->load());
    click(72);
    require(juce::roundToInt(audioValue->load()) == afterWavetableStep,
            "Plus still changed Wavetable after another knob selected a new field");
}

void testInstrumentEditPageOneFadersUpdatePerformance()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 640);
    const auto click = [&](int code) {
        processor->setPanelButton(code, true);
        processBlocks(*processor, audio, 8);
        processor->setPanelButton(code, false);
        processBlocks(*processor, audio, 200);
    };
    click(36);
    auto stableSelectionBlocks = 0;
    for (int block = 0; block < 800 && stableSelectionBlocks < 96; ++block)
    {
        processBlocks(*processor, audio, 1);
        const auto firmware = processor->getMasterFirmwareRuntime()
                                  .currentPerformanceInstrument();
        if (firmware.has_value()
            && *firmware == processor->getSelectedPerformanceInstrument())
            ++stableSelectionBlocks;
        else
            stableSelectionBlocks = 0;
    }
    require(stableSelectionBlocks == 96,
            "Instrument Edit Page 1 selection did not settle");
    auto instrument = processor->getSelectedPerformanceInstrument();
    require(instrument >= 0, "Instrument Edit Page 1 has no selected Instrument");
    const auto instrumentBase = [&] {
        const auto performanceOffset = processor->getMasterFirmwareRuntime()
                                           .currentPerformanceRecordOffset();
        require(performanceOffset.has_value(),
                "Instrument Edit Page 1 has no native Performance record");
        return *performanceOffset + 64u
               + static_cast<uint32_t>(
                     processor->getSelectedPerformanceInstrument()) * 32u;
    };
    constexpr auto channels = wave::panel::performanceFaderAdcChannels;
    constexpr std::array<uint32_t, 8> offsets {
        4u, 5u, 7u, 8u, 9u, 10u, 2u, 3u
    };
    constexpr std::array<float, 8> positions {
        0.50f, 0.25f, 0.25f, 0.75f,
        0.25f, 0.75f, 0.75f, 0.34f
    };
    constexpr std::array<int, 8> expected {
        64, 32, 32, 2, 32, 95, 12, 1
    };

    processor->setPanelFader(3, channels[3], 0.0f, false); // Main output.
    processor->setPanelFader(7, channels[7], 1.0f, false); // Keys + MIDI.
    processor->setPanelFader(0, channels[0], 0.0f, false);
    processBlocks(*processor, audio, 32);
    instrument = processor->getSelectedPerformanceInstrument();
    const auto quiet = processor->probeCurrentLayerVoice(
        0, instrument, 60, 0.8f, 24000, 1).rmsOutput;

    for (size_t fader = 0; fader < channels.size(); ++fader)
    {
        std::array<uint8_t, 32> before {};
        for (size_t offset = 0; offset < before.size(); ++offset)
            before[offset] = processor->getMasterFirmwareRuntime().sharedProgramByte(
                instrumentBase() + static_cast<uint32_t>(offset));
        processor->setPanelFader(static_cast<int>(fader), channels[fader],
                                 positions[fader], false);
        processBlocks(*processor, audio, 32);
        const auto stored = static_cast<int>(
            processor->getMasterFirmwareRuntime().sharedProgramByte(
                instrumentBase() + offsets[fader]) & 0x7fu);
        if (stored != expected[fader])
            throw std::runtime_error(
                "Instrument Edit Page 1 fader did not commit its native Performance byte: fader "
                + std::to_string(fader) + ", expected "
                + std::to_string(expected[fader]) + ", observed "
                + std::to_string(stored));
        for (size_t offset = 0; offset < before.size(); ++offset)
            if (offset != offsets[fader])
                require(processor->getMasterFirmwareRuntime().sharedProgramByte(
                            instrumentBase() + static_cast<uint32_t>(offset)) == before[offset],
                        "Page 1 fader changed another Instrument field");
    }

    // Volume must reach the live layer, not just its LCD/record value.
    processor->setPanelFader(3, channels[3], 0.0f, false); // Main output.
    processor->setPanelFader(7, channels[7], 1.0f, false); // Keys + MIDI.
    processBlocks(*processor, audio, 32);
    const auto audible = processor->probeCurrentLayerVoice(
        0, instrument, 60, 0.8f, 24000, 2).rmsOutput;
    if (!(quiet < 1.0e-8 && audible > 1.0e-4))
        throw std::runtime_error(
            "Instrument Volume changed its record but not the audio engine: quiet "
            + std::to_string(quiet) + ", audible " + std::to_string(audible));

}

void testInstrumentPageOneFaderDoesNotLeakIntoOtherPages()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 640);
    const auto click = [&](int code) {
        processor->setPanelButton(code, true);
        processBlocks(*processor, audio, 8);
        processor->setPanelButton(code, false);
        processBlocks(*processor, audio, 200);
    };
    click(36);
    auto stableSelectionBlocks = 0;
    for (int block = 0; block < 800 && stableSelectionBlocks < 96; ++block)
    {
        processBlocks(*processor, audio, 1);
        const auto firmware = processor->getMasterFirmwareRuntime()
                                  .currentPerformanceInstrument();
        if (firmware.has_value()
            && *firmware == processor->getSelectedPerformanceInstrument())
            ++stableSelectionBlocks;
        else
            stableSelectionBlocks = 0;
    }
    require(stableSelectionBlocks == 96,
            "Instrument Edit Page 1 selection did not settle for isolation test");

    const auto instrument = processor->getSelectedPerformanceInstrument();
    const auto performanceOffset = processor->getMasterFirmwareRuntime()
                                       .currentPerformanceRecordOffset();
    require(instrument >= 0 && performanceOffset.has_value(),
            "Instrument Edit Page 1 has no record for isolation test");
    const auto base = *performanceOffset + 64u
                      + static_cast<uint32_t>(instrument) * 32u;
    std::array<uint8_t, 32> before {};
    for (size_t offset = 0; offset < before.size(); ++offset)
        before[offset] = processor->getMasterFirmwareRuntime().sharedProgramByte(
            base + static_cast<uint32_t>(offset));

    constexpr auto audioOutFader = 3;
    processor->setPanelFader(
        audioOutFader,
        wave::panel::performanceFaderAdcChannels[audioOutFader], 0.92f, false);
    processBlocks(*processor, audio, 1200);

    constexpr auto audioOutRecordOffset = size_t { 8 };
    require((processor->getMasterFirmwareRuntime().sharedProgramByte(
                 base + static_cast<uint32_t>(audioOutRecordOffset))
             & 0x7fu)
                == 3u,
            "Instrument Audio Out fader did not update its Page 1 field");
    for (size_t offset = 0; offset < before.size(); ++offset)
    {
        if (offset == audioOutRecordOffset)
            continue;
        if (processor->getMasterFirmwareRuntime().sharedProgramByte(
                base + static_cast<uint32_t>(offset)) != before[offset])
            throw std::runtime_error(
                "Instrument Page 1 fader leaked into another page byte: "
                + std::to_string(offset));
    }
}

void testInstrumentEditPageTwoVolumeReachesAudioEngine()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 640);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    clickPanelControl(editor, *processor, audio,
                      1402.5f, 494.0f); // Instrument Edit, Page 1.
    processBlocks(*processor, audio, 320);
    clickPanelControl(editor, *processor, audio,
                      942.0f, 553.0f); // Page right to Page 2.
    processBlocks(*processor, audio, 320);
    require(processor->getInstrumentEditPage() == 1,
            "Instrument Edit did not reach Page 2 for its audio test");

    // Firmware remembers Page 2 across mode changes. The host must not reset
    // its fader destinations to Page 1 when Instrument Edit is reopened.
    clickPanelControl(editor, *processor, audio, 1402.5f, 553.0f);
    clickPanelControl(editor, *processor, audio, 1402.5f, 494.0f);
    processBlocks(*processor, audio, 320);
    require(processor->getMasterFirmwareRuntime().localByte(0x56ee9u) == 1u,
            "Firmware did not restore Instrument Edit Page 2");
    require(processor->getInstrumentEditPage() == 1,
            "Reopening Instrument Edit routed Page 2 faders to Page 1");

    const auto instrument = processor->getSelectedPerformanceInstrument();
    const auto performanceOffset = processor->getMasterFirmwareRuntime()
                                       .currentPerformanceRecordOffset();
    require(instrument >= 0 && performanceOffset.has_value(),
            "Instrument Edit Page 2 has no selected native record");
    const auto instrumentBase = [&] {
        const auto currentOffset = processor->getMasterFirmwareRuntime()
                                       .currentPerformanceRecordOffset();
        require(currentOffset.has_value(),
                "Instrument Edit Page 2 lost its native Performance record");
        return *currentOffset + 64u
               + static_cast<uint32_t>(instrument) * 32u;
    };
    // Compare every reachable ADC value with the actual firmware-rendered
    // TuneTable word. The native display has 13 uneven bins, including a
    // transition between two ADC values that share one seven-bit position.
    const auto tuningScreen = [&] {
        const auto& runtime = processor->getMasterFirmwareRuntime();
        const auto video = runtime.lcdVideoSnapshot();
        wave::ui::LcdFramebuffer framebuffer;
        framebuffer.loadHardwareVideoRam(video.data(), video.size(),
                                         runtime.lcdDisplayPage());
        std::array<uint8_t, 600> word {};
        for (int y = 54; y < 64; ++y)
            for (int x = 240; x < 300; ++x)
                word[static_cast<size_t>((y - 54) * 60 + x - 240)]
                    = static_cast<uint8_t>(framebuffer.pixel(x, y));
        return word;
    };
    const auto setTuningFader = [&](int physical) {
        processor->setPanelFader(
            4, wave::panel::performanceFaderAdcChannels[4],
            static_cast<float>(physical) / 127.0f, false);
        processBlocks(*processor, audio, 64);
        return tuningScreen();
    };
    std::array<std::array<uint8_t, 600>, 13> tuningWords {};
    for (int selector = 0; selector < 13; ++selector)
    {
        const auto first = (selector * 128 + 12) / 13;
        const auto last = ((selector + 1) * 128 - 1) / 13;
        tuningWords[static_cast<size_t>(selector)]
            = setTuningFader((first + last) / 2);
        if (selector > 0)
            require(tuningWords[static_cast<size_t>(selector)]
                        != tuningWords[static_cast<size_t>(selector - 1)],
                    "Two TuneTable selectors rendered the same LCD word");
    }
    juce::MidiBuffer heldNote;
    heldNote.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    audio.clear();
    processor->processBlock(audio, heldNote);
    processBlocks(*processor, audio, 32);
    require(processor->getActiveVoiceCount() > 0,
            "TuneTable sweep did not hold a sounding note");
    std::array<uint8_t, 32> recordBefore {};
    for (size_t offset = 0; offset < recordBefore.size(); ++offset)
        recordBefore[offset] = processor->getMasterFirmwareRuntime()
                                   .sharedProgramByte(instrumentBase()
                                                      + static_cast<uint32_t>(offset));
    const auto soundRecordOffset = processor->getMasterFirmwareRuntime()
                                       .performanceInstrumentSoundRecordOffset(instrument);
    require(soundRecordOffset.has_value(),
            "TuneTable sweep could not locate the active Sound record");
    std::array<uint8_t, wave::presets::WaveFactorySet::soundSize> soundBefore {};
    for (size_t offset = 0; offset < soundBefore.size(); ++offset)
        soundBefore[offset] = processor->getMasterFirmwareRuntime()
                                  .sharedProgramByte(*soundRecordOffset
                                                     + static_cast<uint32_t>(offset));
    const auto snapshotBefore = processor->getCurrentPerformanceSnapshot();
    for (int adc = 0; adc < 256; ++adc)
    {
        processor->setPanelFader(
            4, wave::panel::performanceFaderAdcChannels[4],
            static_cast<float>(adc) / 255.0f, false);
        processBlocks(*processor, audio, 64);
        const auto screen = tuningScreen();
        const auto stored = static_cast<int>(
            processor->getMasterFirmwareRuntime()
                .sharedProgramByte(instrumentBase() + 22u)
            & 0x7fu);
        const auto audible = processor->getCurrentPerformanceSnapshot()
                                 .layers[static_cast<size_t>(instrument)]
                                 .tuningTable;
        auto displayed = -1;
        for (int candidate = 0; candidate < 13; ++candidate)
            if (screen == tuningWords[static_cast<size_t>(candidate)])
                displayed = candidate;
        if (displayed < 0 || stored != displayed || audible != displayed)
            throw std::runtime_error(
                "TuneTable LCD and audio disagree at ADC "
                + std::to_string(adc) + ": LCD "
                + std::to_string(displayed) + ", record "
                + std::to_string(stored) + ", engine "
                + std::to_string(audible));
        for (size_t offset = 0; offset < recordBefore.size(); ++offset)
            if (offset != 22u
                && processor->getMasterFirmwareRuntime().sharedProgramByte(
                       instrumentBase() + static_cast<uint32_t>(offset))
                       != recordBefore[offset])
                throw std::runtime_error(
                    "Held-note TuneTable edit changed Instrument record byte "
                    + std::to_string(offset) + " at ADC "
                    + std::to_string(adc));
        for (size_t offset = 0; offset < soundBefore.size(); ++offset)
            if (processor->getMasterFirmwareRuntime().sharedProgramByte(
                    *soundRecordOffset + static_cast<uint32_t>(offset))
                != soundBefore[offset])
                throw std::runtime_error(
                    "Held-note TuneTable edit changed Sound record byte "
                    + std::to_string(offset) + " at ADC "
                    + std::to_string(adc));
        const auto current = processor->getCurrentPerformanceSnapshot();
        const auto& currentLayer = current.layers[static_cast<size_t>(instrument)];
        const auto& previousLayer
            = snapshotBefore.layers[static_cast<size_t>(instrument)];
        if (currentLayer.transposeSemitones != previousLayer.transposeSemitones
            || currentLayer.sound.oscillatorOctaves
                   != previousLayer.sound.oscillatorOctaves)
            throw std::runtime_error(
                "Held-note TuneTable edit changed transpose or oscillator octave at ADC "
                + std::to_string(adc));
    }
    // ADC 55 is in the displayed HMT band, but the old seven-bit mapping
    // selected Linear- in the engine. A second external MIDI key exposes the
    // error as a large downward pitch jump while the first key stays held.
    processor->setPanelFader(
        4, wave::panel::performanceFaderAdcChannels[4], 55.0f / 255.0f, false);
    processBlocks(*processor, audio, 64);
    require(processor->getCurrentPerformanceSnapshot()
                .layers[static_cast<size_t>(instrument)].tuningTable == 2,
            "Displayed HMT band selected another audible tuning");
    juce::MidiBuffer secondHeldNote;
    secondHeldNote.addEvent(juce::MidiMessage::noteOn(1, 72, 0.9f), 0);
    processor->processBlock(audio, secondHeldNote);
    processBlocks(*processor, audio, 4);
    auto secondNotePitch = -1.0f;
    for (const auto& voice : processor->getVoiceStates())
        if (voice.active && voice.keyDown && voice.triggerNote == 72)
            secondNotePitch = voice.glidePitch;
    require(std::abs(secondNotePitch - 72.0f) < 1.0e-4f,
            "New external MIDI note fell to Linear- pitch while HMT was displayed");
    processor->allSoundOffFromUi();
    processBlocks(*processor, audio, 8);
    constexpr auto volumeFader = 0;
    constexpr auto volumeOffset = 4u;
    constexpr std::array<uint32_t, 8> pageTwoOffsets {
        4u, 5u, 7u, 21u, 22u, 15u, 16u, 11u
    };
    constexpr std::array<float, 8> pageTwoPositions {
        0.50f, 0.25f, 0.25f, 0.75f,
        0.25f, 0.75f, 0.75f, 0.75f
    };
    constexpr std::array<int, 8> pageTwoExpected {
        64, 32, 32, 8, 3, 16, 3, 2
    };

    for (size_t fader = 0; fader < pageTwoOffsets.size(); ++fader)
    {
        // Exercise the retained page on every return, not only initial entry.
        clickPanelControl(editor, *processor, audio, 1402.5f, 553.0f);
        clickPanelControl(editor, *processor, audio, 1402.5f, 494.0f);
        processBlocks(*processor, audio, 64);
        std::array<uint8_t, 32> before {};
        for (size_t offset = 0; offset < before.size(); ++offset)
            before[offset] = processor->getMasterFirmwareRuntime().sharedProgramByte(
                instrumentBase() + static_cast<uint32_t>(offset));
        processor->setPanelFader(
            static_cast<int>(fader),
            wave::panel::performanceFaderAdcChannels[fader],
            pageTwoPositions[fader], false);
        processBlocks(*processor, audio, 32);
        const auto stored = static_cast<int>(
            processor->getMasterFirmwareRuntime().sharedProgramByte(
                instrumentBase() + pageTwoOffsets[fader])
            & 0x7fu);
        if (stored != pageTwoExpected[fader])
            throw std::runtime_error(
                "Instrument Edit Page 2 fader did not commit its native field: fader "
                + std::to_string(fader) + ", expected "
                + std::to_string(pageTwoExpected[fader]) + ", observed "
                + std::to_string(stored) + ", host page "
                + std::to_string(processor->getInstrumentEditPage())
                + ", firmware page "
                + std::to_string(processor->getMasterFirmwareRuntime()
                                     .currentInstrumentEditPage().value_or(-1)));
        for (size_t offset = 0; offset < before.size(); ++offset)
            if (offset != pageTwoOffsets[fader])
                require(processor->getMasterFirmwareRuntime().sharedProgramByte(
                            instrumentBase() + static_cast<uint32_t>(offset)) == before[offset],
                        "Page 2 fader changed another Instrument field after mode return");
    }

    const auto decodedPageTwo = processor->getCurrentPerformanceSnapshot();
    require(decodedPageTwo.layers[static_cast<size_t>(instrument)].tuningTable == 3,
            "Instrument TuneTable byte did not reach the engine snapshot");

    // OS 1.700 has thirteen temperament choices. The final MIDI Tune entry is
    // selector 12 and must be reachable at the top of the physical fader.
    processor->setPanelFader(
        4, wave::panel::performanceFaderAdcChannels[4], 1.0f, false);
    processBlocks(*processor, audio, 32);
    require((processor->getMasterFirmwareRuntime().sharedProgramByte(
                 instrumentBase() + pageTwoOffsets[4]) & 0x7fu) == 12u
                && processor->getCurrentPerformanceSnapshot()
                       .layers[static_cast<size_t>(instrument)].tuningTable == 12,
            "Instrument TuneTable could not select the native MIDI Tune entry");

    processor->setPanelFader(
        volumeFader, wave::panel::performanceFaderAdcChannels[volumeFader],
        1.0f, false);
    processBlocks(*processor, audio, 1200);
    const auto audible = processor->probeCurrentLayerVoice(
        0, instrument, 60, 0.8f, 24000, 1).rmsOutput;

    processor->setPanelFader(
        volumeFader, wave::panel::performanceFaderAdcChannels[volumeFader],
        0.0f, false);
    processBlocks(*processor, audio, 1200);
    const auto stored = processor->getMasterFirmwareRuntime().sharedProgramByte(
                            instrumentBase() + volumeOffset)
                        & 0x7fu;
    const auto quiet = processor->probeCurrentLayerVoice(
        0, instrument, 60, 0.8f, 24000, 2).rmsOutput;
    if (!(stored == 0u && audible > 1.0e-4 && quiet < 1.0e-8))
        throw std::runtime_error(
            "Instrument Page 2 Volume changed its LCD/record but not audio: stored "
            + std::to_string(stored) + ", audible "
            + std::to_string(audible) + ", quiet " + std::to_string(quiet));
}

void testWaveEditSoftKeysRelease()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 96);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    clickPanelControl(editor, *processor, audio,
                      1402.5f, 494.0f); // Instrument Edit.
    processBlocks(*processor, audio, 200);
    clickPanelControl(editor, *processor, audio,
                      1402.5f, 376.0f); // Wave Edit.
    processBlocks(*processor, audio, 96);

    clickPanelControl(editor, *processor, audio,
                      959.0f, 199.0f); // Wavetables.
    processBlocks(*processor, audio, 160);
    const auto wavetables
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    clickPanelControl(editor, *processor, audio,
                      903.0f, 199.0f); // Waves.
    processBlocks(*processor, audio, 160);
    const auto waves = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    require(waves != wavetables,
            "Wave Edit Waves and Wavetables produced the same screen");
    for (int transition = 0; transition < 20; ++transition)
    {
        const auto selectWavetables = (transition & 1) == 0;
        clickPanelControl(editor, *processor, audio,
                          selectWavetables ? 959.0f : 903.0f, 199.0f);
        processBlocks(*processor, audio, 160);
        const auto current
            = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        if (current != (selectWavetables ? wavetables : waves))
            throw std::runtime_error(
                "A Wave Edit softkey click did not select its menu at transition "
                + std::to_string(transition));
    }

    clickPanelControl(editor, *processor, audio,
                      903.0f, 199.0f); // First display soft key.
    processBlocks(*processor, audio, 160);
    const auto softKeyBits
        = processor->getMasterFirmwareRuntime().localByte(0x56ed9u);
    if ((softKeyBits & 0x01u) != 0u)
        throw std::runtime_error(
            "A Wave Edit soft key remained latched after its click: action bits "
            + std::to_string(softKeyBits));
    const auto settled = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    processBlocks(*processor, audio, 160);
    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() == settled,
            "A released Wave Edit soft key kept scrolling its menu");

    clickPanelControl(editor, *processor, audio,
                      1125.0f, 199.0f); // Quit Wave Edit soft key.
    processBlocks(*processor, audio, 96);
    clickPanelControl(editor, *processor, audio,
                      1117.0f, 553.0f); // Confirm Quit with OK / Return.
    processBlocks(*processor, audio, 160);

    clickPanelControl(editor, *processor, audio,
                      1402.5f, 494.0f); // Back to Instrument Edit.
    processBlocks(*processor, audio, 240);
    const auto selected = processor->getSelectedPerformanceInstrument();
    const auto firmwareSelected = processor->getMasterFirmwareRuntime()
                                      .currentPerformanceInstrument();
    if (selected < 0 || !processor->isPerformanceInstrumentActive(selected)
        || firmwareSelected != selected
        || processor->getMasterFirmwareRuntime().lcdVideoSnapshot() == settled)
        throw std::runtime_error(
            "Leaving Wave Edit did not restore a selected Instrument page: host "
            + std::to_string(selected)
            + ", firmware "
            + std::to_string(firmwareSelected.value_or(-1))
            + ", actions "
            + std::to_string(processor->getMasterFirmwareRuntime().localByte(
                0x56ed9u))
            + ", mode led "
            + std::to_string(processor->getMasterFirmwareRuntime().panelLed(22)));
}

void testLowerGlideOnOffUsesFirmwareSerial()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 96);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    const auto& firmware = processor->getMasterFirmwareRuntime();
    const auto before = (firmware.currentSoundRecordByte(238u) & 0x01u) != 0u;

    // clickPanelControl accepts upper-panel-local X coordinates. Subtracting
    // the artwork offset lands on the full-SVG lower Glide switch at 228,751.
    // A normal mouse tap can begin and end before the next host audio block.
    clickPanelControl(editor, *processor, audio, 228.0f - 66.0f, 751.0f, 0);
    processBlocks(*processor, audio, 48);
    require(((firmware.currentSoundRecordByte(238u) & 0x01u) != 0u) != before,
            "The visible lower Glide switch did not reach firmware serial 6");
}

void testEveryLowerControllerButtonReachesItsSerialInput()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 96);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);

    // These positions are the centres in the complete Figma SVG. The editor
    // helper accepts upper-panel-local X, hence the common artwork offset.
    constexpr auto offset = 66.0f;
    clickPanelControl(editor, *processor, audio, 110.0f - offset, 766.5f, 16);
    processBlocks(*processor, audio, 48);
    require(processor->getPanelLed(16),
            "The visible lower Button 1 did not reach firmware serial 3");

    clickPanelControl(editor, *processor, audio, 170.0f - offset, 766.5f, 16);
    processBlocks(*processor, audio, 48);
    require(processor->getPanelLed(35),
            "The visible lower Button 2 did not reach firmware serial 4");

    const auto beforeGlidePage
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
    const auto glideEditPosition = juce::Point<float> { 366.173f, 753.174f };
    auto* glideEditButton
        = dynamic_cast<juce::Button*>(editor.getComponentAt(
            glideEditPosition.roundToInt()));
    require(glideEditButton != nullptr,
            "The lower Glide Edit artwork has no interactive button component");
    glideEditButton->setState(juce::Button::buttonDown);
    processBlocks(*processor, audio, 16);
    glideEditButton->setState(juce::Button::buttonNormal);
    processBlocks(*processor, audio, 160);
    require(processor->getPanelLed(75)
                && processor->getMasterFirmwareRuntime().lcdVideoSnapshot()
                       != beforeGlidePage,
            "The visible lower Glide Edit did not reach firmware serial 11");

    clickPanelControl(editor, *processor, audio, 288.0f - offset, 849.0f, 16);
    processBlocks(*processor, audio, 48);
    require(processor->getKeyboardOctaveShift() == 1
                && processor->getPanelLed(58),
            "The visible lower Octave Up did not reach firmware serial 12");

    clickPanelControl(editor, *processor, audio, 288.0f - offset, 927.0f, 16);
    processBlocks(*processor, audio, 48);
    require(processor->getKeyboardOctaveShift() == 0
                && !processor->getPanelLed(58)
                && !processor->getPanelLed(27),
            "The visible lower Octave Down did not reach firmware serial 73");
}

void testWaveEnvelopeSelectorRemainsStable()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 96);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    constexpr std::array<int, 3> selectorLeds { 39, 23, 40 };
    const auto requireStableSelection = [&](size_t selectedOption) {
        for (int sample = 0; sample < 240; ++sample)
        {
            processBlocks(*processor, audio, 1);
            for (size_t option = 0; option < selectorLeds.size(); ++option)
            {
                const auto expected = option == selectedOption;
                if (processor->getPanelLed(selectorLeds[option]) != expected)
                    throw std::runtime_error(
                        "The idle Wave/Free selector illuminated another option at sample "
                        + std::to_string(sample) + ", selected option "
                        + std::to_string(selectedOption) + ", observed option "
                        + std::to_string(option));
            }
        }
    };

    requireStableSelection(0); // Wave 1-4.
    for (size_t selectedOption : { size_t { 1 }, size_t { 2 }, size_t { 0 } })
    {
        clickPanelControl(editor, *processor, audio,
                          392.0f, 555.0f);
        requireStableSelection(selectedOption);
    }
}

void testPerformanceStepButtonsDoNotRepeatAfterRelease()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(40);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 160);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);

    const auto plusMatrix = wave::panel::matrixIndexForDiagnosticCode(72);
    clickPanelControl(
        editor, *processor, audio, 1292.0f, 553.0f, 8,
        [&] {
            require(!processor->getMasterFirmwareRuntime().panelButtonPressed(
                        plusMatrix),
                    "Performance Plus entered the firmware's held-button scanner");
        });
    processBlocks(*processor, audio, 800);
    const auto firmwareProgram = [&] {
        return (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
                    0x54b40u))
                << 8)
               | static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
                   0x54b41u));
    };
    const auto activeRepeatAction = [&] {
        return (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
                    0x58fe4u))
                << 8)
               | static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
                   0x58fe5u));
    };
    require(processor->getCurrentProgram() == 41 && firmwareProgram() == 41
                && activeRepeatAction() == 0,
            "A released Performance Plus button continued changing patches");

    const auto minusMatrix = wave::panel::matrixIndexForDiagnosticCode(69);
    clickPanelControl(
        editor, *processor, audio, 1253.0f, 553.0f, 8,
        [&] {
            require(!processor->getMasterFirmwareRuntime().panelButtonPressed(
                        minusMatrix),
                    "Performance Minus entered the firmware's held-button scanner");
        });
    processBlocks(*processor, audio, 800);
    require(processor->getCurrentProgram() == 40 && firmwareProgram() == 40
                && activeRepeatAction() == 0,
            "A released Performance Minus button continued changing patches");
}

void testColdStartPerformanceStepDoesNotRepeat()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);

    // This is deliberately the first panel action, before a single audio
    // callback has allowed the booted firmware/UI state to settle.
    clickPanelControl(editor, *processor, audio,
                      1292.0f, 553.0f, 0); // Plus.
    require(processor->getCurrentProgram() == 1,
            "Cold-start Performance Plus was not one immediate step");
    processBlocks(*processor, audio, 1200);

    const auto firmwareProgram
        = (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
               0x54b40u)) << 8)
          | processor->getMasterFirmwareRuntime().localByte(0x54b41u);
    const auto activeAction
        = (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
               0x58fe4u)) << 8)
          | processor->getMasterFirmwareRuntime().localByte(0x58fe5u);
    if (processor->getCurrentProgram() != 1 || firmwareProgram != 1
        || activeAction != 0)
        throw std::runtime_error(
            "The first cold-start Performance Plus click repeated: engine "
            + std::to_string(processor->getCurrentProgram()) + ", firmware "
            + std::to_string(firmwareProgram) + ", action "
            + std::to_string(activeAction));
}

void testRestoredEditModeCannotMisrouteFirstPerformanceStep()
{
    auto saved = std::make_unique<WaveEmulationAudioProcessor>();
    saved->prepareToPlay(48000.0, 512);
    saved->setCurrentProgram(40);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*saved, audio, 160);
    const auto waveEdit = wave::panel::matrixIndexForDiagnosticCode(32);
    saved->setPanelButton(waveEdit, true);
    processBlocks(*saved, audio, 8);
    saved->setPanelButton(waveEdit, false);
    processBlocks(*saved, audio, 160);
    require(saved->getPanelSelectedMode() == 32,
            "Saved-state fixture did not enter Wave Edit");

    juce::MemoryBlock state;
    saved->getStateInformation(state);
    saved.reset();

    auto restored = std::make_unique<WaveEmulationAudioProcessor>();
    restored->setStateInformation(state.getData(),
                                  static_cast<int>(state.getSize()));
    restored->prepareToPlay(48000.0, 512);
    require(restored->getCurrentProgram() == 40
                && restored->getPanelSelectedMode() == 39,
            "A restored edit-page selector diverged from the booted Performance screen");

    WaveEmulationAudioProcessorEditor editor(*restored);
    editor.setVisible(true);
    clickPanelControl(editor, *restored, audio,
                      1292.0f, 553.0f, 0); // First action after restore: Plus.
    processBlocks(*restored, audio, 1200);
    require(restored->getCurrentProgram() == 41,
            "Restored edit mode made the first Performance Plus click repeat");
}

void testPerformanceStepCancelsAnOutgoingEditStep()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(40);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 160);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    clickPanelControl(editor, *processor, audio,
                      1402.5f, 553.0f); // Performance.
    processBlocks(*processor, audio, 96);
    const auto filterEdit = wave::panel::matrixIndexForDiagnosticCode(60);
    processor->setPanelButton(filterEdit, true);
    processBlocks(*processor, audio, 8);
    processor->setPanelButton(filterEdit, false);
    processBlocks(*processor, audio, 96);

    // Leave one genuine edit-page Plus serial transaction waiting for the
    // audio thread, then return to Performance and press Plus once. The old
    // transaction must not be reinterpreted by the new page as a held patch
    // selector.
    const auto plus = wave::panel::matrixIndexForDiagnosticCode(72);
    processor->setPanelButton(plus, true);
    processor->setPanelButton(plus, false);
    processBlocks(*processor, audio, 1); // Move the edit edge into the serial ring.
    clickPanelControl(editor, *processor, audio,
                      1402.5f, 553.0f, 0); // Performance.
    require(processor->getCurrentProgram() == 40,
            "Returning to Performance changed the selected program");
    clickPanelControl(editor, *processor, audio,
                      1292.0f, 553.0f, 0); // Plus.
    require(processor->getCurrentProgram() == 41,
            "The Performance Plus press was not exactly one immediate step");
    auto firstUnexpectedBlock = -1;
    for (int block = 0; block < 800; ++block)
    {
        processBlocks(*processor, audio, 1);
        if (processor->getCurrentProgram() != 41)
        {
            firstUnexpectedBlock = block;
            break;
        }
    }

    if (processor->getCurrentProgram() != 41)
        throw std::runtime_error(
            "An outgoing edit-page Plus transaction repeated as Performance changes: program "
            + std::to_string(processor->getCurrentProgram()) + ", firmware "
            + std::to_string((static_cast<int>(
                  processor->getMasterFirmwareRuntime().localByte(0x54b40u)) << 8)
                | processor->getMasterFirmwareRuntime().localByte(0x54b41u))
            + ", action "
            + std::to_string((static_cast<int>(
                  processor->getMasterFirmwareRuntime().localByte(0x58fe4u)) << 8)
                | processor->getMasterFirmwareRuntime().localByte(0x58fe5u))
            + ", block " + std::to_string(firstUnexpectedBlock));
}

void testModeButtonsChangePageOnFirstClick()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 160);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    auto previous = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();

    for (int transition = 0; transition < 20; ++transition)
    {
        const auto instrumentEdit = (transition & 1) == 0;
        clickPanelControl(editor, *processor, audio,
                          1402.5f, instrumentEdit ? 494.0f : 553.0f);
        processBlocks(*processor, audio, 240);
        const auto current
            = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        if (current == previous)
        {
            const auto diagnosticCode = instrumentEdit ? 36 : 39;
            const auto wordAt = [&](uint32_t address) {
                return (static_cast<int>(
                            processor->getMasterFirmwareRuntime().localByte(address))
                        << 8)
                       | static_cast<int>(
                           processor->getMasterFirmwareRuntime().localByte(address + 1u));
            };
            throw std::runtime_error(
                "A single mode-button click did not change the firmware page at transition "
                + std::to_string(transition) + ", active action "
                + std::to_string(wordAt(0x58fe4u)) + ", target action "
                + std::to_string(processor->getMasterFirmwareRuntime().localByte(
                    0x2850eu + static_cast<uint32_t>(diagnosticCode)))
                + ", press pending "
                + std::to_string(processor->getMasterFirmwareRuntime().panelEventPending(
                    0x80u, static_cast<uint8_t>(diagnosticCode), 1u))
                + ", release pending "
                + std::to_string(processor->getMasterFirmwareRuntime().panelEventPending(
                    0x80u, static_cast<uint8_t>(diagnosticCode), 0u)));
        }
        previous = current;

        constexpr std::array<int, 8> modeLeds { 64, 69, 21, 74,
                                                 26, 10, 22, 51 };
        const auto expectedLed = instrumentEdit ? 22 : 51;
        for (const auto led : modeLeds)
            require(processor->getPanelLed(led) == (led == expectedLed),
                    "A mode change left more than one operating-mode LED lit");
    }
}

void testPageButtonsChangePageOnFirstClick()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 160);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    clickPanelControl(editor, *processor, audio,
                      1402.5f, 494.0f); // Instrument Edit, Page 1.
    processBlocks(*processor, audio, 320);
    const auto pageOne
        = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();

    auto previous = pageOne;
    for (int transition = 0; transition < 20; ++transition)
    {
        const auto pageTwo = (transition & 1) == 0;
        clickPanelControl(editor, *processor, audio,
                          pageTwo ? 942.0f : 903.0f, 553.0f);
        processBlocks(*processor, audio, 240);
        const auto current
            = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        if (current == previous)
            throw std::runtime_error(
                "A single Page-button click did not change the Instrument page at transition "
                + std::to_string(transition) + ", actions "
                + std::to_string(processor->getMasterFirmwareRuntime().localByte(
                    0x2850eu + 21u)) + "/"
                + std::to_string(processor->getMasterFirmwareRuntime().localByte(
                    0x2850eu + 23u)) + ", active "
                + std::to_string((static_cast<int>(
                    processor->getMasterFirmwareRuntime().localByte(0x58fe4u))
                    << 8)
                    | static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
                        0x58fe5u))) + ", page chord "
                + std::to_string(processor->getMasterFirmwareRuntime().localByte(
                    0x59890u)));
        if (!pageTwo && current != pageOne)
            throw std::runtime_error(
                "Page-left did not return to Instrument Page 1 at transition "
                + std::to_string(transition));
        const auto activeAction
            = (static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
                   0x58fe4u))
               << 8)
              | static_cast<int>(processor->getMasterFirmwareRuntime().localByte(
                  0x58fe5u));
        require(processor->getMasterFirmwareRuntime().localByte(0x59890u) == 0
                    && activeAction != 8 && activeAction != 9,
                "A Page click left the firmware's chord or repeat state held");
        previous = current;
    }
}

void testModifierEditButtonsOpenFirmwarePages()
{
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    processor->prepareToPlay(48000.0, 512);
    processor->setCurrentProgram(0);
    juce::AudioBuffer<float> audio(2, 512);
    processBlocks(*processor, audio, 320);

    WaveEmulationAudioProcessorEditor editor(*processor);
    editor.setVisible(true);
    constexpr std::array modifierButtons {
        std::pair { 2103.0f, 473.0f },
        std::pair { 2162.0f, 473.0f },
        std::pair { 2103.0f, 551.0f },
        std::pair { 2162.0f, 551.0f }
    };
    constexpr std::array ledCodes { 30, 14, 4, 78 };

    for (size_t index = 0; index < modifierButtons.size(); ++index)
    {
        const auto before
            = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
        clickPanelControl(editor, *processor, audio,
                          modifierButtons[index].first,
                          modifierButtons[index].second, 0);
        processBlocks(*processor, audio, 240);
        require(processor->getPanelLed(ledCodes[index]),
                "A Modifier Edit button did not latch its LED");
        require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() != before,
                "A Modifier Edit button did not open its firmware LCD page");
        const auto callback
            = (static_cast<uint32_t>(processor->getMasterFirmwareRuntime().localByte(
                   0x56bb0u))
               << 24u)
              | (static_cast<uint32_t>(processor->getMasterFirmwareRuntime().localByte(
                     0x56bb1u))
                 << 16u)
              | (static_cast<uint32_t>(processor->getMasterFirmwareRuntime().localByte(
                     0x56bb2u))
                 << 8u)
              | static_cast<uint32_t>(processor->getMasterFirmwareRuntime().localByte(
                    0x56bb3u));
        require(callback != 0u,
                "A Modifier Edit button did not install a firmware screen callback");

        clickPanelControl(editor, *processor, audio,
                          modifierButtons[index].first,
                          modifierButtons[index].second, 0);
        processBlocks(*processor, audio, 160);
        require(!processor->getPanelLed(ledCodes[index]),
                "A Modifier Edit button did not exit its page");
    }
}

} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--instrument-faders")
        {
            testInstrumentEditPageOneFadersUpdatePerformance();
            testInstrumentPageOneFaderDoesNotLeakIntoOtherPages();
            testInstrumentEditPageTwoVolumeReachesAudioEngine();
            std::cout << "Instrument fader checks passed\n";
            return 0;
        }
        testWindowShortcutsPreservePanelLayout();
        testLowerPerformanceWheelsFollowHardwareRules();
        testLayerProcessingPromptAcceptsCancel();
        testInstrumentEditPageOneFadersUpdatePerformance();
        testInstrumentPageOneFaderDoesNotLeakIntoOtherPages();
        testInstrumentEditPageTwoVolumeReachesAudioEngine();
        testRepeatedInstrumentEditLayerClicks();
        testEveryLowerControllerButtonReachesItsSerialInput();
        testLowerGlideOnOffUsesFirmwareSerial();
        testWaveEditSoftKeysRelease();
        testWaveEnvelopeSelectorRemainsStable();
        testPerformanceStepButtonsDoNotRepeatAfterRelease();
        testColdStartPerformanceStepDoesNotRepeat();
        testRestoredEditModeCannotMisrouteFirstPerformanceStep();
        testPerformanceStepCancelsAnOutgoingEditStep();
        testModeButtonsChangePageOnFirstClick();
        testPageButtonsChangePageOnFirstClick();
        testModifierEditButtonsOpenFirmwarePages();
        testFirmwareWavetableStepReachesAudioParameter();
        std::cout << "WaveEditorTests: all checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WaveEditorTests: " << error.what() << '\n';
        return 1;
    }
}
