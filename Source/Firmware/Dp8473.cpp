#include "Dp8473.h"

#include <algorithm>
#include <chrono>

namespace wave::firmware
{
namespace
{
uint16_t little16(const std::vector<uint8_t>& bytes, size_t offset) noexcept
{
    if (offset + 1 >= bytes.size())
        return 0;
    return static_cast<uint16_t>(bytes[offset]
                                 | static_cast<uint16_t>(bytes[offset + 1]) << 8u);
}

uint32_t little32(const std::vector<uint8_t>& bytes, size_t offset) noexcept
{
    if (offset + 3 >= bytes.size())
        return 0;
    return static_cast<uint32_t>(bytes[offset])
           | static_cast<uint32_t>(bytes[offset + 1]) << 8u
           | static_cast<uint32_t>(bytes[offset + 2]) << 16u
           | static_cast<uint32_t>(bytes[offset + 3]) << 24u;
}
}

Dp8473::~Dp8473()
{
    {
        const std::scoped_lock lock(mutex);
        stopping = true;
    }
    saveRequested.notify_one();
    if (saveThread.joinable())
        saveThread.join();
    (void) flush();
}

void Dp8473::saveLoop()
{
    std::unique_lock lock(mutex);
    while (!stopping)
    {
        saveRequested.wait(lock, [this] { return stopping || dirty; });
        if (stopping)
            break;

        // Batch a floppy's sector transfers, with a bounded delay even while
        // the firmware continues writing. Never perform file I/O on audio.
        if (saveRequested.wait_for(lock, std::chrono::milliseconds(100),
                                   [this] { return stopping; }))
            break;
        lock.unlock();
        const auto saved = flush();
        lock.lock();
        if (saved.failed())
            saveRequested.wait_for(lock, std::chrono::seconds(1),
                                   [this] { return stopping; });
    }
}

juce::Result Dp8473::mount(const juce::File& file)
{
    if (!file.existsAsFile())
        return juce::Result::fail("The selected disk image does not exist.");

    const std::scoped_lock saveLock(saveMutex);
    for (;;)
    {
        // The selected path may be the image already in the drive. Commit its
        // pending sector writes before reading it, so a remount cannot install
        // the older on-disk bytes after successfully flushing the newer image.
        const auto previousFlush = flushWithSaveLock();
        if (previousFlush.failed())
            return previousFlush;

        juce::MemoryBlock bytes;
        if (!file.loadFileAsData(bytes))
            return juce::Result::fail("The selected disk image could not be read.");

        std::vector<uint8_t> loaded(bytes.getSize());
        if (!loaded.empty())
            std::copy_n(static_cast<const uint8_t*>(bytes.getData()), loaded.size(),
                        loaded.begin());
        Geometry detected;
        if (!geometryForImage(loaded, detected))
            return juce::Result::fail(
                "This is not a supported raw MS-DOS floppy image (720 KB, 800 KB, "
                "1.44 MB, or a valid FAT BPB geometry).");

        const std::scoped_lock lock(mutex);
        // A completed audio-thread transfer during the read must also reach the
        // old image before it is replaced (including a remount of the same path).
        if (dirty)
            continue;
        if (!saveThread.joinable())
            saveThread = std::thread([this] { saveLoop(); });
        imagePath = file;
        image = std::move(loaded);
        geometry = detected;
        writable = file.hasWriteAccess();
        dirty = false;
        lastSaveError.clear();
        bytesRead = 0;
        ++revision;
        // The Wave polls the drive-status input before it will issue any FDC
        // command.  Leaving the change line asserted here deadlocks the firmware:
        // it keeps displaying "Please insert Disk!" and never seeks/recalibrates,
        // which are the only controller commands that otherwise clear the latch.
        // A host mount represents the completed insertion, so present ready media.
        diskChanged = false;
        resetController();
        return juce::Result::ok();
    }
}

juce::Result Dp8473::flush()
{
    const std::scoped_lock saveLock(saveMutex);
    return flushWithSaveLock();
}

juce::Result Dp8473::flushWithSaveLock()
{
    juce::File destination;
    std::vector<uint8_t> snapshot;
    uint64_t snapshotRevision = 0;
    {
        const std::scoped_lock lock(mutex);
        if (!dirty)
            return juce::Result::ok();
        if (!writable || imagePath == juce::File{})
            return juce::Result::fail("The mounted disk image is read-only.");
        destination = imagePath;
        snapshot = image;
        snapshotRevision = revision;
    }

    // Only replace the original after a complete, successful write. JUCE's
    // File::replaceWithData does not check its temporary-file write result.
    const juce::TemporaryFile temporary(destination, juce::TemporaryFile::useHiddenFile);
    auto saved = false;
    {
        juce::FileOutputStream stream(temporary.getFile());
        if (stream.openedOk() && stream.write(snapshot.data(), snapshot.size()))
        {
            stream.flush();
            saved = stream.getStatus().wasOk();
        }
    }
    if (saved)
        saved = temporary.overwriteTargetFileWithTemporary();

    const std::scoped_lock lock(mutex);
    if (!saved)
    {
        lastSaveError = "The mounted disk image could not be saved.";
        return juce::Result::fail(lastSaveError);
    }
    lastSaveError.clear();
    if (revision == snapshotRevision)
        dirty = false;
    return juce::Result::ok();
}

juce::Result Dp8473::eject()
{
    const std::scoped_lock saveLock(saveMutex);
    for (;;)
    {
        const auto saved = flushWithSaveLock();
        if (saved.failed())
            return saved;
        const std::scoped_lock lock(mutex);
        if (dirty)
            continue;
        imagePath = juce::File{};
        image.clear();
        geometry = {};
        writable = false;
        dirty = false;
        lastSaveError.clear();
        bytesRead = 0;
        diskChanged = true;
        ++revision;
        resetController();
        return juce::Result::ok();
    }
}

juce::MemoryBlock Dp8473::mountedImageSnapshot() const
{
    const std::scoped_lock lock(mutex);
    return image.empty() ? juce::MemoryBlock{}
                         : juce::MemoryBlock(image.data(), image.size());
}

void Dp8473::resetController() noexcept
{
    phase = Phase::idle;
    command.clear();
    transfer.clear();
    result.clear();
    transferPosition = resultPosition = 0;
    irq = false;
    pendingSense = false;
}

bool Dp8473::geometryForImage(const std::vector<uint8_t>& bytes,
                              Geometry& output) noexcept
{
    const auto accept = [&bytes, &output](int bps, int spt, int heads, int cylinders) {
        if (bps <= 0 || spt <= 0 || heads <= 0 || cylinders <= 0)
            return false;
        const auto expected = static_cast<uint64_t>(bps) * static_cast<uint64_t>(spt)
                              * static_cast<uint64_t>(heads)
                              * static_cast<uint64_t>(cylinders);
        if (expected != bytes.size())
            return false;
        output = { bps, spt, heads, cylinders };
        return true;
    };

    if (bytes.size() >= 36)
    {
        const auto bps = static_cast<int>(little16(bytes, 11));
        const auto spt = static_cast<int>(little16(bytes, 24));
        const auto heads = static_cast<int>(little16(bytes, 26));
        const auto shortTotal = static_cast<uint32_t>(little16(bytes, 19));
        const auto sectors = shortTotal != 0 ? shortTotal : little32(bytes, 32);
        if (bps >= 128 && bps <= 8192 && (bps & (bps - 1)) == 0 && spt > 0
            && heads > 0 && sectors > 0 && sectors % static_cast<uint32_t>(spt * heads) == 0
            && accept(bps, spt, heads,
                      static_cast<int>(sectors / static_cast<uint32_t>(spt * heads))))
            return true;
    }

    if (accept(512, 9, 2, 80))
        return true;
    if (accept(512, 10, 2, 80))
        return true;
    return accept(512, 18, 2, 80);
}

int Dp8473::commandLength(uint8_t opcode) noexcept
{
    switch (opcode & 0x1fu)
    {
        case 0x02: return 9; // Read a track.
        case 0x03: return 3; // Specify.
        case 0x04: return 2; // Sense drive status.
        case 0x05: return 9; // Write data.
        case 0x06: return 9; // Read data.
        case 0x07: return 2; // Recalibrate.
        case 0x08: return 1; // Sense interrupt.
        case 0x09: return 9; // Write deleted data.
        case 0x0a: return 2; // Read ID.
        case 0x0c: return 9; // Read deleted data.
        case 0x0d: return 6; // Format track.
        case 0x0f: return 3; // Seek.
        case 0x11: return 9; // Scan equal.
        case 0x19: return 9; // Scan low/equal.
        case 0x1d: return 9; // Scan high/equal.
        default: return 1;
    }
}

uint8_t Dp8473::read(uint32_t cpuAddress) noexcept
{
    const std::scoped_lock lock(mutex);
    const auto offset = cpuAddress & 0x3fu;
    if (offset == 0x09u)
        return mainStatus();
    if (offset == 0x0bu)
        return readDataRegister(false, false);
    if (offset == 0x1bu)
        return readDataRegister(true, false);
    if (offset == 0x2bu)
        return readDataRegister(true, true);
    if (offset == 0x0fu)
        return image.empty() || diskChanged ? 0x80u : 0x00u;
    if (offset == 0x31u)
    {
        irq = false;
        return 0;
    }
    return 0xffu;
}

void Dp8473::write(uint32_t cpuAddress, uint8_t value) noexcept
{
    const std::scoped_lock lock(mutex);
    const auto offset = cpuAddress & 0x3fu;
    if (offset == 0x05u)
    {
        const auto wasReset = (driveControl & 0x04u) == 0;
        driveControl = value;
        if ((value & 0x04u) == 0)
            resetController();
        else if (wasReset)
        {
            pendingSense = true;
            pendingSenseDrive = static_cast<uint8_t>(value & 0x03u);
            irq = true;
        }
        return;
    }
    if (offset == 0x0fu)
    {
        dataRate = static_cast<uint8_t>(value & 0x03u);
        return;
    }
    if (offset == 0x0bu)
        writeDataRegister(value, false);
    else if (offset == 0x1bu)
        writeDataRegister(value, true);
    else if (offset == 0x2bu)
        finishTransfer();
}

uint8_t Dp8473::mainStatus() const noexcept
{
    switch (phase)
    {
        case Phase::idle: return 0x80u;
        case Phase::command: return 0x90u;
        case Phase::readData: return 0xd0u;
        case Phase::writeData:
        case Phase::formatData: return 0x90u;
        case Phase::result: return 0xd0u;
    }
    return 0x80u;
}

uint8_t Dp8473::readDataRegister(bool, bool terminalCount) noexcept
{
    if (terminalCount)
    {
        finishTransfer();
        return 0;
    }
    if (phase == Phase::readData)
    {
        if (transferPosition < transfer.size())
        {
            ++bytesRead;
            return transfer[transferPosition++];
        }
        return 0;
    }
    if (phase == Phase::result)
    {
        const auto value = resultPosition < result.size()
                               ? result[resultPosition++]
                               : static_cast<uint8_t>(0xffu);
        if (resultPosition >= result.size())
        {
            phase = Phase::idle;
            result.clear();
            resultPosition = 0;
            irq = false;
        }
        return value;
    }
    return 0xffu;
}

void Dp8473::writeDataRegister(uint8_t value, bool) noexcept
{
    if (phase == Phase::writeData || phase == Phase::formatData)
    {
        if (transferPosition < transfer.size())
            transfer[transferPosition++] = value;
        return;
    }
    if (phase == Phase::result)
    {
        // A command byte during result phase is the documented forced abort.
        phase = Phase::idle;
        result.clear();
        resultPosition = 0;
    }
    acceptCommandByte(value);
}

void Dp8473::acceptCommandByte(uint8_t value) noexcept
{
    if (phase == Phase::idle)
    {
        command.clear();
        phase = Phase::command;
    }
    command.push_back(value);
    if (static_cast<int>(command.size()) >= commandLength(command.front()))
        executeCommand();
}

void Dp8473::executeCommand() noexcept
{
    const auto opcode = static_cast<uint8_t>(command.front() & 0x1fu);
    switch (opcode)
    {
        case 0x03: // Specify
            phase = Phase::idle;
            command.clear();
            break;
        case 0x04: // Sense drive status
        {
            const auto driveHead = command[1];
            const auto drive = static_cast<uint8_t>(driveHead & 0x03u);
            const auto head = static_cast<uint8_t>((driveHead >> 2u) & 0x01u);
            const auto trackZero = currentCylinder[drive] == 0 ? 0x10u : 0x00u;
            const auto ready = image.empty() ? 0x00u : 0x20u;
            const auto writeProtect = writable ? 0x00u : 0x40u;
            enterResult({ static_cast<uint8_t>(ready | writeProtect | trackZero
                                                | static_cast<uint8_t>(head << 2u)
                                                | drive) },
                        false);
            break;
        }
        case 0x05:
        case 0x09:
            beginReadOrWrite(true);
            break;
        case 0x06:
        case 0x0c:
        case 0x02:
            beginReadOrWrite(false);
            break;
        case 0x07: // Recalibrate
        {
            const auto drive = static_cast<uint8_t>(command[1] & 0x03u);
            currentCylinder[drive] = 0;
            diskChanged = false;
            pendingSense = true;
            pendingSenseDrive = drive;
            irq = true;
            phase = Phase::idle;
            command.clear();
            break;
        }
        case 0x08: // Sense interrupt status
        {
            const auto drive = pendingSenseDrive;
            const auto st0 = pendingSense ? static_cast<uint8_t>(0x20u | drive)
                                          : static_cast<uint8_t>(0x80u);
            pendingSense = false;
            irq = false;
            enterResult({ st0, currentCylinder[drive] }, false);
            break;
        }
        case 0x0a: // Read ID
        {
            const auto driveHead = command[1];
            const auto drive = static_cast<uint8_t>(driveHead & 0x03u);
            const auto head = static_cast<uint8_t>((driveHead >> 2u) & 0x01u);
            if (image.empty())
                enterResult({ static_cast<uint8_t>(0x40u
                                                    | static_cast<uint8_t>(head << 2u)
                                                    | drive), 0x04,
                              0x00, currentCylinder[drive], head, 1, 2 }, true);
            else
                enterResult({ static_cast<uint8_t>((head << 2u) | drive), 0x00, 0x00,
                              currentCylinder[drive], head, 1, 2 }, true);
            break;
        }
        case 0x0d:
            beginFormat();
            break;
        case 0x0f: // Seek
        {
            const auto drive = static_cast<uint8_t>(command[1] & 0x03u);
            currentCylinder[drive] = command[2];
            diskChanged = false;
            pendingSense = true;
            pendingSenseDrive = drive;
            irq = true;
            phase = Phase::idle;
            command.clear();
            break;
        }
        case 0x11:
        case 0x19:
        case 0x1d:
            beginReadOrWrite(false);
            break;
        default:
            enterResult({ 0x80u }, false);
            break;
    }
}

void Dp8473::beginReadOrWrite(bool write) noexcept
{
    const auto driveHead = command[1];
    const auto drive = static_cast<uint8_t>(driveHead & 0x03u);
    transferCylinder = command[2];
    transferHead = command[3];
    transferSector = command[4];
    transferSizeCode = command[5];
    const auto endSector = juce::jmax(transferSector, static_cast<int>(command[6]));
    const auto st0 = static_cast<uint8_t>((transferHead << 2) | drive);

    if (image.empty() || !validSector(transferCylinder, transferHead, transferSector,
                                      transferSizeCode))
    {
        enterResult({ static_cast<uint8_t>(0x40u | st0), 0x04, 0x00,
                      static_cast<uint8_t>(transferCylinder),
                      static_cast<uint8_t>(transferHead),
                      static_cast<uint8_t>(transferSector),
                      static_cast<uint8_t>(transferSizeCode) }, true);
        return;
    }
    if (write && !writable)
    {
        enterResult({ static_cast<uint8_t>(0x40u | st0), 0x02, 0x00,
                      static_cast<uint8_t>(transferCylinder),
                      static_cast<uint8_t>(transferHead),
                      static_cast<uint8_t>(transferSector),
                      static_cast<uint8_t>(transferSizeCode) }, true);
        return;
    }

    const auto sectorBytes = static_cast<size_t>(128u << transferSizeCode);
    const auto sectorCount = static_cast<size_t>(endSector - transferSector + 1);
    transferImageOffset = sectorOffset(transferCylinder, transferHead, transferSector);
    transfer.assign(sectorBytes * sectorCount, 0);
    if (!write)
    {
        const auto available = image.size() - std::min(transferImageOffset, image.size());
        const auto count = std::min(transfer.size(), available);
        std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(transferImageOffset), count,
                    transfer.begin());
    }
    transferPosition = 0;
    phase = write ? Phase::writeData : Phase::readData;
    command.clear();
}

