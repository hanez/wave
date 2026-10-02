#include "WaveFactorySet.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cstring>

namespace wave::presets
{
namespace
{
bool nativeName(const uint8_t* bytes) noexcept
{
    // Store can write NULs inside the otherwise space-padded 16-byte name.
    // These also occur in SET files saved by the firmware, not just host state.
    return std::all_of(bytes, bytes + 16, [](uint8_t value) {
        return value == 0u || (value >= 0x20u && value <= 0x7eu);
    });
}

int clampProgram(int bank, int program) noexcept
{
    return juce::jlimit(0, WaveFactorySet::programCount - 1,
                        juce::jlimit(0, WaveFactorySet::bankCount - 1, bank)
                                * WaveFactorySet::bankSize
                            + juce::jlimit(0, WaveFactorySet::bankSize - 1, program));
}
} // namespace

WaveFactorySet::Report WaveFactorySet::load(const void* bytes, size_t size)
{
    report = {};
    sounds.fill(0);
    performances.fill(0);
    loadedImage.reset();
    if (bytes == nullptr || size < minimumSize)
    {
        report.detail = "SET image is shorter than the native sound and performance banks";
        return report;
    }

    const auto* source = static_cast<const uint8_t*>(bytes);
    loadedImage.append(bytes, size);
    std::copy_n(source + soundBankOffset, sounds.size(), sounds.begin());
    std::copy_n(source + performanceBankOffset, performances.size(), performances.begin());

    for (int index = 0; index < programCount; ++index)
    {
        const auto* soundRecord = sounds.data() + static_cast<size_t>(index) * soundSize;
        const auto* performanceRecord = performances.data()
                                        + static_cast<size_t>(index) * performanceSize;
        if (soundRecord[239] == 0x55u && nativeName(soundRecord + 240))
            ++report.validSounds;
        else if (std::all_of(soundRecord, soundRecord + soundSize,
                            [](uint8_t value) { return value == 0; }))
            ++report.emptySounds;
        else
            ++report.invalidSounds;
        if (performanceRecord[48] == 0x55u && nativeName(performanceRecord + 32))
            ++report.validPerformances;
        else if (std::all_of(performanceRecord, performanceRecord + performanceSize,
                            [](uint8_t value) { return value == 0; }))
            ++report.emptyPerformances;
        else
            ++report.invalidPerformances;
    }

    report.sha256 = juce::SHA256(bytes, size).toHexString();
    report.validLayout = report.validSounds > 0 && report.validPerformances > 0
                         && report.validSounds + report.emptySounds >= 250
                         && report.validPerformances + report.emptyPerformances >= 250;
    if (report.validLayout)
    {
        // A few damaged records must not strand Total Recall using the old
        // host bank after the OS has loaded a new one. Quarantine only those
        // slots in the working banks; retain the source disk bytes verbatim.
        for (int index = 0; index < programCount; ++index)
        {
            auto* soundRecord = sounds.data() + static_cast<size_t>(index) * soundSize;
            auto* performanceRecord = performances.data()
                                      + static_cast<size_t>(index) * performanceSize;
            if (soundRecord[239] != 0x55u || !nativeName(soundRecord + 240))
                std::fill_n(soundRecord, soundSize, uint8_t{});
            if (performanceRecord[48] != 0x55u || !nativeName(performanceRecord + 32))
                std::fill_n(performanceRecord, performanceSize, uint8_t{});
        }
    }
    report.detail = report.validLayout
                        ? "Native Waldorf Wave SET: two 128-program sound banks and two 128-program performance banks"
                        : "SET bank markers or names do not match the Waldorf Wave layout";
    if (report.validLayout && report.invalidSounds + report.invalidPerformances > 0)
        report.detail += "; working bank omits " + juce::String(report.invalidSounds)
                         + " malformed Sounds and " + juce::String(report.invalidPerformances)
                         + " malformed Performances";
    return report;
}

WaveFactorySet::Report WaveFactorySet::loadStateSnapshot(const juce::MemoryBlock& data)
{
    load(data);
    if (data.getSize() >= minimumSize)
    {
        // Host snapshots represent exact SRAM, including records a disk
        // import would quarantine. Do not change those saved machine bytes.
        const auto* bytes = static_cast<const uint8_t*>(data.getData());
        std::copy_n(bytes + soundBankOffset, sounds.size(), sounds.begin());
        std::copy_n(bytes + performanceBankOffset, performances.size(), performances.begin());
        report.validLayout = true;
        report.detail = "Saved native Wave sound and performance SRAM banks";
    }
    return report;
}

juce::MemoryBlock WaveFactorySet::imageWithStoredBanks(
    std::span<const uint8_t> storedSounds,
    std::span<const uint8_t> storedPerformances) const
{
    auto image = loadedImage;
    if (image.getSize() >= minimumSize && storedSounds.size() == sounds.size()
        && storedPerformances.size() == performances.size())
    {
        auto* bytes = static_cast<uint8_t*>(image.getData());
        std::copy(storedSounds.begin(), storedSounds.end(), bytes + soundBankOffset);
        std::copy(storedPerformances.begin(), storedPerformances.end(),
                  bytes + performanceBankOffset);
    }
    return image;
}

std::span<const uint8_t, WaveFactorySet::soundSize> WaveFactorySet::sound(
    int bank, int program) const noexcept
{
    const auto index = static_cast<size_t>(clampProgram(bank, program));
    return std::span<const uint8_t, soundSize>(sounds.data() + index * soundSize, soundSize);
}

std::span<const uint8_t, WaveFactorySet::performanceSize> WaveFactorySet::performance(
    int bank, int program) const noexcept
{
    const auto index = static_cast<size_t>(clampProgram(bank, program));
    return std::span<const uint8_t, performanceSize>(
        performances.data() + index * performanceSize, performanceSize);
}

std::string WaveFactorySet::nameAt(std::span<const uint8_t> record, size_t offset)
{
    if (offset + 16 > record.size())
        return {};
    std::string result(reinterpret_cast<const char*>(record.data() + offset), 16);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\0'))
        result.pop_back();
    return result;
}

std::string WaveFactorySet::soundName(int bank, int program) const
{
    return nameAt(sound(bank, program), 240);
}

std::string WaveFactorySet::performanceName(int bank, int program) const
{
    return nameAt(performance(bank, program), 32);
}

std::vector<uint8_t> WaveFactorySet::soundDump(int bank, int program,
                                                int instrument) const
{
    std::vector<uint8_t> result { 0xf0u, 0x3eu, 0x03u, 0x7fu, 0x00u,
                                  static_cast<uint8_t>(juce::jlimit(0, 7, instrument)),
                                  static_cast<uint8_t>(juce::jlimit(0, 1, bank)),
                                  static_cast<uint8_t>(juce::jlimit(0, 127, program)) };
    auto checksum = 0u;
    for (size_t index = 5; index < result.size(); ++index)
        checksum += result[index];
    for (const auto byte : sound(bank, program))
    {
        const auto midiByte = static_cast<uint8_t>(byte & 0x7fu);
        result.push_back(midiByte);
        checksum += midiByte;
    }
    result.push_back(static_cast<uint8_t>(checksum & 0x7fu));
    result.push_back(0xf7u);
    return result;
}

std::vector<uint8_t> WaveFactorySet::performanceDump(int bank, int program) const
{
    std::vector<uint8_t> result { 0xf0u, 0x3eu, 0x03u, 0x7fu, 0x01u,
                                  static_cast<uint8_t>(juce::jlimit(0, 1, bank)),
                                  static_cast<uint8_t>(juce::jlimit(0, 127, program)) };
    auto checksum = 0u;
    for (size_t index = 5; index < result.size(); ++index)
        checksum += result[index];
    for (const auto byte : performance(bank, program))
    {
        const auto midiByte = static_cast<uint8_t>(byte & 0x7fu);
        result.push_back(midiByte);
        checksum += midiByte;
    }
    result.push_back(static_cast<uint8_t>(checksum & 0x7fu));
    result.push_back(0xf7u);
    return result;
}
} // namespace wave::presets
