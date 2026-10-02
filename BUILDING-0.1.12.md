# Building Wave Emulation 0.1.12

This archive includes the source and the JUCE/Musashi dependencies used for this release, including local patches. It contains no Git history or private instrument data.

On macOS, from this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWAVE_EMBED_PRIVATE_ASSETS=OFF -DWAVE_SIGN_RELEASE_ARTIFACTS=OFF -DFETCHCONTENT_SOURCE_DIR_JUCE="$PWD/vendor/juce" -DFETCHCONTENT_SOURCE_DIR_MUSASHI="$PWD/vendor/musashi"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

On Windows with Visual Studio 2022 and CMake:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DWAVE_EMBED_PRIVATE_ASSETS=OFF -DFETCHCONTENT_SOURCE_DIR_JUCE="$PWD/vendor/juce" -DFETCHCONTENT_SOURCE_DIR_MUSASHI="$PWD/vendor/musashi"
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

macOS bundles are universal arm64/x86_64 and target macOS 13.0. Windows bundles target x64 and use the static release CRT. Signing, PACE wrapping and notarization require the maintainer's separate account configuration; no credentials are included. See README.md for cross-compilation and firmware setup.
