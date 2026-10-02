#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace wave::firmware
{
// National Semiconductor DP8473 as wired on Wave CPU-board sheet 7.  The
// controller is software-compatible with the uPD765, but the Wave also uses
// its integrated drive-control/data-rate registers and DMA/terminal-count
// aliases.
class Dp8473 final
{
public:
    Dp8473() = default;
    ~Dp8473();

    juce::Result mount(const juce::File& imageFile);
    juce::Result flush();
    juce::Result eject();
    void resetController() noexcept;

    [[nodiscard]] uint8_t read(uint32_t cpuAddress) noexcept;
    void write(uint32_t cpuAddress, uint8_t value) noexcept;

    [[nodiscard]] bool drqAsserted() const noexcept;
    [[nodiscard]] bool interruptPending() const noexcept;
    [[nodiscard]] bool isMounted() const noexcept;
    [[nodiscard]] bool isWritable() const noexcept;
    [[nodiscard]] bool isDirty() const noexcept;
    [[nodiscard]] uint64_t mediaBytesRead() const noexcept;
    [[nodiscard]] juce::File mountedFile() const;
    [[nodiscard]] juce::String mountedDescription() const;
    [[nodiscard]] juce::MemoryBlock mountedImageSnapshot() const;

private:
    enum class Phase
    {
        idle,
        command,
        readData,
        writeData,
        formatData,
        result
    };

    struct Geometry
    {
        int bytesPerSector = 512;
        int sectorsPerTrack = 0;
        int heads = 0;
        int cylinders = 0;
    };

    [[nodiscard]] static bool geometryForImage(const std::vector<uint8_t>& bytes,
                                               Geometry& result) noexcept;
    [[nodiscard]] static int commandLength(uint8_t opcode) noexcept;
    void acceptCommandByte(uint8_t value) noexcept;
    void executeCommand() noexcept;
    void beginReadOrWrite(bool write) noexcept;
    void beginFormat() noexcept;
    void finishTransfer() noexcept;
    void enterResult(std::initializer_list<uint8_t> bytes, bool raiseInterrupt) noexcept;
    void enterResult(const std::vector<uint8_t>& bytes, bool raiseInterrupt) noexcept;
    [[nodiscard]] size_t sectorOffset(int cylinder, int head, int sector) const noexcept;
    [[nodiscard]] bool validSector(int cylinder, int head, int sector,
                                   int sizeCode) const noexcept;
    [[nodiscard]] uint8_t mainStatus() const noexcept;
    [[nodiscard]] uint8_t readDataRegister(bool dma, bool terminalCount) noexcept;
    void writeDataRegister(uint8_t value, bool dma) noexcept;
    void setDirty() noexcept;
    juce::Result flushWithSaveLock();
    void saveLoop();

    mutable std::mutex mutex;
    // Serialize host file writes and media changes independently of the
    // controller lock: audio only touches the in-memory image.
    std::mutex saveMutex;
    std::condition_variable saveRequested;
    std::thread saveThread;
    bool stopping = false;
    juce::String lastSaveError;
    juce::File imagePath;
    std::vector<uint8_t> image;
    Geometry geometry;
    Phase phase = Phase::idle;
    std::vector<uint8_t> command;
    std::vector<uint8_t> transfer;
    std::vector<uint8_t> result;
    size_t transferPosition = 0;
    size_t resultPosition = 0;
    size_t transferImageOffset = 0;
    int transferCylinder = 0;
    int transferHead = 0;
    int transferSector = 1;
    int transferSizeCode = 2;
    int formatSectorCount = 0;
    uint8_t formatFill = 0;
    std::array<uint8_t, 4> currentCylinder{};
    uint8_t driveControl = 0;
    uint8_t dataRate = 2;
    uint8_t pendingSenseDrive = 0;
    bool pendingSense = false;
    bool irq = false;
    bool diskChanged = true;
    bool writable = false;
    bool dirty = false;
    uint64_t bytesRead = 0;
    uint64_t revision = 0;
};
} // namespace wave::firmware
