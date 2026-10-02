#include "Dsp/Cem3387.h"
#include "Dsp/PpgWaveRom.h"
#include "Dsp/ReferenceComparator.h"
#include "Dsp/WaldorfAsic.h"
#include "Dsp/WaldorfEngine.h"
#include "Dsp/WdvEnvelope.h"
#include "Dsp/WaveEnvelope.h"
#include "Dsp/WaveLfo.h"
#include "Dsp/DspMath.h"
#include "Firmware/DosFloppyImage.h"
#include "Firmware/FirmwareBundle.h"
#include "Firmware/Dp8473.h"
#include "Firmware/M68000.h"
#include "Firmware/MasterFirmwareRuntime.h"
#include "Firmware/Via6522.h"
#include "Firmware/VoiceFirmwareRuntime.h"
#include "Firmware/VoiceBoardProtocol.h"
#include "PanelWiring.h"
#include "Presets/WaveFactorySet.h"
#include "UI/WaveLcdModel.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <cmath>
#include <bit>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testDosFloppyImageCreation()
{
    const auto temporaryDirectory = juce::File::getSpecialLocation(
                                        juce::File::tempDirectory)
                                        .getNonexistentChildFile(
                                            "wave-dd-image-test", {}, true);
    require(temporaryDirectory.createDirectory().wasOk(),
            "Could not create the temporary DD-image test directory");
    const auto setupFile = temporaryDirectory.getChildFile("Factory Bank.set");
    const auto imageFile = temporaryDirectory.getChildFile("Factory Bank.img");
    const auto blankImageFile = temporaryDirectory.getChildFile("Blank Disk.img");
    std::vector<uint8_t> setup(503419u);
    for (size_t index = 0; index < setup.size(); ++index)
        setup[index] = static_cast<uint8_t>((index * 37u + 11u) & 0xffu);
    require(setupFile.replaceWithData(setup.data(), setup.size()),
            "Could not create the temporary Wave Setup");

    require(wave::firmware::DosFloppyImage::createEmpty(blankImageFile).wasOk(),
            "Could not create a blank canonical DD Wave disk image");
    juce::MemoryBlock blankImage;
    require(blankImageFile.loadFileAsData(blankImage)
                && blankImage.getSize() == 720u * 1024u,
            "Blank Wave disk is not exactly 720 KB");
    const auto* blankBytes = static_cast<const uint8_t*>(blankImage.getData());
    require(blankBytes[510] == 0x55u && blankBytes[511] == 0xaau
                && blankBytes[512] == 0xf9u && blankBytes[513] == 0xffu
                && blankBytes[514] == 0xffu && blankBytes[3584] == 0u,
            "Blank Wave disk is not an empty formatted FAT12 medium");

    const auto created = wave::firmware::DosFloppyImage::createWithWaveSetup(
        imageFile, setupFile);
    require(created.wasOk(), "Could not create a canonical DD Wave disk image");
    juce::MemoryBlock image;
    require(imageFile.loadFileAsData(image) && image.getSize() == 720u * 1024u,
            "Created Wave disk is not exactly 720 KB");
    const auto* bytes = static_cast<const uint8_t*>(image.getData());
    const auto get16 = [bytes](size_t offset) {
        return static_cast<uint16_t>(bytes[offset]
                                     | static_cast<uint16_t>(bytes[offset + 1u])
                                           << 8u);
    };
    require(get16(11) == 512 && bytes[13] == 2 && get16(19) == 1440
                && bytes[21] == 0xf9 && get16(22) == 3
                && get16(24) == 9 && get16(26) == 2,
            "Created Wave disk does not use canonical 720 KB DD geometry");
    require(std::equal(bytes + 512u, bytes + 2048u, bytes + 2048u),
            "Created Wave disk FAT copies differ");
    require(std::equal(bytes + 3584u, bytes + 3595u,
                       reinterpret_cast<const uint8_t*>("FACTORYBSET")),
            "Created Wave disk has an incorrect DOS 8.3 directory name");
    require(get16(3584u + 26u) == 2,
            "Created Wave Setup does not start at FAT cluster 2");
    const auto fileSize = static_cast<uint32_t>(bytes[3584u + 28u])
                          | static_cast<uint32_t>(bytes[3584u + 29u]) << 8u
                          | static_cast<uint32_t>(bytes[3584u + 30u]) << 16u
                          | static_cast<uint32_t>(bytes[3584u + 31u]) << 24u;
    require(fileSize == setup.size(),
            "Created Wave disk directory records the wrong Setup size");
    require(std::equal(setup.begin(), setup.end(), bytes + 7168u),
            "Created Wave disk does not preserve the Setup byte-for-byte");
    wave::firmware::DosFloppyImage::SetupFile extracted;
    require(wave::firmware::DosFloppyImage::readWaveSetup(imageFile, extracted).wasOk()
                && extracted.data.getSize() == setup.size()
                && std::equal(setup.begin(), setup.end(),
                              static_cast<const uint8_t*>(extracted.data.getData())),
            "Wave disk reader did not recover the created Setup byte-for-byte");
    for (const auto halfWaves : { 0u, 61u, 64u })
    {
        const auto wtbFile = temporaryDirectory.getChildFile("User Table.WTB");
        std::vector<uint8_t> wtb(138u + halfWaves * 64u, 0);
        std::copy_n(reinterpret_cast<const uint8_t*>("USER WT  "), 9, wtb.begin());
        wtb[9] = 0x55;
        std::fill(wtb.begin() + 10, wtb.begin() + 138, 0xff);
        for (size_t sample = 138; sample < wtb.size(); ++sample)
            wtb[sample] = static_cast<uint8_t>((sample * 37u + 3u) & 0xffu);
        require(wtbFile.replaceWithData(wtb.data(), wtb.size())
                    && wave::firmware::DosFloppyImage::createWithWaveWavetable(
                           imageFile, wtbFile).wasOk(),
                "Native WTB could not be packaged in a disk image");
        image.reset();
        require(imageFile.loadFileAsData(image) && image.getSize() == 720u * 1024u,
                "WTB disk has incorrect DD geometry");
        bytes = static_cast<const uint8_t*>(image.getData());
        require(std::equal(bytes + 3584, bytes + 3595,
                           reinterpret_cast<const uint8_t*>("USERTABLWTB"))
                    && std::equal(wtb.begin(), wtb.end(), bytes + 7168)
                    && wave::firmware::DosFloppyImage::readWaveSetup(imageFile, extracted).failed(),
                "WTB packaging changed the file or misidentified it as a SET");
        const auto size = static_cast<uint32_t>(bytes[3612])
                          | static_cast<uint32_t>(bytes[3613]) << 8u
                          | static_cast<uint32_t>(bytes[3614]) << 16u
                          | static_cast<uint32_t>(bytes[3615]) << 24u;
        require(size == wtb.size(), "WTB directory size differs from the original");
        require(std::equal(bytes + 512, bytes + 2048, bytes + 2048), "WTB FAT copies differ");
        const auto clusters = (wtb.size() + 1023u) / 1024u;
        for (size_t cluster = 2; cluster < 2 + clusters; ++cluster)
        {
            const auto offset = 512 + cluster * 3 / 2;
            const auto entry = cluster % 2 == 0
                ? static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] & 15u) << 8u)
                : static_cast<uint16_t>((bytes[offset] >> 4u) | bytes[offset + 1] << 4u);
            require(entry == (cluster + 1 == 2 + clusters ? 0xfffu : cluster + 1),
                    "WTB FAT cluster chain is truncated or unterminated");
        }
        const auto imageBefore = image;
        require(wave::firmware::DosFloppyImage::createWithWaveWavetable(wtbFile, wtbFile).failed(),
                "WTB conversion overwrote its source file");
        wtb[9] = 0;
        image.reset();
        require(wtbFile.replaceWithData(wtb.data(), wtb.size())
                    && wave::firmware::DosFloppyImage::createWithWaveWavetable(
                           imageFile, wtbFile).failed()
                    && imageFile.loadFileAsData(image) && image == imageBefore,
                "Rejected WTB conversion changed the destination image");
        wtb[9] = 0x55;
        wtb.push_back(1);
        require(wtbFile.replaceWithData(wtb.data(), wtb.size())
                    && wave::firmware::DosFloppyImage::createWithWaveWavetable(
                           imageFile, wtbFile).failed(),
                "Truncated WTB Wave data passed validation");
    }
    require(temporaryDirectory.deleteRecursively(),
            "Could not remove the temporary DD-image test directory");
}

void testDp8473MountedDiskImage()
{
    const auto temporary = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("wave-dp8473-test", ".img", false);
    std::vector<uint8_t> image(720u * 1024u, 0);
    image[11] = 0x00;
    image[12] = 0x02; // 512-byte sectors.
    image[19] = 0xa0;
    image[20] = 0x05; // 1440 sectors.
    image[24] = 9;
    image[26] = 2;
    std::fill_n(image.begin() + 512, 512, 0x5a);
    require(temporary.replaceWithData(image.data(), image.size()),
            "Could not create the temporary DP8473 disk image");

    wave::firmware::Dp8473 controller;
    const auto mounted = controller.mount(temporary);
    require(mounted.wasOk() && controller.isMounted(),
            "DP8473 rejected a valid 720 KB DOS image");
    require((controller.read(0xa8000f) & 0x80u) == 0,
            "DP8473 left the disk-change input asserted after insertion");
    controller.write(0xa80005, 0x1c); // Drive 0, motor/DMA on, reset released.

    const auto send = [&controller](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes)
            controller.write(0xa8000b, byte);
    };
    send({ 0x46, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x2a, 0xff });
    require(controller.drqAsserted(), "DP8473 did not assert DRQ for Read Data");
    for (int byte = 0; byte < 512; ++byte)
        require(controller.read(0xa8001b) == 0x5a,
                "DP8473 returned incorrect mounted-sector data");
    (void) controller.read(0xa8002b); // Wave GAL terminal-count alias.
    require(controller.interruptPending(),
            "DP8473 did not complete Read Data after terminal count");
    for (int byte = 0; byte < 7; ++byte)
        (void) controller.read(0xa8000b);

    send({ 0x45, 0x00, 0x00, 0x00, 0x03, 0x02, 0x03, 0x2a, 0xff });
    require(controller.drqAsserted(), "DP8473 did not assert DRQ for Write Data");
    for (int byte = 0; byte < 512; ++byte)
        controller.write(0xa8001b, 0xa5);
    (void) controller.read(0xa8002b);
    for (int byte = 0; byte < 7; ++byte)
        (void) controller.read(0xa8000b);
    require(controller.flush().wasOk(),
            "DP8473 did not persist a written sector");

    juce::MemoryBlock persisted;
    require(temporary.loadFileAsData(persisted) && persisted.getSize() == image.size(),
            "Could not reload the DP8473 disk image");
    const auto* persistedBytes = static_cast<const uint8_t*>(persisted.getData());
    require(std::all_of(persistedBytes + 1024, persistedBytes + 1536,
                        [](uint8_t value) { return value == 0xa5; }),
            "DP8473 sector writes were not saved at the correct CHS offset");
    // Reopening the same path must flush pending writes before reading it.
    send({ 0x45, 0x00, 0x00, 0x00, 0x04, 0x02, 0x04, 0x2a, 0xff });
    for (int byte = 0; byte < 512; ++byte)
        controller.write(0xa8001b, 0x3c);
    (void) controller.read(0xa8002b);
    for (int byte = 0; byte < 7; ++byte)
        (void) controller.read(0xa8000b);
    require(controller.mount(temporary).wasOk(),
            "DP8473 could not reopen its dirty mounted image");
    controller.write(0xa80005, 0x1c);
    send({ 0x46, 0x00, 0x00, 0x00, 0x04, 0x02, 0x04, 0x2a, 0xff });
    for (int byte = 0; byte < 512; ++byte)
        require(controller.read(0xa8001b) == 0x3c,
                "Reopening the mounted image discarded pending sector changes");
    (void) controller.read(0xa8002b);
    for (int byte = 0; byte < 7; ++byte)
        (void) controller.read(0xa8000b);
    require(controller.eject().wasOk(), "DP8473 could not eject its mounted image");
    require((controller.read(0xa8000f) & 0x80u) != 0,
            "DP8473 did not report an empty drive after ejecting its image");
    require(temporary.deleteFile(), "Could not remove the temporary DP8473 image");
}

