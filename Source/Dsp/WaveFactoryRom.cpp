#include "WaveFactoryRom.h"
#include "Firmware/M68000.h"

#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <mutex>

namespace wave::dsp
{
namespace
{
class FactoryBus final : public firmware::M68000Bus
{
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x200000);
    bool completed = false;
    bool unmapped = false;

    uint8_t read8(uint32_t address) noexcept override
    {
        if (address < ram.size())
            return ram[address];
        unmapped = true;
        return 0xff;
    }
    void write8(uint32_t address, uint8_t value) noexcept override
    {
        if (address < ram.size())
            ram[address] = value;
        else
            unmapped = true;
    }
    bool usesInstructionInterception() const noexcept override { return true; }
    bool interceptInstruction(firmware::M68000& cpu, uint32_t pc) noexcept override
    {
        // All 64 half-waves have been filled when the master reaches its
        // WDV transfer call. Stop before the hardware handshake.
        if (pc == 0x00bd6e)
        {
            completed = true;
            cpu.endTimeslice();
            return true;
        }
        return false;
    }
};
} // namespace

bool WaveFactoryRom::decode(const void* image, size_t size, std::vector<int8_t>& output)
{
    // Absolute routine/data addresses are specific to this official image.
    if (image == nullptr || size != 302768
        || juce::SHA256(image, size).toHexString()
               != "4282457d9bf7d70da2e2aa4d6a1e69467c178d0d8d8be0a27f524ab8d62e3286")
        return false;

    static std::mutex mutex;
    static std::vector<int8_t> cached;
    const std::lock_guard lock(mutex);
    if (!cached.empty())
    {
        output = cached;
        return true;
    }

    const auto* bytes = static_cast<const uint8_t*>(image);
    std::vector<uint8_t> halfWaves;
    halfWaves.reserve(tableCount * wavesPerTable * 64);
    FactoryBus bus;
    for (int table = 0; table < tableCount; ++table)
    {
        std::fill(bus.ram.begin(), bus.ram.end(), 0);
        std::copy_n(bytes, size, bus.ram.begin() + 0x1000);
        bus.completed = false;
        bus.unmapped = false;
        firmware::M68000 cpu;
        cpu.start(bus, 0x0ffffe, 0x011bb8);
        cpu.setDataRegister(bus, 0, static_cast<uint32_t>(table));
        cpu.setDataRegister(bus, 1, 0);
        for (int cycles = 0; !bus.completed && !bus.unmapped && cycles < 2000000;
             cycles += 10000)
            cpu.execute(bus, 10000);
        if (!bus.completed || bus.unmapped)
            return false;
        halfWaves.insert(halfWaves.end(), bus.ram.begin() + 0x104000,
                         bus.ram.begin() + 0x105000);
    }
    // Guard the complete result, including the Wave-specific calculated
    // tables, slot 60, and the original triangle/square/saw terminal waves.
    if (juce::SHA256(halfWaves.data(), halfWaves.size()).toHexString()
            != "e2d3bdd4d22053058458962df7dc9a7ad08e895190f7b08e7f6b1ef63d1976c3")
        return false;

    std::vector<int8_t> decoded(tableCount * wavesPerTable * samplesPerWave);
    for (size_t wave = 0; wave < tableCount * wavesPerTable; ++wave)
        for (size_t sample = 0; sample < 64; ++sample)
        {
            const auto value = halfWaves[wave * 64 + sample];
            decoded[wave * 128 + sample] = static_cast<int8_t>(static_cast<int>(value) - 128);
            decoded[wave * 128 + 127 - sample]
                = static_cast<int8_t>(static_cast<int>(static_cast<uint8_t>(~value)) - 128);
        }
    cached = std::move(decoded);
    output = cached;
    return true;
}

bool WaveFactoryRom::decodeRomWaves(const void* image, size_t size, std::vector<int8_t>& output)
{
    if (image == nullptr || size != 302768
        || juce::SHA256(image, size).toHexString()
               != "4282457d9bf7d70da2e2aa4d6a1e69467c178d0d8d8be0a27f524ab8d62e3286")
        return false;
    // OS 1.700's Wave lookup at $0120EE resolves R000..R299 to
    // $042850 + Wave * 64. The system image begins at CPU address $001000.
    constexpr size_t halfWaveOffset = 0x42850u - 0x1000u;
    const auto* bytes = static_cast<const uint8_t*>(image);
    std::vector<int8_t> decoded(static_cast<size_t>(romWaveCount) * samplesPerWave);
    for (size_t wave = 0; wave < romWaveCount; ++wave)
        for (size_t sample = 0; sample < 64; ++sample)
        {
            const auto value = bytes[halfWaveOffset + wave * 64 + sample];
            decoded[wave * 128 + sample] = static_cast<int8_t>(static_cast<int>(value) - 128);
            decoded[wave * 128 + 127 - sample]
                = static_cast<int8_t>(static_cast<int>(static_cast<uint8_t>(~value)) - 128);
        }
    output = std::move(decoded);
    return true;
}
} // namespace wave::dsp
