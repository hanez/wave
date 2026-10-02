#!/usr/bin/env python3
"""Export current source files without Git history, private data, or build outputs."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
ALLOWED_ROOTS = {"Source", "Tests", "Tools", "cmake", "media", "scripts",
                 "Installer", "reference-captures", "LICENSES", ".github", "docs"}
ALLOWED_FILES = {".gitignore", "CMakeLists.txt", "README.md", "LICENSE",
                 "THIRD_PARTY_NOTICES.md", "CONTRIBUTING.md", "PUBLIC_RELEASE.md",
                 "waldorf-wave-firmware-reverse-engineering.md",
                 "data/ppg-v6-wavetables.bin", "data/README.md",
                 "media/wave-emulation-screenshot.png"}
EXCLUDED = {"Source/Dsp/FactoryUpperWavetables.h", "media/WaldorfWaveUI.svg"}
SOURCE_SUFFIXES = {".cpp", ".h", ".cmake", ".svg", ".sh", ".py", ".md", ".txt", ".yml"}


def export(destination):
    if destination.exists():
        raise SystemExit(f"Destination already exists; choose a new directory: {destination}")
    names = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
    ).decode().split("\0")
    selected = []
    for name in sorted(set(filter(None, names))):
        relative = Path(name)
        source = ROOT / relative
        if name in EXCLUDED or not source.is_file():
            continue
        if source.is_symlink():
            raise SystemExit(f"Refusing symlink: {name}")
        if name not in ALLOWED_FILES and not (
            relative.parts[0] in ALLOWED_ROOTS and relative.suffix in SOURCE_SUFFIXES
            and not any(part.startswith('.') for part in relative.parts[1:])
        ):
            continue
        if relative.parts[0] == 'Installer' and name != 'Installer/build_installer.sh':
            continue
        selected.append(relative)
    for required in ('LICENSE', 'CMakeLists.txt', 'media/WaldorfWaveUI_NOLOGO.svg', 'data/ppg-v6-wavetables.bin'):
        if Path(required) not in selected:
            raise SystemExit(f"Required public source missing: {required}")
    destination.mkdir(parents=True)
    for relative in selected:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, target)
    print(f"Exported {len(selected)} source files to {destination}")
    print("No Git history was copied. Review, then initialize a new public repository here.")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    export(parser.parse_args().destination.resolve())
