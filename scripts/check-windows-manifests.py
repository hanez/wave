#!/usr/bin/env python3
"""Check the embedded Common Controls v6 manifests in Windows release binaries."""
import argparse
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET


def check_manifest(path, resource_id):
    data = path.read_bytes()

    def unpack(format, offset):
        return struct.unpack_from(format, data, offset)

    if data[:2] != b"MZ":
        raise ValueError("not a PE executable")
    pe = unpack("<I", 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("invalid PE signature")
    machine, section_count = unpack("<HH", pe + 4)
    optional_size = unpack("<H", pe + 20)[0]
    optional = pe + 24
    if machine != 0x8664 or unpack("<H", optional)[0] != 0x20b:
        raise ValueError("expected a Windows x64 PE32+ binary")
    if unpack("<I", optional + 108)[0] < 3:
        raise ValueError("PE resource directory is missing")
    resource_rva, resource_size = unpack("<II", optional + 128)
    sections = []
    for index in range(section_count):
        header = optional + optional_size + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = unpack("<IIII", header + 8)
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))

    def file_offset(rva, size):
        for virtual_address, virtual_size, raw_offset, raw_size in sections:
            relative = rva - virtual_address
            if 0 <= relative and relative + size <= raw_size:
                offset = raw_offset + relative
                if offset + size <= len(data):
                    return offset
        raise ValueError("resource data is outside the PE file")

    if not resource_rva or not resource_size:
        raise ValueError("embedded manifest is missing")
    base = file_offset(resource_rva, resource_size)

    def directory(relative):
        if relative + 16 > resource_size:
            raise ValueError("invalid resource directory")
        named, ids = unpack("<HH", base + relative + 12)
        if relative + 16 + (named + ids) * 8 > resource_size:
            raise ValueError("invalid resource entries")
        return [unpack("<II", base + relative + 16 + index * 8)
                for index in range(named + ids)]

    def child(relative, identifier):
        for name, target in directory(relative):
            if name == identifier and target & 0x80000000:
                return target & 0x7fffffff
        raise ValueError(f"embedded manifest resource {resource_id} is missing")

    languages = child(child(0, 24), resource_id)  # RT_MANIFEST
    manifests = directory(languages)
    if not manifests:
        raise ValueError("embedded manifest has no language entry")
    for _, target in manifests:
        if target & 0x80000000 or target + 16 > resource_size:
            raise ValueError("invalid manifest resource")
        rva, size = unpack("<II", base + target)
        offset = file_offset(rva, size)
        assembly = ET.fromstring(data[offset:offset + size].rstrip(b"\0"))
        namespace = {"asm": "urn:schemas-microsoft-com:asm.v1"}
        dependencies = assembly.findall(
            "asm:dependency/asm:dependentAssembly/asm:assemblyIdentity", namespace)
        if not any(identity.get("name") == "Microsoft.Windows.Common-Controls"
                   and identity.get("version") == "6.0.0.0"
                   and identity.get("type", "").lower() == "win32"
                   and identity.get("processorArchitecture") in ("*", "amd64")
                   and identity.get("publicKeyToken", "").lower() == "6595b64144ccf1df"
                   for identity in dependencies):
            raise ValueError("manifest does not select Common Controls v6")
    print(f"{path}: Common Controls v6 manifest verified (resource {resource_id})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--vst3", type=Path, required=True,
                        help="VST3 bundle directory or inner Windows DLL")
    args = parser.parse_args()
    plugin = args.vst3
    if plugin.is_dir():
        candidates = list((plugin / "Contents/x86_64-win").glob("*.vst3"))
        if len(candidates) != 1:
            parser.error("VST3 bundle must contain exactly one x64 plugin DLL")
        plugin = candidates[0]
    try:
        check_manifest(args.exe, 1)
        check_manifest(plugin, 2)
    except (OSError, ValueError, struct.error, ET.ParseError) as error:
        print(f"Windows manifest check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
