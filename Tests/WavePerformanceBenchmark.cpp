#include "PluginProcessor.h"

#include <chrono>
#include <iostream>
#include <memory>

#if JUCE_WINDOWS
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;

double processCpuSeconds()
{
#if JUCE_WINDOWS
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0.0;
    const auto seconds = [](const FILETIME& value) {
        return static_cast<double>((static_cast<uint64_t>(value.dwHighDateTime) << 32u)
                                   | value.dwLowDateTime) * 1.0e-7;
    };
    return seconds(kernel) + seconds(user);
#else
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
           + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1.0e-6;
#endif
}

struct Timing
{
    double wallSeconds = 0.0;
    double cpuSeconds = 0.0;
};

template <typename Render>
Timing measure(Render&& render)
{
    const auto cpuStart = processCpuSeconds();
    const auto start = Clock::now();
    render();
    const auto wallSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    return { wallSeconds, processCpuSeconds() - cpuStart };
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
    const auto sampleRate = argc > 4
                                ? juce::jlimit(8000.0, 384000.0, juce::String(argv[4]).getDoubleValue())
                                : 48000.0;
    const auto blockSize = argc > 1
                               ? juce::jlimit(16, 4096,
                                              juce::String(argv[1]).getIntValue())
                               : 512;
    const auto voiceCount = argc > 2
                                ? juce::jlimit(1, wave::dsp::WaldorfEngine::voiceCount,
                                               juce::String(argv[2]).getIntValue())
                                : wave::dsp::WaldorfEngine::voiceCount;
    const auto duration = argc > 5
                              ? juce::jlimit(0.25, 60.0, juce::String(argv[5]).getDoubleValue())
                              : 2.0;
    const auto blocks = juce::roundToInt(std::ceil(duration * sampleRate / blockSize));
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
    auto processor = std::make_unique<WaveEmulationAudioProcessor>(
        juce::File{}, WaveEmulationAudioProcessor::InitialBank::embeddedFactory);
    const auto bootSeconds = std::chrono::duration<double>(Clock::now() - bootStart).count();
    processor->prepareToPlay(sampleRate, blockSize);
    const auto program = argc > 6 ? juce::String(argv[6]).getIntValue() : 12;
    processor->setCurrentProgram(program); // Default A013: one full-range instrument.
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
              << "Sample rate: " << sampleRate << " Hz, duration: " << renderedSeconds << " s\n"
              << "Active DSP voices: " << engine.activeVoiceCount() << '\n'
              << "Parallel DSP: " << engineSeconds.wallSeconds << " s, "
              << renderedSeconds / engineSeconds.wallSeconds << "x realtime\n"
              << "Serial DSP: " << serialSeconds.wallSeconds << " s, "
              << renderedSeconds / serialSeconds.wallSeconds << "x realtime\n"
              << "Voice-card threading speedup: " << serialSeconds.wallSeconds / engineSeconds.wallSeconds << "x\n"
              << "Processor boot: " << bootSeconds << " s\n"
              << "Full processor: " << processorSeconds.wallSeconds << " s, "
              << renderedSeconds / processorSeconds.wallSeconds << "x realtime\n"
              << "Parallel DSP CPU: " << engineSeconds.cpuSeconds << " s\n"
              << "Serial DSP CPU: " << serialSeconds.cpuSeconds << " s\n"
              << "Full processor CPU: " << processorSeconds.cpuSeconds << " s, "
              << 100.0 * processorSeconds.cpuSeconds / renderedSeconds
              << "% of one core's audio-time budget (all process threads combined)\n";
    std::cout << "Firmware loaded: " << processor->getMasterFirmwareRuntime().isLoaded()
              << ", active processor voices: " << processor->getActiveVoiceCount() << '\n';
    std::cout << "Master delay cycles fast-forwarded: "
              << processor->getMasterFirmwareRuntime().fastForwardedCycleCount() << '\n';
    return 0;
}
