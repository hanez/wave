#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace wave::dsp
{
// Execute the original OS 1.700 factory-table routines. No reconstructed
// PPG generator or separately captured upper bank is involved.
class WaveFactoryRom
{
public:
    static constexpr int tableCount = 64;
    static constexpr int wavesPerTable = 64;
    static constexpr int samplesPerWave = 128;
    static constexpr int romWaveCount = 300;
    static bool decode(const void* image, size_t size, std::vector<int8_t>& output);
    static bool decodeRomWaves(const void* image, size_t size, std::vector<int8_t>& output);
};
} // namespace wave::dsp
