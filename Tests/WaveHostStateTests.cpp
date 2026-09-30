#include "PluginProcessor.h"
#include "PanelWiring.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
// macOS CI's libc++ does not yet provide jthread/stop_token. Keep the same
// exception-safe shutdown with an atomic stop flag and an ordinary thread.
class RenderThread
{
public:
    template <typename Function>
    explicit RenderThread(Function render)
        : worker([this, render] { render(stop); }) {}
    ~RenderThread()
    {
        stop.store(true);
        worker.join();
    }
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

private:
    std::atomic<bool> stop { false };
    std::thread worker;
};
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    try
    {
        if (const auto* stateFile = std::getenv("WAVE_HOST_STATE_FILE"))
        {
            // Optional local fixture: raw jucePluginState from an AU preset.
            // User presets and firmware stay outside the source repository.
            juce::TemporaryFile isolatedPreference(".txt");
            auto processor = std::make_unique<WaveEmulationAudioProcessor>(isolatedPreference.getFile());
            juce::MemoryBlock state;
            if (!juce::File(stateFile).loadFileAsData(state))
                throw std::runtime_error("Unable to read host state fixture");
            processor->prepareToPlay(96000.0, 512);
            const auto renderAndCheck = [&]
            {
                juce::AudioBuffer<float> audio(2, 512);
                juce::MidiBuffer midi;
                for (int block = 0; block < 1000; ++block)
                {
                    const juce::ScopedLock lock(processor->getCallbackLock());
                    audio.clear();
                    processor->processBlock(audio, midi);
                }
                const juce::ScopedLock lock(processor->getCallbackLock());
                if (!processor->getMasterFirmwareRuntime().completedVoiceBoardLoaderHandoff()
                    || processor->getMasterFirmwareRuntime().unmappedReadCount() != 0)
                    throw std::runtime_error("Recalled master firmware is not operational");
                for (int board = 0; board < 3; ++board)
                    if (!processor->getVoiceFirmwareRuntime(board).reachedServiceLoop())
                        throw std::runtime_error("Recalled voice firmware left its service loop");
                return processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
            };
            processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            const auto referenceScreen = renderAndCheck();
            if (std::getenv("WAVE_TEST_PATCH_STEPS") != nullptr)
            {
                auto render = [&](int blocks)
                {
                    juce::AudioBuffer<float> audio(2, 512);
                    juce::MidiBuffer midi;
                    for (int b = 0; b < blocks; ++b)
                    {
                        audio.clear();
                        processor->processBlock(audio, midi);
                    }
                };
                auto edge = [&](int code, bool down)
                {
                    processor->setPanelButton(wave::panel::matrixIndexForDiagnosticCode(code), down);
                };
                for (const auto delay : { 0, 1, 2, 3, 8, 16, 32 })
                {
                    processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                    render(64);
                    edge(60, true); render(8); edge(60, false); render(32);
                    edge(72, true); edge(72, false); render(delay);
                    processor->setCurrentProgram(10);
                    edge(72, true); render(1); edge(72, false);
                    render(768);
                    const auto selected = processor->getMasterFirmwareRuntime().currentPerformanceId();
                    std::cout << "Edit-to-performance delay " << delay << " selected "
                              << selected.value_or(-1) << std::endl;
                    if (!selected.has_value() || *selected != 11)
                        throw std::runtime_error("Released Plus changed patches after leaving Edit");
                }
                // A press already in the controller queue can arrive after
                // the user has selected Performance and released the mouse.
                processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                render(64);
                auto& firmware = const_cast<wave::firmware::MasterFirmwareRuntime&>(
                    processor->getMasterFirmwareRuntime());
                firmware.pushPanelEvent(0x80u, 72u, 1u);
                edge(72, true); edge(72, false);
                render(768);
                const auto lateSelected = firmware.currentPerformanceId();
                std::cout << "Late press selected " << lateSelected.value_or(-1) << std::endl;
                if (!lateSelected.has_value() || *lateSelected != 1)
                    throw std::runtime_error("A late released press changed the selected patch");
                processor->setCurrentProgram(0); render(64);
                for (int click = 0; click < 12; ++click)
                {
                    edge(72, true); edge(72, false);
                    render(click % 3);
                }
                render(768);
                if (firmware.currentPerformanceId() != std::optional<int>(12))
                    throw std::runtime_error("Rapid Plus clicks were lost or repeated");
                for (int click = 0; click < 5; ++click)
                {
                    edge(69, true); edge(69, false); render(2);
                }
                render(768);
                if (firmware.currentPerformanceId() != std::optional<int>(7))
                    throw std::runtime_error("Rapid Minus clicks were lost or repeated");
                edge(69, true); render(768); edge(69, false); render(768);
                if (firmware.currentPerformanceId() != std::optional<int>(6))
                    throw std::runtime_error("A held Minus button repeated after release");
                firmware.pushPanelEvent(0x80u, 69u, 1u);
                render(768);
                if (firmware.currentPerformanceId() != std::optional<int>(6))
                    throw std::runtime_error("An idle Performance page accepted a stale Minus press");
                std::cout << "Patch step transitions passed\n";
                return 0;
            }
            std::atomic<unsigned> renderedBlocks { 0 };
            RenderThread concurrentRender([&](const std::atomic<bool>& stop)
            {
                juce::AudioBuffer<float> audio(2, 512);
                juce::MidiBuffer midi;
                while (!stop.load())
                {
                    {
                        const juce::ScopedLock lock(processor->getCallbackLock());
                        audio.clear();
                        processor->processBlock(audio, midi);
                        ++renderedBlocks;
                    }
                    std::this_thread::yield();
                }
            });
            while (renderedBlocks.load() == 0)
                std::this_thread::yield();
            for (int recall = 0; recall < 3; ++recall)
            {
                // Match the AU wrapper: the host state calls do not take the
                // processor's callback lock on our behalf.
                processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                if (renderAndCheck() != referenceScreen)
                    throw std::runtime_error("Live recall changed the firmware LCD from the serial reference");
                juce::MemoryBlock saved;
                processor->getStateInformation(saved);
                processor->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
                if (renderAndCheck() != referenceScreen)
                    throw std::runtime_error("Live save/recall changed the firmware LCD");
            }
            std::cout << "Live AU preset recall preserved firmware service and the reference LCD\n";
            return 0;
        }
        if (argc == 4 && juce::String(argv[1]) == "--check-remembered-firmware")
        {
            auto reopened = std::make_unique<WaveEmulationAudioProcessor>(juce::File(argv[2]));
            if (!reopened->getFirmwareReport().hasBothImages()
                || reopened->getFirmwareDirectory() != juce::File(argv[3]))
                throw std::runtime_error("A fresh process did not reload remembered firmware");
            return 0;
        }
        // Never read or change the developer's real firmware preference.
        juce::TemporaryFile preference(".txt");
        auto first = std::make_unique<WaveEmulationAudioProcessor>(preference.getFile());
        auto second = std::make_unique<WaveEmulationAudioProcessor>(preference.getFile());
        const auto* directory = std::getenv("WAVE_FIRMWARE_DIR");
        for (auto* processor : { first.get(), second.get() })
        {
            if (directory != nullptr
                && !processor->loadFirmware(juce::File(directory)).hasBothImages())
                throw std::runtime_error("Unable to load the supplied test firmware");
            processor->prepareToPlay(48000.0, 128);
        }
        if (directory != nullptr)
        {
            const auto remembered = preference.getFile().loadFileAsString();
            juce::TemporaryFile emptyFolder(".dir");
            emptyFolder.getFile().createDirectory();
            const auto invalidReport = first->loadFirmware(emptyFolder.getFile());
            emptyFolder.getFile().deleteRecursively();
            if (invalidReport.hasBothImages()
                || preference.getFile().loadFileAsString() != remembered
                || !first->getFirmwareReport().hasBothImages())
                throw std::runtime_error("Invalid firmware selection replaced a working preference");
            juce::ChildProcess reopened;
            const juce::StringArray command {
                juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
                "--check-remembered-firmware", preference.getFile().getFullPathName(),
                juce::File(directory).getFullPathName()
            };
            if (!reopened.start(command) || !reopened.waitForProcessToFinish(30000)
                || reopened.getExitCode() != 0)
                throw std::runtime_error("Firmware preference did not survive process restart");

            // A saved project chooses its own firmware without changing the
            // default for unrelated new instances.
            juce::MemoryBlock savedProject;
            first->getStateInformation(savedProject);
            juce::TemporaryFile unrelatedPreference(".txt");
            const auto missingDefault = juce::File(directory)
                                            .getChildFile("missing-test-default").getFullPathName();
            unrelatedPreference.getFile().replaceWithText(missingDefault);
            auto recalled = std::make_unique<WaveEmulationAudioProcessor>(unrelatedPreference.getFile());
            recalled->setStateInformation(savedProject.getData(), static_cast<int>(savedProject.getSize()));
            if (!recalled->getFirmwareReport().hasBothImages()
                || recalled->getFirmwareDirectory() != juce::File(directory)
                || unrelatedPreference.getFile().loadFileAsString() != missingDefault)
                throw std::runtime_error("Project recall replaced the default firmware preference");
        }
        auto render = [](const std::atomic<bool>& stop, WaveEmulationAudioProcessor* processor)
        {
            juce::AudioBuffer<float> audio(2, 128);
            juce::MidiBuffer midi;
            while (!stop.load())
            {
                const juce::ScopedLock lock(processor->getCallbackLock());
                audio.clear();
                midi.clear();
                processor->processBlock(audio, midi);
            }
        };
        RenderThread firstRender([&](const std::atomic<bool>& stop) { render(stop, first.get()); });
        RenderThread secondRender([&](const std::atomic<bool>& stop) { render(stop, second.get()); });
        for (int recall = 0; recall < 8; ++recall)
        {
            juce::MemoryBlock state;
            first->getStateInformation(state);
            first->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            if (state.isEmpty())
                throw std::runtime_error("Host preset contained no state");
            auto restored = std::make_unique<WaveEmulationAudioProcessor>(preference.getFile());
            restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            if (directory != nullptr && !restored->getFirmwareReport().hasBothImages())
                throw std::runtime_error("Host preset lost the firmware reference");
            if (restored->getCurrentProgram() != first->getCurrentProgram())
                throw std::runtime_error("Host preset lost the selected performance");
            restored->prepareToPlay(48000.0, 128);
            juce::AudioBuffer<float> audio(2, 128);
            juce::MidiBuffer midi;
            restored->processBlock(audio, midi);
        }
        std::cout << "Host state saved and recalled while two instances rendered"
                  << (directory != nullptr ? " with firmware\n" : " without firmware\n");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