void Dp8473::beginFormat() noexcept
{
    const auto driveHead = command[1];
    const auto drive = static_cast<uint8_t>(driveHead & 0x03u);
    transferCylinder = currentCylinder[drive];
    transferHead = static_cast<int>((driveHead >> 2u) & 0x01u);
    transferSizeCode = command[2];
    formatSectorCount = command[3];
    formatFill = command[5];
    if (image.empty() || !writable || formatSectorCount <= 0)
    {
        const auto st1 = static_cast<uint8_t>(writable ? 0x04u : 0x02u);
        enterResult({ static_cast<uint8_t>(0x40u
                                            | static_cast<unsigned int>(transferHead << 2)
                                            | drive), st1,
                      0x00, static_cast<uint8_t>(transferCylinder),
                      static_cast<uint8_t>(transferHead), 1,
                      static_cast<uint8_t>(transferSizeCode) }, true);
        return;
    }
    transfer.assign(static_cast<size_t>(formatSectorCount) * 4u, 0);
    transferPosition = 0;
    phase = Phase::formatData;
    command.clear();
}

void Dp8473::finishTransfer() noexcept
{
    if (phase == Phase::writeData)
    {
        const auto count = std::min(transferPosition,
                                    image.size() - std::min(transferImageOffset, image.size()));
        std::copy_n(transfer.begin(), count,
                    image.begin() + static_cast<std::ptrdiff_t>(transferImageOffset));
        if (count > 0)
            setDirty();
    }
    else if (phase == Phase::formatData)
    {
        for (int index = 0; index < formatSectorCount; ++index)
        {
            const auto header = static_cast<size_t>(index) * 4u;
            if (header + 3u >= transferPosition)
                break;
            const auto cylinder = transfer[header];
            const auto head = transfer[header + 1u];
            const auto sector = transfer[header + 2u];
            const auto sizeCode = transfer[header + 3u];
            if (!validSector(cylinder, head, sector, sizeCode))
                continue;
            const auto offset = sectorOffset(cylinder, head, sector);
            const auto count = static_cast<size_t>(128u << sizeCode);
            std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(offset), count,
                        formatFill);
            transferCylinder = cylinder;
            transferHead = head;
            transferSector = sector;
            transferSizeCode = sizeCode;
            setDirty();
        }
    }

    if (phase == Phase::readData || phase == Phase::writeData || phase == Phase::formatData)
    {
        const auto drive = static_cast<uint8_t>(driveControl & 0x03u);
        const auto sectorsTransferred = geometry.bytesPerSector > 0
                                            ? static_cast<int>(transferPosition
                                                               / static_cast<size_t>(
                                                                   geometry.bytesPerSector))
                                            : 0;
        const auto lastSector = juce::jlimit(
            1, juce::jmax(1, geometry.sectorsPerTrack),
            transferSector + juce::jmax(0, sectorsTransferred - 1));
        enterResult({ static_cast<uint8_t>((transferHead << 2) | drive), 0x00, 0x00,
                      static_cast<uint8_t>(transferCylinder),
                      static_cast<uint8_t>(transferHead),
                      static_cast<uint8_t>(lastSector),
                      static_cast<uint8_t>(transferSizeCode) }, true);
    }
}

