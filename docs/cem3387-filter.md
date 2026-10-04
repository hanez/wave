# CEM3387 filter model

The Curtis CEM3387 contains a three-pole reconstruction filter followed by a
separate four-pole resonant low-pass. The reconstruction section is modeled in
`MixedSignalPath`; resonance acts only around the four-pole section in `Cem3387`.

The [Curtis datasheet](https://www.alldatasheet.com/datasheet-pdf/pdf/1157738/CES/CEM3387.html)
(pages 5–6) gives the second-order section transfer function and specifies
`Ca = 4 Cb` for the classical musical low-pass response. Two such sections give
`H(s) = 1 / (1 + s/wc)^4`. Four equal trapezoidal one-poles implement that
equivalent small-signal response. The nonlinear feedback equation is solved
within each circuit step, before committing the integrator states. Feedback
comes from the current fourth-stage output. Using the previous integrator state
introduces digital phase delay, changes resonance with sample rate, and causes
premature self-oscillation at high cutoff.

Drive scales the incoming signal, leaving feedback gain unchanged. The
normalized resonance control represents the Wave's increasing panel control;
it is not a literal voltage at pin 13. The datasheet specifies decreasing chip
CV for increasing resonance (typically 4.5 V for no feedback and 1 V for
sustained oscillation). The panel-to-chip voltage mapping and nonlinear gain
law still need hardware measurements. The current feedback range is 0–4.15,
slightly exceeding the ideal four-pole oscillation threshold of 4.

The existing Wave calibration references are approximately 28 Hz, 957 Hz, and
7779 Hz at Sound cutoff values 0, 62, and 100. The model interpolates their
frequency scale in log frequency, independently of host sample rate. Beyond
the measured range it holds the nearest scale; that extrapolation remains an
assumption. The measured passband-loss approximation remains in the input path;
the additional high-resonance makeup factor used by the delayed loop has been
removed.

`WaveCoreTests` checks the classical small-signal resonant response, existing
Wave passband loss and oscillation references, output stability, and resonance
behavior at 44.1, 48, and 96 kHz with 0 and 18 dB drive. Below-threshold
excitation must decay, maximum resonance must sustain oscillation, and its
frequency must remain consistent across sample rates and drive settings.

This is a behavioral model with a datasheet-guided nonlinear estimate, not a
transistor-level CEM3387 simulation. No physical Wave was available to validate
distortion, oscillation amplitude or the signal-voltage mapping. Thermal noise,
VCA/control settling and the resonance gain law remain approximations.

## Educated nonlinear estimate (2026-10-04)

The [original Curtis datasheet](https://www.synfo.nl/datasheets/CEM3387.pdf),
page 3, specifies typical filter THD of 0.1% at 5.0 V peak-to-peak input with
+12 V/-5 V supplies at 20 C, and a typical maximum VCF output of +/-3.5 V. Page 5
gives a signal-handling expression of `(7/12)*Vcc - 0.5 V`, or 6.5 V for a
12 V supply. The latter does not clearly identify peak or peak-to-peak units.

The working assumptions are:

- DSP input peak 1 represents 2.5 V peak, or 5 Vpp, before the VCF. This is an
  assumed scale, not a measured property of the Wave voice card.
- Interpret the 6.5 V input-handling figure as peak-to-peak: estimated input
  headroom is +/-3.25 V, or +/-1.3 DSP units. Output headroom is +/-3.5 V, or
  +/-1.4 DSP units. A smooth 16th-power shoulder approaches each limit.
- Weak symmetric transconductor curves replace the old strongly compressing
  unity-scale tanh stages. Their scale of 26 DSP units is a behavioral curve
  parameter, not a physical transistor voltage or internal supply rail.
  The same weak curve replaces excessive reconstruction-stage compression.
- VCF output limiting operates inside the instantaneous feedback loop. It
  controls autonomous oscillation amplitude without a second saturator after
  the filter. Drive multiplies input before the input headroom limit.
- Cutoff calibration, passband loss, feedback range 0–4.15, CV quantization,
  250 kHz oscillator sampling and eight-bit DAC/mixer behavior are retained.
  Digital wrap/quantization distortion is separate from analog chip THD.

The nominal VCF distortion target is approximately 0.1%, with gently rising
distortion at lower levels and a stronger shoulder under overload. Symmetric
curves favor odd harmonics; actual even harmonics, resonance-dependent
distortion and surrounding VCA/voice-card loading remain unmeasured. Aging
slightly strengthens the weak curves without an arbitrary post-filter gain.
The less compressed signal can be louder than the previous approximation,
especially with resonance or drive; self-oscillation is substantially louder.

Small-argument polynomial evaluation agrees with the exact transfer curves
to float precision, with the full curves used outside those intervals.
Reconstruction transfers remain cached per DAC code. Regression tests check
transfer accuracy, nominal/overdriven THD, bounded noise-seeded oscillation,
small-signal response, and frequency tracking across sample rates.

On this development Mac, three paired 48-voice benchmarks (48 kHz, 512-sample
blocks, resonance 0.7, three seconds of audio) gave median serial DSP CPU time
of 3.214 s before and 2.937 s after, approximately 9% lower. This measures
the isolated engine rather than firmware or GUI cost; other patches, drive
levels, polyphony and hardware can produce different results.

## Offline SPICE and DSP comparison

The neighboring `bb830` project now has an offline ngspice reference in
`spice/cem3387-vcf-linear.cir`. Its existing sample-based simulator is not a
SPICE solver, and its CEM3320 component is not a CEM3387 substitute. The deck
implements only the datasheet's equivalent small-signal four-pole response,
using buffered RC poles and instantaneous feedback. It excludes nonlinear
chip behavior, reconstruction, VCAs, panning and the panel-to-chip CV law.
Its AC result was checked against `H/(1 + K*H)` with
`H = (1 + j*f/fc)^-4`: maximum gain error over 10 Hz–100 kHz was less than
0.000001 dB at fc = 957 Hz and K = 3. This validates the reference circuit,
not the nonlinear hardware model.

`spice/cem3387-vcf-estimate.cir` adds the assumed weak curves and headroom,
using physical volts with the assumed 2.5 V-per-DSP-unit scale. Its buffered
RC poles are an equivalent behavioral circuit, not the chip's OTA topology.
It excludes reconstruction, VCA/pan, output coupling, noise and CV settling.
Running the same nonlinear assumptions in ngspice checks discretization and
solver behavior; it provides no independent evidence of hardware accuracy.

At cutoff 7779 Hz, zero resonance and 100 Hz/5 Vpp input, ngspice gives
0.08848% H2–H10 THD with a 2 us timestep. Halving the timestep to 1 us changes
this to 0.08848%. The DSP result is 0.08858% at 48 kHz. The small difference
includes digital sampling and the DSP's output coupling. At fc = 957 Hz,
K = 4.15 and zero input after a small startup pulse, SPICE sustains oscillation
at 957.00 Hz with 2.4099 V RMS (2 us timestep). At 1 us it gives 957.01 Hz
and 2.4099 V RMS. The DSP's centered-channel RMS of 0.6815 corresponds to
about 2.4095 V RMS before pan under the assumed voltage mapping.

Build the ROM-free trace tool with
`cmake --build build --target ProbeWaveFilter`. For example:

```sh
build/ProbeWaveFilter 48000 2 100 0 100 0.1 0 > filter.csv
```

Arguments are sample rate, duration in seconds, cutoff code, resonance code,
input sine frequency, input peak and drive dB. The output CSV contains time,
input and stereo output samples. Zero input peak tests oscillation seeded
solely by the model's internal noise. The trace includes the current Cem3387
filter, input headroom, VCA and output coupling, but excludes the separate
reconstruction stage and the rest of the synth. Input amplitude is in DSP
units; peak 1 is estimated as 5 Vpp input. That physical scale is unverified.

### Characterization of the estimate

At zero drive, zero age/tolerance, centered pan and open VCA, zero-input
maximum resonance sustains oscillation at all three tested sample rates:

| Cutoff code | 44.1 kHz | 48 kHz | 96 kHz |
| --- | ---: | ---: | ---: |
| 0 | 28.00 Hz | 28.00 Hz | 28.00 Hz |
| 62 | 956.78 Hz | 956.80 Hz | 956.78 Hz |
| 100 | 7773.76 Hz | 7773.81 Hz | 7773.81 Hz |

Measurement uses positive-going crossings in the final second after 3 seconds
of silence (14 seconds for cutoff 0). At cutoff 62, centered left-channel
RMS is 0.6815 at 44.1/48 kHz and 0.6816 at 96 kHz. These amplitudes follow
the estimated output rail; the real Wave's onset and amplitude need comparison.

A 100 Hz sine at cutoff 100, zero resonance and 48 kHz produces these H2–H10
THD values, measured by harmonic projections over the final full second of a
2-second run:

| Input peak (DSP units) | Previous model THD | Estimated model THD |
| ---: | ---: | ---: |
| 0.10 | 0.454% | 0.00049% |
| 0.70 | 12.32% | 0.02424% |
| 1.00 | 17.79% | 0.08858% |

At nominal peak 1, THD is 0.08858% at both 44.1 and 96 kHz as well. These
figures characterize the behavioral DSP, not measured Wave hardware. The
datasheet's single specification point does not uniquely identify a nonlinear
curve, test spectrum, full-scale mapping or self-oscillation amplitude. Future
measurements should constrain H2–H10 versus level/cutoff/resonance, autonomous
oscillation onset/amplitude, and physical pin voltages before claiming a
hardware-calibrated nonlinear response.