void testDp8473AutomaticDiskWrites()
{
    juce::TemporaryFile directory;
    const auto media = directory.getFile().getChildFile("media");
    const auto offlineMedia = directory.getFile().getChildFile("offline");
    require(media.createDirectory().wasOk(), "Could not create disk-write test directory");
    const auto first = media.getChildFile("First.img");
    const auto second = media.getChildFile("Second.img");
    require(wave::firmware::DosFloppyImage::createEmpty(first).wasOk()
                && wave::firmware::DosFloppyImage::createEmpty(second).wasOk(),
            "Could not create disk-write fixtures");
    juce::MemoryBlock expectedFirst, expectedSecond;
    require(first.loadFileAsData(expectedFirst) && second.loadFileAsData(expectedSecond),
            "Could not read disk-write fixtures");
    const auto fillSector = [](juce::MemoryBlock& bytes, int sector, uint8_t value) {
        std::fill_n(static_cast<uint8_t*>(bytes.getData()) + (sector - 1) * 512,
                    512, value);
    };
    const auto matches = [](const juce::File& file, const juce::MemoryBlock& expected) {
        juce::MemoryBlock bytes;
        return file.loadFileAsData(bytes) && bytes == expected;
    };
    const auto waitFor = [](const auto& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    };

    {
        wave::firmware::Dp8473 controller;
        require(controller.mount(first).wasOk(), "Could not mount disk-write fixture");
        controller.write(0xa80005, 0x1c);
        const auto send = [&controller](std::initializer_list<uint8_t> bytes) {
            for (const auto byte : bytes)
                controller.write(0xa8000b, byte);
        };
        const auto finish = [&controller] {
            (void) controller.read(0xa8002b);
            for (int byte = 0; byte < 7; ++byte)
                (void) controller.read(0xa8000b);
        };
        const auto writeSector = [&](uint8_t sector, uint8_t value) {
            send({ 0x45, 0x00, 0x00, 0x00, sector, 0x02, sector, 0x2a, 0xff });
            for (int byte = 0; byte < 512; ++byte)
                controller.write(0xa8001b, value);
            finish();
        };

        writeSector(3, 0xa5);
        fillSector(expectedFirst, 3, 0xa5);
        require(waitFor([&] { return !controller.isDirty(); })
                    && matches(first, expectedFirst),
                "Sector writes did not reach the mounted file without a manual save");

        send({ 0x4d, 0x00, 0x02, 0x02, 0x2a, 0xe5 });
        for (const auto byte : { 0, 0, 4, 2, 0, 0, 5, 2 })
            controller.write(0xa8001b, static_cast<uint8_t>(byte));
        finish();
        fillSector(expectedFirst, 4, 0xe5);
        fillSector(expectedFirst, 5, 0xe5);
        require(waitFor([&] { return !controller.isDirty(); })
                    && matches(first, expectedFirst),
                "Formatted sectors did not automatically replace the disk image");

        // Manual flushes and the automatic writer may overlap newer DMA writes.
        // The last revision must win and all unrelated bytes must survive.
        std::atomic<bool> flushesSucceeded{ true };
        std::thread flusher([&] {
            for (int flush = 0; flush < 32; ++flush)
            {
                if (controller.flush().failed())
                    flushesSucceeded.store(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        });
        for (uint8_t value = 0; value < 32; ++value)
        {
            writeSector(6, value);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        flusher.join();
        fillSector(expectedFirst, 6, 31);
        require(flushesSucceeded.load()
                    && waitFor([&] { return !controller.isDirty(); })
                    && matches(first, expectedFirst),
                "Concurrent saves replaced newer sectors with an older image");

        // A disconnected destination must retain pending bytes and retry when
        // it returns, without replacing the old file with incomplete data.
        require(media.moveFileTo(offlineMedia), "Could not disconnect test media");
        writeSector(7, 0x7c);
        require(waitFor([&] { return controller.mountedDescription().contains("save failed"); })
                    && controller.isDirty()
                    && matches(offlineMedia.getChildFile(first.getFileName()), expectedFirst),
                "A failed automatic save lost data or was not reported");
        fillSector(expectedFirst, 7, 0x7c);
        require(offlineMedia.moveFileTo(media), "Could not reconnect test media");
        require(waitFor([&] { return !controller.isDirty(); })
                    && matches(first, expectedFirst)
                    && !controller.mountedDescription().contains("save failed"),
                "Automatic disk saving did not recover after reconnecting media");

        writeSector(8, 0x8d);
        fillSector(expectedFirst, 8, 0x8d);
        require(controller.mount(second).wasOk() && matches(first, expectedFirst),
                "Changing disks lost pending writes to the previous image");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(matches(second, expectedSecond),
                "The old disk's automatic save overwrote the replacement image");
        controller.write(0xa80005, 0x1c);
        writeSector(9, 0x9e);
        fillSector(expectedSecond, 9, 0x9e);
        // Destruction must save even if the batching interval has not elapsed.
    }
    require(matches(second, expectedSecond),
            "Controller destruction lost the final pending disk write");

    require(second.setReadOnly(true), "Could not write-protect test media");
    auto writeProtected = false;
    {
        wave::firmware::Dp8473 controller;
        const auto mounted = controller.mount(second);
        controller.write(0xa80005, 0x1c);
        for (const auto byte : { 0x45, 0, 0, 0, 9, 2, 9, 0x2a, 0xff })
            controller.write(0xa8000b, static_cast<uint8_t>(byte));
        const auto st0 = controller.read(0xa8000b);
        const auto st1 = controller.read(0xa8000b);
        writeProtected = mounted.wasOk() && !controller.isWritable()
                         && !controller.drqAsserted() && !controller.isDirty()
                         && (st0 & 0x40u) != 0 && (st1 & 0x02u) != 0;
    }
    require(second.setReadOnly(false), "Could not unlock test media");
    require(writeProtected && matches(second, expectedSecond),
            "Automatic saving bypassed native floppy write protection");
}

void testCutoffControlLaw()
{
    require(std::abs(wave::parameters::cutoffFrequencyForStep(0.0f) - 20.0f) < 1.0e-5f
                && std::abs(wave::parameters::cutoffFrequencyForStep(12.0f) - 40.0f) < 1.0e-4f
                && std::abs(wave::parameters::cutoffFrequencyForStep(60.0f) - 640.0f) < 1.0e-3f,
            "Cutoff control does not advance by one octave per twelve Wave steps");
    for (const auto step : { 0.0f, 31.75f, 63.5f, 95.25f, 127.0f })
        require(std::abs(wave::parameters::cutoffStepForFrequency(
                             wave::parameters::cutoffFrequencyForStep(step)) - step)
                    < 1.0e-3f,
                "Cutoff frequency and panel-control mappings are not inverse");
    require(wave::parameters::cutoffFrequencyForStep(127.0f) > 30000.0f,
            "Cutoff control no longer reaches the CEM3387's full-open range");
}

void testQuickEditFastAccessControls()
{
    wave::parameters::Snapshot sound;
    sound.attackSeconds = 0.1f;
    sound.filterAttackSeconds = 0.2f;
    sound.decaySeconds = 0.3f;
    sound.filterDecaySeconds = 0.4f;
    sound.sustainLevel = 0.4f;
    sound.filterSustainLevel = 0.5f;
    sound.releaseSeconds = 0.5f;
    sound.filterReleaseSeconds = 0.6f;
    sound.waveEnvelopeKeyOffPoint = 2;
    sound.waveEnvelopeTimes = { 10.0f, 20.0f, 30.0f, 40.0f,
                                50.0f, 60.0f, 70.0f, 80.0f };
    sound.waveEnvelopeLevels[2] = 48.0f;
    sound.freeEnvelopeTimes = { 10.0f, 20.0f, 30.0f, 40.0f };
    sound.freeEnvelopeLevels[2] = 10.0f;
    sound.wavePosition = 20.0f;
    sound.wavePosition2 = 24.0f;
    sound.cutoffHz = wave::parameters::cutoffFrequencyForStep(60.0f);
    sound.resonanceAmount = 0.3f;
    sound.waveScan = 20.0f;
    sound.waveScan2 = -16.0f;
    sound.modulationRoutes[wave::parameters::osc1PitchMod1].amount = 12.0f;
    sound.modulationRoutes[wave::parameters::wave1Mod1].amount = 10.0f;
    sound.quickEditAmounts = { 0.5f, 0.5f, 0.5f, 0.5f,
                               0.5f, 0.5f, 0.5f, 0.5f };

    wave::parameters::applyQuickEdit(sound);
    require(sound.attackSeconds > 0.1f && sound.filterAttackSeconds > 0.2f
                && sound.waveEnvelopeTimes[0] > 10.0f
                && sound.freeEnvelopeTimes[0] > 10.0f,
            "Quick Attack did not control all four envelope families");
    require(sound.decaySeconds > 0.3f && sound.filterDecaySeconds > 0.4f
                && sound.waveEnvelopeTimes[1] > 20.0f
                && sound.freeEnvelopeTimes[1] > 20.0f,
            "Quick Decay did not control all four envelope families");
    require(sound.sustainLevel > 0.4f && sound.filterSustainLevel > 0.5f
                && sound.waveEnvelopeLevels[2] > 48.0f
                && sound.freeEnvelopeLevels[2] > 10.0f,
            "Quick Sustain did not control all four envelope families");
    require(sound.releaseSeconds > 0.5f && sound.filterReleaseSeconds > 0.6f
                && sound.waveEnvelopeTimes[3] > 40.0f
                && sound.freeEnvelopeTimes[3] > 40.0f,
            "Quick Release did not control all four envelope families");
    require(sound.modulationRoutes[wave::parameters::osc1PitchMod1].amount > 12.0f,
            "Quick Pitch Mod did not scale oscillator modulation");
    require(sound.wavePosition > 20.0f && sound.wavePosition2 > 24.0f
                && sound.cutoffHz > wave::parameters::cutoffFrequencyForStep(60.0f)
                && sound.resonanceAmount > 0.3f,
            "Quick Timbre did not move both Waves and the filter");
    require(sound.modulationRoutes[wave::parameters::wave1Mod1].amount > 10.0f,
            "Quick Timbre Mod did not scale Wave/filter modulation");
    require(sound.waveScan > 20.0f && sound.waveScan2 < -16.0f,
            "Quick Wavescan did not scale both Wave-envelope amounts");
}

void testWavetableQuantisation()
{
    wave::dsp::WavetableBank bank;
    // Authentic generated tables use the complete signed-byte range,
    // including -128; the oscillator normalises that value by 128 below.

    const auto exact = bank.sample(7, 17.0f, 47.0 / 128.0, false);
    const auto raw = static_cast<float>(bank.rawSample(7, 17, 47)) / 128.0f;
    require(std::abs(exact - raw) < 1.0e-7f,
            "Oscillator-chip proxy lookup is not exact at integer coordinates");

    auto changingSamples = 0;
    auto absoluteDifference = 0;
    for (int sample = 0; sample < wave::dsp::WavetableBank::samplesPerWave; ++sample)
    {
        const auto difference = std::abs(
            static_cast<int>(bank.rawSample(32, 0, sample))
            - static_cast<int>(bank.rawSample(32, 31, sample)));
        changingSamples += difference != 0 ? 1 : 0;
        absoluteDifference += difference;
    }
    require(changingSamples > 100 && absoluteDifference > 1000,
            "Wavetable is effectively static across its sweep");

#if WAVE_HAS_PRIVATE_UPPER_TABLES
    const auto countCrossings = [&bank](int position) {
        auto crossings = 0;
        auto previous = bank.rawSample(32, position, 0);
        for (int sample = 1; sample < wave::dsp::WavetableBank::samplesPerWave; ++sample)
        {
            const auto current = bank.rawSample(32, position, sample);
            crossings += (previous < 0 && current >= 0) || (previous >= 0 && current < 0)
                             ? 1
                             : 0;
            previous = current;
        }
        return crossings;
    };
    require(countCrossings(60) > countCrossings(0),
            "SawSync 1 does not increase its slave ratio across the table");
#endif
}

void testAsicClockMixOverflowAndVcfSaturation()
{
    require(wave::dsp::OscillatorChipProxy::modelClockRate() == 250000.0,
            "ASIC proxy no longer runs at the original 250 kHz synthesis rate");

    using Mixer = wave::dsp::AsicOutputMixer;
    require(Mixer::mixOscillatorCodes(127, 127, 64, 65) == 127,
            "ES2 mixer wraps before the documented combined-level boundary");
    require(Mixer::mixOscillatorCodes(127, 127, 65, 65) == -128,
            "Positive ES2 numerical overflow clips instead of wrapping negative");
    require(Mixer::mixOscillatorCodes(-128, -128, 64, 64) == -128,
            "Negative ES2 mixer boundary changed its valid endpoint");
    require(Mixer::mixOscillatorCodes(-128, -128, 64, 65) == 127,
            "Negative ES2 numerical overflow clips instead of wrapping positive");

    const auto small = wave::dsp::Cem3387::saturateVcfInput(0.1f);
    const auto knee = wave::dsp::Cem3387::saturateVcfInput(0.7f);
    const auto full = wave::dsp::Cem3387::saturateVcfInput(1.0f);
    require(std::abs(small - 0.1f) < 0.001f
                && knee > 0.60f && knee < 0.67f
                && full > knee && full < 0.90f
                && std::abs(wave::dsp::Cem3387::saturateVcfInput(-0.7f) + knee)
                       < 1.0e-6f,
            "VCF input saturation is not a mild symmetric compression near 70% level");
}

void testAsicResampling(double clockRate = 250000.0)
{
    for (const auto hostRate : { 32000.0, 44100.0, 48000.0, 88200.0,
                                 96000.0, 192000.0, 250000.0, 384000.0 })
    {
        wave::dsp::AsicResampler resampler;
        resampler.prepare(hostRate, clockRate);
        auto clockPhase = 0.0;
        auto tick = int64_t { 0 };
        const auto render = [&](double frequency, int samples) {
            auto energy = 0.0;
            for (int sample = 0; sample < samples; ++sample)
            {
                clockPhase += clockRate / hostRate;
                while (clockPhase >= 1.0)
                {
                    clockPhase -= 1.0;
                    ++tick;
                    resampler.push(static_cast<float>(std::cos(
                        juce::MathConstants<double>::twoPi * frequency
                        * static_cast<double>(tick) / clockRate)));
                }
                const auto value = resampler.read(clockPhase);
                energy += static_cast<double>(value) * value;
            }
            return std::sqrt(energy / samples);
        };
        const auto measure = [&](double frequency) {
            resampler.reset();
            clockPhase = 0.0;
            tick = 0;
            static_cast<void>(render(frequency, 4096));
            return render(frequency, 8192);
        };
        require(std::abs(measure(0.0) - 1.0) < 2.0e-6,
                "ASIC resampler changes DC gain");
        const auto passband = measure(0.40 * std::min(hostRate, clockRate));
        require(std::abs(passband - std::sqrt(0.5)) < 0.002,
                "ASIC resampler attenuates the audible passband");
        if (hostRate < clockRate)
            for (const auto fraction : { 0.501, 0.55, 0.73, 1.13, 1.91 })
            {
                const auto frequency = fraction * hostRate;
                if (frequency < clockRate * 0.5)
                    require(measure(frequency) < 0.0002,
                            "ASIC resampler folds ultrasonic harmonics into the host band");
            }

        // Check the actual fractional sampling time, including hosts faster
        // than the ASIC (some output frames contain no new internal tick).
        resampler.reset();
        clockPhase = 0.0;
        tick = 0;
        constexpr auto frequency = 1000.0;
        const auto delayTicks = 2.0 * std::ceil(24.0 * clockRate / std::min(hostRate, clockRate));
        static_cast<void>(render(frequency, 4096));
        for (int sample = 0; sample < 1024; ++sample)
        {
            static_cast<void>(render(frequency, 1));
            const auto expected = std::cos(juce::MathConstants<double>::twoPi * frequency
                                          * (static_cast<double>(tick) + clockPhase
                                             - delayTicks) / clockRate);
            require(std::abs(resampler.read(clockPhase) - expected) < 0.0001,
                    "ASIC resampling has incorrect fractional timing");
        }
        resampler.reset();
        require(resampler.read(0.37) == 0.0f,
                "ASIC resampler retains history after reset");
    }
}

void testResamplerSimdAgainstScalar(double clockRate = 250000.0)
{
    constexpr auto phases = 64;
    for (const auto rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 250000.0 })
    {
        wave::dsp::AsicResampler resampler;
        resampler.prepare(rate, clockRate);
        const auto ratio = clockRate / std::min(rate, clockRate);
        const auto taps = 4 * static_cast<int>(std::ceil(24.0 * ratio));
        const auto radius = static_cast<double>(taps) * 0.5;
        const auto cutoff = 0.45 / ratio;
        std::vector<float> coefficients(static_cast<size_t>((phases + 1) * taps));
        for (int phase = 0; phase <= phases; ++phase)
        {
            double sum = 0.0;
            for (int tap = 0; tap < taps; ++tap)
            {
                const auto distance = tap + static_cast<double>(phase) / phases - radius;
                const auto angle = juce::MathConstants<double>::twoPi * cutoff * distance;
                const auto sinc = std::abs(angle) < 1.0e-12 ? 1.0 : std::sin(angle) / angle;
                const auto window = 0.42 + 0.5 * std::cos(
                    juce::MathConstants<double>::pi * distance / radius)
                    + 0.08 * std::cos(juce::MathConstants<double>::twoPi * distance / radius);
                auto& value = coefficients[static_cast<size_t>(phase * taps + tap)];
                value = static_cast<float>(2.0 * cutoff * sinc * window);
                sum += value;
            }
            for (int tap = 0; tap < taps; ++tap)
            {
                auto& value = coefficients[static_cast<size_t>(phase * taps + tap)];
                value = static_cast<float>(value / sum);
            }
        }
        std::vector<float> history(static_cast<size_t>(taps));
        int head = 0;
        uint32_t random = 12345;
        for (int tick = 0; tick < taps * 2 + 9; ++tick)
        {
            random = random * 1664525u + 1013904223u;
            const auto input = static_cast<float>(static_cast<int32_t>(random)) / 2147483648.0f;
            resampler.push(input);
            head = (head + taps - 1) % taps;
            history[static_cast<size_t>(head)] = input;
            for (const auto fraction : { 0.0, 0.013, 0.37, 0.999, 1.0 })
            {
                const auto position = fraction * phases;
                const auto phase = std::min(phases - 1, static_cast<int>(position));
                const auto blend = static_cast<float>(position - phase);
                std::array<float, 4> a {}, b {};
                for (int tap = 0; tap < taps; ++tap)
                {
                    const auto sample = history[static_cast<size_t>((head + tap) % taps)];
                    const auto lane = static_cast<size_t>(tap % 4);
                    a[lane] += sample * coefficients[static_cast<size_t>(phase * taps + tap)];
                    b[lane] += sample * coefficients[static_cast<size_t>((phase + 1) * taps + tap)];
                }
                const auto first = (a[0] + a[1]) + (a[2] + a[3]);
                const auto second = (b[0] + b[1]) + (b[2] + b[3]);
                const auto expected = first + blend * (second - first);
                require(std::abs(resampler.read(fraction) - expected) < 2.0e-6f,
                        "SIMD resampler differs from the scalar convolution");
            }
        }
    }
}

void testHighRegisterOscillatorResampling()
{
    // A synthetic bright wave has a 10 kHz fundamental and a 30 kHz third
    // harmonic. Point sampling at 48 kHz folds the third down to 18 kHz.
    using Bank = wave::dsp::WavetableBank;
    std::vector<int8_t> rom(static_cast<size_t>(Bank::factoryTableCount)
                            * Bank::wavesPerTable * Bank::samplesPerWave);
    for (size_t sample = 0; sample < rom.size(); ++sample)
    {
        const auto phase = juce::MathConstants<double>::twoPi
                           * static_cast<double>(sample % Bank::samplesPerWave)
                           / Bank::samplesPerWave;
        rom[sample] = static_cast<int8_t>(std::lround(48.0 * (std::cos(phase)
                                                            + std::cos(3.0 * phase))));
    }
    Bank bank;
    require(bank.loadSigned8BitRom(rom.data(), rom.size()),
            "Synthetic high-register wavetable did not load");
    wave::dsp::OscillatorChipProxy oscillator;
    oscillator.prepare(48000.0);
    oscillator.setFrequency(10000.0f);
    for (int sample = 0; sample < 4096; ++sample)
        static_cast<void>(oscillator.process(bank, 0, 0.0f));
    std::array<double, 2> real {};
    std::array<double, 2> imaginary {};
    constexpr std::array<double, 2> frequencies { 10000.0, 18000.0 };
    constexpr int sampleCount = 4800;
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto output = oscillator.process(bank, 0, 0.0f);
        for (size_t bin = 0; bin < frequencies.size(); ++bin)
        {
            const auto angle = juce::MathConstants<double>::twoPi * frequencies[bin]
                               * sample / 48000.0;
            real[bin] += output * std::cos(angle);
            imaginary[bin] += output * std::sin(angle);
        }
    }
    const auto fundamental = 2.0 * std::hypot(real[0], imaginary[0]) / sampleCount;
    const auto alias = 2.0 * std::hypot(real[1], imaginary[1]) / sampleCount;
    require(fundamental > 0.35 && fundamental < 0.40,
            "High-register oscillator loses its in-band fundamental");
    require(alias < 0.0001,
            "High-register oscillator aliases its ultrasonic third harmonic");
}

void testWaveEnvelopeTraversal()
{
    wave::parameters::Snapshot parameters;
    parameters.waveEnvelopeTimes.fill(0.0f);
    parameters.waveEnvelopeLevels.fill(0.0f);
    parameters.waveEnvelopeTimes[0] = 8.0f;
    parameters.waveEnvelopeLevels[0] = 127.0f;
    parameters.waveEnvelopeTimes[1] = 8.0f;
    parameters.waveEnvelopeLevels[1] = 0.0f;
    parameters.waveEnvelopeKeyOffPoint = 1;

    wave::dsp::WaveEnvelope envelope;
    envelope.prepare(1000.0);
    envelope.noteOn();
    const auto first = envelope.process(parameters);
    auto peak = first;
    for (int sample = 0; sample < 30; ++sample)
        peak = juce::jmax(peak, envelope.process(parameters));
    require(peak > first && peak > 0.9f,
            "Eight-stage Wave envelope did not traverse its first level");

    envelope.noteOff();
    for (int sample = 0; sample < 30; ++sample)
        static_cast<void>(envelope.process(parameters));
    require(envelope.currentValue() < 0.1f,
            "Wave envelope did not traverse its post-key-off segment");

    parameters.waveEnvelopeLoop = true;
    parameters.waveEnvelopeLoopStartPoint = 0;
    envelope.noteOn();
    auto minimum = 1.0f;
    auto maximum = 0.0f;
    for (int sample = 0; sample < 80; ++sample)
    {
        const auto value = envelope.process(parameters);
        minimum = juce::jmin(minimum, value);
        maximum = juce::jmax(maximum, value);
    }
    require(minimum < 0.1f && maximum > 0.9f,
            "Wave envelope sustain loop did not revisit its wavetable range");
}

void testWaveLfo()
{
    require(std::abs(wave::dsp::WaveLfo::rateHz(0.0f) - 0.09) < 1.0e-9
                && std::abs(wave::dsp::WaveLfo::rateHz(127.0f) - 24.0) < 1.0e-9,
            "Wave LFO rate endpoints are incorrect");

    wave::dsp::WaveLfo lfo;
    lfo.prepare(1000.0, 0x12345678u);
    lfo.noteOn(2, 0.0f);
    auto minimum = 1.0f;
    auto maximum = -1.0f;
    for (int sample = 0; sample < 1000; ++sample)
    {
        const auto value = lfo.process(90.0f, 0, 0.0f, 0);
        minimum = juce::jmin(minimum, value);
        maximum = juce::jmax(maximum, value);
    }
    require(minimum < -0.95f && maximum > 0.95f,
            "Wave sine LFO did not traverse its bipolar range");

    lfo.noteOn(2, 90.0f);
    const auto retriggered = lfo.process(40.0f, 5, 0.0f, 0);
    for (int sample = 0; sample < 100; ++sample)
        require(std::abs(lfo.process(40.0f, 5, 0.0f, 0) - retriggered) < 1.0e-7f,
                "Wave sample-and-hold LFO changed within a cycle");
}

void testDspMathTables()
{
    for (int note = 0; note < 128; ++note)
        require(std::abs(wave::dsp::math::midiFrequency(note)
                         - static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(note)))
                    <= wave::dsp::math::midiFrequency(note) * 1.0e-6f,
                "MIDI pitch lookup table exceeded its error bound");
}