void Dp8473::enterResult(std::initializer_list<uint8_t> bytes,
                         bool raiseInterrupt) noexcept
{
    enterResult(std::vector<uint8_t>(bytes), raiseInterrupt);
}

void Dp8473::enterResult(const std::vector<uint8_t>& bytes,
                         bool raiseInterrupt) noexcept
{
    result = bytes;
    resultPosition = 0;
    transfer.clear();
    transferPosition = 0;
    command.clear();
    phase = Phase::result;
    irq = irq || raiseInterrupt;
}

size_t Dp8473::sectorOffset(int cylinder, int head, int sector) const noexcept
{
    return (static_cast<size_t>(cylinder * geometry.heads + head)
            * static_cast<size_t>(geometry.sectorsPerTrack)
            + static_cast<size_t>(sector - 1))
           * static_cast<size_t>(geometry.bytesPerSector);
}

bool Dp8473::validSector(int cylinder, int head, int sector, int sizeCode) const noexcept
{
    if (cylinder < 0 || cylinder >= geometry.cylinders || head < 0
        || head >= geometry.heads || sector < 1 || sector > geometry.sectorsPerTrack
        || sizeCode < 0 || sizeCode > 6
        || (128 << sizeCode) != geometry.bytesPerSector)
        return false;
    const auto offset = sectorOffset(cylinder, head, sector);
    return offset + static_cast<size_t>(geometry.bytesPerSector) <= image.size();
}

