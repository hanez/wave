#include "Dsp/Cem3387.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

// Offline, ROM-free trace for comparison with SPICE or hardware captures.
// Amplitudes are DSP units. Peak 1 is estimated as 5 Vpp input, not measured.
int main(int argc, char** argv)
{
    if (argc != 8)
    {
        std::cerr << "Usage: ProbeWaveFilter sampleRate seconds cutoffCode resonanceCode inputHz inputPeak driveDb\n"
                     "Cutoff/resonance codes: 0..127. Zero inputPeak tests noise-seeded oscillation.\n";
        return 1;
    }
    try
    {
        const auto number = [&](int index) {
            size_t end = 0;
            const auto value = std::stod(argv[index], &end);
            if (argv[index][end] != '\0' || !std::isfinite(value))
                throw std::runtime_error("Arguments must be finite numbers");
            return value;
        };
        const auto rate = number(1), seconds = number(2);
        const auto cutoff = number(3), resonance = number(4);
        const auto hz = number(5), peak = number(6), drive = number(7);
        if (rate < 8000 || rate > 384000 || seconds <= 0 || seconds > 60
            || cutoff < 0 || cutoff > 127 || resonance < 0 || resonance > 127
            || hz < 0 || hz >= rate * 0.5 || peak < 0 || peak > 8
            || drive < 0 || drive > 18)
            throw std::runtime_error("Argument outside supported range");
        wave::dsp::Cem3387 filter;
        filter.prepare(rate, 0.0f);
        filter.setControls(static_cast<float>(20.0 * std::exp2(cutoff / 12.0)),
                           static_cast<float>(resonance / 127.0),
                           static_cast<float>(drive), 0.0f, 0.0f);
        std::cout << "time_s,input,left,right\n" << std::setprecision(10);
        const auto count = static_cast<int64_t>(std::round(rate * seconds));
        for (int64_t i = 0; i < count; ++i)
        {
            const auto time = static_cast<double>(i) / rate;
            const auto input = static_cast<float>(peak * std::sin(6.283185307179586 * hz * time));
            const auto output = filter.process(input, 1.0f);
            if (!std::isfinite(output.left) || !std::isfinite(output.right))
                throw std::runtime_error("Filter produced a non-finite sample");
            std::cout << time << ',' << input << ',' << output.left << ',' << output.right << '\n';
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