void testPerformanceTuningTables()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 32);

    wave::dsp::WaldorfEngine::PerformanceSnapshot performance;
    auto& layer = performance.layers[0];
    layer.enabled = true;
    layer.source = 2;
    layer.sound.attackSeconds = 0.001f;

    juce::AudioBuffer<float> audio(2, 32);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    engine.render(audio, noteOn, performance);

    const auto heldPitch = [&engine](int triggerNote) {
        for (const auto& state : engine.voiceStates())
            if (state.active && state.keyDown && state.triggerNote == triggerNote)
                return state.glidePitch;
        throw std::runtime_error("Expected tuning-table test voice is not active");
    };

    require(std::abs(heldPitch(60) - 60.0f) < 1.0e-5f,
            "Linear+ temperament changed equal-tempered pitch");

    // A live Instrument edit must reach already sounding voices. Linear- is
    // mirrored around MIDI note 64, so note 60 becomes note 68.
    layer.tuningTable = 3;
    audio.clear();
    engine.render(audio, {}, performance);
    require(std::abs(heldPitch(60) - 68.0f) < 1.0e-5f,
            "Instrument TuneTable changed state but not oscillator pitch");

    // Native SET user-table data consists of note/detune pairs. Install a
    // deliberately conspicuous G+25c mapping for middle C in User table 1.
    juce::MemoryBlock setImage(0x4327cu, true);
    auto* bytes = static_cast<uint8_t*>(setImage.getData());
    for (size_t table = 0; table < 4u; ++table)
        for (size_t key = 0; key < 128u; ++key)
        {
            const auto offset = 0x42e7cu + table * 256u + key * 2u;
            bytes[offset] = static_cast<uint8_t>(key);
            bytes[offset + 1u] = 64u;
        }
    bytes[0x42e7cu + 60u * 2u] = 67u;
    bytes[0x42e7cu + 60u * 2u + 1u] = 89u;
    juce::ignoreUnused(engine.loadWaveSetUserTables(setImage));

    layer.tuningTable = 0;
    audio.clear();
    engine.render(audio, {}, performance);
    require(std::abs(heldPitch(60) - 67.25f) < 1.0e-5f,
            "Wave SET user Tuning Table note/detune pair was not decoded through Global");

    layer.tuningTable = 8;
    audio.clear();
    engine.render(audio, {}, performance);
    require(std::abs(heldPitch(60) - 67.25f) < 1.0e-5f,
            "Wave SET User table 1 could not be selected directly");

    // A SET with displaced Performance bytes in its tuning pool must not
    // remap every Global Instrument to arbitrary notes. Reset each damaged
    // table completely while preserving an adjacent valid custom tuning.
    const auto pristineTuning = setImage;
    bytes[0x42e7cu + 69u * 2u + 1u] = 0;
    bytes[0x42e7cu + 256u + 60u * 2u] = 62;
    bytes[0x42e7cu + 256u + 60u * 2u + 1u] = 114;
    bytes[0x42e7cu + 512u + 69u * 2u] = 0x80;
    bytes[0x42e7cu + 768u + 69u * 2u + 1u] = 115;
    const auto damagedTuning = setImage;
    juce::ignoreUnused(engine.loadWaveSetUserTables(setImage));
    const auto checkPitch = [&](int table, int key, float expected) {
        layer.tuningTable = table;
        juce::MidiBuffer events;
        events.addEvent(juce::MidiMessage::allSoundOff(1), 0);
        events.addEvent(juce::MidiMessage::noteOn(1, key, 0.9f), 1);
        engine.render(audio, events, performance);
        require(std::abs(heldPitch(key) - expected) < 1.0e-5f,
                "Malformed SET tuning data changed the played note");
    };
    for (const auto key : { 48, 60, 69, 72 })
        checkPitch(0, key, static_cast<float>(key));
    checkPitch(8, 60, 60.0f);
    checkPitch(9, 60, 62.5f);
    checkPitch(10, 60, 60.0f);
    checkPitch(11, 60, 60.0f);
    require(setImage == damagedTuning, "Tuning quarantine changed source SET bytes");
    juce::ignoreUnused(engine.loadWaveSetUserTables(pristineTuning));
    checkPitch(0, 60, 67.25f);

    // HMT retunes a held chord as notes arrive. The oscillator must keep that
    // pitch after the next voice-board control update, including when changing
    // back to HMT while the chord is already sounding.
    wave::dsp::WaldorfEngine hmtEngine;
    hmtEngine.prepare(48000.0, 32);
    wave::dsp::WaldorfEngine::PerformanceSnapshot hmtPerformance;
    auto& hmtLayer = hmtPerformance.layers[0];
    hmtLayer.enabled = true;
    hmtLayer.source = 2;
    hmtLayer.tuningTable = 2;
    hmtLayer.sound.attackSeconds = 0.001f;
    hmtLayer.sound.detuneCents = 0.0f;

    const auto playHmtNote = [&](int midiNote) {
        juce::MidiBuffer event;
        event.addEvent(juce::MidiMessage::noteOn(1, midiNote, 0.9f), 0);
        audio.clear();
        hmtEngine.render(audio, event, hmtPerformance);
    };
    const auto oscillatorPitch = [&](int triggerNote) {
        for (const auto& state : hmtEngine.voiceStates())
            if (state.active && state.keyDown && state.triggerNote == triggerNote)
                return state.oscillator1FrequencyHz;
        throw std::runtime_error("Expected HMT test voice is not active");
    };
    const auto frequencyForNote = [](float midiNote) {
        return 440.0f * std::exp2((midiNote - 69.0f) / 12.0f);
    };
    const auto requireOscillatorPitch = [&](int triggerNote, float tunedNote,
                                            const char* message) {
        const auto expected = frequencyForNote(tunedNote);
        require(std::abs(oscillatorPitch(triggerNote) - expected) < expected * 1.0e-4f,
                message);
    };

    playHmtNote(60);
    playHmtNote(64);
    audio.clear();
    hmtEngine.render(audio, {}, hmtPerformance);
    requireOscillatorPitch(64, 63.86314f,
                           "New HMT chord note reverted to equal temperament");

    hmtLayer.tuningTable = 1;
    audio.clear();
    hmtEngine.render(audio, {}, hmtPerformance);
    requireOscillatorPitch(64, 64.0f,
                           "Changing from HMT did not retune a held note");

    hmtLayer.tuningTable = 2;
    audio.clear();
    hmtEngine.render(audio, {}, hmtPerformance);
    requireOscillatorPitch(64, 63.86314f,
                           "Switching to HMT retuned only until the next control update");

    // A held note started in Linear- can be dozens of semitones below its HMT
    // pitch. Retuning the voice must also retune its layer glide history, or
    // every later note with Glide enabled starts down at the old pitch.
    wave::dsp::WaldorfEngine glideEngine;
    glideEngine.prepare(48000.0, 32);
    auto glidePerformance = hmtPerformance;
    auto& glideLayer = glidePerformance.layers[0];
    glideLayer.tuningTable = 3;
    glideLayer.sound.glideEnabled = true;
    glideLayer.sound.glideRateValue = 50.0f;
    juce::MidiBuffer highNote;
    highNote.addEvent(juce::MidiMessage::noteOn(1, 84, 0.9f), 0);
    glideEngine.render(audio, highNote, glidePerformance);
    glideLayer.tuningTable = 2;
    glideEngine.render(audio, {}, glidePerformance);
    juce::MidiBuffer nextNote;
    nextNote.addEvent(juce::MidiMessage::noteOn(1, 86, 0.9f), 0);
    glideEngine.render(audio, nextNote, glidePerformance);
    auto nextPitch = -1.0f;
    for (const auto& state : glideEngine.voiceStates())
        if (state.active && state.keyDown && state.triggerNote == 86)
            nextPitch = state.glidePitch;
    require(nextPitch > 83.0f && nextPitch < 86.1f,
            "Changing to HMT left later Glide notes at the previous low tuning");

    // With Glide disabled, changing tuning under a held key must not leave
    // subsequent notes at the pitch of the old table.
    wave::dsp::WaldorfEngine directEngine;
    directEngine.prepare(48000.0, 32);
    auto directPerformance = hmtPerformance;
    auto& directLayer = directPerformance.layers[0];
    directLayer.sound.glideEnabled = false;
    directLayer.sound.oscillatorOctaves[0] = -1;
    for (const auto table : { 3, 2, 1, 2, 3, 1 })
    {
        directLayer.tuningTable = table;
        juce::MidiBuffer heldNote;
        heldNote.addEvent(juce::MidiMessage::noteOn(1, 72, 0.9f), 0);
        directEngine.render(audio, heldNote, directPerformance);
        const auto nextTable = table == 3 ? 2 : 3;
        directLayer.tuningTable = nextTable;
        directEngine.render(audio, {}, directPerformance);
        juce::MidiBuffer releaseAndPlay;
        releaseAndPlay.addEvent(juce::MidiMessage::noteOff(1, 72), 0);
        releaseAndPlay.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 1);
        directEngine.render(audio, releaseAndPlay, directPerformance);
        auto freshFrequency = 0.0f;
        for (const auto& state : directEngine.voiceStates())
            if (state.active && state.keyDown && state.triggerNote == 60)
                freshFrequency = state.oscillator1FrequencyHz;
        const auto expectedFrequency
            = 0.5f * frequencyForNote(nextTable == 3 ? 68.0f : 60.0f);
        require(std::abs(freshFrequency - expectedFrequency)
                    < expectedFrequency * 1.0e-4f,
                "Changing a tuning table under a held key left a new note subsonic without Glide");
        juce::MidiBuffer release;
        release.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        directEngine.render(audio, release, directPerformance);
    }

    wave::dsp::WaldorfEngine chordEngine;
    chordEngine.prepare(48000.0, 32);
    auto chordPerformance = directPerformance;
    auto& chordLayer = chordPerformance.layers[0];
    chordLayer.tuningTable = 1;
    chordLayer.sound.oscillatorSemitones[0] = 3.0f;
    juce::MidiBuffer firstKey;
    firstKey.addEvent(juce::MidiMessage::noteOn(1, 72, 0.9f), 0);
    chordEngine.render(audio, firstKey, chordPerformance);
    chordLayer.tuningTable = 2;
    chordEngine.render(audio, {}, chordPerformance);
    juce::MidiBuffer secondKey;
    secondKey.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    chordEngine.render(audio, secondKey, chordPerformance);
    chordEngine.render(audio, {}, chordPerformance);
    for (const auto& state : chordEngine.voiceStates())
    {
        if (!state.active || !state.keyDown)
            continue;
        const auto expected
            = frequencyForNote(static_cast<float>(state.triggerNote) - 9.0f);
        require(std::abs(state.oscillator1FrequencyHz - expected)
                    < expected * 1.0e-4f,
                "Second key pressed while holding HMT chord has incorrect pitch");
    }
}

void testFreeRunningEngineLfo()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(1000.0, 16);
    wave::parameters::Snapshot parameters;
    parameters.lfos[0].rate = 127.0f;
    parameters.lfos[0].shape = 0;
    parameters.lfos[0].sync = 0;
    parameters.attackSeconds = 0.001f;

    juce::AudioBuffer<float> idle(2, 10);
    engine.render(idle, {}, parameters);

    juce::AudioBuffer<float> onset(2, 1);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(onset, noteOn, parameters);
    require(engine.firstActiveLfoValue(0) > 0.9f,
            "Unsynchronised Wave LFO phase froze while no voice was sounding");
}

void testSparsePerformanceBanks()
{
    constexpr size_t soundOffset = 0x12e7c;
    constexpr size_t performanceOffset = 0x22e7c;
    juce::MemoryBlock image(performanceOffset + 256 * 512, true);
    auto* bytes = static_cast<uint8_t*>(image.getData());
    bytes[soundOffset + 239] = 0x55;
    bytes[performanceOffset + 48] = 0x55;
    std::copy_n("ONLY PATCH", 10, bytes + performanceOffset + 32);
    wave::presets::WaveFactorySet set;
    const auto report = set.load(image);
    require(report.validLayout && report.validPerformances == 1
                && report.emptyPerformances == 255,
            "A one-patch SET with empty slots was rejected");
    require(set.performanceName(0, 1).empty() && set.performance(1, 127)[48] == 0,
            "An empty Performance was replaced during SET import");
    const auto pristine = image;
    auto* damaged = bytes + performanceOffset + 151u * 512u;
    std::fill_n(damaged, 512, uint8_t{ 0xff });
    damaged[48] = 0x55;
    auto damagedReport = set.load(image);
    require(damagedReport.validLayout && damagedReport.invalidPerformances == 1
                && set.performanceName(0, 0) == "ONLY PATCH"
                && std::all_of(set.performance(1, 23).begin(), set.performance(1, 23).end(),
                               [](uint8_t value) { return value == 0; })
                && set.sourceImage() == image,
            "One damaged Performance rejected the bank or changed the source disk bytes");
    require(set.loadStateSnapshot(image).validLayout && set.performance(1, 23)[0] == 0xff,
            "Disk-import quarantine changed an exact host SRAM snapshot");
    image = pristine;
    bytes = static_cast<uint8_t*>(image.getData());
    for (size_t slot = 1; slot <= 7; ++slot)
        bytes[performanceOffset + slot * 512u + 32u] = 0xff;
    require(!set.load(image).validLayout,
            "A bank with more than six malformed Performances passed SET validation");
    image.reset();
    image.setSize(performanceOffset + 256 * 512, true);
    require(!set.load(image).validLayout, "A zero-filled file passed disk SET validation");
    require(set.loadStateSnapshot(image).validLayout,
            "An empty native host bank failed snapshot recall");
    require(set.performanceName(0, 0).empty(), "Empty snapshot recalled a prior patch");
}

