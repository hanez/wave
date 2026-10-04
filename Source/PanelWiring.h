#pragma once

#include "WaveParameters.h"

#include <array>
#include <cmath>

namespace wave::panel
{
// OS 1.700 contains the same serial codes used by the service manual's panel
// diagnostics.  These tables are the single wiring contract between the exact
// Figma coordinates and the emulated front-panel electronics.
struct Switch
{
    float x;
    float y;
    int diagnosticCode;
    int matrixIndex = -1;
    const char* name = "";
};

// OS 1.700 applies this private dispatch permutation after the panel scanner
// has produced a physical serial code. It is not a second set of matrix
// coordinates and must never be applied by the UI; BUTTON TEST's serial code
// is already the input consumed by the emulated panel electronics.
inline constexpr std::array<int, 87> buttonDispatchCode {
    10, 11, 29, 40, 41, 15, 35, 16, 20, 21, 28, 32, 42, 80, 34, 13,
    12, 22, 23, 31, 19, 8, 0, 9, 44, 1, 2, 3, 5, 6, 7, 46,
    67, 70, 68, 69, 65, 66, 71, 64, 47, 48, 49, 14, 62, 60, 51, 61,
    57, 56, 55, 58, 50, 54, 59, 53, 52, 72, 73, 26, 24, 30, 33, 36,
    38, 37, 27, 25, 100, 75, 17, 18, 74, 43, 100, 85, 79, 77, 45, 4,
    78, 81, 82, 83, 76, 86, 39
};

constexpr int matrixIndexForDiagnosticCode(int diagnosticCode) noexcept
{
    return diagnosticCode >= 0 && diagnosticCode < 128 ? diagnosticCode : -1;
}

constexpr int diagnosticCodeForMatrixIndex(int matrixIndex) noexcept
{
    return matrixIndex >= 0 && matrixIndex < 128 ? matrixIndex : -1;
}

constexpr Switch button(float x, float y, int diagnosticCode, const char* name = "") noexcept
{
    return { x, y, diagnosticCode,
             matrixIndexForDiagnosticCode(diagnosticCode), name };
}

constexpr Switch specialButton(float x, float y, int matrixIndex, const char* name = "") noexcept
{
    return { x, y, -1, matrixIndex, name };
}

constexpr int physicalMatrixIndex(const Switch& item) noexcept
{
    return item.matrixIndex >= 0
               ? item.matrixIndex
               : matrixIndexForDiagnosticCode(item.diagnosticCode);
}

inline constexpr std::array visibleSwitches {
    button(98.0f, 160.0f, 0, "Oscillator 1 Octave"),
    button(39.0f, 358.0f, 16, "Oscillator Link"),
    button(98.0f, 358.0f, 1, "Oscillator 2 Octave"),
    button(392.0f, 358.0f, 15, "Wave Link"),
    button(39.0f, 555.0f, 2, "LFO Select"),
    button(98.0f, 555.0f, 5, "LFO Shape"),
    button(154.0f, 551.0f, 7, "LFO Trigger"),
    button(392.0f, 555.0f, 14, "Wave Envelope Select"),
    button(786.0f, 555.0f, 20, "Knob Mode"),

    button(292.173f, 120.174f, 8, "Oscillator 1 Edit"),
    button(706.173f, 120.174f, 17, "Wave 1 Edit"),
    button(785.173f, 220.174f, 86, "Oscillator Mixer Edit"),
    button(292.173f, 318.174f, 9, "Oscillator 2 Edit"),
    button(706.173f, 318.174f, 18, "Wave 2 Edit"),
    button(292.173f, 514.174f, 10, "LFO Edit"),
    button(706.173f, 514.174f, 19, "Wave / Free Envelope Edit"),

    Switch { 903.0f, 139.0f, 24, -1, "Mute" },
    Switch { 959.0f, 139.0f, 78, -1, "Solo" },
    Switch { 1276.5f, 139.0f, 31, -1, "Group Edit" },
    Switch { 903.0f, 199.0f, 22, -1, "Instrument 1" },
    Switch { 959.0f, 199.0f, 25, -1, "Instrument 2" },
    Switch { 1014.0f, 199.0f, 26, -1, "Instrument 3" },
    Switch { 1070.0f, 199.0f, 27, -1, "Instrument 4" },
    Switch { 1125.0f, 199.0f, 79, -1, "Instrument 5" },
    Switch { 1181.0f, 199.0f, 28, -1, "Instrument 6" },
    Switch { 1236.0f, 199.0f, 29, -1, "Instrument 7" },
    Switch { 1292.0f, 199.0f, 30, -1, "Instrument 8" },
    Switch { 903.0f, 553.0f, 21, -1, "Page Left" },
    Switch { 942.0f, 553.0f, 23, -1, "Page Right" },
    specialButton(1078.0f, 553.0f, 71, "Cancel"),
    specialButton(1117.0f, 553.0f, 70, "OK"),
    Switch { 1253.0f, 553.0f, 69, -1, "Minus" },
    Switch { 1292.0f, 553.0f, 72, -1, "Plus" },

    Switch { 1402.5f, 139.0f, 38, -1, "Quick Edit" },
    Switch { 1402.5f, 198.0f, 33, -1, "Global Edit" },
    Switch { 1402.5f, 257.0f, 35, -1, "Sequencer" },
    Switch { 1402.5f, 316.0f, 34, -1, "Option" },
    Switch { 1402.5f, 376.0f, 32, -1, "Wave Edit" },
    Switch { 1402.5f, 435.0f, 37, -1, "External Edit" },
    Switch { 1402.5f, 494.0f, 36, -1, "Instrument Edit" },
    Switch { 1402.5f, 553.0f, 39, -1, "Performance" },

    Switch { 1484.0f, 237.0f, 43, -1, "Filter Type" },
    Switch { 1543.0f, 237.0f, 75, -1, "Filter Select" },
    Switch { 1859.173f, 195.174f, 60, -1, "Filter Edit" },
    Switch { 2152.173f, 195.174f, 67, -1, "Amplifier Edit" },
    Switch { 1800.173f, 354.174f, 59, -1, "Filter Envelope Edit" },
    Switch { 2152.173f, 354.174f, 66, -1, "Amplifier Envelope Edit" },
    Switch { 1956.173f, 509.174f, 61, -1, "Panning Edit" },

    Switch { 1484.0f, 435.0f, 42, -1, "Copy" },
    Switch { 1484.0f, 494.0f, 41, -1, "Compare / Undo" },
    Switch { 1484.0f, 553.0f, 40, -1, "Recall / Init" },
    Switch { 1543.0f, 553.0f, 44, -1, "Bank" },
    Switch { 1602.0f, 553.0f, 45, -1, "1__" },
    Switch { 1680.0f, 553.0f, 47, -1, "Hold" },
    Switch { 1602.0f, 435.0f, 48, -1, "7" },
    Switch { 1641.0f, 435.0f, 51, -1, "8" },
    Switch { 1680.0f, 435.0f, 54, -1, "9" },
    Switch { 1602.0f, 475.0f, 53, -1, "4" },
    Switch { 1641.0f, 475.0f, 50, -1, "5" },
    Switch { 1680.0f, 475.0f, 49, -1, "6" },
    Switch { 1602.0f, 513.0f, 46, -1, "1" },
    Switch { 1641.0f, 513.0f, 56, -1, "2" },
    Switch { 1680.0f, 513.0f, 55, -1, "3" },
    Switch { 1641.0f, 553.0f, 52, -1, "0" },
    Switch { 1788.0f, 492.0f, 58, -1, "Disk" },
    Switch { 1789.5f, 553.0f, 57, -1, "Store" },
    Switch { 2103.0f, 473.0f, 62, -1, "Aux" },
    Switch { 2162.0f, 473.0f, 64, -1, "Control Mixer / Comparator" },
    Switch { 2103.0f, 551.0f, 65, -1, "Control Delay / S&H" },
    Switch { 2162.0f, 551.0f, 63, -1, "Control Shaper / Ramp" },
};

inline const Switch* switchAt(float x, float y, float tolerance = 1.5f) noexcept
{
    for (const auto& item : visibleSwitches)
        if (std::abs(item.x - x) <= tolerance && std::abs(item.y - y) <= tolerance)
            return &item;
    return nullptr;
}

// These six controls are mounted on the keyboard/left-controller assembly,
// so their coordinates are in the full SVG rather than the upper panel's
// offset-local coordinate system. Their serials are the genuine BUTTON TEST
// values from OS 1.700.
inline constexpr std::array keyboardPanelSwitches {
    button(110.0f, 766.5f, 3, "Button 1"),
    button(170.0f, 766.5f, 4, "Button 2"),
    button(228.0f, 751.0f, 6, "Glide On/Off"),
    button(366.173f, 753.174f, 11, "Glide Edit"),
    button(288.0f, 849.0f, 12, "Octave Up"),
    button(288.0f, 927.0f, 73, "Octave Down"),
};

inline const Switch* keyboardPanelSwitchAt(float x, float y,
                                            float tolerance = 1.5f) noexcept
{
    for (const auto& item : keyboardPanelSwitches)
        if (std::abs(item.x - x) <= tolerance && std::abs(item.y - y) <= tolerance)
            return &item;
    return nullptr;
}

struct KeyboardControllerButton
{
    float x;
    float y;
    uint8_t asciiCode;
    const char* name;
};

// These controls live on the Wave keyboard assembly rather than the upper
// front-panel scanner. The values are the ASCII command bytes decoded by the
// genuine OS 1.700 keyboard dispatcher.
inline constexpr std::array keyboardControllerButtons {
    KeyboardControllerButton { 982.0f, 678.0f, 0x27u, "Rewind" },
    KeyboardControllerButton { 1021.0f, 678.0f, 0x0du, "Fast Forward" },
    KeyboardControllerButton { 1079.5f, 678.0f, 0x4cu, "Stop" },
    KeyboardControllerButton { 1145.5f, 678.0f, 0x4du, "Play" },
    KeyboardControllerButton { 1196.0f, 678.0f, 0x50u, "Locator In" },
    KeyboardControllerButton { 1261.0f, 678.0f, 0x54u, "Record" },
    KeyboardControllerButton { 1300.0f, 678.0f, 0x51u, "Locator Out" },
    KeyboardControllerButton { 1372.0f, 678.0f, 0x52u, "Shift" }
};

inline const KeyboardControllerButton* keyboardControllerButtonAt(
    float x, float y, float tolerance = 1.5f) noexcept
{
    for (const auto& item : keyboardControllerButtons)
        if (std::abs(item.x - x) <= tolerance && std::abs(item.y - y) <= tolerance)
            return &item;
    return nullptr;
}

// Display names share the same physical IDs as the original and compact
// layouts, so alternate artwork cannot change a button's tooltip identity.
inline const char* buttonName(int matrixIndex) noexcept
{
    for (const auto& item : visibleSwitches)
        if (physicalMatrixIndex(item) == matrixIndex)
            return item.name;
    for (const auto& item : keyboardPanelSwitches)
        if (physicalMatrixIndex(item) == matrixIndex)
            return item.name;
    return "";
}

inline const char* keyboardControllerButtonName(int asciiCode) noexcept
{
    for (const auto& item : keyboardControllerButtons)
        if (item.asciiCode == asciiCode)
            return item.name;
    return "";
}

struct Pot
{
    const char* parameterId;
    int diagnosticCode;
    int adcChannel;
};

// The diagnostic code is the zero-based serial number printed by the genuine
// POT test. The CPU board selects the corresponding analogue mux at code + 1.
inline constexpr std::array visiblePots {
    Pot { parameters::oscillatorDetune[0], 23, 24 },
    Pot { parameters::modulationAmount[parameters::osc1PitchMod1], 24, 25 },
    Pot { parameters::oscillatorSemitone[0], 19, 20 },
    Pot { parameters::modulationAmount[parameters::osc1PitchMod2], 29, 30 },
    Pot { parameters::wavePhase[0], 31, 32 },
    Pot { parameters::waveEnvelopeVelocity[0], 26, 27 },
    Pot { parameters::modulationAmount[parameters::wave1Mod1], 22, 23 },
    Pot { parameters::position, 28, 29 },
    Pot { parameters::scan, 27, 28 },
    Pot { parameters::waveKeytrack[0], 25, 26 },
    Pot { parameters::modulationAmount[parameters::wave1Mod2], 9, 10 },
    Pot { parameters::waveLevel[0], 11, 12 },

    Pot { parameters::oscillatorDetune[1], 17, 18 },
    Pot { parameters::modulationAmount[parameters::osc2PitchMod1], 18, 19 },
    Pot { parameters::oscillatorSemitone[1], 30, 31 },
    Pot { parameters::modulationAmount[parameters::osc2PitchMod2], 16, 17 },
    Pot { parameters::wavePhase[1], 8, 9 },
    Pot { parameters::waveEnvelopeVelocity[1], 2, 3 },
    Pot { parameters::modulationAmount[parameters::wave2Mod1], 14, 15 },
    Pot { parameters::position2, 13, 14 },
    Pot { parameters::scan2, 0, 1 },
    Pot { parameters::waveKeytrack[1], 5, 6 },
    Pot { parameters::modulationAmount[parameters::wave2Mod2], 1, 2 },
    Pot { parameters::waveLevel[1], 15, 16 },
    Pot { parameters::noise, 3, 4 },

    Pot { parameters::lfoRate[0], 21, 22 },
    Pot { parameters::modulationAmount[parameters::lfo1RateMod], 4, 5 },
    Pot { parameters::modulationAmount[parameters::lfo1LevelMod], 6, 7 },
    Pot { parameters::lfoRate[1], 21, 22 },
    Pot { parameters::modulationAmount[parameters::lfo2RateMod], 4, 5 },
    Pot { parameters::modulationAmount[parameters::lfo2LevelMod], 6, 7 },

    Pot { parameters::resonance, 61, 62 },
    Pot { parameters::filterVelocity, 49, 50 },
    Pot { parameters::modulationAmount[parameters::resonanceMod], 54, 55 },
    Pot { parameters::modulationAmount[parameters::filterMod1], 46, 47 },
    Pot { parameters::cutoff, 62, 63 },
    Pot { parameters::filterEnv, 51, 52 },
    Pot { parameters::filterKeytrack, 55, 56 },
    Pot { parameters::modulationAmount[parameters::filterMod2], 43, 44 },

    Pot { parameters::highpassVelocity, 35, 36 },
    Pot { parameters::highpassEnvelopeAmount, 38, 39 },
    Pot { parameters::highpassKeytrack, 36, 37 },
    Pot { parameters::modulationAmount[parameters::highpassMod1], 32, 33 },
    Pot { parameters::modulationAmount[parameters::highpassMod2], 37, 38 },

    Pot { parameters::filterDelay, 63, 64 },
    Pot { parameters::filterAttack, 60, 61 },
    Pot { parameters::filterDecay, 50, 51 },
    Pot { parameters::filterSustain, 48, 49 },
    Pot { parameters::filterRelease, 53, 54 },
    Pot { parameters::attack, 40, 41 },
    Pot { parameters::decay, 45, 46 },
    Pot { parameters::sustain, 44, 45 },
    Pot { parameters::release, 34, 35 },
    Pot { parameters::modulationAmount[parameters::panMod1], 52, 53 },
    Pot { parameters::modulationAmount[parameters::panMod2], 42, 43 },
};

inline constexpr std::array<int, 8> performanceFaderAdcChannels {
    40, 34, 48, 60, 57, 42, 58, 59
};

// UI encoder order: Time 1-4, Level 1-4, then Wavetable/Data.
inline constexpr std::array<int, 9> encoderSerialCodes {
    12, 13, 14, 15, 11, 10, 9, 8, 0
};

struct Led
{
    float x;
    float y;
    int redSerialCode;
    int greenSerialCode = -1;
};

struct EditIndicator
{
    int buttonDiagnosticCode;
    int ledSerialCode;
};

// Each Edit-page switch selects one mutually exclusive firmware overlay. Keep
// the input/output relationship in the physical wiring contract so the
// processor cannot independently assign two lamps to one selected section.
inline constexpr std::array editIndicators {
    EditIndicator { 8, 15 },  EditIndicator { 17, 80 },
    EditIndicator { 86, 95 }, EditIndicator { 60, 68 },
    EditIndicator { 67, 77 }, EditIndicator { 9, 47 },
    EditIndicator { 18, 32 }, EditIndicator { 59, 29 },
    EditIndicator { 66, 61 }, EditIndicator { 10, 18 },
    EditIndicator { 19, 48 }, EditIndicator { 61, 20 },
    EditIndicator { 62, 30 }, EditIndicator { 64, 14 },
    EditIndicator { 65, 4 },  EditIndicator { 63, 78 },
    EditIndicator { 11, 75 }, // Keyboard-panel Glide Edit
};

// LED order is the physical top-to-bottom +2, +1, 0, -1, -2 sequence.
inline constexpr std::array<std::array<int, 5>, 2>
    oscillatorOctaveLedSerialCodes {{
        {{ 9, 73, 25, 1, 65 }},
        {{ 12, 28, 76, 44, 60 }}
    }};

inline constexpr std::array visibleLeds {
    Led { 97.0f, 82.0f, 9 }, Led { 97.0f, 94.0f, 73 },
    Led { 97.0f, 107.0f, -1, 25 }, Led { 97.0f, 119.0f, 1 },
    Led { 97.0f, 132.0f, 65 }, Led { 324.0f, 91.0f, 15 },
    Led { 735.0f, 91.0f, 80 }, Led { 814.0f, 189.0f, 95 },
    Led { 38.0f, 328.0f, 33 },
    Led { 97.0f, 278.0f, 12 }, Led { 97.0f, 290.0f, 28 },
    Led { 97.0f, 303.0f, -1, 76 }, Led { 97.0f, 315.0f, 44 },
    Led { 97.0f, 328.0f, 60 }, Led { 324.0f, 288.0f, 47 },
    Led { 391.0f, 329.0f, 63 }, Led { 735.0f, 288.0f, 32 },
    Led { 38.0f, 513.0f, 24 }, Led { 38.0f, 526.0f, 8 },
    Led { 97.0f, 463.0f, 49 }, Led { 97.0f, 476.0f, 2 },
    Led { 97.0f, 488.0f, 66 }, Led { 97.0f, 501.0f, 72 },
    Led { 97.0f, 513.0f, 19 }, Led { 97.0f, 526.0f, 67 },
    Led { 156.0f, 513.0f, 17 }, Led { 156.0f, 526.0f, 56 },
    Led { 324.0f, 485.0f, 18 }, Led { 391.0f, 475.0f, 34 },
    Led { 391.0f, 501.0f, 40 }, Led { 391.0f, 513.0f, 23 },
    Led { 391.0f, 525.0f, 39 }, Led { 735.0f, 484.0f, 48 },
    Led { 784.0f, 488.0f, 6 }, Led { 784.0f, 501.0f, 70 },
    Led { 784.0f, 513.0f, 71 }, Led { 784.0f, 526.0f, 7 },

    Led { 903.0f, 111.0f, 43 }, Led { 959.0f, 111.0f, 59 },
    Led { 1279.0f, 112.0f, 54 },
    Led { 903.0f, 171.0f, 97, 96 }, Led { 959.0f, 171.0f, 99, 98 },
    Led { 1014.0f, 171.0f, 101, 100 }, Led { 1070.0f, 171.0f, 103, 102 },
    Led { 1125.0f, 171.0f, 105, 104 }, Led { 1181.0f, 171.0f, 107, 106 },
    Led { 1236.0f, 171.0f, 109, 108 }, Led { 1292.0f, 171.0f, 111, 110 },
    Led { 1362.0f, 139.0f, 64 }, Led { 1362.0f, 198.0f, 69 },
    Led { 1362.0f, 257.0f, 21 }, Led { 1362.0f, 316.0f, 74 },
    Led { 1362.0f, 376.0f, 26 }, Led { 1362.0f, 435.0f, 10 },
    Led { 1362.0f, 494.0f, 22 }, Led { 1362.0f, 553.0f, 51 },
    Led { 1078.0f, 524.0f, 3 }, Led { 1117.0f, 524.0f, 82 },

    Led { 1485.0f, 172.0f, 45 }, Led { 1485.0f, 185.0f, 37 },
    Led { 1485.0f, 197.0f, 5 }, Led { 1485.0f, 210.0f, 0 },
    Led { 1543.0f, 197.0f, 53 }, Led { 1543.0f, 210.0f, 41 },
    Led { 1888.0f, 168.0f, 68 }, Led { 2181.0f, 168.0f, 77 },
    Led { 1829.0f, 325.0f, 29 }, Led { 2181.0f, 325.0f, 61 },
    Led { 1985.0f, 482.0f, 20 },
    Led { 1484.0f, 407.0f, 83 }, Led { 1484.0f, 466.0f, 62 },
    Led { 1484.0f, 525.0f, 46 }, Led { 1544.0f, 512.0f, 85 },
    Led { 1544.0f, 526.0f, 84 }, Led { 1709.0f, 552.0f, 88 },
    Led { 1788.0f, 465.0f, 52 }, Led { 1788.0f, 525.0f, 87 },
    Led { 2103.0f, 444.0f, 30 }, Led { 2162.0f, 444.0f, 14 },
    Led { 2103.0f, 525.0f, 4 }, Led { 2162.0f, 525.0f, 78 },
};

inline const Led* ledAt(float x, float y, float tolerance = 1.0f) noexcept
{
    for (const auto& item : visibleLeds)
        if (std::abs(item.x - x) <= tolerance && std::abs(item.y - y) <= tolerance)
            return &item;
    return nullptr;
}

// LED TEST serials on the lower keyboard panel. The octave outputs were
// confirmed against OS 1.700's keyboard-controller response: Up=58 and
// Down=27. Their latched state is also the keyboard transposition feedback.
inline constexpr std::array keyboardPanelLeds {
    Led { 110.0f, 724.0f, 16 },
    Led { 170.0f, 724.0f, 35 },
    Led { 228.0f, 724.0f, 11 },
    Led { 288.0f, 821.0f, 58 },
    Led { 288.0f, 900.0f, 27 },
    Led { 396.0f, 724.0f, 75 },
};

inline const Led* keyboardPanelLedAt(float x, float y,
                                     float tolerance = 1.0f) noexcept
{
    for (const auto& item : keyboardPanelLeds)
        if (std::abs(item.x - x) <= tolerance && std::abs(item.y - y) <= tolerance)
            return &item;
    return nullptr;
}
} // namespace wave::panel
