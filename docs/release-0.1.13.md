# Wave Emulation 0.1.13

- New instances mount a private, writable copy of the bundled Blank Wave.img and automatically load its INIT SET. Instrument 1 is active and selected, so the knobs have a Sound to edit after system firmware loads. Saved sessions restore their own bank and disk.
- Includes Blank Wave.img in the macOS installer and at the DMG root. Each bank contains INIT Sounds and MULTI INIT Performances; no factory sound library or executable firmware is bundled.
- Fixes the first knob gesture after firmware startup by reading the current physical pot position at the start of a drag.
- Adds the compact panel skin, with repositioned controls, remembered selection and the same live firmware LCD.
- Corrects front-panel switch wiring and numeric keypad delivery, including rapid presses and firmware acknowledgement.
- Incorporates firmware-derived oscillator, modulation, voice-control and Instrument audio-output routing corrections.
- Refines the CEM3387 transfer model and nonlinear filter behavior. The filter model remains an approximation; its measurements and validation are documented in docs/cem3387-filter.md.

The binary download is one macOS DMG, targeting macOS 13.0 or later. Its installer contains universal Apple Silicon and Intel standalone, AU, VST3 and PACE-wrapped AAX formats. Dependency notices are inside the DMG’s License and Source Information folder. The corresponding source ZIP is available as a separate release download.

Supply your own supported Wave OS 1.700 firmware. The bundled blank disk provides an initialized working bank; other sound libraries and wavetables can be loaded through the existing Disk controls.

Store writes to the Wave's internal bank. Use the Disk Save controls to write the bank to the mounted image, which is flushed automatically. Project-restored disks remain private copies and can be exported with Save Mounted Disk Image As.