void testDamagedUserWavetableImport()
{
    constexpr size_t tableOffset = 0x4387c;
    constexpr size_t waveOffset = 0x45afc;
    std::vector<uint8_t> setup(waveOffset + 1000u * 64u, 128);
    const auto reference = [&](int table, int wave, uint16_t value) {
        const auto offset = tableOffset + static_cast<size_t>(table) * 138u
                            + 10u + static_cast<size_t>(wave) * 2u;
        setup[offset] = static_cast<uint8_t>(value >> 8u);
        setup[offset + 1] = static_cast<uint8_t>(value);
    };
    for (int table = 0; table < 64; ++table)
    {
        const auto record = tableOffset + static_cast<size_t>(table) * 138u;
        setup[record + 9] = 0x55;
        std::fill_n(setup.begin() + static_cast<std::ptrdiff_t>(record + 10), 128, 0xff);
        reference(table, 0, 0);
        reference(table, 60, 1);
    }
    wave::dsp::WavetableBank init;
    require(init.loadWaveSetUserTables(setup.data(), setup.size()), "INIT fixture failed to import");
    const auto hash = [](const wave::dsp::WavetableBank& bank, int first, int last) {
        std::vector<int8_t> codes;
        for (int table = first; table < last; ++table)
            for (int wave = 0; wave < 64; ++wave)
                for (int sample = 0; sample < 128; ++sample)
                    codes.push_back(bank.rawSample(table, wave, sample));
        return juce::SHA256(codes.data(), codes.size()).toHexString();
    };
    auto imported = init;
    const auto audioSnapshot = imported.renderSnapshot();
    const auto previousHash = hash(init, 0, 128);
    const auto factoryHash = hash(init, 0, 64);
    reference(0, 0, 300);
    reference(63, 0, 1299); // Last valid user Wave, after damaged table 93.
    setup[waveOffset] = 230;
    setup[waveOffset + 999u * 64u] = 20;
    reference(28, 0, 0x4c54); // DG_PE's misplaced Performance record.
    setup[tableOffset + 30u * 138u + 9u] = 0;
    reference(31, 0, 1300); // One past the last stored user Wave.
    reference(32, 0, 0xffff); // Sparse table with no leading anchor.
    reference(32, 15, 2);
    const auto source = setup;
    require(imported.loadWaveSetUserTables(setup.data(), setup.size())
                && imported.hasWaveSetUserTables() && imported.damagedUserTableCount() == 3,
            "Damaged records aborted the complete user-table import");
    require(imported.rawSample(64, 0, 0) == 102 && imported.rawSample(127, 0, 0) == -108,
            "Valid tables on either side of a damaged record were not imported");
    for (const auto table : { 92, 94, 95 })
        for (int wave = 0; wave < 64; ++wave)
            for (int sample = 0; sample < 128; ++sample)
                require(imported.rawSample(table, wave, sample) == init.rawSample(64, wave, sample),
                        "A damaged user table retained old samples instead of INIT");
    require(imported.rawSample(96, 0, 0) == imported.rawSample(0, 0, 0)
                && hash(imported, 0, 64) == factoryHash && hash(init, 0, 128) == previousHash
                && hash(audioSnapshot, 0, 128) == previousHash
                && setup == source,
            "User import changed factory waves, a shared bank, or its source bytes");
    const auto importedHash = hash(imported, 0, 128);
    require(!imported.loadWaveSetUserTables(setup.data(), waveOffset)
                && hash(imported, 0, 128) == importedHash,
            "Truncated SET changed the active wavetable bank");
    for (int table = 0; table < 64; ++table)
        setup[tableOffset + static_cast<size_t>(table) * 138u + 9u] = 0;
    require(!imported.loadWaveSetUserTables(setup.data(), setup.size())
                && hash(imported, 0, 128) == importedHash,
            "Unrelated data replaced the active wavetable bank");
    require(imported.loadWaveSetUserTables(source.data(), source.size()), "Repeated SET import failed");

    // Publishing from the loader must not change a bank being sampled by
    // the audio thread, including when factory ROMs are replaced explicitly.
    std::vector<int8_t> firstBank(128u * 64u * 128u, 24);
    std::vector<int8_t> secondBank(firstBank.size(), -53);
    require(imported.loadSigned8BitRom(firstBank.data(), firstBank.size()), "Concurrent fixture failed");
    std::atomic<bool> finished{ false };
    std::atomic<bool> coherent{ true };
    std::atomic<int> snapshots{ 0 };
    std::thread renderer([&] {
        while (!finished.load(std::memory_order_acquire))
        {
            const auto snapshot = imported.renderSnapshot();
            const auto expected = snapshot.rawSample(0, 0, 0);
            if (expected != 24 && expected != -53)
                coherent.store(false, std::memory_order_relaxed);
            for (int table = 0; table < 128; ++table)
                for (int wave = 0; wave < 64; ++wave)
                    if (snapshot.rawSample(table, wave, (wave * 13) % 128) != expected)
                        coherent.store(false, std::memory_order_relaxed);
            snapshots.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (snapshots.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();
    bool loaded = true;
    for (int load = 0; load < 32; ++load)
    {
        const auto& bank = load % 2 == 0 ? secondBank : firstBank;
        loaded = imported.loadSigned8BitRom(bank.data(), bank.size()) && loaded;
    }
    finished.store(true, std::memory_order_release);
    renderer.join();
    require(loaded && coherent.load() && snapshots.load() > 0,
            "Concurrent wavetable publication changed samples within an audio snapshot");
}

void testFactorySetWhenAvailable()
{
    const auto* path = std::getenv("WAVE_FACTORY_SET");
    if (path == nullptr)
        return;

    juce::MemoryBlock data;
    require(juce::File(path).loadFileAsData(data),
            "WAVE_FACTORY_SET could not be read");
    wave::presets::WaveFactorySet factory;
    const auto report = factory.load(data);
    require(report.validLayout && report.validSounds == 256
                && report.validPerformances == 256,
            "Factory SET native sound or performance banks are incomplete");
    require(report.sha256
                == "cdcd1a1882f3c0ca38752cde552dcebf7579be94deb46fdfd2b26fbccaba44c9",
            "Factory SET image hash changed unexpectedly");
    require(factory.soundName(0, 0) == "sitar"
                && factory.soundName(0, 1) == "DROOPOLYFLANGE"
                && factory.performanceName(0, 0) == "drooSyn 1 oo DN"
                && factory.performanceName(1, 0) == "WoodOrgan    WMF",
            "Factory SET bank addressing or fixed-width name decoding is incorrect");

    const auto soundDump = factory.soundDump(0, 1, 0);
    const auto performanceDump = factory.performanceDump(0, 0);
    require(soundDump.size() == 266 && performanceDump.size() == 521
                && soundDump.front() == 0xf0 && soundDump.back() == 0xf7
                && performanceDump.front() == 0xf0 && performanceDump.back() == 0xf7,
            "Factory records were not framed as Wave SysEx dumps");
    const auto checksum = [](const std::vector<uint8_t>& dump) {
        auto sum = 0u;
        for (size_t index = 5; index + 2 < dump.size(); ++index)
            sum += dump[index];
        return static_cast<uint8_t>(sum & 0x7fu);
    };
    require(checksum(soundDump) == soundDump[soundDump.size() - 2]
                && checksum(performanceDump) == performanceDump[performanceDump.size() - 2],
            "Factory-record Wave SysEx checksum is incorrect");

    wave::dsp::WavetableBank wavetableBank;
    require(wavetableBank.loadWaveSetUserTables(data.getData(), data.getSize())
                && wavetableBank.hasWaveSetUserTables(),
            "Factory SET user Wavetables were not decoded");

    // TSITAR3's first 13 references are native U987..U999 (1287..1299),
    // followed by legacy Wave Edit workspace references $100D..$103F.
    constexpr auto sitarTable = 95;
    constexpr size_t userWaveBankOffset = 0x45afc;
    const auto* setBytes = static_cast<const uint8_t*>(data.getData());
    for (int wave = 0; wave < 13; ++wave)
        for (int sample = 0; sample < 64; ++sample)
        {
            const auto stored = setBytes[userWaveBankOffset
                                         + static_cast<size_t>(987 + wave) * 64u
                                         + static_cast<size_t>(sample)];
            require(wavetableBank.rawSample(sitarTable, wave, sample)
                        == static_cast<int8_t>(static_cast<int>(stored) - 128)
                        && wavetableBank.rawSample(sitarTable, wave, 127 - sample)
                               == static_cast<int8_t>(static_cast<int>(
                                                         static_cast<uint8_t>(~stored)) - 128),
                    "TSITAR3 native user-Wave reference was decoded incorrectly");
        }

    for (int sample = 0; sample < 64; ++sample)
    {
        const auto stored = setBytes[userWaveBankOffset + 13u * 64u
                                     + static_cast<size_t>(sample)];
        require(wavetableBank.rawSample(sitarTable, 13, sample)
                    == static_cast<int8_t>(static_cast<int>(stored) - 128)
                    && wavetableBank.rawSample(sitarTable, 13, 127 - sample)
                           == static_cast<int8_t>(static_cast<int>(
                                                     static_cast<uint8_t>(~stored))
                                                 - 128),
                "TSITAR3 user Wave was not reconstructed from its SET half-wave");
    }
}

std::vector<uint8_t> makeSyntheticPpgRom()
{
    std::vector<uint8_t> rom(wave::dsp::PpgWaveRom::minimumImageBytes, 0);
    size_t cursor = 0;
    for (int table = 0; table < wave::dsp::PpgWaveRom::storedTableCount + 1; ++table)
    {
        rom[cursor++] = static_cast<uint8_t>(table);
        rom[cursor++] = 0;
        rom[cursor++] = 0;
        rom[cursor++] = 1;
        rom[cursor++] = 60;
    }

    const auto waveformOffset = wave::dsp::PpgWaveRom::tableDirectoryBytes;
    for (size_t sample = 0; sample < wave::dsp::PpgWaveRom::halfWaveSamples; ++sample)
    {
        rom[waveformOffset + sample] = 128;
        rom[waveformOffset + wave::dsp::PpgWaveRom::halfWaveSamples
            + sample] = 192;
    }
    return rom;
}

void testPpgRomDecoding()
{
    auto rom = makeSyntheticPpgRom();
    wave::dsp::WavetableBank bank;
    require(bank.loadPpgWaveRom(rom.data(), rom.size()),
            "Valid sparse PPG EPROM image was rejected");
    require(bank.importedTableCount() == 30 && bank.isExternalRomLoaded(),
            "PPG EPROM source metadata is incorrect");
    require(bank.rawSample(0, 0, 7) == 0,
            "PPG key waveform was not read from the half-wave bank");
    require(bank.rawSample(0, 30, 7) == 32,
            "PPG sparse-table interpolation is incorrect");
    require(bank.rawSample(0, 1, 7) == 2,
            "PPG firmware midpoint interpolation order is incorrect");
    require(bank.rawSample(0, 30, 120) == -33,
            "PPG half-wave mirroring or polarity is incorrect");
    require(bank.rawSample(28, 0, 0) == -64
                && bank.rawSample(29, 0, 67) == 32
                && bank.rawSample(29, 0, 68) == -32
                && bank.rawSample(28, 3, 11) == -51,
            "PPG firmware-generated tables 28/29 are incorrect");
    require(bank.rawSample(0, 60, 0) == -96
                && bank.rawSample(0, 60, 64) == 96
                && bank.rawSample(0, 61, 125) == -48
                && bank.rawSample(0, 61, 126) == 127
                && bank.rawSample(0, 62, 0) == -96
                && bank.rawSample(0, 62, 64) == 96
                && bank.rawSample(0, 63, 0) == -64
                && bank.rawSample(0, 63, 127) == 63,
            "Classic PPG firmware tail waves were not generated byte-exactly");

    wave::dsp::WavetableBank rejected;
    const auto before = rejected.rawSample(0, 0, 0);
    rom[4] = 59; // Terminal key must be slot 60.
    require(!rejected.loadPpgWaveRom(rom.data(), rom.size()),
            "Malformed PPG sparse table was accepted");
    require(!rejected.isExternalRomLoaded() && rejected.rawSample(0, 0, 0) == before,
            "Rejected PPG image changed the active wavetable bank");
}

void testBundledPpgWavetables()
{
    wave::dsp::WavetableBank bank;
    std::vector<int8_t> samples;
    samples.reserve(30 * 64 * 128);
    for (int table = 0; table < 30; ++table)
        for (int wave = 0; wave < 64; ++wave)
            for (int sample = 0; sample < 128; ++sample)
                samples.push_back(bank.rawSample(table, wave, sample));
    require(juce::SHA256(samples.data(), samples.size()).toHexString()
                == "1e573f91e6dbd7b331e8287f6cf82b5b8c82c08f29c3f72cd3ce8386761cd105",
            "Bundled PPG sample bank is missing or differs from the decoded reference");
}

void testFactoryUpperWavetableBank()
{
#if WAVE_HAS_PRIVATE_UPPER_TABLES
    wave::dsp::WavetableBank bank;
    uint64_t fingerprint = 1469598103934665603ull;
    for (int table = 30; table < wave::dsp::WavetableBank::factoryTableCount; ++table)
    {
        for (int wave = 0; wave < 61; ++wave)
        {
            for (int sample = 0; sample < 64; ++sample)
            {
                const auto unsignedHalfWave = static_cast<uint8_t>(
                    static_cast<int>(bank.rawSample(table, wave, sample)) + 128);
                fingerprint ^= unsignedHalfWave;
                fingerprint *= 1099511628211ull;
                require(bank.rawSample(table, wave, 127 - sample)
                            == static_cast<int8_t>(static_cast<int>(
                                                      static_cast<uint8_t>(~unsignedHalfWave))
                                                  - 128),
                        "Upper factory-bank half-wave symmetry is incorrect");
            }
        }
        require(bank.rawSample(table, 61, 0) == -96
                    && bank.rawSample(table, 62, 126) == 127
                    && bank.rawSample(table, 63, 64) == 96,
                "An upper factory table lost its standard terminal waves");
    }
    require(fingerprint == 0x8021f195e5862f4full,
            "Wave factory tables 31..64 are not the complete reference bank");
#endif
}

void testOriginalWaveFactoryTablesWhenAvailable()
{
    const auto* directory = std::getenv("WAVE_FIRMWARE_DIR");
    if (directory == nullptr)
        return;
    juce::MemoryBlock image;
    require(juce::File(directory).getChildFile("w2sys.bin").loadFileAsData(image),
            "Wave factory-table source firmware could not be read");
    wave::dsp::WavetableBank bank;
    const auto untouchedUserWave = bank.rawSample(64, 12, 34);
    require(bank.loadWaveFactoryRom(image.getData(), image.getSize()),
            "Original Wave factory routines failed to produce the verified bank");
    require(bank.hasOriginalWaveFactoryTables() && bank.importedTableCount() == 64,
            "Original factory bank was not marked as complete");
    std::vector<uint8_t> halfWaves;
    for (int table = 0; table < 64; ++table)
        for (int wave = 0; wave < 64; ++wave)
            for (int sample = 0; sample < 64; ++sample)
            {
                const auto value = static_cast<uint8_t>(
                    static_cast<int>(bank.rawSample(table, wave, sample)) + 128);
                halfWaves.push_back(value);
                require(bank.rawSample(table, wave, 127 - sample)
                            == static_cast<int8_t>(static_cast<int>(
                                                       static_cast<uint8_t>(~value)) - 128),
                        "Wave factory waveform lost its complemented half-cycle");
            }
    require(juce::SHA256(halfWaves.data(), halfWaves.size()).toHexString()
                == "e2d3bdd4d22053058458962df7dc9a7ad08e895190f7b08e7f6b1ef63d1976c3",
            "Factory bank differs from the original OS 1.700 routine output");
    require(bank.rawSample(0, 60, 0) == 2
                && bank.rawSample(0, 61, 0) == 2
                && bank.rawSample(0, 62, 0) == 64
                && bank.rawSample(0, 63, 0) == 64
                && bank.rawSample(0, 63, 63) == 1,
            "Wave endpoint or triangle/square/saw slots use PPG replacements");
    require(bank.rawSample(64, 12, 34) == untouchedUserWave,
            "Factory installation overwrote a user wavetable");
    // The individual ROM Wave palette is separate from the 64 factory
    // tables: user records address R000..R299, then U000..U999.
    const auto* romBytes = static_cast<const uint8_t*>(image.getData());
    for (int wave = 0; wave < 300; ++wave)
        for (int sample = 0; sample < 64; ++sample)
        {
            const auto value = romBytes[0x41850u + static_cast<size_t>(wave) * 64u
                                       + static_cast<size_t>(sample)];
            require(bank.rawRomWaveSample(wave, sample)
                        == static_cast<int8_t>(static_cast<int>(value) - 128)
                        && bank.rawRomWaveSample(wave, 127 - sample)
                               == static_cast<int8_t>(static_cast<int>(
                                                         static_cast<uint8_t>(~value)) - 128),
                    "Original ROM Wave palette differs from firmware data");
        }

    constexpr size_t tableOffset = 0x4387c;
    constexpr size_t waveOffset = 0x45afc;
    std::vector<uint8_t> setup(waveOffset + 1000u * 64u, 128);
    for (int table = 0; table < 64; ++table)
    {
        const auto record = tableOffset + static_cast<size_t>(table) * 138u;
        setup[record + 9] = 0x55;
        std::fill_n(setup.begin() + static_cast<std::ptrdiff_t>(record + 10), 128, 0xff);
        setup[record + 10] = 0;
        setup[record + 11] = 0;
        setup[record + 130] = 0;
        setup[record + 131] = 1;
    }
    class UserTableBus final : public wave::firmware::M68000Bus
    {
    public:
        std::vector<uint8_t> ram = std::vector<uint8_t>(0x200000);
        bool completed = false;
        bool unmapped = false;
        uint8_t read8(uint32_t address) noexcept override
        {
            if (address < ram.size()) return ram[address];
            unmapped = true;
            return 0xff;
        }
        void write8(uint32_t address, uint8_t value) noexcept override
        {
            if (address < ram.size()) ram[address] = value;
            else unmapped = true;
        }
        bool usesInstructionInterception() const noexcept override { return true; }
        bool interceptInstruction(wave::firmware::M68000& cpu, uint32_t pc) noexcept override
        {
            if (pc != 0x00bd6e) return false;
            completed = true;
            cpu.endTimeslice();
            return true;
        }
    } bus;
    for (int fixture = 0; fixture < 2; ++fixture)
    {
        if (fixture == 1)
        {
            const auto reference = [&](int wave, uint16_t value) {
                setup[tableOffset + 10u + static_cast<size_t>(wave) * 2u]
                    = static_cast<uint8_t>(value >> 8u);
                setup[tableOffset + 11u + static_cast<size_t>(wave) * 2u]
                    = static_cast<uint8_t>(value);
            };
            reference(0, 299);
            reference(30, 300);
            reference(60, 1299);
            for (size_t sample = 0; sample < 64; ++sample)
            {
                setup[waveOffset + sample] = static_cast<uint8_t>(sample * 3u);
                setup[waveOffset + 999u * 64u + sample] = static_cast<uint8_t>(255u - sample * 2u);
            }
        }
        require(bank.loadWaveSetUserTables(setup.data(), setup.size()),
                "Native user Wave fixture failed to import");
        std::fill(bus.ram.begin(), bus.ram.end(), 0);
        bus.completed = false;
        bus.unmapped = false;
        std::copy_n(romBytes, image.getSize(), bus.ram.begin() + 0x1000);
        std::copy_n(setup.begin() + tableOffset, 138, bus.ram.begin() + 0x148a00);
        std::copy_n(setup.begin() + waveOffset, 1000u * 64u, bus.ram.begin() + 0x14ac80);
        wave::firmware::M68000 cpu;
        cpu.start(bus, 0x0ffffe, 0x011bb8);
        cpu.setDataRegister(bus, 0, 64);
        cpu.setDataRegister(bus, 1, 0);
        for (int cycles = 0; !bus.completed && !bus.unmapped && cycles < 2000000;
             cycles += 10000)
            cpu.execute(bus, 10000);
        require(bus.completed && !bus.unmapped,
                "Original firmware failed to generate a native user table");
        for (int wave = 0; wave < 64; ++wave)
            for (int sample = 0; sample < 64; ++sample)
            {
                const auto value = bus.ram[0x104000u + static_cast<size_t>(wave) * 64u
                                          + static_cast<size_t>(sample)];
                require(bank.rawSample(64, wave, sample)
                            == static_cast<int8_t>(static_cast<int>(value) - 128)
                            && bank.rawSample(64, wave, 127 - sample)
                                   == static_cast<int8_t>(static_cast<int>(
                                                             static_cast<uint8_t>(~value)) - 128),
                        "Native user table differs from original firmware output");
            }
    }

    auto corrupted = image;
    static_cast<uint8_t*>(corrupted.getData())[100] ^= 1;
    require(!bank.loadWaveFactoryRom(corrupted.getData(), corrupted.getSize())
                && bank.hasOriginalWaveFactoryTables() && bank.rawSample(0, 63, 63) == 1,
            "Unverified firmware was accepted or changed the existing bank");
}

void testExpandedFirst32Loading()
{
    constexpr auto byteCount = static_cast<size_t>(32)
                               * wave::dsp::WavetableBank::wavesPerTable
                               * wave::dsp::WavetableBank::samplesPerWave;
    std::vector<int8_t> expanded(byteCount);
    for (size_t i = 0; i < expanded.size(); ++i)
        expanded[i] = static_cast<int8_t>(static_cast<int>(i % 255) - 127);

    wave::dsp::WavetableBank bank;
    const auto untouched = bank.rawSample(32, 4, 9);
    require(bank.loadRomImage(expanded.data(), expanded.size()),
            "Expanded first-32 PPG/Waldorf image was rejected");
    require(bank.importedTableCount() == 32
                && bank.rawSample(31, 63, 127) == expanded.back(),
            "Expanded first-32 image did not map table-major data exactly");
    require(bank.rawSample(32, 4, 9) == untouched,
            "First-32 loader overwrote Waldorf table slots 32-63");
}

void testUserPpgRomWhenAvailable()
{
    const auto* path = std::getenv("PPG_WAVETABLE_ROM");
    if (path == nullptr)
        return;

    juce::MemoryBlock image;
    require(juce::File(path).loadFileAsData(image),
            "PPG_WAVETABLE_ROM could not be read");
    wave::dsp::WavetableBank bank;
    require(bank.loadRomImage(image.getData(), image.getSize()),
            "PPG_WAVETABLE_ROM is not a supported raw or expanded image");
    require(bank.importedTableCount() >= 30,
            "PPG_WAVETABLE_ROM loaded fewer than the stored PPG table set");

    const auto sourceHash = juce::SHA256(image.getData(), image.getSize()).toHexString();
    if (sourceHash == "ff9393d3649a402eab07d2d763bbdb6161def53f74101b7cb08969d436b9741a")
    {
        constexpr auto reconstructedBytes = static_cast<size_t>(30)
                                            * wave::dsp::WavetableBank::wavesPerTable
                                            * wave::dsp::WavetableBank::samplesPerWave;
        juce::MemoryBlock reconstructed(reconstructedBytes);
        auto* destination = static_cast<int8_t*>(reconstructed.getData());
        size_t cursor = 0;
        for (int table = 0; table < 30; ++table)
            for (int wave = 0; wave < wave::dsp::WavetableBank::wavesPerTable; ++wave)
                for (int sample = 0; sample < wave::dsp::WavetableBank::samplesPerWave; ++sample)
                    destination[cursor++] = bank.rawSample(table, wave, sample);
        require(juce::SHA256(reconstructed.getData(), reconstructed.getSize()).toHexString()
                    == "1e573f91e6dbd7b331e8287f6cf82b5b8c82c08f29c3f72cd3ce8386761cd105",
                "PPG V6 reconstructed 30-table image changed unexpectedly");
    }
}

void testCemStability()
{
    wave::dsp::Cem3387 filter;
    filter.prepare(48000.0, 0.72f);
    filter.setControls(18500.0f, 1.0f, 18.0f, 0.0f, 1.0f);

    auto peak = 0.0f;
    for (int i = 0; i < 96000; ++i)
    {
        const auto input = i < 48000
            ? std::sin(juce::MathConstants<float>::twoPi * 3200.0f
                       * static_cast<float>(i) / 48000.0f)
            : 0.0f;
        const auto output = filter.process(input, 1.0f);
        require(std::isfinite(output.left) && std::isfinite(output.right),
                "CEM3387 model became non-finite");
        peak = juce::jmax(peak, std::abs(output.left), std::abs(output.right));
    }
    require(peak < 1.01f, "CEM3387 model exceeded its nonlinear output rail");
}

void testCemFilterResponse()
{
    const auto measure = [](float cutoff, float resonance) {
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, 0.0f);
        filter.setControls(cutoff, resonance, 0.0f, 0.0f, 0.0f);
        double energy = 0.0;
        constexpr auto frequency = 4000.0;
        constexpr auto sampleRate = 48000.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * frequency
                                   * static_cast<double>(sample) / sampleRate));
            const auto output = filter.process(input, 1.0f).left;
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        return std::sqrt(energy / 20000.0);
    };

    const auto closed = measure(250.0f, 0.0f);
    const auto open = measure(12000.0f, 0.0f);
    require(open > closed * 20.0,
            "CEM3387 cutoff control does not open its four-pole low-pass response");
}

void testMeasuredWaveResonancePassbandLoss()
{
    const auto measure = [](float resonance) {
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, 0.0f);
        filter.setControls(20000.0f, resonance, 0.0f, 0.0f, 0.0f);
        double inPhase = 0.0;
        double quadrature = 0.0;
        constexpr auto frequency = 130.8128;
        for (int sample = 0; sample < 48000; ++sample)
        {
            const auto phase = juce::MathConstants<double>::twoPi
                               * frequency * static_cast<double>(sample) / 48000.0;
            const auto input = 0.02f * std::sin(static_cast<float>(phase));
            const auto output = filter.process(input, 1.0f).left;
            if (sample >= 12000)
            {
                const auto position = static_cast<double>(sample - 12000) / 35999.0;
                const auto window = 0.5 - 0.5 * std::cos(
                    juce::MathConstants<double>::twoPi * position);
                inPhase += window * static_cast<double>(output) * std::sin(phase);
                quadrature += window * static_cast<double>(output) * std::cos(phase);
            }
        }
        return 2.0 * std::sqrt(inPhase * inPhase + quadrature * quadrature)
               / 17999.5;
    };

    const auto unresonant = measure(0.0f);
    const auto lossDb = [unresonant, &measure](float resonance) {
        return juce::Decibels::gainToDecibels(
            static_cast<float>(measure(resonance) / unresonant));
    };
    const auto loss30 = lossDb(30.0f / 127.0f);
    const auto loss62 = lossDb(62.0f / 127.0f);
    const auto loss100 = lossDb(100.0f / 127.0f);
    const auto loss127 = lossDb(1.0f);
    require(loss30 < -3.0f && loss30 > -4.5f
                && loss62 < -4.7f && loss62 > -5.9f
                && loss100 < -5.4f && loss100 > -6.6f
                && loss127 < -5.8f && loss127 > -7.0f,
            "CEM resonance passband loss no longer matches the measured Wave sweep");
}

void testAllVoiceFiltersAreCalibrated()
{
    std::array<bool, 4096> usedCodes{};
    auto distinctCodes = 0;
    for (int voice = 0; voice < 48; ++voice)
    {
        const auto tolerance
            = static_cast<float>(((voice * 37 + 11) % 19) - 9) / 9.0f;
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, tolerance);
        filter.setControls(1000.0f, 0.0f, 0.0f, 0.0f, 1.0f);

        const auto code = filter.cutoffCalibrationCode();
        require(code <= 0x0fffu,
                "A CEM3387 VCF calibration word exceeds the Wave's 12-bit table");
        if (!usedCodes[code])
        {
            usedCodes[code] = true;
            ++distinctCodes;
        }
        require(std::abs(filter.calibratedCutoffCvResidual())
                    <= (0.5f / 4095.0f + 1.0e-7f),
                "A voice-card VCF retained more than one half trim LSB of cutoff error");
    }
    require(distinctCodes >= 10,
            "The 48 VCFs were assigned one fabricated global calibration value");

    auto minimumResponse = std::numeric_limits<double>::max();
    auto maximumResponse = 0.0;
    for (int voice = 0; voice < 48; ++voice)
    {
        const auto tolerance
            = static_cast<float>(((voice * 37 + 11) % 19) - 9) / 9.0f;
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, tolerance);
        filter.setControls(1800.0f, 0.35f, 0.0f, 0.0f, 0.18f);
        filter.setCutoffCalibrationCode(filter.cutoffCalibrationCode());
        double energy = 0.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * 4000.0
                                   * static_cast<double>(sample) / 48000.0));
            const auto output = filter.process(input, 1.0f).left;
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        const auto response = std::sqrt(energy / 20000.0);
        minimumResponse = std::min(minimumResponse, response);
        maximumResponse = std::max(maximumResponse, response);
    }
    require(maximumResponse / minimumResponse < 1.01,
            "The 12-bit VCF calibration leaves excessive residual spread");

    minimumResponse = std::numeric_limits<double>::max();
    maximumResponse = 0.0;
    for (int voice = 0; voice < 48; ++voice)
    {
        const auto tolerance
            = static_cast<float>(((voice * 37 + 11) % 19) - 9) / 9.0f;
        wave::dsp::ReconstructionStage reconstruction;
        reconstruction.prepare(48000.0, tolerance);
        reconstruction.setAge(0.18f);
        double energy = 0.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * 10000.0
                                   * static_cast<double>(sample) / 48000.0));
            const auto output = reconstruction.process(input);
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        const auto response = std::sqrt(energy / 20000.0);
        minimumResponse = std::min(minimumResponse, response);
        maximumResponse = std::max(maximumResponse, response);
    }
    require(maximumResponse / minimumResponse < 1.0001,
            "Fixed reconstruction filters retain an undocumented voice spread");
}

void testReconstructionCachedTransfer()
{
    // Independent, uncached signal path: exercise every DAC code, rounding
    // boundary, clipping, reset, and live age changes against the original math.
    for (const auto rate : { 44100.0, 48000.0, 96000.0, 250000.0 })
    {
        wave::dsp::ReconstructionStage reconstruction;
        reconstruction.prepare(rate, 0.0f);
        const auto firstPole = 1.0f - std::exp(
            -juce::MathConstants<float>::twoPi * 15400.0f * 0.5f
            / static_cast<float>(rate));
        const auto g = std::tan(juce::MathConstants<float>::pi * 15400.0f
                                / static_cast<float>(rate));
        const auto a1 = 1.0f / (1.0f + g * (g + 0.5f));
        const auto a2 = g * a1;
        const auto a3 = g * a2;
        const auto highpass = std::exp(-juce::MathConstants<float>::twoPi * 7.0f
                                       / static_cast<float>(rate));
        float state = 0.0f, secondState = 0.0f, thirdState = 0.0f, dcState = 0.0f;
        for (const auto age : { 0.0f, 0.18f, 1.0f, 0.4f, 0.0f })
        {
            reconstruction.setAge(age);
            for (int code = -140; code <= 140; ++code)
                for (const auto offset : { -0.501f, -0.5f, 0.0f, 0.5f, 0.501f })
                {
                    const auto input = (static_cast<float>(code) + offset) / 127.0f;
                    const auto quantised = std::round(
                        juce::jlimit(-1.0f, 1.0f, input) * 127.0f) / 127.0f;
                    const auto saturated = std::tanh(quantised * (1.0f + age * 0.16f));
                    state += firstPole * (saturated - state);
                    const auto v3 = state - thirdState;
                    const auto v1 = a1 * secondState + a2 * v3;
                    const auto v2 = thirdState + a2 * secondState + a3 * v3;
                    secondState = 2.0f * v1 - secondState;
                    thirdState = 2.0f * v2 - thirdState;
                    const auto dc = v2 + highpass * (dcState - v2);
                    const auto expected = v2 - dc;
                    dcState = dc;
                    require(std::bit_cast<uint32_t>(reconstruction.process(input))
                                == std::bit_cast<uint32_t>(expected),
                            "Cached reconstruction differs from the uncached transfer");
                }
            reconstruction.reset();
            state = secondState = thirdState = dcState = 0.0f;
        }
    }
}

