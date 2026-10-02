#include "Dsp/WaveFactoryRom.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: DecodeWaveFactoryWavetables /path/to/w2sys.bin /path/to/private-output.bin\n";
        return 1;
    }
    juce::MemoryBlock image;
    std::vector<int8_t> decoded;
    if (!juce::File(argv[1]).loadFileAsData(image)
        || !wave::dsp::WaveFactoryRom::decode(image.getData(), image.getSize(), decoded))
    {
        std::cerr << "A verified Wave OS 1.700 master image is required.\n";
        return 2;
    }
    if (!juce::File(argv[2]).replaceWithData(decoded.data(), decoded.size()))
        return 3;
    std::cout << "64 original Wave tables, 64 waves each, 128 signed 8-bit samples per wave\n"
              << "SHA256: " << juce::SHA256(decoded.data(), decoded.size()).toHexString() << '\n';
}
