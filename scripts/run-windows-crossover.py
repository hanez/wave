#!/usr/bin/env python3
"""Run a Windows executable in the dedicated Wave CrossOver test bottle."""
import os
from pathlib import Path
import subprocess
import sys


def windows_path(value):
    if Path(value).exists():
        return "Z:" + str(Path(value).resolve()).replace("/", "\\")
    return value


def main():
    if len(sys.argv) < 2:
        raise SystemExit("Usage: run-windows-crossover.py executable.exe [arguments ...]")
    wine = Path("/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine")
    if not wine.is_file():
        raise SystemExit("CrossOver is not installed in /Applications")
    env = os.environ.copy()
    if "CX_BOTTLE_PATH" not in env:
        setting = subprocess.run(
            ["defaults", "read", "com.codeweavers.CrossOver", "BottleDir"],
            capture_output=True, text=True)
        if setting.returncode == 0:
            env["CX_BOTTLE_PATH"] = setting.stdout.strip()
    for key in ("WAVE_FIRMWARE_DIR", "WAVE_TEST_ARTIFACT_DIR", "WAVE_HOST_STATE_FILE",
                "PPG_WAVETABLE_ROM", "WAVE_FACTORY_SET", "WAVE_REFERENCE_CAPTURE"):
        if key in env:
            # Output directories need not exist yet.
            if key == "WAVE_TEST_ARTIFACT_DIR":
                Path(env[key]).mkdir(parents=True, exist_ok=True)
            env[key] = windows_path(env[key])
    env.setdefault("MVK_CONFIG_LOG_LEVEL", "0")
    command = [str(wine), "--bottle",
               env.get("WAVE_CROSSOVER_BOTTLE", "Wave Windows Testing"),
               "--wait", "--no-convert", "--debugmsg",
               env.get("WAVE_CROSSOVER_DEBUG", "-all")]
    if env.get("WAVE_CROSSOVER_GUI") == "1":
        # Use ShellExecute for user-facing launches; keep direct execution for
        # CMake generators and tests, which need their process exit code.
        if desktop := env.get("WAVE_CROSSOVER_DESKTOP"):
            command += ["--desktop", desktop]
        command += ["--start"]
    command += [windows_path(arg) for arg in sys.argv[1:]]
    result = subprocess.run(command, env=env)
    if result.returncode != 0:
        print(f"CrossOver exited with status {result.returncode}", file=sys.stderr)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