void testCemControlVoltageSettling()
{
    wave::dsp::Cem3387 circuit;
    circuit.prepare(48000.0, 0.0f);
    circuit.setControls(1000.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    (void) circuit.process(0.0f, 1.0f);
    const auto firstSample = circuit.currentVcaCv();
    require(firstSample > 0.0f && firstSample < 0.1f,
            "CEM3387 VCA control still changes as an ideal instantaneous step");

    for (int sample = 1; sample < 48; ++sample)
        (void) circuit.process(0.0f, 1.0f);
    const auto afterOneMillisecond = circuit.currentVcaCv();
    require(afterOneMillisecond > 0.96f && afterOneMillisecond < 0.985f,
            "CEM3387 sample-and-hold settling is outside its lightly smoothed transition");

    for (int sample = 48; sample < 240; ++sample)
        (void) circuit.process(0.0f, 1.0f);
    require(circuit.currentVcaCv() > 0.999f,
            "CEM3387 control voltage does not settle before the next WDV update");
}

void testLiveCutoffUsesContinuousBaseControlVoltage()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);

    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.decaySeconds = 0.001f;
    parameters.sustainLevel = 1.0f;
    parameters.releaseSeconds = 0.1f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.cutoffHz = 200.0f;

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(audio, noteOn, parameters);
    const auto activeCutoff = [&engine]
    {
        for (const auto& voice : engine.voiceStates())
            if (voice.active)
                return voice.cutoffHz;
        return -1.0f;
    };

    parameters.cutoffHz = 12800.0f;
    audio.setSize(2, 1, false, false, true);
    audio.clear();
    juce::MidiBuffer noMidi;
    engine.render(audio, noMidi, parameters);
    const auto firstStep = activeCutoff();
    require(firstStep >= 190.0f && firstStep < 260.0f,
            "A live Cutoff edit still reaches the filter as an instantaneous step");

    audio.setSize(2, 2400, false, false, true);
    audio.clear();
    engine.render(audio, noMidi, parameters);
    const auto settled = activeCutoff();
    require(settled > 9000.0f && settled < 12800.0f,
            "The reconstructed Cutoff control voltage does not glide to its target");
}

void testAsicHighpassResponse()
{
    const auto measure = [](float frequency) {
        wave::dsp::AsicHighpassFilter filter;
        filter.prepare(48000.0);
        double energy = 0.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * frequency
                                   * static_cast<double>(sample) / 48000.0));
            const auto output = filter.process(input, 1200.0f);
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        return std::sqrt(energy / 20000.0);
    };

    require(measure(6000.0f) > measure(100.0f) * 30.0,
            "ASIC 12 dB high-pass does not reject frequencies below its cutoff");
}

void testSerialBandpassTopology()
{
    const auto measure = [](float frequency) {
        wave::dsp::AsicHighpassFilter highpass;
        wave::dsp::Cem3387 lowpass;
        highpass.prepare(48000.0);
        lowpass.prepare(48000.0, 0.0f);
        lowpass.setControls(2200.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        double energy = 0.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * frequency
                                   * static_cast<double>(sample) / 48000.0));
            const auto filtered = highpass.process(input, 500.0f);
            const auto output = lowpass.process(filtered, 1.0f).left;
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        return std::sqrt(energy / 20000.0);
    };

    const auto passband = measure(1000.0f);
    require(passband > measure(80.0f) * 12.0
                && passband > measure(9000.0f) * 12.0,
            "Serial ASIC high-pass and CEM low-pass do not form the Wave band-pass");
}

void testCemSelfOscillation()
{
    const auto tailLevel = [](float resonance) {
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, 0.0f);
        filter.setControls(1000.0f, resonance, 0.0f, 0.0f, 0.0f);
        double energy = 0.0;
        for (int sample = 0; sample < 96000; ++sample)
        {
            const auto output = filter.process(sample == 0 ? 0.1f : 0.0f, 1.0f).left;
            if (sample >= 72000)
                energy += static_cast<double>(output) * output;
        }
        return std::sqrt(energy / 24000.0);
    };

    const auto unresonantTail = tailLevel(0.0f);
    const auto oscillatingTail = tailLevel(1.0f);
    require(oscillatingTail > unresonantTail * 20.0 && oscillatingTail > 1.0e-4,
            "Maximum CEM3387 resonance does not sustain oscillation after excitation");

    const auto oscillationFrequency = [](float cutoff) {
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, 0.0f);
        filter.setControls(cutoff, 1.0f, 0.0f, 0.0f, 0.0f);
        auto previous = 0.0f;
        auto crossings = 0;
        for (int sample = 0; sample < 144000; ++sample)
        {
            const auto output
                = filter.process(sample == 0 ? 0.1f : 0.0f, 1.0f).left;
            if (sample >= 96000 && previous <= 0.0f && output > 0.0f)
                ++crossings;
            previous = output;
        }
        return static_cast<float>(crossings);
    };
    const auto atZero = oscillationFrequency(
        wave::parameters::cutoffFrequencyForStep(0.0f));
    const auto at62 = oscillationFrequency(
        wave::parameters::cutoffFrequencyForStep(62.0f));
    const auto at100 = oscillationFrequency(
        wave::parameters::cutoffFrequencyForStep(100.0f));
    require(std::abs(atZero - 28.0f) <= 3.0f
                && std::abs(at62 - 957.0f) <= 50.0f
                && std::abs(at100 - 7779.0f) <= 400.0f,
            "CEM self-oscillation tracking no longer matches the measured Wave cutoffs");
}

void testCemResonanceAcrossSampleRates()
{
    // A continuous analogue feedback loop must not start oscillating earlier
    // as its cutoff approaches the digital sample rate. Drive is an input
    // level control, so it must not change the unexcited loop's threshold.
    for (const auto cutoff : { 1000.0f, 6000.0f })
    {
        auto referenceFrequency = 0;
        for (const auto rate : { 44100.0, 48000.0, 96000.0 })
            for (const auto drive : { 0.0f, 18.0f })
            {
                const auto measure = [=](float resonance) {
                    wave::dsp::Cem3387 filter;
                    filter.prepare(rate, 0.0f);
                    filter.setControls(cutoff, resonance, drive, 0.0f, 0.0f);
                    double energy = 0.0;
                    auto crossings = 0;
                    auto previous = 0.0f;
                    for (int sample = 0; sample < static_cast<int>(rate * 2.0); ++sample)
                    {
                        const auto output = filter.process(sample == 0 ? 0.1f : 0.0f,
                                                           1.0f).left;
                        if (sample >= static_cast<int>(rate))
                        {
                            energy += static_cast<double>(output) * output;
                            if (previous <= 0.0f && output > 0.0f)
                                ++crossings;
                        }
                        previous = output;
                    }
                    return std::pair { std::sqrt(energy / rate), crossings };
                };
                const auto belowThreshold = measure(0.85f);
                require(belowThreshold.first < 1.0e-5,
                        "CEM resonance oscillates below threshold at high cutoff or drive");
                require(measure(0.95f).first < 1.0e-5,
                        "CEM resonance starts oscillating before the analogue threshold");
                const auto oscillating = measure(1.0f);
                require(oscillating.first > 0.01,
                        "CEM maximum resonance fails to oscillate across sample rates");
                if (referenceFrequency == 0)
                    referenceFrequency = oscillating.second;
                require(std::abs(static_cast<float>(oscillating.second)
                                 / static_cast<float>(referenceFrequency) - 1.0f) < 0.02f,
                        "CEM oscillation frequency shifts with sample rate or drive");
            }
    }
}

void testCemSmallSignalResonanceResponse()
{
    // Datasheet's classical four-pole application: at w = wc, the
    // open-loop response is -1/4. Negative feedback k therefore gives
    // |H(wc)| / |H(0)| = (1+k)/(4-k), independent of input makeup gain.
    constexpr auto resonance = 0.7f;
    constexpr auto feedback = 4.15f * resonance;
    constexpr auto expectedRatio = (1.0f + feedback) / (4.0f - feedback);
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto measure = [=](double frequency) {
            wave::dsp::Cem3387 filter;
            filter.prepare(rate, 0.0f);
            filter.setControls(wave::parameters::cutoffFrequencyForStep(62.0f),
                               resonance, 0.0f, 0.0f, 0.0f);
            double energy = 0.0;
            for (int sample = 0; sample < static_cast<int>(rate); ++sample)
            {
                const auto input = 0.001f * static_cast<float>(std::sin(
                    juce::MathConstants<double>::twoPi * frequency * sample / rate));
                const auto output = filter.process(input, 1.0f).left;
                if (sample >= static_cast<int>(rate * 0.5))
                    energy += static_cast<double>(output) * output;
            }
            return std::sqrt(energy / (rate * 0.5));
        };
        const auto ratio = measure(957.0) / measure(100.0);
        require(std::abs(ratio / expectedRatio - 1.0) < 0.03,
                "CEM resonance peak does not match the analogue four-pole response");
    }
}

void testIndependentFilterEnvelope()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(1000.0, 64);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.sustainLevel = 1.0f;
    parameters.filterDelaySeconds = 0.020f;
    parameters.filterAttackSeconds = 0.010f;
    parameters.filterDecaySeconds = 0.001f;
    parameters.filterSustainLevel = 1.0f;

    juce::AudioBuffer<float> beforeDelay(2, 10);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(beforeDelay, noteOn, parameters);
    require(engine.firstActiveFilterEnvelopeValue() == 0.0f,
            "Filter envelope ignored its factory delay stage");

    juce::AudioBuffer<float> afterDelay(2, 20);
    engine.render(afterDelay, {}, parameters);
    require(engine.firstActiveFilterEnvelopeValue() > 0.5f,
            "Filter envelope did not start independently of the amplifier envelope");
}

void testAmplifierEnvelopeStages()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(1000.0, 128);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.050f;
    parameters.decaySeconds = 0.050f;
    parameters.sustainLevel = 0.4f;
    parameters.releaseSeconds = 0.050f;
    parameters.cutoffHz = 18000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> attack(2, 25);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(attack, noteOn, parameters);
    const auto attackLevel = engine.firstActiveAmplifierEnvelopeValue();
    require(attackLevel > 0.35f && attackLevel < 0.65f,
            "VCA envelope did not traverse its attack stage");

    juce::AudioBuffer<float> decay(2, 600);
    engine.render(decay, {}, parameters);
    const auto sustainLevel = engine.firstActiveAmplifierEnvelopeValue();
    require(std::abs(sustainLevel - 0.4f) < 0.03f,
            "VCA envelope did not reach its programmed sustain level");

    juce::AudioBuffer<float> firstReleaseHalf(2, 25);
    juce::MidiBuffer noteOff;
    noteOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    engine.render(firstReleaseHalf, noteOff, parameters);
    const auto releaseLevel = engine.firstActiveAmplifierEnvelopeValue();
    require(releaseLevel > 0.1f && releaseLevel < sustainLevel,
            "VCA envelope did not traverse its release stage");

    juce::AudioBuffer<float> releaseEnd(2, 500);
    engine.render(releaseEnd, {}, parameters);
    require(engine.activeVoiceCount() == 0
                && engine.firstActiveAmplifierEnvelopeValue() == 0.0f,
            "VCA envelope did not close the voice after release");
}

void testMeasuredFastAmplifierAttackScaling()
{
    const auto riseTimeMilliseconds = [](uint8_t rate) {
        wave::dsp::WdvEnvelope envelope;
        envelope.prepare(48000.0);
        wave::dsp::WdvEnvelope::Parameters parameters;
        parameters.attack
            = wave::dsp::WdvEnvelope::amplifierAttackTimeConstantForRate(rate);
        parameters.decay = 10.0f;
        parameters.sustain = 1.0f;
        parameters.release = 10.0f;
        parameters.useMeasuredAmplifierAttackScaling = true;
        envelope.setParameters(parameters);
        envelope.noteOn();

        auto tenPercentSample = -1;
        auto ninetyPercentSample = -1;
        for (auto sample = 0; sample < 2048 && ninetyPercentSample < 0; ++sample)
        {
            const auto level = envelope.getNextSample();
            const auto vcaGain = level * level * (3.0f - 2.0f * level);
            if (tenPercentSample < 0 && vcaGain >= 0.1f)
                tenPercentSample = sample;
            if (vcaGain >= 0.9f)
                ninetyPercentSample = sample;
        }
        require(tenPercentSample >= 0 && ninetyPercentSample >= tenPercentSample,
                "Fast amplifier attack did not traverse its measured range");
        return static_cast<float>(ninetyPercentSample - tenPercentSample)
               * 1000.0f / 48000.0f;
    };

    require(riseTimeMilliseconds(0) < 0.5f,
            "VCA attack zero lost the Wave's abrupt click-prone response");
    require(std::abs(riseTimeMilliseconds(1) - 5.0f) < 0.25f,
            "VCA attack one no longer matches the measured five-millisecond rise");
}

void testShortVcaReleaseDrainsAnalogueControl()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 2048);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.decaySeconds = 0.001f;
    parameters.sustainLevel = 1.0f;
    parameters.releaseSeconds = 0.001f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.driveDb = 0.0f;

    juce::AudioBuffer<float> held(2, 2048);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(held, noteOn, parameters);

    juce::AudioBuffer<float> releaseStart(2, 64);
    juce::MidiBuffer noteOff;
    noteOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    engine.render(releaseStart, noteOff, parameters);
    auto largestHeldStep = 0.0f;
    for (int sample = 1; sample < held.getNumSamples(); ++sample)
        largestHeldStep = juce::jmax(
            largestHeldStep,
            std::abs(held.getSample(0, sample) - held.getSample(0, sample - 1)));
    auto largestReleaseStep = std::abs(
        releaseStart.getSample(0, 0)
        - held.getSample(0, held.getNumSamples() - 1));
    for (int sample = 1; sample < releaseStart.getNumSamples(); ++sample)
        largestReleaseStep = juce::jmax(
            largestReleaseStep,
            std::abs(releaseStart.getSample(0, sample)
                     - releaseStart.getSample(0, sample - 1)));
    require(engine.activeVoiceCount() == 1
                && releaseStart.getMagnitude(0, 0, releaseStart.getNumSamples()) > 1.0e-6f,
            "Shortest VCA release bypassed the analogue hold-capacitor discharge");
    require(largestReleaseStep <= largestHeldStep * 1.25f + 1.0e-4f,
            "Shortest VCA release introduced a discontinuity larger than the waveform");

    juce::AudioBuffer<float> releaseEnd(2, 1024);
    engine.render(releaseEnd, {}, parameters);
    require(engine.activeVoiceCount() == 0,
            "Analogue VCA drain did not retire the released voice");
}

void testCentredVoiceHasNoArtificialPanSpread()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 4096);
    wave::parameters::Snapshot parameters;
    parameters.panAmount = 0.0f;
    parameters.circuitAgeAmount = 1.0f;
    parameters.attackSeconds = 0.001f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.cutoffHz = 18000.0f;

    juce::AudioBuffer<float> audio(2, 4096);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(audio, noteOn, parameters);
    const auto left = audio.getRMSLevel(0, 512, 3584);
    const auto right = audio.getRMSLevel(1, 512, 3584);
    require(std::abs(left - right) < juce::jmax(left, right) * 0.01f,
            "Per-voice circuit tolerance introduced an artificial stereo pan offset");
}

void testSampleAccurateMidiStart()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 128);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.cutoffHz = 18000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 37);
    engine.render(audio, midi, parameters);

    require(audio.getMagnitude(0, 0, 37) < 1.0e-4f
                && audio.getMagnitude(1, 0, 37) < 1.0e-4f,
            "Idle analogue noise exceeded the calibrated pre-event floor");
    require(audio.getMagnitude(0, 37, 91) > 1.0e-6f,
            "Voice did not produce audio after the MIDI event offset");
}

void testAudibleKeyboardRange()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(96000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.008f;
    parameters.cutoffHz = 8200.0f;
    parameters.filterEnvelopeSemitones = 28.0f;
    parameters.outputDb = -7.0f;

    for (const auto note : { 36, 60, 84 })
    {
        engine.reset();
        juce::AudioBuffer<float> audio(2, 512);
        auto peak = 0.0f;
        for (int block = 0; block < 6; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.35f), 0);
            engine.render(audio, midi, parameters);
            peak = juce::jmax(peak, audio.getMagnitude(0, 0, audio.getNumSamples()),
                             audio.getMagnitude(1, 0, audio.getNumSamples()));
        }
        require(peak > 0.005f,
                "A playable keyboard note was attenuated below an audible output level");
    }
}

void testEmptyPerformanceSilencesHeldNotes()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::dsp::WaldorfEngine::PerformanceSnapshot performance;
    performance.layers[0].enabled = true;
    performance.layers[0].source = 3;
    performance.circuitAgeAmount = 0.0f;
    performance.layers[0].sound.attackSeconds = 0.001f;
    performance.layers[0].sound.filterEnvelopeSemitones = 0.0f;
    performance.layers[0].sound.cutoffHz = 16000.0f;
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(audio, noteOn, performance);
    require(audio.getMagnitude(0, 0, 512) > 1.0e-6f,
            "Empty Performance fixture did not start an audible held note");
    performance.layers[0].enabled = false;
    // Drain the analogue output stage's stored charge after removing its input.
    for (int block = 0; block < 64; ++block)
    {
        audio.clear();
        engine.render(audio, {}, performance);
    }
    // The output-stage model retains its tiny analogue noise floor.
    require(audio.getMagnitude(0, 0, 512) < 2.0e-6f
                && audio.getMagnitude(1, 0, 512) < 2.0e-6f,
            "Selecting an empty Performance retained the preceding held voice audio");
}

void testPerformanceMidi()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 0.002f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.wavePosition = 0.0f;
    parameters.waveScan = 0.0f;
    parameters.modulationRoutes[wave::parameters::wave1Mod1]
        = { 22, 38, 63.0f };

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 64, 0.9f), 1);
    noteOn.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 2);
    noteOn.addEvent(juce::MidiMessage::channelPressureChange(1, 127), 3);
    engine.render(audio, noteOn, parameters);
    require(engine.activeVoiceCount() == 1,
            "Performance MIDI note did not allocate a voice");
    require(engine.firstActiveWavePosition() > 62.0f,
            "Full mod-wheel route did not traverse the full 64-position wavetable");
    require(std::abs(engine.getPitchBendSemitones() - 2.0f) < 0.001f,
            "Full-up pitch wheel did not produce the two-semitone bend range");
    require(audio.getMagnitude(0, 0, audio.getNumSamples()) > 1.0e-6f,
            "Performance controller sequence produced no audio");
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            require(std::isfinite(audio.getSample(channel, sample)),
                    "Performance controller sequence produced non-finite audio");

    juce::MidiBuffer sustain;
    sustain.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    sustain.addEvent(juce::MidiMessage::noteOff(1, 64), 1);
    engine.render(audio, sustain, parameters);
    require(engine.isSustainPedalDown() && engine.activeVoiceCount() == 1,
            "Sustain pedal did not hold a released key");

    juce::MidiBuffer pedalUp;
    pedalUp.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    engine.render(audio, pedalUp, parameters);
    juce::AudioBuffer<float> releaseTail(2, 1024);
    engine.render(releaseTail, {}, parameters);
    require(!engine.isSustainPedalDown() && engine.activeVoiceCount() == 0,
            "Pedal release did not complete the held voice envelope");

    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::noteOn(1, 67, 1.0f), 0);
    panic.addEvent(juce::MidiMessage::allSoundOff(1), 32);
    engine.render(audio, panic, parameters);
    require(engine.activeVoiceCount() == 0,
            "MIDI all-sound-off did not stop voices immediately");
}

