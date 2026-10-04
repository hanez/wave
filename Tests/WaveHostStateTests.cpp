#include "PluginProcessor.h"
#include "PanelWiring.h"
#include "Firmware/DosFloppyImage.h"

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
            if (std::getenv("WAVE_TEST_STARTUP_RECALL") != nullptr)
            {
                renderAndCheck();
                const auto capture = [&] {
                    std::array<uint8_t, 256> sound {};
                    for (size_t i = 0; i < sound.size(); ++i)
                        sound[i] = processor->getMasterFirmwareRuntime()
                            .currentSoundRecordByte(static_cast<uint32_t>(i));
                    return sound;
                };
                const auto initial = capture();
                processor->stepFactoryPerformance(1);
                renderAndCheck();
                processor->stepFactoryPerformance(-1);
                renderAndCheck();
                if (initial != capture())
                    throw std::runtime_error("Startup Sound differs from its Performance recall");
                std::cout << "Startup and returned Performance have identical native Sound records\n";
                return 0;
            }
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
            if (!processor->hasMountedDiskImage()
                || processor->getMountedDiskImageFile().getFileName() != "Blank Wave.img"
                || processor->getNumPrograms() != 256
                || processor->getProgramName(0) != "MULTI INIT"
                || processor->getProgramName(255) != "MULTI INIT"
                || processor->getSelectedPerformanceInstrument() != 0
                || !processor->isPerformanceInstrumentActive(0))
                throw std::runtime_error("Fresh instance did not load the default INIT bank and Instrument");
            for (int instrument = 1; instrument < 8; ++instrument)
                if (processor->isPerformanceInstrumentActive(instrument))
                    throw std::runtime_error("Default INIT bank enabled an unintended Instrument");
            wave::firmware::DosFloppyImage::SetupFile startupSetup;
            if (!processor->mountedDiskImageIsWritable()
                || wave::firmware::DosFloppyImage::readWaveSetup(
                       processor->getMountedDiskImageFile(), startupSetup).failed())
                throw std::runtime_error("Default mounted disk did not contain a writable SET");
            if (directory != nullptr
                && !processor->loadFirmware(juce::File(directory)).hasBothImages())
                throw std::runtime_error("Unable to load the supplied test firmware");
            processor->prepareToPlay(48000.0, 128);
        }
        if (first->getMountedDiskImageFile() == second->getMountedDiskImageFile())
            throw std::runtime_error("Fresh instances share the same writable startup disk");
        if (directory != nullptr)
        {
            juce::AudioBuffer<float> startupAudio(2, 128);
            const auto renderStartup = [&] {
                for (int block = 0; block < 1000; ++block)
                {
                    juce::MidiBuffer midi;
                    first->processBlock(startupAudio, midi);
                }
            };
            renderStartup();
            const auto& runtime = first->getMasterFirmwareRuntime();
            if (runtime.currentPerformanceId() != 0
                || runtime.currentPerformanceInstrument() != 0
                || first->getPanelSelectedMode() != 39
                || first->isPanelModeDisplayTransitionActive())
                throw std::runtime_error("Default SET did not finish on its editable Performance page");
            const auto cutoffBefore = runtime.currentSoundRecordByte(79);
            first->setPanelPotValue(wave::parameters::cutoff, 0.9f);
            renderStartup();
            first->setPanelPotValue(wave::parameters::cutoff, 0.1f);
            renderStartup();
            if (runtime.currentSoundRecordByte(79) == cutoffBefore)
                throw std::runtime_error("Startup knob movement did not edit the INIT Sound");
            juce::MidiBuffer startupNote;
            startupNote.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
            first->processBlock(startupAudio, startupNote);
            if (first->getActiveVoiceCount() == 0)
                throw std::runtime_error("Default INIT Instrument could not play a note");
            juce::MidiBuffer releaseStartupNote;
            releaseStartupNote.addEvent(juce::MidiMessage::allSoundOff(1), 0);
            first->processBlock(startupAudio, releaseStartupNote);

            // A nonzero wavetable makes phantom startup encoder steps audible.
            // Preserve an Instrument-local edit through a fresh instance.
            auto& editableRuntime = const_cast<wave::firmware::MasterFirmwareRuntime&>(runtime);
            if (!editableRuntime.writeCurrentSoundRecordByte(25, 42))
                throw std::runtime_error("Cannot install startup wavetable regression edit");
            renderStartup();
            juce::MemoryBlock startupState;
            first->getStateInformation(startupState);
            juce::TemporaryFile startupPreference(".txt");
            auto startupReopened = std::make_unique<WaveEmulationAudioProcessor>(startupPreference.getFile());
            startupReopened->prepareToPlay(96000.0, 512);
            startupReopened->setStateInformation(startupState.getData(), static_cast<int>(startupState.getSize()));
            juce::AudioBuffer<float> recalledAudio(2, 512);
            const auto renderRecalled = [&] {
                for (int block = 0; block < 1000; ++block)
                {
                    juce::MidiBuffer midi;
                    startupReopened->processBlock(recalledAudio, midi);
                }
            };
            renderRecalled();
            auto& recalledRuntime = const_cast<wave::firmware::MasterFirmwareRuntime&>(
                startupReopened->getMasterFirmwareRuntime());
            if (recalledRuntime.currentSoundRecordByte(25) != 42)
                throw std::runtime_error("Fresh project recall changed the saved wavetable selector");
            startupReopened->turnPanelEncoder(8, -1);
            renderRecalled();
            if (recalledRuntime.currentSoundRecordByte(25) != 41)
                throw std::runtime_error("A genuine wavetable decrement was undone after startup");

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
        // Restore a one-patch image over an existing populated project bank.
        // Zero slots must replace the old records rather than reject the bank.
        {
            constexpr size_t soundOffset = 0x12e7c, performanceOffset = 0x22e7c;
            juce::MemoryBlock image(performanceOffset + 256 * 512, true);
            auto* bytes = static_cast<uint8_t*>(image.getData());
            bytes[soundOffset + 239] = 0x55;
            bytes[performanceOffset + 48] = 0x55;
            std::copy_n("ONLY PATCH", 10, bytes + performanceOffset + 32);
            juce::TemporaryFile setupFile(".set"), diskFile(".img");
            if (!setupFile.getFile().replaceWithData(image.getData(), image.getSize())
                || wave::firmware::DosFloppyImage::createWithWaveSetup(
                       diskFile.getFile(), setupFile.getFile()).failed()
                || first->mountDiskImage(diskFile.getFile()).failed()
                || !first->getProgramName(1).isEmpty())
                throw std::runtime_error("Mounting a one-patch disk retained the previous bank");
            juce::MemoryBlock state;
            first->getStateInformation(state);
            auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
            auto tree = juce::ValueTree::fromXml(*xml);
            tree.setProperty("machineHasActiveSet", true, nullptr);
            tree.setProperty("machineActiveSetData", juce::var(image), nullptr);
            tree.setProperty("factoryProgram", 0, nullptr);
            for (const auto* key : { "machinePerformanceSnapshot", "machineFirmwareEditBuffers" })
                tree.removeProperty(key, nullptr);
            juce::AudioProcessor::copyXmlToBinary(*tree.createXml(), state);
            if (!diskFile.getFile().deleteFile())
                throw std::runtime_error("Could not remove one-patch source disk");
            second->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            if (second->getNumPrograms() != 256 || second->getProgramName(0) != "ONLY PATCH"
                || !second->getProgramName(1).isEmpty() || !second->getProgramName(255).isEmpty()
                || !second->hasMountedDiskImage()
                || second->getMountedDiskImageFile().getFileName() != diskFile.getFile().getFileName())
                throw std::runtime_error("One-patch project inherited old Performance slots");
            // An explicit empty-bank flag wins over an old saved bank payload.
            tree.setProperty("machineHasActiveSet", false, nullptr);
            for (const auto* key : { "machineDiskImageData", "machineDiskImageName", "mountedDiskImage" })
                tree.removeProperty(key, nullptr);
            juce::AudioProcessor::copyXmlToBinary(*tree.createXml(), state);
            first->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            first->getStateInformation(state);
            xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
            tree = juce::ValueTree::fromXml(*xml);
            if (first->getNumPrograms() != 1 || tree.hasProperty("machineActiveSetData")
                || tree.hasProperty("machineDiskImageData"))
                throw std::runtime_error("Empty project resurrected a previous bank or disk");
        }
        if (const auto* imagePath = std::getenv("WAVE_TEST_DISK_IMAGE"))
        {
            wave::firmware::DosFloppyImage::SetupFile setup;
            if (wave::firmware::DosFloppyImage::readWaveSetup(juce::File(imagePath), setup).failed())
                throw std::runtime_error("Cannot read supplied project-recall disk fixture");
            wave::presets::WaveFactorySet expected;
            if (!expected.load(setup.data).validLayout)
                throw std::runtime_error("Supplied disk fixture has no valid bank");
            juce::TemporaryFile disk(".img"), isolatedPreference(".txt");
            if (!juce::File(imagePath).copyFileTo(disk.getFile()))
                throw std::runtime_error("Cannot copy project-recall disk fixture");
            auto reader = std::make_unique<WaveEmulationAudioProcessor>(isolatedPreference.getFile());
            reader->prepareToPlay(48000.0, 512);
            if (reader->mountDiskImage(disk.getFile()).failed())
                throw std::runtime_error("Cannot mount project-recall fixture copy");
            juce::MemoryBlock state;
            reader->getStateInformation(state);
            auto reopened = std::make_unique<WaveEmulationAudioProcessor>(isolatedPreference.getFile());
            reopened->prepareToPlay(48000.0, 512);
            reopened->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            juce::AudioBuffer<float> audio(2, 512);
            for (int block = 0; block < 32; ++block)
            {
                juce::MidiBuffer midi;
                reopened->processBlock(audio, midi);
            }
            for (int program = 0; program < 256; ++program)
                if (reopened->getProgramName(program)
                    != juce::String(expected.performanceName(program / 128, program % 128)))
                    throw std::runtime_error("Disk fixture reopened with a different Performance name");
            const auto& runtime = reopened->getMasterFirmwareRuntime();
            if (runtime.isLoaded())
                for (size_t byte = 0; byte < expected.performanceBank().size(); ++byte)
                    if (runtime.sharedProgramByte(0x28000u + static_cast<uint32_t>(byte))
                        != expected.performanceBank()[byte])
                        throw std::runtime_error("Disk fixture's recalled Performance bank inherited old bytes");
            reopened->getStateInformation(state);
            auto rereopened = std::make_unique<WaveEmulationAudioProcessor>(isolatedPreference.getFile());
            rereopened->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            for (int program = 0; program < 256; ++program)
                if (rereopened->getProgramName(program)
                    != juce::String(expected.performanceName(program / 128, program % 128)))
                    throw std::runtime_error("Saving the recalled disk fixture changed an INIT slot");
            // Older state can contain a mixed bank even though its disk image
            // is correct. Reload replaces all slots, not just the selected one.
            auto cachedXml = juce::AudioProcessor::getXmlFromBinary(
                state.getData(), static_cast<int>(state.getSize()));
            auto contaminated = juce::ValueTree::fromXml(*cachedXml);
            auto wrongBank = setup.data;
            auto* wrong = static_cast<uint8_t*>(wrongBank.getData());
            std::fill_n(wrong + 0x22e7c + 5 * 512 + 32, 16, static_cast<uint8_t>(' '));
            std::copy_n("OLD DISK PATCH", 14, wrong + 0x22e7c + 5 * 512 + 32);
            contaminated.setProperty("machineActiveSetData", juce::var(wrongBank), nullptr);
            juce::AudioProcessor::copyXmlToBinary(*contaminated.createXml(), state);
            rereopened->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            if (rereopened->reloadBankFromMountedImage().failed())
                throw std::runtime_error("Could not reload correct bank from remembered image");
            for (int program = 0; program < 256; ++program)
                if (rereopened->getProgramName(program)
                    != juce::String(expected.performanceName(program / 128, program % 128)))
                    throw std::runtime_error("Reloading the image retained an older disk patch");
            std::cout << "Supplied disk bank retained all 256 Performance slots across project recall\n";
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
            if (recall == 0)
            {
                auto legacy = juce::AudioProcessor::getXmlFromBinary(state.getData(),
                                                                   static_cast<int>(state.getSize()));
                legacy->setAttribute("ecoMode", true);
                juce::MemoryBlock legacyState;
                juce::AudioProcessor::copyXmlToBinary(*legacy, legacyState);
                restored->setStateInformation(legacyState.getData(), static_cast<int>(legacyState.getSize()));
                restored->getStateInformation(legacyState);
                const auto cleaned = juce::AudioProcessor::getXmlFromBinary(
                    legacyState.getData(), static_cast<int>(legacyState.getSize()));
                if (cleaned->hasAttribute("ecoMode"))
                    throw std::runtime_error("Recalled state retained the retired Eco setting");
            }
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
