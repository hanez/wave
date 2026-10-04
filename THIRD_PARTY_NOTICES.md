# Third-party notices

## Musashi

This project fetches and links the Musashi Motorola 680x0 emulation engine from
https://github.com/kstenerud/Musashi at commit
`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`.

The build applies `cmake/PrepareMusashi.cmake` to a private build copy to make
MC68000 execution registers, cycle counters, callbacks, and exception jump
buffers thread-local. The upstream checkout is not modified.

Copyright (c) 1998-2001 Karl Stenerud

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## JUCE 8.0.15 and bundled dependencies

JUCE is fetched from https://github.com/juce-framework/JUCE at tag `8.0.15`.
The build applies `cmake/PrepareJuce.cmake` to the fetched checkout. This changes
the Windows VBlank thread to observe thread shutdown and send paced repaint
notifications when `IDXGIOutput::WaitForVBlank` fails. This prevents frozen
repaints and window-recreation deadlocks under Wine. The rest of the pinned JUCE source is unchanged.
Its framework modules are dual-licensed under AGPLv3 and the commercial JUCE
license. This project's GPL-3.0-or-later license does not replace JUCE's terms.
The upstream notice and dependency license index are reproduced in
`LICENSES/JUCE.md`; the AGPLv3 text is in `LICENSES/AGPL-3.0.txt`.
Paths in the upstream index are relative to the JUCE source checkout. Each
bundled dependency retains its own copyright and license notices there.
Distributors must retain applicable dependency notices with their artifacts.

## External instrument data

Waldorf/PPG executable firmware, ROM archives, Wave factory banks, other disk images, and recordings are not
part of the public source distribution or its license. The PPG-derived decoded
waveform sample bank is included separately; see `data/README.md` for provenance.
The maintainer-supplied blank INIT disk is also included; its native INIT records
retain their original provenance and are not relicensed by the project GPL grant.
The project GPL license does not assert ownership of that third-party data.
Import tools operate on
user-supplied files or separately downloaded firmware. Public availability of
a download does not grant this project permission to redistribute its contents.

Waldorf, Wave, and PPG names identify the modelled instruments. This project is
independent and is not endorsed by their owners.