void testInstrumentVoiceAllocation()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::dsp::WaldorfEngine::PerformanceSnapshot performance;
    auto& layer = performance.layers[0];
    layer.enabled = true;
    layer.source = 3;
    layer.sound.attackSeconds = 0.3f;
    layer.sound.filterAttackSeconds = 0.3f;
    layer.sound.releaseSeconds = 0.2f;
    juce::AudioBuffer<float> audio(2, 1);
    const auto send = [&](const juce::MidiMessage& message, bool local = false) {
        juce::MidiBuffer midi;
        midi.addEvent(message, 0);
        if (local)
            engine.render(audio, {}, midi, performance);
        else
            engine.render(audio, midi, performance);
    };
    const auto warm = [&] {
        juce::AudioBuffer<float> block(2, 512);
        for (int count = 0; count < 16; ++count)
            engine.render(block, {}, performance);
    };
    const auto heldNote = [&] {
        auto note = -1;
        auto count = 0;
        for (const auto& voice : engine.voiceStates())
            if (voice.active && voice.keyDown && voice.layer == 0)
            {
                note = voice.triggerNote;
                ++count;
            }
        require(count == 1, "Monophonic Instrument allocated more than one held voice");
        return note;
    };
    for (int mode = 17; mode <= 22; ++mode)
    {
        engine.reset();
        layer.allocationMode = mode;
        const auto priority = (mode - 17) % 3;
        send(juce::MidiMessage::noteOn(1, 60, 0.8f));
        warm();
        const auto envelopeBefore = engine.firstActiveAmplifierEnvelopeValue();
        const auto filterBefore = engine.firstActiveFilterEnvelopeValue();
        require(envelopeBefore > 0.05f && filterBefore > 0.05f,
                "Mono envelope fixture did not advance its attack");
        send(juce::MidiMessage::noteOn(1, 67, 0.9f));
        require(heldNote() == (priority == 1 ? 60 : 67),
                "Mono last/low/high note priority is incorrect");
        if (mode >= 20 || priority == 1)
            require(engine.firstActiveAmplifierEnvelopeValue() >= envelopeBefore - 0.01f
                        && engine.firstActiveFilterEnvelopeValue() >= filterBefore - 0.01f,
                    "Single-trigger or non-priority key restarted an envelope");
        else
            require(engine.firstActiveAmplifierEnvelopeValue() < envelopeBefore * 0.25f
                        && engine.firstActiveFilterEnvelopeValue() < filterBefore * 0.25f,
                    "Mono retrigger mode did not restart the envelopes");
        send(juce::MidiMessage::noteOn(1, 55, 0.7f));
        require(heldNote() == (priority == 2 ? 67 : 55),
                "Mono low/high priority failed with three overlapping keys");
        send(juce::MidiMessage::noteOff(1, 55));
        require(heldNote() == (priority == 1 ? 60 : 67),
                "Mono release did not return to the correct held key");
        send(juce::MidiMessage::noteOff(1, 67));
        require(heldNote() == 60, "Mono allocation forgot the first held key");
        send(juce::MidiMessage::controllerEvent(1, 64, 127));
        send(juce::MidiMessage::noteOff(1, 60));
        require(engine.heldVoiceCount() == 0 && engine.activeVoiceCount() == 1,
                "Mono sustain released its voice too early or retained a held key");
        send(juce::MidiMessage::controllerEvent(1, 64, 0));
        send(juce::MidiMessage::allSoundOff(1));
        require(engine.activeVoiceCount() == 0, "Mono panic left a sounding voice");
        // Local keyboard and repeated-pitch MIDI Note Offs use the same
        // priority stack without consuming another physical voice.
        send(juce::MidiMessage::noteOn(1, 60, 0.8f), true);
        send(juce::MidiMessage::noteOn(1, 60, 0.9f), true);
        send(juce::MidiMessage::noteOff(1, 60), true);
        require(heldNote() == 60, "Repeated mono pitch lost its remaining keystroke");
        send(juce::MidiMessage::noteOff(1, 60), true);
        require(engine.heldVoiceCount() == 0, "Repeated mono pitch became stuck");
    }

    engine.reset();
    layer.allocationMode = 1;
    send(juce::MidiMessage::noteOn(1, 60, 0.8f));
    send(juce::MidiMessage::noteOn(1, 64, 0.8f));
    send(juce::MidiMessage::noteOn(1, 67, 0.8f));
    require(engine.heldVoiceCount() == 3, "Poly 1 incorrectly limited ordinary polyphony");
    layer.allocationMode = 21;
    engine.render(audio, {}, performance);
    require(heldNote() == 60 && engine.activeVoiceCount() == 1,
            "A live change from polyphonic to mono did not take effect");

    engine.reset();
    layer.allocationMode = 20;
    performance.layers[1] = layer;
    performance.layers[1].allocationMode = 0;
    send(juce::MidiMessage::noteOn(1, 60, 0.8f));
    send(juce::MidiMessage::noteOn(1, 67, 0.8f));
    send(juce::MidiMessage::noteOn(1, 55, 0.8f));
    require(heldNote() == 55 && engine.heldVoiceCount() == 4,
            "Mono allocation leaked into another Performance Instrument");
    send(juce::MidiMessage::noteOff(1, 55));
    require(heldNote() == 67 && engine.heldVoiceCount() == 3,
            "Layered mono fallback released the wrong polyphonic keystroke");

    engine.reset();
    performance.layers[1].enabled = false;
    layer.sound.glideEnabled = true;
    layer.sound.glideTypeMode = 5; // Fingered portamento.
    layer.sound.glideRateValue = 100.0f;
    send(juce::MidiMessage::noteOn(1, 60, 0.8f));
    send(juce::MidiMessage::noteOn(1, 72, 0.8f));
    for (const auto& voice : engine.voiceStates())
        if (voice.active && voice.keyDown)
            require(voice.glidePitch >= 60.0f && voice.glidePitch < 61.0f,
                    "Single-trigger mono legato bypassed fingered portamento");
}

void testInstrumentVoiceSharing()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 32);
    wave::dsp::WaldorfEngine::PerformanceSnapshot performance;
    for (int index = 0; index < 3; ++index)
    {
        auto& layer = performance.layers[static_cast<size_t>(index)];
        layer.enabled = true;
        layer.source = 2;
        layer.midiChannel = index + 1;
    }
    juce::AudioBuffer<float> audio(2, 1);
    const auto on = [&](int channel, int note) {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(channel, note, 0.8f), 0);
        engine.render(audio, midi, performance);
    };
    const auto count = [&](int layer) {
        auto total = 0;
        for (const auto& voice : engine.voiceStates())
            total += voice.active && voice.layer == layer ? 1 : 0;
        return total;
    };
    for (int note = 0; note < 48; ++note)
        on(1, note);
    on(2, 60);
    require(count(0) == 48 && count(1) == 0,
            "Dynamic allocation stole another Instrument's sounding voice");
    performance.layers[1].allocationMode = 2;
    on(2, 61);
    on(2, 62);
    on(2, 63);
    require(count(0) == 46 && count(1) == 2,
            "Poly N did not respect its cross-Instrument voice-sharing allowance");
    performance.layers[2].allocationMode = 17;
    on(3, 70);
    on(3, 72);
    require(count(2) == 1, "Mono allocation did not reclaim and reuse one voice");
    for (int note = 60; note < 110; ++note)
        on(1, note);
    require(count(2) == 1, "Another Instrument stole a protected mono voice");
}

void testWaveGlideModes()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot sound;
    sound.glideEnabled = true;
    sound.glideTypeMode = 1;
    sound.glideRateValue = 50.0f;
    sound.glideTimeModeValue = 0;
    sound.glideRateModulationSource = 38;
    sound.glideRateModulationAmount = 0.0f;
    sound.attackSeconds = 0.001f;
    sound.cutoffHz = 18000.0f;
    sound.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 512);
    const auto send = [&](const juce::MidiMessage& message) {
        juce::MidiBuffer midi;
        midi.addEvent(message, 0);
        audio.clear();
        engine.render(audio, midi, sound);
    };
    const auto pitchFor = [&](int triggerNote) {
        for (const auto& voice : engine.voiceStates())
            if (voice.active && voice.triggerNote == triggerNote)
                return voice.glidePitch;
        return -1000.0f;
    };
    const auto heldPitchFor = [&](int triggerNote) {
        for (const auto& voice : engine.voiceStates())
            if (voice.active && voice.keyDown && voice.triggerNote == triggerNote)
                return voice.glidePitch;
        return -1000.0f;
    };

    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    require(pitchFor(72) > 59.9f && pitchFor(72) < 61.0f,
            "Portamento did not start the new voice at the preceding pitch");
    for (int block = 0; block < 48; ++block)
    {
        audio.clear();
        engine.render(audio, {}, sound);
    }
    require(pitchFor(72) > 60.0f && pitchFor(72) < 72.0f,
            "Portamento did not travel continuously towards its target");

    const auto pitchBeforeReferenceBlock = heldPitchFor(72);
    audio.clear();
    engine.render(audio, {}, sound);
    const auto pitchAfterReferenceBlock = heldPitchFor(72);
    const auto referenceAdvance
        = pitchAfterReferenceBlock - pitchBeforeReferenceBlock;
    require(referenceAdvance > 0.0f,
            "Portamento trajectory stopped before the retrigger test");

    send(juce::MidiMessage::noteOff(1, 72));
    const auto releasedTrajectoryPitch = pitchFor(72);
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    const auto retriggeredPitch = heldPitchFor(72);
    require(retriggeredPitch > releasedTrajectoryPitch && retriggeredPitch < 72.0f,
            "Repeated glide destination reset or jumped to its target");
    require(std::abs((retriggeredPitch - releasedTrajectoryPitch)
                     - referenceAdvance)
                < juce::jmax(0.002f, referenceAdvance * 0.05f),
            "Repeated glide destination did not inherit the existing trajectory");

    audio.clear();
    engine.render(audio, {}, sound);
    const auto continuedPitch = heldPitchFor(72);
    require(std::abs((continuedPitch - retriggeredPitch) - referenceAdvance)
                < juce::jmax(0.002f, referenceAdvance * 0.05f),
            "Inherited glide trajectory changed speed after retrigger");

    // The glide accumulator belongs to the layer rather than to an allocated
    // voice. Let every released voice finish before the next repeated note so
    // this cannot pass by finding a still-active voice from the preceding key.
    engine.reset();
    sound.releaseSeconds = 0.001f;
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    auto repeatedPitch = heldPitchFor(72);
    require(repeatedPitch > 59.9f && repeatedPitch < 61.0f,
            "Persistent portamento did not start at the preceding pitch");
    for (int repetition = 0; repetition < 5; ++repetition)
    {
        send(juce::MidiMessage::noteOff(1, 72));
        require(pitchFor(72) < -900.0f,
                "Short release did not retire the previous glide voice");
        send(juce::MidiMessage::noteOn(1, 72, 0.9f));
        const auto nextPitch = heldPitchFor(72);
        require(nextPitch > repeatedPitch && nextPitch < 72.0f,
                "Continuous glide reset during repeated destination notes");
        repeatedPitch = nextPitch;
    }

    // Changing the destination during the same flight also has to start from
    // the live curve, rather than from the last discrete key (72 here).
    send(juce::MidiMessage::noteOff(1, 72));
    send(juce::MidiMessage::noteOn(1, 67, 0.9f));
    const auto redirectedPitch = heldPitchFor(67);
    require(redirectedPitch > repeatedPitch
                && redirectedPitch < repeatedPitch + 1.0f,
            "A new glide destination reset to the preceding MIDI note");

    // A lower target below the live pitch must reverse both the newly played
    // voice and the still-audible VCA tail of the note that was released.
    engine.reset();
    sound.releaseSeconds = 0.5f;
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, sound);
    }
    send(juce::MidiMessage::noteOff(1, 72));
    send(juce::MidiMessage::noteOn(1, 48, 0.9f));
    const auto lowerPitchBefore = heldPitchFor(48);
    const auto releasedUpperPitchBefore = pitchFor(72);
    audio.clear();
    engine.render(audio, {}, sound);
    const auto lowerPitchAfter = heldPitchFor(48);
    const auto releasedUpperPitchAfter = pitchFor(72);
    require(lowerPitchAfter < lowerPitchBefore,
            "A lower destination did not reverse the live glide");
    require(releasedUpperPitchAfter < releasedUpperPitchBefore,
            "An audible release tail kept following the abandoned upward glide");

    engine.reset();
    sound.releaseSeconds = 0.2f;
    sound.glideTypeMode = 2;
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    for (int block = 0; block < 48; ++block)
    {
        audio.clear();
        engine.render(audio, {}, sound);
    }
    require(std::abs(pitchFor(72) - std::round(pitchFor(72))) < 1.0e-6f,
            "Glissando did not quantise the moving pitch to semitone steps");

    engine.reset();
    sound.glideTypeMode = 5;
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    require(std::abs(pitchFor(72) - 72.0f) < 0.01f,
            "Fingered portamento incorrectly affected a staccato note");

    engine.reset();
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    require(pitchFor(72) > 59.9f && pitchFor(72) < 61.0f,
            "Fingered portamento ignored a legato note transition");
}

void testLfoLevelModifierGate()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.lfos[0].rate = 100.0f;
    parameters.modulationRoutes[wave::parameters::osc1PitchMod1]
        = { 0, 38, 24.0f };
    parameters.modulationRoutes[wave::parameters::lfo1LevelMod]
        = { 38, 22, 63.0f }; // Maximum x Modwheel, as used by factory pads.

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    engine.render(audio, noteOn, parameters);
    auto wheelDownPeak = std::abs(engine.firstActivePitchModulation());
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, parameters);
        wheelDownPeak = juce::jmax(
            wheelDownPeak, std::abs(engine.firstActivePitchModulation()));
    }
    require(wheelDownPeak < 1.0e-5f,
            "Modwheel-gated LFO remained full-on with the wheel down");

    juce::MidiBuffer wheelUp;
    wheelUp.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 0);
    auto wheelUpPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, block == 0 ? wheelUp : juce::MidiBuffer {}, parameters);
        wheelUpPeak = juce::jmax(
            wheelUpPeak, std::abs(engine.firstActivePitchModulation()));
    }
    require(wheelUpPeak > 0.25f,
            "Modwheel-gated LFO did not fade in with the wheel raised");

    engine.reset();
    parameters.modulationRoutes[wave::parameters::lfo1LevelMod].amount = 0.0f;
    engine.render(audio, noteOn, parameters);
    auto bypassPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, parameters);
        bypassPeak = juce::jmax(
            bypassPeak, std::abs(engine.firstActivePitchModulation()));
    }
    require(bypassPeak > 0.25f,
            "Zero LFO Level Modifier did not retain the Wave's full-level bypass");
}

void testPerformanceControlXFeedsLfoRoutes()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);

    wave::dsp::WaldorfEngine::PerformanceSnapshot performance;
    performance.controlXController = 74;
    auto& layer = performance.layers[0];
    layer.enabled = true;
    layer.source = 2;
    layer.sound.attackSeconds = 0.001f;
    layer.sound.cutoffHz = 16000.0f;
    layer.sound.filterEnvelopeSemitones = 0.0f;
    layer.sound.filterVelocitySemitones = 0.0f;
    layer.sound.filterKeytrackAmount = 0.0f;
    layer.sound.lfos[0].rate = 100.0f;
    layer.sound.modulationRoutes[wave::parameters::osc1PitchMod1]
        = { 0, 34, 24.0f }; // LFO 1 x Performance Control X.
    layer.sound.modulationRoutes[wave::parameters::filterMod1]
        = { 0, 34, 24.0f };

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    engine.render(audio, noteOn, performance);
    auto controlDownPeak = 0.0f;
    auto controlDownMinimumCutoff = std::numeric_limits<float>::max();
    auto controlDownMaximumCutoff = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, performance);
        controlDownPeak = juce::jmax(
            controlDownPeak, std::abs(engine.firstActivePitchModulation()));
        for (const auto& state : engine.voiceStates())
            if (state.active)
            {
                controlDownMinimumCutoff
                    = juce::jmin(controlDownMinimumCutoff, state.cutoffHz);
                controlDownMaximumCutoff
                    = juce::jmax(controlDownMaximumCutoff, state.cutoffHz);
            }
    }
    require(controlDownPeak < 1.0e-5f,
            "Control-X-gated LFO was active before its assigned MIDI controller moved");
    require(controlDownMaximumCutoff - controlDownMinimumCutoff < 1.0f,
            "Control-X-gated filter cutoff moved before its controller moved");

    juce::MidiBuffer controlUp;
    controlUp.addEvent(juce::MidiMessage::controllerEvent(1, 74, 127), 0);
    auto controlUpPeak = 0.0f;
    auto controlUpMinimumCutoff = std::numeric_limits<float>::max();
    auto controlUpMaximumCutoff = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, block == 0 ? controlUp : juce::MidiBuffer {}, performance);
        controlUpPeak = juce::jmax(
            controlUpPeak, std::abs(engine.firstActivePitchModulation()));
        for (const auto& state : engine.voiceStates())
            if (state.active)
            {
                controlUpMinimumCutoff
                    = juce::jmin(controlUpMinimumCutoff, state.cutoffHz);
                controlUpMaximumCutoff
                    = juce::jmax(controlUpMaximumCutoff, state.cutoffHz);
            }
    }
    require(controlUpPeak > 0.25f,
            "Performance Control X did not open its LFO modulation routes");
    require(controlUpMaximumCutoff / controlUpMinimumCutoff > 4.0f,
            "Performance Control X did not open the LFO filter-cutoff route");
}

void testOscillatorLinkUsesOscillatorOneModulation()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.lfos[0].rate = 100.0f;
    parameters.modulationRoutes[wave::parameters::osc1PitchMod1]
        = { 0, 38, 24.0f };
    parameters.modulationRoutes[wave::parameters::osc2PitchMod1]
        = { 37, 38, 0.0f };

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    engine.render(audio, noteOn, parameters);
    auto unlinkedPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, parameters);
        unlinkedPeak = juce::jmax(
            unlinkedPeak, std::abs(engine.firstActivePitchModulation(1)));
    }
    require(unlinkedPeak < 1.0e-5f,
            "Unlinked Oscillator 2 used Oscillator 1's pitch modulation");

    engine.reset();
    parameters.oscillatorLinkEnabled = true;
    engine.render(audio, noteOn, parameters);
    auto linkedPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, parameters);
        linkedPeak = juce::jmax(
            linkedPeak, std::abs(engine.firstActivePitchModulation(1)));
    }
    require(linkedPeak > 0.25f,
            "Wave Link did not apply Oscillator 1 pitch modulation to Oscillator 2");
}

void testReleasedVoicesAreStolenBeforeHeldChord()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 64);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 20.0f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer chord;
    for (const auto note : { 60, 64, 67, 71 })
        chord.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
    engine.render(audio, chord, parameters);
    require(engine.heldVoiceCount() == 4,
            "Held chord did not allocate all four voices");

    // Long release tails deliberately fill all remaining voices. Further bass
    // notes must reuse those tails rather than cutting keys still held down.
    for (int index = 0; index < wave::dsp::WaldorfEngine::voiceCount; ++index)
    {
        juce::MidiBuffer bass;
        const auto note = 36 + index % 8;
        bass.addEvent(juce::MidiMessage::noteOn(1, note, 0.9f), 0);
        bass.addEvent(juce::MidiMessage::noteOff(1, note), 32);
        engine.render(audio, bass, parameters);
    }
    require(engine.activeVoiceCount() == wave::dsp::WaldorfEngine::voiceCount,
            "Voice-stealing test did not exhaust polyphony");
    require(engine.heldVoiceCount() == 4,
            "Released bass tails stole voices from the held chord");
}

void testVoiceStealPreservesAnalogueHandover()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.sustainLevel = 1.0f;
    parameters.filterAttackSeconds = 1.0f;
    parameters.filterSustainLevel = 1.0f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer fillPolyphony;
    for (int note = 48; note < 48 + wave::dsp::WaldorfEngine::voiceCount; ++note)
        fillPolyphony.addEvent(juce::MidiMessage::noteOn(1, note, 0.9f), 0);
    engine.render(audio, fillPolyphony, parameters);
    for (int block = 0; block < 8; ++block)
        engine.render(audio, {}, parameters);
    require(engine.firstActiveVcaControlValue() > 0.75f,
            "Voice-stealing fixture did not reach a stable VCA voltage");
    const auto filterEnvelopeBeforeSteal = engine.firstActiveFilterEnvelopeValue();
    require(filterEnvelopeBeforeSteal > 0.05f,
            "Voice-stealing fixture did not establish a prior filter envelope");

    juce::MidiBuffer steal;
    steal.addEvent(juce::MidiMessage::noteOn(1, 108, 0.9f), 0);
    engine.render(audio, steal, parameters);
    require(engine.firstActiveVcaControlValue() > 0.60f,
            "Voice stealing discharged the analogue VCA and introduced a click");
    require(engine.firstActiveFilterEnvelopeValue()
                < filterEnvelopeBeforeSteal * 0.4f,
            "Voice stealing carried the previous note's filter tuning into the new note");
}

