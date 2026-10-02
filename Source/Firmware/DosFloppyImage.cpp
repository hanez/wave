#include "DosFloppyImage.h"

#include <algorithm>
#include <array>
#include <vector>

namespace wave::firmware
{
namespace
{
constexpr size_t bytesPerSector = 512;

constexpr size_t totalSectors = 1440;
constexpr size_t sectorsPerCluster = 2;
constexpr size_t fatSectors = 3;
constexpr size_t rootEntries = 112;
constexpr uint8_t mediaDescriptor = 0xf9;
constexpr uint16_t sectorsPerTrack = 9;

void put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) noexcept;
void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) noexcept;

std::vector<uint8_t> makeFormattedDisk()
{
    std::vector<uint8_t> disk(bytesPerSector * totalSectors, 0);
    disk[0] = 0xeb;
    disk[1] = 0x3c;
    disk[2] = 0x90;
    std::copy_n(reinterpret_cast<const uint8_t*>("MSDOS5.0"), 8, disk.begin() + 3);
    put16(disk, 11, static_cast<uint16_t>(bytesPerSector));
    disk[13] = static_cast<uint8_t>(sectorsPerCluster);
    put16(disk, 14, 1);                         // Reserved boot sector.
    disk[16] = 2;                              // Two FAT copies.
    put16(disk, 17, static_cast<uint16_t>(rootEntries));
    put16(disk, 19, static_cast<uint16_t>(totalSectors));
    disk[21] = mediaDescriptor;
    put16(disk, 22, static_cast<uint16_t>(fatSectors));
    put16(disk, 24, sectorsPerTrack);
    put16(disk, 26, 2);                        // Heads.
    put32(disk, 28, 0);
    put32(disk, 32, 0);
    disk[36] = 0;
    disk[38] = 0x29;
    put32(disk, 39, static_cast<uint32_t>(juce::Time::getCurrentTime().toMilliseconds()));
    std::copy_n(reinterpret_cast<const uint8_t*>("WAVE DISK  "), 11,
                disk.begin() + 43);
    std::copy_n(reinterpret_cast<const uint8_t*>("FAT12   "), 8,
                disk.begin() + 54);
    disk[510] = 0x55;
    disk[511] = 0xaa;

    auto* firstFat = disk.data() + bytesPerSector;
    firstFat[0] = mediaDescriptor;
    firstFat[1] = 0xff;
    firstFat[2] = 0xff;
    std::copy_n(firstFat, fatSectors * bytesPerSector,
                disk.begin() + static_cast<std::ptrdiff_t>((1 + fatSectors)
                                                           * bytesPerSector));
    return disk;
}

void put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) noexcept
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8u);
}

void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) noexcept
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8u);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16u);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24u);
}

uint16_t get16(const uint8_t* bytes, size_t offset) noexcept
{
    return static_cast<uint16_t>(bytes[offset]
                                 | static_cast<uint16_t>(bytes[offset + 1u]) << 8u);
}

uint32_t get32(const uint8_t* bytes, size_t offset) noexcept
{
    return static_cast<uint32_t>(bytes[offset])
           | static_cast<uint32_t>(bytes[offset + 1u]) << 8u
           | static_cast<uint32_t>(bytes[offset + 2u]) << 16u
           | static_cast<uint32_t>(bytes[offset + 3u]) << 24u;
}

uint16_t fat12Entry(const uint8_t* fat, uint16_t cluster) noexcept
{
    const auto offset = static_cast<size_t>(cluster) * 3u / 2u;
    if ((cluster & 1u) == 0u)
        return static_cast<uint16_t>(fat[offset]
                                     | static_cast<uint16_t>(fat[offset + 1u] & 0x0fu)
                                           << 8u);
    return static_cast<uint16_t>((fat[offset] >> 4u)
                                 | static_cast<uint16_t>(fat[offset + 1u]) << 4u);
}

void setFat12Entry(uint8_t* fat, uint16_t cluster, uint16_t value) noexcept
{
    const auto offset = static_cast<size_t>(cluster) * 3u / 2u;
    value &= 0x0fffu;
    if ((cluster & 1u) == 0)
    {
        fat[offset] = static_cast<uint8_t>(value);
        fat[offset + 1u] = static_cast<uint8_t>((fat[offset + 1u] & 0xf0u)
                                                | ((value >> 8u) & 0x0fu));
    }
    else
    {
        fat[offset] = static_cast<uint8_t>((fat[offset] & 0x0fu)
                                           | static_cast<uint16_t>(
                                               (static_cast<uint32_t>(value) << 4u)
                                               & 0x00f0u));
        fat[offset + 1u] = static_cast<uint8_t>(value >> 4u);
    }
}

