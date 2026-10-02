#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace wave::firmware
{
class DosFloppyImage final
{
public:
    struct SetupFile
    {
        juce::MemoryBlock data;
        juce::String dosName;
    };

    // Creates an empty, formatted 720 KB DD FAT12 medium. The Wave can then
    // populate it through its own Disk pages.
    static juce::Result createEmpty(const juce::File& destinationImage);
    // Creates the standard 720 KB DD FAT12 medium specified by the Wave
    // manual, containing one Wave Setup file in an ordinary 8.3 directory.
    static juce::Result createWithWaveSetup(const juce::File& destinationImage,
                                            const juce::File& waveSetupFile);
    // Packages a native Wave .WTB unchanged for loading via the Disk pages.
    static juce::Result createWithWaveWavetable(const juce::File& destinationImage,
                                                const juce::File& wavetableFile);
    // Reads the first ordinary .SET file from a FAT12 floppy image. This is
    // used by the audio engine as the same bank source the firmware sees.
    static juce::Result readWaveSetup(const juce::File& imageFile,
                                      SetupFile& setupFile);
private:
    static juce::Result createWithWaveFile(const juce::File& destinationImage,
                                           const juce::File& sourceFile,
                                           bool wavetable);
};
} // namespace wave::firmware
