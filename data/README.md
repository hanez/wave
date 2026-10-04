# PPG V6 wavetable samples

`ppg-v6-wavetables.bin` contains 30 tables × 64 waves × 128 signed eight-bit
samples (245,760 bytes), in table/wave/sample order, with no header. It is
embedded in public builds as the lower 30 tables. The remaining tables use
procedural data unless a user imports a bank.

SHA-256: `1e573f91e6dbd7b331e8287f6cf82b5b8c82c08f29c3f72cd3ce8386761cd105`.

The bank was decoded from the maintainer-supplied PPG Wave 2.3 V6 research
image using `PpgWaveRom::decode`. That input has SHA-256
`ff9393d3649a402eab07d2d763bbdb6161def53f74101b7cb08969d436b9741a`.
The import process and its out-of-range table-13 behavior are documented in
`scripts/import-ppg-wave-23-v6-rom.sh`. The output contains only the decoded
sample bank, including the algorithmically generated tables and standard waves;
the executable ROM images and original ROM archive are excluded.

To reproduce from your own imported image:

```sh
cmake --build build --target DecodePpgWavetables
./build/DecodePpgWavetables /path/to/ppg-image.rom /tmp/ppg-v6-wavetables.bin
```

This third-party PPG-derived data is included at the maintainer's direction.
The GPL-3.0-or-later grant for project-authored code and artwork does not
relicense third-party sample data or assert ownership of it.

## Blank startup disk

`BlankWave.img` is the maintainer-supplied `wave-init-all2.img`, a 720 KB FAT12
floppy containing a SET with 256 identical INIT SOUND records and 256 MULTI INIT
Performances. Each Performance enables Instrument 1 and assigns INIT SOUND.
The INIT records match the safe defaults in Wave OS 1.700. The image contains
no executable firmware or factory sound presets.

SHA-256: `a29a5926cd89275f231bb12d00ef70bfafbd77395b89bdb5ad9660b34b3a80d1`.

New instances mount their own writable copy and load the default SET. Saved
projects restore their own bank and disk instead. Release packages also include
the image as `Blank Wave.img`. This native instrument data is included at the
maintainer's direction; the project GPL grant does not relicense it.
