#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <memory>

namespace wave::dsp
{
class Cem3387
{
public:
    struct StereoSample
    {
        float left = 0.0f;
        float right = 0.0f;
    };

    void prepare(double hostSampleRate, float voiceTolerance) noexcept;
    void reset() noexcept;
    void setControls(float cutoffHz, float resonance, float driveDb, float pan,
                     float circuitAge) noexcept;
    void setCutoffCalibrationCode(uint16_t code) noexcept;
    [[nodiscard]] StereoSample process(float input, float vcaLevel) noexcept;

    // Estimated input headroom, assuming full scale = 5 Vpp at the chip.
    // This behavioral voltage mapping is separate from ES2 digital mix wrap.
    [[nodiscard]] static float saturateVcfInput(float input) noexcept;

    // Exposed for mixed-signal regression tests. These are the voltages after
    // the modeled sample-and-hold settling, not the digital DAC targets.
    [[nodiscard]] float currentVcaCv() const noexcept { return vcaCv; }
    [[nodiscard]] float currentCutoffCv() const noexcept { return cutoffCv; }
    [[nodiscard]] uint16_t cutoffCalibrationCode() const noexcept
    {
        return cutoffTrimCode;
    }
    [[nodiscard]] float uncalibratedCutoffCvOffset() const noexcept
    {
        return uncalibratedCutoffOffset;
    }
    [[nodiscard]] float calibratedCutoffCvResidual() const noexcept
    {
        return cutoffCvOffset;
    }

private:
    static constexpr size_t cutoffTableSize = 4097;
    using CutoffCoefficientTable = std::array<float, cutoffTableSize>;
    [[nodiscard]] static std::shared_ptr<const CutoffCoefficientTable>
        coefficientTableForSampleRate(double sampleRate);
    [[nodiscard]] float runFilter(float input) noexcept;
    [[nodiscard]] static float quantiseCv(float normalised) noexcept;
    void updateControlVoltages(float vcaLevel) noexcept;
    void calibrateCutoff() noexcept;
    void applyCutoffCalibrationCode(uint16_t code, bool force = false) noexcept;

    double sampleRate = 88200.0;
    std::array<float, 4> integrators{};
    float coefficient = 0.1f;
    float resonanceAmount = 0.0f;
    float resonanceInputGain = 1.0f;
    float inputDrive = 1.0f;
    float transconductorScale = 26.0f;
    float panPosition = 0.0f;
    float targetCutoffCv = 0.0f;
    float targetResonanceCv = 0.0f;
    float targetPanCv = 0.5f;
    float targetVcaCv = 0.0f;
    float cutoffCv = 0.0f;
    float resonanceCv = 0.0f;
    float panCv = 0.5f;
    float vcaCv = 0.0f;
    float tolerance = 0.0f;
    float age = 0.0f;
    float previousInput = 0.0f;
    float couplingInput = 0.0f;
    float couplingOutput = 0.0f;
    float couplingCoefficient = 0.0f;
    float controlSlew = 1.0f;
    float leftPanGain = 0.70710678f;
    float rightPanGain = 0.70710678f;
    float lastCutoffInput = -1.0f;
    float lastResonanceInput = -1.0f;
    float lastDriveInput = -1.0f;
    float lastPanInput = -2.0f;
    float lastAgeInput = -1.0f;
    float lastVcaInput = -1.0f;
    float coefficientCutoffCv = -1.0f;
    float coefficientResonanceCv = -1.0f;
    float coefficientPanCv = -1.0f;
    float uncalibratedCutoffOffset = 0.0f;
    float cutoffCvOffset = 0.0f;
    uint16_t cutoffTrimCode = 0x0800u;
    bool cutoffCalibrationManuallySet = false;
    std::shared_ptr<const CutoffCoefficientTable> cutoffCoefficientTable;
    uint32_t noiseState = 0x13579bdfu;
    bool controlsInitialised = false;
};
} // namespace wave::dsp