std::array<uint8_t, 11> dosNameFor(const juce::File& source)
{
    std::array<uint8_t, 11> result{};
    result.fill(static_cast<uint8_t>(' '));
    auto base = source.getFileNameWithoutExtension().toUpperCase();
    auto extension = source.getFileExtension().trimCharactersAtStart(".").toUpperCase();
    if (extension.isEmpty())
        extension = "SET";
    const auto legal = [](juce::juce_wchar character) {
        return juce::CharacterFunctions::isLetterOrDigit(character)
               || juce::String("$%'-_@~`!(){}^#&").containsChar(character);
    };
    juce::String cleanedBase;
    for (const auto character : base)
        if (legal(character) && character <= 0x7f)
            cleanedBase += character;
    if (cleanedBase.isEmpty())
        cleanedBase = "WAVE";
    const auto shortBase = cleanedBase.substring(0, 8);

    const auto copyPart = [&result, &legal](const juce::String& text, size_t start,
                                            size_t maximum) {
        auto destination = start;
        for (const auto character : text)
        {
            if (destination >= start + maximum)
                break;
            if (legal(character) && character <= 0x7f)
                result[destination++] = static_cast<uint8_t>(character);
        }
    };
    copyPart(shortBase, 0, 8);
    copyPart(extension, 8, 3);
    return result;
}
}

juce::Result DosFloppyImage::createEmpty(const juce::File& destination)
{
    auto output = destination;
    if (output.getFileExtension().isEmpty())
        output = output.withFileExtension(".img");
    const auto disk = makeFormattedDisk();
    if (!output.replaceWithData(disk.data(), disk.size()))
        return juce::Result::fail("The blank floppy image could not be written.");
    return juce::Result::ok();
}

juce::Result DosFloppyImage::createWithWaveSetup(const juce::File& destination,
                                                  const juce::File& source)
{
    return createWithWaveFile(destination, source, false);
}

juce::Result DosFloppyImage::createWithWaveWavetable(const juce::File& destination,
                                                    const juce::File& source)
{
    return createWithWaveFile(destination, source, true);
}

juce::Result DosFloppyImage::createWithWaveFile(const juce::File& destination,
                                               const juce::File& source,
                                               bool wavetable)
{
    auto output = destination;
    if (output.getFileExtension().isEmpty())
        output = output.withFileExtension(".img");
    if (output == source)
        return juce::Result::fail("Save the disk image to a different file from its source.");
    const auto rootSectors = rootEntries * 32u / bytesPerSector;
    const auto rootStartSector = 1u + 2u * fatSectors;
    const auto dataStartSector = rootStartSector + rootSectors;
    const auto clusterBytes = bytesPerSector * sectorsPerCluster;
    const auto availableClusters = (totalSectors - dataStartSector) / sectorsPerCluster;
    if (!source.existsAsFile())
        return juce::Result::fail("The selected Wave file does not exist.");
    juce::MemoryBlock setup;
    if (!source.loadFileAsData(setup) || setup.getSize() == 0)
        return juce::Result::fail("The Wave file could not be read.");
    if (wavetable)
    {
        constexpr size_t recordBytes = 138;
        constexpr size_t halfWaveBytes = 64;
        const auto* bytes = static_cast<const uint8_t*>(setup.getData());
        if (!source.hasFileExtension(".wtb") || setup.getSize() < recordBytes
            || (setup.getSize() - recordBytes) % halfWaveBytes != 0
            || setup.getSize() > recordBytes + 1000u * halfWaveBytes
            || bytes[9] != 0x55u)
            return juce::Result::fail(
                "Select a native Waldorf Wave .WTB file (a wavetable record with optional Wave data).");
    }
    const auto requiredClusters = (setup.getSize() + clusterBytes - 1u) / clusterBytes;
    if (requiredClusters > availableClusters)
        return juce::Result::fail(
            "The selected Wave file does not fit on a 720 KB DD floppy.");

    auto disk = makeFormattedDisk();
    std::copy_n(reinterpret_cast<const uint8_t*>(wavetable ? "WAVE WTB   " : "WAVE SET   "), 11,
                disk.begin() + 43);

    auto* firstFat = disk.data() + bytesPerSector;
    for (size_t index = 0; index < requiredClusters; ++index)
    {
        const auto cluster = static_cast<uint16_t>(index + 2u);
        const auto next = index + 1u == requiredClusters
                              ? static_cast<uint16_t>(0x0fffu)
                              : static_cast<uint16_t>(cluster + 1u);
        setFat12Entry(firstFat, cluster, next);
    }
    std::copy_n(firstFat, fatSectors * bytesPerSector,
                disk.begin() + static_cast<std::ptrdiff_t>((1 + fatSectors)
                                                           * bytesPerSector));

    const auto rootOffset = rootStartSector * bytesPerSector;
    const auto dosName = dosNameFor(source);
    std::copy(dosName.begin(), dosName.end(),
              disk.begin() + static_cast<std::ptrdiff_t>(rootOffset));
    disk[rootOffset + 11u] = 0x20;
    put16(disk, rootOffset + 26u, 2);
    put32(disk, rootOffset + 28u, static_cast<uint32_t>(setup.getSize()));
    const auto dataOffset = dataStartSector * bytesPerSector;
    std::copy_n(static_cast<const uint8_t*>(setup.getData()), setup.getSize(),
                disk.begin() + static_cast<std::ptrdiff_t>(dataOffset));

    if (!output.replaceWithData(disk.data(), disk.size()))
        return juce::Result::fail("The floppy image could not be written.");
    return juce::Result::ok();
}

