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

This is a behavioral model with a corrected analogue small-signal response,
not a transistor-level CEM3387 simulation. Per-stage saturation, thermal noise,
control settling, and the resonance gain law remain approximations. Additional
Wave captures across cutoff, resonance, input level, and voice cards are needed
to establish the full nonlinear hardware response.