void testFullThreeCardPolyphony()
{
    static_assert(wave::dsp::WaldorfEngine::voiceBoardCount == 3);
    static_assert(wave::dsp::WaldorfEngine::voicesPerBoard == 16);
    static_assert(wave::dsp::WaldorfEngine::voiceCount == 48);

    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 64);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 1.0f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer notes;
    for (int note = 36; note < 36 + wave::dsp::WaldorfEngine::voiceCount; ++note)
        notes.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
    engine.render(audio, notes, parameters);
    require(engine.activeVoiceCount() == 48 && engine.heldVoiceCount() == 48,
            "Three-card engine did not allocate all 48 simultaneous voices");

    juce::MidiBuffer fortyNinth;
    fortyNinth.addEvent(juce::MidiMessage::noteOn(1, 96, 0.8f), 0);
    engine.render(audio, fortyNinth, parameters);
    require(engine.activeVoiceCount() == 48,
            "Voice allocator exceeded the 48-voice hardware limit");
}

void testParallelVoiceCardsMatchSerialRenderer()
{
    constexpr auto sampleRate = 48000.0;
    constexpr auto blockSize = 256;
    wave::dsp::WaldorfEngine serial;
    wave::dsp::WaldorfEngine parallel;
    serial.setVoiceCardThreadingEnabled(false);
    serial.prepare(sampleRate, blockSize);
    parallel.prepare(sampleRate, blockSize);

    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 0.8f;
    parameters.cutoffHz = 7200.0f;
    parameters.resonanceAmount = 0.46f;
    parameters.filterEnvelopeSemitones = 18.0f;
    parameters.noiseLevel = 0.11f;
    parameters.wavePosition = 17.25f;
    parameters.wavePosition2 = 42.5f;

    juce::AudioBuffer<float> serialAudio(2, blockSize);
    juce::AudioBuffer<float> parallelAudio(2, blockSize);
    for (int block = 0; block < 10; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
                midi.addEvent(juce::MidiMessage::noteOn(1, 36 + voice, 0.8f), 0);
        if (block == 4)
        {
            // Exercise serial and parallel sub-ranges in the same host block.
            midi.addEvent(juce::MidiMessage::noteOff(1, 36), 31);
            midi.addEvent(juce::MidiMessage::noteOn(1, 92, 0.7f), 96);
        }

        serialAudio.clear();
        parallelAudio.clear();
        serial.render(serialAudio, midi, parameters);
        parallel.render(parallelAudio, midi, parameters);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < blockSize; ++sample)
                require(std::abs(serialAudio.getSample(channel, sample)
                                 - parallelAudio.getSample(channel, sample))
                            < 1.0e-7f,
                        "Parallel voice cards changed the deterministic audio result");
    }

    require(serial.parallelVoiceCardRenderCount() == 0,
            "Disabled card threading still dispatched a worker");
    require(parallel.parallelVoiceCardRenderCount() > 0,
            "Full polyphony did not dispatch the voice-card workers");
    const auto serialVoices = serial.voiceStates();
    const auto parallelVoices = parallel.voiceStates();
    for (size_t voice = 0; voice < serialVoices.size(); ++voice)
        require(serialVoices[voice].triggerNote == parallelVoices[voice].triggerNote
                    && serialVoices[voice].active == parallelVoices[voice].active
                    && serialVoices[voice].keyDown == parallelVoices[voice].keyDown,
                "Parallel voice-card state diverged from the serial allocator");

    // Hosts may change sample rate and block size repeatedly. This must stop
    // and replace the persistent workers without leaving an in-flight job.
    for (int prepare = 0; prepare < 4; ++prepare)
    {
        const auto size = prepare % 2 == 0 ? 64 : 128;
        parallel.prepare(prepare % 2 == 0 ? 44100.0 : 96000.0, size);
        juce::AudioBuffer<float> lifecycleAudio(2, size);
        parallel.render(lifecycleAudio, {}, parameters);
    }
}

juce::AudioBuffer<float> renderReferenceSequence(int samples)
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 256);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 0.1f;
    parameters.cutoffHz = 12000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, samples);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
    if (samples > 24000)
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 24000);
    engine.render(audio, midi, parameters);
    return audio;
}

void testReferenceComparison()
{
    const auto reference = renderReferenceSequence(48000);
    const auto repeat = renderReferenceSequence(48000);
    const auto deterministic = wave::dsp::ReferenceComparator::compare(reference, repeat, 8);
    require(deterministic.lagSamples == 0 && deterministic.correlation > 0.999999f
                && deterministic.rmsError < 1.0e-7f,
            "Audio model is not deterministic enough for capture comparison");

    const auto* capturePath = std::getenv("WAVE_REFERENCE_CAPTURE");
    if (capturePath == nullptr)
        return;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(juce::File(capturePath)));
    require(reader != nullptr, "WAVE_REFERENCE_CAPTURE is not a readable audio file");
    const auto captureSamples = static_cast<int>(juce::jmin<int64_t>(reader->lengthInSamples,
                                                                     48000 * 20));
    juce::AudioBuffer<float> capture(juce::jmin(2, static_cast<int>(reader->numChannels)),
                                     captureSamples);
    require(reader->read(&capture, 0, captureSamples, 0, true, true),
            "Real-Wave reference capture could not be read");
    const auto model = renderReferenceSequence(captureSamples);
    const auto metrics = wave::dsp::ReferenceComparator::compare(capture, model, 4096);
    require(std::isfinite(metrics.correlation) && std::isfinite(metrics.rmsError),
            "Reference comparison produced invalid metrics");
    std::cerr << "Real Wave comparison: lag=" << metrics.lagSamples
              << " gain=" << metrics.fittedGain
              << " corr=" << metrics.correlation
              << " rms=" << metrics.rmsError
              << " peak=" << metrics.peakError << '\n';
}

void testFirmwareBundleRejectsMissingPath()
{
    // Used to search the parent ("/") recursively and effectively never return.
    wave::firmware::Bundle bundle;
    const auto report = bundle.load(juce::File("/nonexistent-wave-firmware-dir"));
    require(report.authenticity != wave::firmware::Bundle::Authenticity::verifiedOs1700,
            "Missing firmware path was accepted");
}

void testOfficialFirmwareWhenAvailable()
{
    const auto* path = std::getenv("WAVE_FIRMWARE_DIR");
    if (path == nullptr)
        return;

    wave::firmware::Bundle bundle;
    const auto report = bundle.load(juce::File(path));
    require(report.authenticity == wave::firmware::Bundle::Authenticity::verifiedOs1700,
            "WAVE_FIRMWARE_DIR does not contain the known OS 1.700 pair");

    const auto* master = static_cast<const uint8_t*>(bundle.getMasterImage().getData());
    const auto* voice = static_cast<const uint8_t*>(bundle.getVoiceImage().getData());
    const auto bigEndian32 = [](const uint8_t* bytes) {
        return (static_cast<uint32_t>(bytes[0]) << 24u)
               | (static_cast<uint32_t>(bytes[1]) << 16u)
               | (static_cast<uint32_t>(bytes[2]) << 8u)
               | static_cast<uint32_t>(bytes[3]);
    };
    require(master[0] == 0x4e && master[1] == 0xf9 && bigEndian32(master + 2) == 0x100c,
            "Master OS entry jump does not match the authenticated image layout");
    require(bigEndian32(master + 0xdf78) == 0xfea00000
                && bigEndian32(master + 0xdf7e) == 0xfea04000,
            "Master OS LCD video-memory references changed unexpectedly");
    require(bigEndian32(voice + 0x20) == 0x00008ffe
                && bigEndian32(voice + 0x24) == 0x00000400,
            "Voice image loader header or reset vectors changed unexpectedly");

    auto sharedMemoryStorage = std::make_unique<wave::firmware::SharedFirmwareMemory>();
    auto& sharedMemory = *sharedMemoryStorage;
    sharedMemory.clear();
    auto masterRuntimeStorage = std::make_unique<wave::firmware::MasterFirmwareRuntime>();
    auto& masterRuntime = *masterRuntimeStorage;
    masterRuntime.attachSharedMemory(sharedMemory);
    require(masterRuntime.loadAndStart(bundle.getMasterImage()),
            "Authenticated master firmware could not be installed on the CPU-board bus");
    require(masterRuntime.localByte(wave::firmware::MasterFirmwareRuntime::imageBase) == 0x4e
                && masterRuntime.localByte(
                       wave::firmware::MasterFirmwareRuntime::imageBase + 1u) == 0xf9,
            "Master OS image is not installed at its linked $001000 address");
    const auto executedMasterCycles = masterRuntime.runCycles(1000);
    require(executedMasterCycles > 0,
            "Master firmware did not consume its deterministic CPU cycle budget");
    if (!masterRuntime.completedColdHardwareSetup())
        std::cerr << "Master boot trace: PC=0x" << std::hex << masterRuntime.programCounter()
                  << " SP=0x" << masterRuntime.stackPointer()
                  << " last-unmapped=0x" << masterRuntime.lastUnmappedReadAddress()
                  << " unmapped=" << std::dec << masterRuntime.unmappedReadCount() << '\n';
    require(masterRuntime.completedColdHardwareSetup(),
            "Master firmware did not perform its observed cold-start hardware writes");

    masterRuntime.runCycles(2000000);
    require(masterRuntime.runOs1700InitialisationFileLoad(),
            "Master OS did not read synthetic INIT.SND and INIT.PFM through its file loader");
    const auto localLong = [&masterRuntime](uint32_t address) {
        return (static_cast<uint32_t>(masterRuntime.localByte(address)) << 24u)
               | (static_cast<uint32_t>(masterRuntime.localByte(address + 1u)) << 16u)
               | (static_cast<uint32_t>(masterRuntime.localByte(address + 2u)) << 8u)
               | masterRuntime.localByte(address + 3u);
    };
    require(localLong(0x4de14u) >= wave::firmware::MasterFirmwareRuntime::imageBase
                && localLong(0x4de14u) < 0x00050000u
                && localLong(0x4de18u) >= wave::firmware::MasterFirmwareRuntime::imageBase
                && localLong(0x4de18u) < 0x00050000u,
            "Genuine startup did not install the OS MIDI callbacks after floppy bypass");
    require(masterRuntime.loadedSyntheticInitialisationFiles(),
            "Both synthetic initialization files were not consumed by the genuine loader");
    require(masterRuntime.lcdVideoWriteCount() > 0,
            "Genuine master firmware did not reach its LCD drawing path");
    require(masterRuntime.unmappedReadCount() == 0,
            "Extended main-OS boot reached an unmodelled bus address");
    auto voiceRuntimeStorage = std::make_unique<wave::firmware::VoiceFirmwareRuntime>();
    auto& voiceRuntime = *voiceRuntimeStorage;
    voiceRuntime.attachSharedMemory(sharedMemory);
    require(voiceRuntime.loadAndReset(bundle.getVoiceImage()),
            "Authenticated voice firmware could not be installed on the WDV bus");
    auto executedVoiceCycles = 0;
    for (int slice = 0; slice < 20 && !voiceRuntime.waitingForMasterAcknowledgement(); ++slice)
        executedVoiceCycles += voiceRuntime.runCycles(100000);
    require(voiceRuntime.waitingForMasterAcknowledgement(),
            "Voice firmware did not publish and wait at its master acknowledgement mailbox");
    require(sharedMemory.program[0x5086] == 0xff && sharedMemory.program[0x508a] == 0x00,
            "Voice bootstrap mailbox state is inconsistent before master acknowledgement");

    const auto handoffCompleted = masterRuntime.runOs1700VoiceBoardLoaderHandoff(1);
    if (!handoffCompleted)
        std::cerr << "Master handoff trace: PC=0x" << std::hex << masterRuntime.programCounter()
                  << " ack=0x" << static_cast<int>(sharedMemory.program[0x508a])
                  << " service=0x" << static_cast<int>(sharedMemory.program[0x508e]) << '\n';
    require(handoffCompleted,
            "Master OS 1.700 did not execute its post-copy WDV loader handoff");
    require(sharedMemory.program[0x508a] == 0x01,
            "Main 68000 did not issue the observed WDV acknowledgement value");
    for (int slice = 0; slice < 20 && !voiceRuntime.reachedServiceLoop(); ++slice)
        executedVoiceCycles += voiceRuntime.runCycles(500000);
    require(executedVoiceCycles > 0,
            "Voice firmware did not consume its deterministic CPU cycle budget");
    require(voiceRuntime.controlTickCount() > 0,
            "WDV control interrupt clock did not advance");
    if (!voiceRuntime.reachedServiceLoop())
        std::cerr << "WDV boot trace: PC=0x" << std::hex << voiceRuntime.programCounter()
                  << " publish=0x" << static_cast<int>(voiceRuntime.sharedByte(0x5086))
                  << " service=0x" << static_cast<int>(voiceRuntime.sharedByte(0x508e))
                  << " asic=0x" << voiceRuntime.asicWord(0x12)
                  << " unmapped=" << std::dec << voiceRuntime.unmappedReadCount() << '\n';
    require(voiceRuntime.reachedServiceLoop(),
            "Voice firmware did not complete bootstrap and enter its shared-memory loop");
    require(masterRuntime.completeOs1700VoiceBoardServiceHandoff(),
            "Master firmware did not resume through the WDV service-success branch");
    require(voiceRuntime.asicWord(0x12) == 0x0004,
            "Voice firmware did not perform the observed oscillator-chip register write");
    const auto hardwareWrites = voiceRuntime.consumeHardwareWrites();
    require(!hardwareWrites.empty(), "WDV hardware writes were not timestamped");
    require(std::is_sorted(hardwareWrites.begin(), hardwareWrites.end(), [](const auto& a,
                                                                           const auto& b) {
                return a.cycle < b.cycle;
            }),
            "WDV hardware-write trace is not monotonic");
    require(std::any_of(hardwareWrites.begin(), hardwareWrites.end(), [](const auto& write) {
                return write.address == wave::firmware::VoiceFirmwareRuntime::asicBase + 0x12u;
            }),
            "WDV trace omitted the observed oscillator-chip control write");

    for (int slice = 0; slice < 40; ++slice)
    {
        masterRuntime.runCycles(50000);
        voiceRuntime.runCycles(100000);
    }
    require(masterRuntime.timerTickCount() > 0,
            "Main-board 6522 timer did not advance after OS hardware setup");
    require(masterRuntime.installedSyntheticInitialisationRecords(),
            "Master runtime did not install the synthetic INIT.SND and INIT.PFM records");
    require(std::equal(master + 0x0a42e, master + 0x0a52e,
                       sharedMemory.program.begin() + 0x5300),
            "Installed INIT.SND bytes differ from the genuine OS fallback record");
    require(std::equal(master + 0x0a22e, master + 0x0a42e,
                       sharedMemory.program.begin() + 0x5400),
            "Installed INIT.PFM bytes differ from the genuine OS fallback record");
    require(masterRuntime.programCounter() >= wave::firmware::MasterFirmwareRuntime::imageBase
                && masterRuntime.programCounter()
                       < wave::firmware::MasterFirmwareRuntime::localRamSize,
            "Master OS did not return safely to its genuine main loop after WDV startup");
    require(masterRuntime.unmappedReadCount() == 0,
            "Post-WDV main loop reached an unmodelled CPU-board address");

    const auto discardedWrites = voiceRuntime.consumeHardwareWrites();
    juce::ignoreUnused(discardedWrites);
    masterRuntime.pushMidiByte(0, 0x90);
    masterRuntime.pushMidiByte(0, 60);
    masterRuntime.pushMidiByte(0, 100);
    for (int slice = 0; slice < 30; ++slice)
    {
        masterRuntime.runCycles(50000);
        voiceRuntime.runCycles(100000);
    }
    const auto midiWrites = voiceRuntime.consumeHardwareWrites();
    require(!midiWrites.empty(),
            "MIDI note did not produce WDV oscillator/CV hardware writes");
    require(std::any_of(midiWrites.begin(), midiWrites.end(), [](const auto& write) {
                return write.address >= wave::firmware::VoiceFirmwareRuntime::asicBase
                       && write.address
                              < wave::firmware::VoiceFirmwareRuntime::asicBase + 0x200u;
            }),
            "Firmware-driven note did not reach either oscillator-chip register page");
    require(masterRuntime.unmappedReadCount() == 0 && voiceRuntime.unmappedReadCount() == 0,
            "Firmware-driven MIDI note reached an unmodelled system-bus address");

    // Disk machine settings may disable Performance reception or remap MIDI
    // programs. Internal panel recall must work, then release its bypass so
    // the following external MIDI packet still obeys those settings.
    sharedMemory.work[0x8824u] = 1u;
    sharedMemory.work[0x880du] = 7u;
    sharedMemory.work[0x8806u] = 1u;
    const auto displayRevision = masterRuntime.lcdDisplayRevision();
    const auto displayWrites = masterRuntime.lcdVideoWriteCount();
    require(masterRuntime.requestPerformanceSelection(128),
            "Internal Performance recall was rejected");
    require(masterRuntime.lcdDisplayRevision() != displayRevision
                && masterRuntime.lcdVideoWriteCount() == displayWrites,
            "Holding a recall frame did not notify the LCD without a VRAM write");
    const auto heldDisplayRevision = masterRuntime.lcdDisplayRevision();
    for (int slice = 0; slice < 40; ++slice)
    {
        masterRuntime.runCycles(50000);
        voiceRuntime.runCycles(100000);
    }
    require(masterRuntime.currentPerformanceId() == 128,
            "External MIDI settings redirected internal panel recall");
    require(masterRuntime.lcdDisplayRevision() != heldDisplayRevision,
            "Completed Performance recall did not publish a new LCD revision");
    masterRuntime.pushMidiByte(0, 0xc0u);
    masterRuntime.pushMidiByte(0, 2u);
    for (int slice = 0; slice < 40; ++slice)
    {
        masterRuntime.runCycles(50000);
        voiceRuntime.runCycles(100000);
    }
    require(masterRuntime.currentPerformanceId() == 128
                && sharedMemory.work[0x8824u] == 1u
                && sharedMemory.work[0x880du] == 7u
                && sharedMemory.work[0x8806u] == 1u,
            "Internal recall left external MIDI reception bypassed or changed its settings");
}

void testDecodedVoiceBoardProtocol()
{
    wave::firmware::SharedFirmwareMemory memory;
    memory.clear();
    wave::firmware::VoiceBoardProtocol protocol(memory);
    auto record = protocol.voiceRecord(0, 7);
    record[6] = 0x01;
    record[44] = 0x12;
    require(protocol.requestUpdate(0, 7),
            "Decoded WDV update request could not acquire the shared bus mutex");
    const auto pending = protocol.pendingUpdates();
    require(pending.size() == 1 && pending[0].voice == 7
                && pending[0].recordOffset == 0x700
                && pending[0].semaphoreOffset == 0x50a9,
            "WDV update mask, record stride, or semaphore layout is incorrect");
    require(memory.program[wave::firmware::VoiceBoardProtocol::busMutex] == 0,
            "WDV shared bus mutex was not released");
}

void testVoiceBoardWaveRamDecode()
{
    // Hand-built WDV image (no firmware needed): SP 0x8FFE, reset 0x400, then
    // byte stores through RAM A, RAM B and the write-both alias, and a spin.
    static constexpr uint8_t code[] = {
        0x13, 0xfc, 0x00, 0xa5, 0x00, 0x06, 0x00, 0x01, // move.b #$a5,$060001
        0x13, 0xfc, 0x00, 0x5a, 0x00, 0x05, 0x00, 0x03, // move.b #$5a,$050003
        0x13, 0xfc, 0x00, 0x11, 0x00, 0x04, 0x00, 0x05, // move.b #$11,$040005
        0x60, 0xfe                                       // bra.s *
    };
    juce::MemoryBlock image(0x20 + 0x400 + sizeof(code), true);
    auto* bytes = static_cast<uint8_t*>(image.getData());
    bytes[0x20 + 2] = 0x8f;
    bytes[0x20 + 3] = 0xfe;
    bytes[0x20 + 6] = 0x04;
    std::copy(std::begin(code), std::end(code), bytes + 0x20 + 0x400);

    wave::firmware::SharedFirmwareMemory memory;
    memory.clear();
    wave::firmware::VoiceFirmwareRuntime runtime;
    runtime.attachSharedMemory(memory);
    require(runtime.loadAndReset(image), "Synthetic WDV image was rejected");
    runtime.runCycles(2000);
    require(runtime.waveRamByte(0, 1) == 0xa5 && runtime.waveRamByte(1, 1) == 0xa5,
            "Write at $060001 did not reach both wave RAMs");
    require(runtime.waveRamByte(1, 3) == 0x5a && runtime.waveRamByte(0, 3) == 0,
            "Write at $050003 did not reach wave RAM B only");
    require(runtime.waveRamByte(0, 5) == 0x11 && runtime.waveRamByte(1, 5) == 0,
            "Write at $040005 did not reach wave RAM A only");
    require(memory.mainRam[0x60001] == 0 && memory.mainRam[0x50003] == 0
                && memory.mainRam[0x40005] == 0,
            "Voice-board wave RAM stores leaked into master DRAM");
}