juce::Result DosFloppyImage::readWaveSetup(const juce::File& imageFile,
                                            SetupFile& output)
{
    output = {};
    juce::MemoryBlock image;
    if (!imageFile.loadFileAsData(image) || image.getSize() < bytesPerSector)
        return juce::Result::fail("The floppy image could not be read.");
    const auto* bytes = static_cast<const uint8_t*>(image.getData());
    const auto imageSize = image.getSize();
    const auto sectorBytes = static_cast<size_t>(get16(bytes, 11));
    const auto clusterSectors = static_cast<size_t>(bytes[13]);
    const auto reservedSectors = static_cast<size_t>(get16(bytes, 14));
    const auto fatCount = static_cast<size_t>(bytes[16]);
    const auto directoryEntries = static_cast<size_t>(get16(bytes, 17));
    const auto sectorsInFat = static_cast<size_t>(get16(bytes, 22));
    if (sectorBytes < 128u || clusterSectors == 0u || reservedSectors == 0u
        || fatCount == 0u || directoryEntries == 0u || sectorsInFat == 0u)
        return juce::Result::fail("The floppy image has an invalid FAT12 layout.");
    const auto fatOffset = reservedSectors * sectorBytes;
    const auto rootOffset = (reservedSectors + fatCount * sectorsInFat) * sectorBytes;
    const auto rootBytes = directoryEntries * 32u;
    const auto rootSectors = (rootBytes + sectorBytes - 1u) / sectorBytes;
    const auto dataOffset = rootOffset + rootSectors * sectorBytes;
    const auto clusterBytes = clusterSectors * sectorBytes;
    if (fatOffset + sectorsInFat * sectorBytes > imageSize
        || rootOffset + rootBytes > imageSize || dataOffset > imageSize)
        return juce::Result::fail("The floppy image FAT12 structures are truncated.");

    const uint8_t* entry = nullptr;
    for (size_t index = 0; index < directoryEntries; ++index)
    {
        const auto* candidate = bytes + rootOffset + index * 32u;
        if (candidate[0] == 0x00u)
            break;
        if (candidate[0] == 0xe5u || candidate[11] == 0x0fu
            || (candidate[11] & 0x18u) != 0u)
            continue;
        if (candidate[8] == 'S' && candidate[9] == 'E' && candidate[10] == 'T')
        {
            entry = candidate;
            break;
        }
    }
    if (entry == nullptr)
        return juce::Result::fail("The floppy image contains no Wave .SET file.");

    const auto fileSize = static_cast<size_t>(get32(entry, 28));
    auto cluster = get16(entry, 26);
    if (fileSize == 0u || cluster < 2u)
        return juce::Result::fail("The Wave .SET directory entry is invalid.");
    output.data.setSize(fileSize, true);
    auto* destination = static_cast<uint8_t*>(output.data.getData());
    auto copied = size_t{};
    auto chainLength = size_t{};
    const auto maximumClusters = (imageSize - dataOffset) / clusterBytes;
    while (copied < fileSize && cluster >= 2u && cluster < 0x0ff8u
           && chainLength++ <= maximumClusters)
    {
        const auto sourceOffset = dataOffset
                                  + static_cast<size_t>(cluster - 2u) * clusterBytes;
        if (sourceOffset >= imageSize)
            break;
        const auto count = std::min(
            { clusterBytes, fileSize - copied, imageSize - sourceOffset });
        std::copy_n(bytes + sourceOffset, count, destination + copied);
        copied += count;
        cluster = fat12Entry(bytes + fatOffset, cluster);
    }
    if (copied != fileSize)
    {
        output = {};
        return juce::Result::fail("The Wave .SET cluster chain is incomplete.");
    }

    output.dosName = juce::String::fromUTF8(
        reinterpret_cast<const char*>(entry), 8).trimEnd()
                     + ".SET";
    return juce::Result::ok();
}
} // namespace wave::firmware
