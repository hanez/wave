# Building Wave Emulation 0.1.11

This source archive contains the release source and the JUCE and Musashi sources used for the build, including local dependency patches. It contains no Git history or private instrument data.

On macOS, from this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWAVE_EMBED_PRIVATE_ASSETS=OFF -DFETCHCONTENT_SOURCE_DIR_JUCE="$PWD/vendor/juce" -DFETCHCONTENT_SOURCE_DIR_MUSASHI="$PWD/vendor/musashi"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Release bundles target macOS 13.0 and contain arm64 and x86_64 binaries. Public tests run without instrument firmware. Signing, PACE wrapping, and notarization require your own release credentials. See README.md and THIRD_PARTY_NOTICES.md for build and license details.
