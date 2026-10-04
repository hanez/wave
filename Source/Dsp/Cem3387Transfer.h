#pragma once

#include <cmath>

namespace wave::dsp::cem3387
{
// Educated behavioral calibration, not measured Wave pin voltages. Full-scale
// peak 1 is assumed to be 2.5 V (5 Vpp, the datasheet's typical 0.1% THD point).
// The weak OTA curve approximates linearised transconductors; its scale is not
// an internal voltage rail. Only input/output headroom has a pin-voltage mapping.
inline constexpr auto nominalPeakVolts = 2.5f;
inline constexpr auto linearityScale = 26.0f;
inline constexpr auto inputHeadroom = 3.25f / nominalPeakVolts;
inline constexpr auto outputHeadroom = 3.5f / nominalPeakVolts;

struct Transfer
{
    float value;
    float derivative;
};

[[nodiscard]] inline Transfer linearisedTransconductor(
    float input, float scale = linearityScale) noexcept
{
    const auto argument = input / scale;
    // Linearised stages spend almost all their time in this small interval.
    // Through x^9 the tanh series is accurate to float precision here; use
    // the library curve for larger overloads rather than extending the series.
    auto unit = 0.0f;
    if (std::abs(argument) <= 0.25f)
    {
        const auto squared = argument * argument;
        unit = argument * (1.0f + squared * (-1.0f / 3.0f
            + squared * (2.0f / 15.0f + squared * (-17.0f / 315.0f
            + squared * (62.0f / 2835.0f)))));
    }
    else
        unit = std::tanh(argument);
    return { scale * unit, 1.0f - unit * unit };
}

// Smooth, symmetric headroom shoulder. The 16th-power shape keeps the nominal
// signal nearly linear while approaching the rail without a hard clip.
[[nodiscard]] inline Transfer limitHeadroom(float input, float rail) noexcept
{
    const auto unit = input / rail;
    if (std::abs(unit) >= 16.0f)
        return { std::copysign(rail, input), 0.0f };
    const auto squared = unit * unit;
    const auto fourth = squared * squared;
    const auto eighth = fourth * fourth;
    const auto shoulder = eighth * eighth;
    if (shoulder <= 0.01f)
    {
        // Binomial series avoids four square roots below the rail shoulder.
        // Value and slope agree with the full curve to float precision.
        const auto gain = 1.0f + shoulder * (-1.0f / 16.0f
            + shoulder * (17.0f / 512.0f - shoulder * (187.0f / 8192.0f)));
        const auto slope = 1.0f + shoulder * (-17.0f / 16.0f
            + shoulder * (561.0f / 512.0f - shoulder * (9163.0f / 8192.0f)));
        return { input * gain, slope };
    }
    const auto term = 1.0f + shoulder;
    const auto divisor = std::sqrt(std::sqrt(std::sqrt(std::sqrt(term))));
    return { input / divisor, 1.0f / (term * divisor) };
}
} // namespace wave::dsp::cem3387
