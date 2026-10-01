#include "PluginProcessor.h"

#include <chrono>
#include <iostream>
#include <memory>

namespace
{
using Clock = std::chrono::steady_clock;

template <typename Render>
double measure(Render&& render)
{
    const auto start = Clock::now();
    render();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

juce::MidiBuffer notes(int count)
{
    juce::MidiBuffer midi;
    for (int voice = 0; voice < count; ++voice)
        midi.addEvent(juce::MidiMessage::noteOn(1, 36 + voice, 0.8f), 0);
    return midi;
}
} // namespace

int main(int argc, char** argv)
{
    constexpr auto sampleRate = 48000.0;
    const auto blockSize = argc > 1
                               ? juce::jlimit(16, 4096,
                                              juce::String(argv[1]).getIntValue())
                               : 512;
    const auto voiceCount = argc > 2
                                ? juce::jlimit(1, wave::dsp::WaldorfEngine::voiceCount,
                                               juce::String(argv[2]).getIntValue())
                                : wave::dsp::WaldorfEngine::voiceCount;
    const auto blocks = juce::roundToInt(std::ceil(2.0 * sampleRate / blockSize));
    const auto renderedSeconds = static_cast<double>(blockSize * blocks) / sampleRate;

    wave::dsp::WaldorfEngine engine;
    engine.prepare(sampleRate, blockSize);
    wave::dsp::WaldorfEngine serialEngine;
    serialEngine.setVoiceCardThreadingEnabled(false);
    serialEngine.prepare(sampleRate, blockSize);
    wave::parameters::Snapshot sound;
    sound.attackSeconds = 0.001f;
    sound.releaseSeconds = 2.0f;
    sound.cutoffHz = 12000.0f;
    sound.filterEnvelopeSemitones = 0.0f;
    if (argc > 3)
        sound.resonanceAmount = juce::jlimit(0.0f, 1.0f,
                                            juce::String(argv[3]).getFloatValue());
    juce::AudioBuffer<float> audio(2, blockSize);
    auto engineMidi = notes(voiceCount);
    const auto engineSeconds = measure([&] {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            engine.render(audio, block == 0 ? engineMidi : juce::MidiBuffer{}, sound);
        }
    });
    auto serialMidi = notes(voiceCount);
    const auto serialSeconds = measure([&] {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            serialEngine.render(audio, block == 0 ? serialMidi : juce::MidiBuffer{}, sound);
        }
    });

    const auto bootStart = Clock::now();
    auto processor = std::make_unique<WaveEmulationAudioProcessor>();
    const auto bootSeconds = std::chrono::duration<double>(Clock::now() - bootStart).count();
    processor->prepareToPlay(sampleRate, blockSize);
    processor->setCurrentProgram(12); // A013: one full-range instrument.
    auto processorMidi = notes(voiceCount);
    const auto processorSeconds = measure([&] {
        for (int block = 0; block < blocks; ++block)
        {
            audio.clear();
            auto midi = block == 0 ? processorMidi : juce::MidiBuffer{};
            processor->processBlock(audio, midi);
        }
    });

    std::cout << "Block size: " << blockSize << " samples, voices: " << voiceCount << '\n'
              << "DSP resonance: " << sound.resonanceAmount << '\n'
              << "Active DSP voices: " << engine.activeVoiceCount() << '\n'
              << "Parallel DSP: " << engineSeconds << " s, "
              << renderedSeconds / engineSeconds << "x realtime\n"
              << "Serial DSP: " << serialSeconds << " s, "
              << renderedSeconds / serialSeconds << "x realtime\n"
              << "Voice-card threading speedup: " << serialSeconds / engineSeconds << "x\n"
              << "Processor boot: " << bootSeconds << " s\n"
              << "Full processor: " << processorSeconds << " s, "
              << renderedSeconds / processorSeconds << "x realtime\n";
    std::cout << "Firmware loaded: " << processor->getMasterFirmwareRuntime().isLoaded()
              << ", active processor voices: " << processor->getActiveVoiceCount() << '\n';
    std::cout << "Master delay cycles fast-forwarded: "
              << processor->getMasterFirmwareRuntime().fastForwardedCycleCount() << '\n';
    return 0;
}