void Dp8473::setDirty() noexcept
{
    dirty = true;
    ++revision;
    saveRequested.notify_one();
}

bool Dp8473::drqAsserted() const noexcept
{
    const std::scoped_lock lock(mutex);
    return phase == Phase::readData || phase == Phase::writeData
           || phase == Phase::formatData;
}

bool Dp8473::interruptPending() const noexcept
{
    const std::scoped_lock lock(mutex);
    return irq;
}

bool Dp8473::isMounted() const noexcept
{
    const std::scoped_lock lock(mutex);
    return !image.empty();
}

bool Dp8473::isWritable() const noexcept
{
    const std::scoped_lock lock(mutex);
    return writable;
}

bool Dp8473::isDirty() const noexcept
{
    const std::scoped_lock lock(mutex);
    return dirty;
}

uint64_t Dp8473::mediaBytesRead() const noexcept
{
    const std::scoped_lock lock(mutex);
    return bytesRead;
}

juce::File Dp8473::mountedFile() const
{
    const std::scoped_lock lock(mutex);
    return imagePath;
}

juce::String Dp8473::mountedDescription() const
{
    const std::scoped_lock lock(mutex);
    if (image.empty())
        return "No disk mounted";
    auto description = imagePath.getFileName() + " — "
                       + juce::String(geometry.cylinders) + " tracks, "
                       + juce::String(geometry.heads) + " sides, "
                       + juce::String(geometry.sectorsPerTrack) + " sectors";
    if (!writable)
        description += " (read-only)";
    if (dirty)
        description += " *";
    if (lastSaveError.isNotEmpty())
        description += " (save failed; retrying)";
    return description;
}
} // namespace wave::firmware
