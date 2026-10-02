#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace wave::presets
{
class WaveFactorySet
{
public:
    static constexpr size_t soundSize = 256;
    static constexpr size_t performanceSize = 512;
    static constexpr int bankSize = 128;
    static constexpr int bankCount = 2;
    static constexpr int programCount = bankSize * bankCount;

    struct Report
    {
        bool validLayout = false;
        int validSounds = 0;
        int validPerformances = 0;
        int emptySounds = 0;
        int emptyPerformances = 0;
        int invalidSounds = 0;
        int invalidPerformances = 0;
        juce::String sha256;
        juce::String detail;
    };

    Report load(const void* bytes, size_t size);
    Report load(const juce::MemoryBlock& data) { return load(data.getData(), data.getSize()); }
    // Host snapshots contain native SRAM, including uninitialised/empty slots.
    // Preserve those bytes without applying disk-import record validation.
    Report loadStateSnapshot(const juce::MemoryBlock& data);

    [[nodiscard]] bool isLoaded() const noexcept { return report.validLayout; }
    [[nodiscard]] const Report& getReport() const noexcept { return report; }
    [[nodiscard]] std::span<const uint8_t, soundSize> sound(int bank, int program) const noexcept;
    [[nodiscard]] std::span<const uint8_t> performanceBank() const noexcept { return performances; }
    [[nodiscard]] std::span<const uint8_t> soundBank() const noexcept { return sounds; }
    [[nodiscard]] std::span<const uint8_t, performanceSize> performance(
        int bank, int program) const noexcept;
    [[nodiscard]] std::string soundName(int bank, int program) const;
    [[nodiscard]] std::string performanceName(int bank, int program) const;
    [[nodiscard]] std::vector<uint8_t> soundDump(int bank, int program,
                                                 int instrument) const;
    [[nodiscard]] std::vector<uint8_t> performanceDump(int bank, int program) const;
    [[nodiscard]] const juce::MemoryBlock& sourceImage() const noexcept
    {
        return loadedImage;
    }
    [[nodiscard]] juce::MemoryBlock imageWithStoredBanks(
        std::span<const uint8_t> storedSounds,
        std::span<const uint8_t> storedPerformances) const;

private:
    static constexpr size_t soundBankOffset = 0x12e7c;
    static constexpr size_t performanceBankOffset = 0x22e7c;
    static constexpr size_t minimumSize = performanceBankOffset
                                          + static_cast<size_t>(programCount) * performanceSize;

    static std::string nameAt(std::span<const uint8_t> record, size_t offset);
    std::array<uint8_t, static_cast<size_t>(programCount) * soundSize> sounds{};
    std::array<uint8_t, static_cast<size_t>(programCount) * performanceSize> performances{};
    juce::MemoryBlock loadedImage;
    Report report;
};
} // namespace wave::presets
