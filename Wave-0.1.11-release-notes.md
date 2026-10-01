Wave Emulation 0.1.11 fixes disk loading, Store controls, and bank restoration when reopening.

- Restores the complete stored Performance and Sound banks, including Store changes, when reopening the standalone app or recalling host state.
- Accepts native Store names containing NUL bytes so a saved bank is not rejected in favour of an older bank.
- Returns Total Recall to Performance correctly when cancelling the optional machine-specific voice and filter adjustment data.
- Shows the Performance lamp after saving a Performance and the Instrument Edit lamp after saving a Sound.
- Prevents the + button from repeating after Store, including rapid Performance browsing clicks.
- Flushes pending disk writes before remounting the same image.

Store updates the Wave's internal banks. Use Wave Disk → Save to write those banks to the mounted disk, then System → Save Mounted Disk Image to flush the image to disk.

The macOS installer targets macOS 13.0 and later and contains universal Apple Silicon and Intel builds. Firmware, ROM dumps, factory SETs, and disk images are not included; supply your own instrument data.