class Test68000Bus final : public wave::firmware::M68000Bus
{
public:
    [[nodiscard]] uint8_t read8(uint32_t address) noexcept override
    {
        return memory[static_cast<size_t>(address) % memory.size()];
    }

    void write8(uint32_t address, uint8_t value) noexcept override
    {
        memory[static_cast<size_t>(address) % memory.size()] = value;
    }

    void write16(uint32_t address, uint16_t value)
    {
        write8(address, static_cast<uint8_t>(value >> 8u));
        write8(address + 1u, static_cast<uint8_t>(value));
    }

    void write32(uint32_t address, uint32_t value)
    {
        write16(address, static_cast<uint16_t>(value >> 16u));
        write16(address + 2u, static_cast<uint16_t>(value));
    }

    std::array<uint8_t, 65536> memory{};
};

void test68000ExecutionCore()
{
    Test68000Bus bus;
    bus.write32(0x0000, 0x00008000); // Initial supervisor stack pointer.
    bus.write32(0x0004, 0x00000100); // Reset program counter.
    bus.write16(0x0100, 0x702a);     // MOVEQ #42,D0
    bus.write16(0x0102, 0x5280);     // ADDQ.L #1,D0
    bus.write16(0x0104, 0x13fc);     // MOVE.B #$5a,$00000200
    bus.write16(0x0106, 0x005a);
    bus.write32(0x0108, 0x00000200);
    bus.write16(0x010c, 0x4e72);     // STOP #$2700
    bus.write16(0x010e, 0x2700);

    wave::firmware::M68000 cpu;
    cpu.reset(bus);
    require(cpu.programCounter() == 0x100 && cpu.stackPointer() == 0x8000,
            "68000 reset did not fetch the big-endian vector table");
    require(cpu.execute(bus, 100) > 0, "68000 core did not execute a cycle budget");
    require(cpu.dataRegister(0) == 43, "68000 arithmetic/register execution is incorrect");
    require(bus.memory[0x200] == 0x5a, "68000 absolute byte write did not reach the bus");

    wave::firmware::M68000 delayCpu;
    delayCpu.start(bus, 0x8000, 0x0100);
    delayCpu.setDataRegister(bus, 4, 1000u);
    const auto skipped = delayCpu.fastForwardDbraLoop(bus, 4, 4096u);
    require(skipped == 4090u && (delayCpu.dataRegister(4) & 0xffffu) == 591u,
            "68000 DBRA fast-forward did not preserve its cycle/register state");
}

void test6522TimerOneInterruptPath()
{
    wave::firmware::Via6522 via;
    via.reset();

    // The CPU-board wiring leaves PA0-PA6 pulled high and PA7 as the floppy
    // DRQ input. DDR bits must select between those pins and the output latch.
    via.write(3, 0xf0u);
    via.write(1, 0xa5u);
    require(via.read(1, 0x3cu) == 0xacu,
            "6522 port A did not merge its data-direction, latch, and input pins");

    via.write(11, 0x40u); // Timer 1 continuous mode.
    via.write(4, 0xceu);
    via.write(5, 0x04u);  // OS 1.700's $04CE timer latch.
    via.write(14, 0xc0u); // Enable Timer 1 IRQ.
    require(!via.interruptAsserted(),
            "6522 asserted Timer 1 IRQ before the counter expired");
    require(via.advanceCpuCycles(12319) == 0 && !via.interruptAsserted(),
            "6522 Timer 1 expired before its E-clock count elapsed");
    require(via.advanceCpuCycles(1) == 1 && via.interruptAsserted()
                && (via.read(13) & 0xc0u) == 0xc0u,
            "6522 Timer 1 did not raise its enabled interrupt flag");

    (void) via.read(4); // The Wave handler acknowledges by reading T1 low.
    require(!via.interruptAsserted() && (via.read(13) & 0x40u) == 0u,
            "6522 Timer 1 low-byte read did not acknowledge IRQ");
    require(via.advanceCpuCycles(12320) == 1 && via.interruptAsserted(),
            "6522 continuous Timer 1 did not reload from its latch");
}

void testLcdFramebuffer()
{
    wave::ui::LcdFramebuffer lcd;
    lcd.clear();
    require(wave::ui::LcdFramebuffer::memorySize == 3840,
            "LCD framebuffer size does not match 480 x 64 at one bit per pixel");

    lcd.write(0, 0x80);
    lcd.write(59, 0x01);
    lcd.write(60, 0x80);
    require(lcd.pixel(0, 0), "LCD first controller bit is not the first pixel");
    require(lcd.pixel(479, 0), "LCD last byte did not address the end of the scanline");
    require(lcd.pixel(0, 1), "LCD row stride is not 60 bytes");

    lcd.clear();
    lcd.line(3, 4, 19, 11);
    require(lcd.pixel(3, 4) && lcd.pixel(19, 11), "LCD line rasteriser lost an endpoint");
    lcd.text(24, 16, "WAVE 1.700");
    require(lcd.data() != std::array<uint8_t, wave::ui::LcdFramebuffer::memorySize>{},
            "LCD glyph renderer did not alter display RAM");

    lcd.write(static_cast<uint16_t>(wave::ui::LcdFramebuffer::memorySize), 0x55);
    require(lcd.read(0) == 0x55, "LCD controller address wrap is inconsistent");

    std::array<uint8_t, 0x4000> videoRam{};
    constexpr auto page = 2u;
    constexpr auto pageOffset = page * 0x1000u;
    videoRam[pageOffset] = 0x80;      // First visible byte, first pixel.
    videoRam[pageOffset + 59] = 0x01; // Last visible byte, last pixel.
    videoRam[pageOffset + 60] = 0xff; // Hardware padding must remain invisible.
    const auto byte = static_cast<size_t>(2 * 64 + 3);
    videoRam[pageOffset + byte] = 0x40;
    lcd.loadHardwareVideoRam(videoRam.data(), videoRam.size(), page);
    require(lcd.pixel(0, 0) && lcd.pixel(479, 0),
            "LCD visible byte span or bit order is incorrect");
    require(lcd.pixel(25, 2), "LCD scanout word stride is incorrect");
    require(!lcd.pixel(0, 1) && !lcd.pixel(32, 1),
            "LCD hardware padding leaked into the next scanline");
}

void testCompletePanelWiringContract()
{
    require(wave::panel::buttonDispatchCode.size() == 87
                && wave::panel::buttonDispatchCode[0] == 10
                && wave::panel::buttonDispatchCode[8] == 20
                && wave::panel::buttonDispatchCode[41] == 48,
            "Known OS 1.700 post-scan dispatch anchors changed");
    require(wave::panel::matrixIndexForDiagnosticCode(0) == 0
                && wave::panel::matrixIndexForDiagnosticCode(22) == 22
                && wave::panel::matrixIndexForDiagnosticCode(39) == 39,
            "The UI reapplied the firmware's private dispatch permutation");

    std::array<bool, 128> visibleInputs{};
    for (const auto& control : wave::panel::visibleSwitches)
    {
        const auto matrix = wave::panel::physicalMatrixIndex(control);
        require(matrix >= 0 && matrix < 87,
                "A visible switch has no physical matrix input");
        require(!visibleInputs[static_cast<size_t>(matrix)],
                "Two visible controls are wired to the same matrix input");
        visibleInputs[static_cast<size_t>(matrix)] = true;
    }
    for (const auto& control : wave::panel::keyboardPanelSwitches)
    {
        const auto matrix = wave::panel::physicalMatrixIndex(control);
        require(matrix >= 0 && matrix < 87,
                "A keyboard-panel switch has no physical matrix input");
        require(!visibleInputs[static_cast<size_t>(matrix)],
                "Two visible controls are wired to the same matrix input");
        visibleInputs[static_cast<size_t>(matrix)] = true;
    }
    require(visibleInputs[71] && visibleInputs[70],
            "CANCEL and OK are absent from the visible switch map");
    const auto* glideSwitch = wave::panel::keyboardPanelSwitchAt(228.0f, 751.0f);
    const auto* glideEdit = wave::panel::keyboardPanelSwitchAt(366.173f, 753.174f);
    require(glideSwitch != nullptr && glideSwitch->diagnosticCode == 6
                && glideEdit != nullptr && glideEdit->diagnosticCode == 11,
            "The lower Glide switches are absent from the canonical input map");
    const auto* glideEditLed = wave::panel::keyboardPanelLedAt(396.0f, 724.0f);
    require(glideEditLed != nullptr && glideEditLed->redSerialCode == 75,
            "The lower Glide Edit lamp is absent from the serial-output map");
    require(std::any_of(
                wave::panel::editIndicators.begin(),
                wave::panel::editIndicators.end(), [](const auto& indicator) {
                    return indicator.buttonDiagnosticCode == 11
                           && indicator.ledSerialCode == 75;
                }),
            "Glide Edit is absent from the mutually-exclusive Edit lamp map");
    constexpr std::array lowerKeyboardSwitches {
        std::tuple { 110.0f, 766.5f, 3 },  // Button 1
        std::tuple { 170.0f, 766.5f, 4 },  // Button 2
        std::tuple { 288.0f, 849.0f, 12 }, // Octave Up
        std::tuple { 288.0f, 927.0f, 73 }  // Octave Down
    };
    for (const auto& [x, y, serial] : lowerKeyboardSwitches)
    {
        const auto* control = wave::panel::keyboardPanelSwitchAt(x, y);
        require(control != nullptr && control->diagnosticCode == serial,
                "An updated-SVG lower keyboard switch is absent from the serial map");
    }
    const auto* octaveUpLed = wave::panel::keyboardPanelLedAt(288.0f, 821.0f);
    const auto* octaveDownLed = wave::panel::keyboardPanelLedAt(288.0f, 900.0f);
    require(octaveUpLed != nullptr && octaveUpLed->redSerialCode == 58
                && octaveDownLed != nullptr
                && octaveDownLed->redSerialCode == 27,
            "Keyboard octave LEDs are absent from the serial-output map");
    const auto* knobModeSelect = wave::panel::switchAt(786.0f, 555.0f);
    require(knobModeSelect != nullptr
                && knobModeSelect->diagnosticCode == 20
                && wave::panel::physicalMatrixIndex(*knobModeSelect) == 20,
            "Knob Mode Select is absent from the canonical panel input map");
    constexpr std::array<float, 4> knobModeLedY { 488.0f, 501.0f, 513.0f, 526.0f };
    constexpr std::array<int, 4> knobModeLedSerials { 6, 70, 71, 7 };
    for (size_t mode = 0; mode < knobModeLedY.size(); ++mode)
    {
        const auto* led = wave::panel::ledAt(784.0f, knobModeLedY[mode]);
        require(led != nullptr && led->redSerialCode == knobModeLedSerials[mode]
                    && led->greenSerialCode < 0,
                "A Knob Mode lamp is absent from the canonical LED output map");
    }

    std::array<int, 128> dispatchCounts{};
    for (const auto serial : wave::panel::buttonDispatchCode)
    {
        require(serial >= 0 && serial <= 100,
                "An OS button dispatch serial is out of range");
        ++dispatchCounts[static_cast<size_t>(serial)];
    }
    for (size_t serial = 0; serial < dispatchCounts.size(); ++serial)
    {
        const auto expectedMaximum = serial == 100 ? 2 : 1;
        require(dispatchCounts[serial] <= expectedMaximum,
                "Two genuine OS button actions share one dispatch serial");
    }
    require(dispatchCounts[100] == 2,
            "The two unused OS button-dispatch sentinels changed");

    std::array<bool, 128> ledOutputs{};
    for (const auto& led : wave::panel::visibleLeds)
    {
        for (const auto serial : { led.redSerialCode, led.greenSerialCode })
        {
            if (serial < 0)
                continue;
            require(serial < 128, "A visible LED serial code is out of range");
            require(!ledOutputs[static_cast<size_t>(serial)],
                    "Two visible LEDs share one physical output");
            ledOutputs[static_cast<size_t>(serial)] = true;
        }
    }
    for (const auto& led : wave::panel::keyboardPanelLeds)
    {
        require(led.redSerialCode >= 0 && led.redSerialCode < 128,
                "A keyboard-panel LED serial code is out of range");
        require(!ledOutputs[static_cast<size_t>(led.redSerialCode)],
                "Two visible LEDs share one physical output");
        ledOutputs[static_cast<size_t>(led.redSerialCode)] = true;
    }

    std::array<int, 128> potDiagnosticCounts{};
    std::array<int, 128> potAdcCounts{};
    for (size_t index = 0; index < wave::panel::visiblePots.size(); ++index)
    {
        const auto& pot = wave::panel::visiblePots[index];
        require(pot.parameterId != nullptr
                    && std::string_view(pot.parameterId).size() > 0,
                "A visible pot has no parameter identity");
        require(pot.diagnosticCode >= 0 && pot.diagnosticCode < 128
                    && pot.adcChannel >= 0 && pot.adcChannel < 128,
                "A visible pot serial is out of range");
        require(pot.adcChannel == pot.diagnosticCode + 1,
                "A pot diagnostic serial and analogue mux channel disagree");
        for (size_t previous = 0; previous < index; ++previous)
            require(std::string_view(pot.parameterId)
                        != wave::panel::visiblePots[previous].parameterId,
                    "Two visible pot entries control the same parameter");
        ++potDiagnosticCounts[static_cast<size_t>(pot.diagnosticCode)];
        ++potAdcCounts[static_cast<size_t>(pot.adcChannel)];
    }

    // LFO Select rebinds these three physical pots between LFO 1 and LFO 2.
    // They are the only legitimate duplicate entries in the analogue map.
    constexpr std::array<int, 3> sharedLfoDiagnosticSerials { 4, 6, 21 };
    constexpr std::array<int, 3> sharedLfoAdcChannels { 5, 7, 22 };
    const auto isExpectedAlias = [](int serial, const auto& aliases) {
        return std::find(aliases.begin(), aliases.end(), serial) != aliases.end();
    };
    for (size_t serial = 0; serial < potDiagnosticCounts.size(); ++serial)
    {
        const auto count = potDiagnosticCounts[serial];
        if (count == 0)
            continue;
        require(count == (isExpectedAlias(static_cast<int>(serial),
                                          sharedLfoDiagnosticSerials) ? 2 : 1),
                "An analogue pot diagnostic serial is duplicated unexpectedly");
    }
    for (const auto serial : sharedLfoDiagnosticSerials)
        require(potDiagnosticCounts[static_cast<size_t>(serial)] == 2,
                "An intentional LFO pot diagnostic alias is incomplete");
    for (size_t channel = 0; channel < potAdcCounts.size(); ++channel)
    {
        const auto count = potAdcCounts[channel];
        if (count == 0)
            continue;
        require(count == (isExpectedAlias(static_cast<int>(channel),
                                          sharedLfoAdcChannels) ? 2 : 1),
                "An analogue pot ADC channel is duplicated unexpectedly");
    }
    for (const auto channel : sharedLfoAdcChannels)
        require(potAdcCounts[static_cast<size_t>(channel)] == 2,
                "An intentional LFO pot ADC alias is incomplete");

    std::array<bool, 128> faderAdcChannels{};
    for (const auto channel : wave::panel::performanceFaderAdcChannels)
    {
        require(channel >= 0 && channel < 128,
                "A Performance fader ADC channel is out of range");
        require(!faderAdcChannels[static_cast<size_t>(channel)],
                "Two Performance faders share one ADC channel");
        require(potAdcCounts[static_cast<size_t>(channel)] == 0,
                "A Performance fader and panel pot share one ADC channel");
        faderAdcChannels[static_cast<size_t>(channel)] = true;
    }

    std::array<bool, 128> encoderSerials{};
    for (const auto serial : wave::panel::encoderSerialCodes)
    {
        require(serial >= 0 && serial < 128,
                "A dial-board encoder serial is out of range");
        require(!encoderSerials[static_cast<size_t>(serial)],
                "Two dial-board encoders share one serial code");
        encoderSerials[static_cast<size_t>(serial)] = true;
    }

    const auto adcFor = [](const char* parameterId) {
        for (const auto& pot : wave::panel::visiblePots)
            if (std::string_view(pot.parameterId) == parameterId)
                return pot.adcChannel;
        return -1;
    };
    require(adcFor(wave::parameters::modulationAmount[wave::parameters::resonanceMod]) == 55
                && adcFor(wave::parameters::modulationAmount[wave::parameters::filterMod1]) == 47,
            "Resonance and cutoff modulation ADC channels are crossed");
    require(adcFor(wave::parameters::highpassVelocity) == 36
                && adcFor(wave::parameters::modulationAmount[wave::parameters::highpassMod2]) == 38,
            "High-pass panel pots are missing from the analogue multiplexer");
    require(adcFor(wave::parameters::modulationAmount[wave::parameters::panMod1]) == 53
                && adcFor(wave::parameters::modulationAmount[wave::parameters::panMod2]) == 43,
            "Panning modulation pots are not wired to their panel channels");

    constexpr std::array<int, 9> expectedEncoders { 12, 13, 14, 15, 11, 10, 9, 8, 0 };
    require(wave::panel::encoderSerialCodes == expectedEncoders,
            "Dial-board encoder serial order changed");
}
} // namespace

int main()
{
    try
    {
        testWavetableQuantisation();
        testAsicClockMixOverflowAndVcfSaturation();
        testAsicResampling();
        testResamplerSimdAgainstScalar();
        testHighRegisterOscillatorResampling();
        testCutoffControlLaw();
        testQuickEditFastAccessControls();
        testWaveEnvelopeTraversal();
        testWaveLfo();
        testDspMathTables();
        testPerformanceTuningTables();
        testFreeRunningEngineLfo();
        testFactorySetWhenAvailable();
        testDamagedUserWavetableImport();
        testSparsePerformanceBanks();
        testPpgRomDecoding();
        testBundledPpgWavetables();
        testFactoryUpperWavetableBank();
        testOriginalWaveFactoryTablesWhenAvailable();
        testExpandedFirst32Loading();
        testUserPpgRomWhenAvailable();
        testCemStability();
        testCemFilterResponse();
        testMeasuredWaveResonancePassbandLoss();
        testAllVoiceFiltersAreCalibrated();
        testReconstructionCachedTransfer();
        testCemControlVoltageSettling();
        testLiveCutoffUsesContinuousBaseControlVoltage();
        testAsicHighpassResponse();
        testSerialBandpassTopology();
        testCemSelfOscillation();
        testCemResonanceAcrossSampleRates();
        testCemSmallSignalResonanceResponse();
        testMeasuredFastAmplifierAttackScaling();
        testAmplifierEnvelopeStages();
        testShortVcaReleaseDrainsAnalogueControl();
        testIndependentFilterEnvelope();
        testCentredVoiceHasNoArtificialPanSpread();
        testSampleAccurateMidiStart();
        testAudibleKeyboardRange();
        testPerformanceMidi();
        testEmptyPerformanceSilencesHeldNotes();
        testInstrumentVoiceAllocation();
        testInstrumentVoiceSharing();
        testWaveGlideModes();
        testReleasedVoicesAreStolenBeforeHeldChord();
        testVoiceStealPreservesAnalogueHandover();
        testFullThreeCardPolyphony();
        testParallelVoiceCardsMatchSerialRenderer();
        testLfoLevelModifierGate();
        testOscillatorLinkUsesOscillatorOneModulation();
        testPerformanceControlXFeedsLfoRoutes();
        testReferenceComparison();
        test68000ExecutionCore();
        test6522TimerOneInterruptPath();
        testDosFloppyImageCreation();
        testDp8473MountedDiskImage();
        testDp8473AutomaticDiskWrites();
        testLcdFramebuffer();
        testCompletePanelWiringContract();
        testDecodedVoiceBoardProtocol();
        testVoiceBoardWaveRamDecode();
        testFirmwareBundleRejectsMissingPath();
        testOfficialFirmwareWhenAvailable();
        std::cout << "WaveCoreTests: all checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WaveCoreTests: " << error.what() << '\n';
        return 1;
    }
}
