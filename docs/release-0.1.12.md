# Wave Emulation 0.1.12

This release improves disk loading, saved Sounds, Performance browsing, voice allocation, and the lower keyboard controls.

- Generates all 64 original factory wavetables from the user's authenticated Wave OS 1.700 firmware. Factory tables remain intact when loading user banks or restoring projects.
- Corrects native ROM and user-wave references and interpolation in imported wavetables. Invalid individual tables and tuning records fall back safely without changing the source image.
- Preserves the audible wavetable selection through Sound Store, Performance recall, and DAW reopening, including the reported Sitar table mismatch.
- Fixes LCD freezes during disk/Total Recall operations and rapid Performance browsing. The LCD and mode lamps remain synchronized after Store. Cancel distinguishes normal mode exits from Total Recall calibration prompts.
- Implements native Instrument voice-allocation settings, including fixed polyphony and mono note-priority/retrigger modes, and corrects Instrument Edit and Glide Edit fader routing.
- Connects Button 1 and Button 2 to their dedicated modulation sources, with Performance touch/toggle modes and MIDI assignments. Handles quick clicks, dropped firmware events, and audio restarts. Makes keyboard Shift delivery reliable for native service controls.
- Adds System → Create Disk Image from WTB for native Wave wavetable files.
- Flushes emulated disk writes to the mounted image automatically. Fresh installations start with a blank disk; standalone sessions and DAW projects restore their disk and bank state. Cold starts keep the cleared playback bank empty after firmware initialization.
- Retains the full internal oscillator sampling rate while optimizing voice rendering; the reduced-rate Eco mode is removed.

macOS builds target macOS 13.0 and later and contain universal Apple Silicon and Intel standalone, AU, VST3, and AAX builds. Windows builds contain the x64 standalone EXE and VST3, with the runtime linked statically.

Firmware, ROM dumps, factory Sound SETs, and disk images are not included. Supply your own instrument data; the README links to the original sources.

Store saves into the Wave's internal bank. Use the Wave's Disk Save controls to write the bank to the mounted image; the image is then flushed to disk automatically. Project-restored disk snapshots are isolated from the original file and can be exported with Save Mounted Disk Image As.

The project owner confirmed DAW testing and approved publication of this version. Windows execution and plugin validation under CrossOver do not replace checks of native Windows audio/MIDI drivers.
