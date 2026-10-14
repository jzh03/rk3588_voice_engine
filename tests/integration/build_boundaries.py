#!/usr/bin/env python3
"""Build engine, mock audio, and native audio without unrelated dependencies."""

from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def run(command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {shlex.join(command)}\n"
                           f"{result.stdout}")


def main():
    source = Path(sys.argv[1]).resolve()
    cmake, ctest = sys.argv[2:4]
    pkg_config = shutil.which("pkg-config")
    if not pkg_config:
        raise RuntimeError("pkg-config is required for the engine dependency boundary check")

    with tempfile.TemporaryDirectory(prefix="voice-engine-boundaries-") as directory:
        root = Path(directory)
        wrapper = root / "pkg-config-without-audio"
        blocked = root / "blocked-audio-dependency"
        wrapper.write_text(
            f"#!{sys.executable}\n"
            "import os, pathlib, sys\n"
            "if any(name in arg for arg in sys.argv[1:] "
            "for name in ('libpulse', 'samplerate', 'sndfile')):\n"
            f"    pathlib.Path({str(blocked)!r}).touch()\n"
            "    sys.exit(1)\n"
            f"os.execv({pkg_config!r}, [{pkg_config!r}, *sys.argv[1:]])\n",
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
        engine = root / "engine"
        run([cmake, "-S", str(source), "-B", str(engine), "-G", "Ninja",
             "-DVOICE_ENGINE_BUILD_ENGINE=ON", "-DVOICE_ENGINE_BUILD_AUDIO=OFF",
             "-DVOICE_ENGINE_BUILD_ENV_CHECK=OFF", "-DBUILD_TESTING=ON",
             "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE",
             f"-DPKG_CONFIG_EXECUTABLE={wrapper}"])
        run([cmake, "--build", str(engine), "--parallel", "2"])
        # The nested build registers this test too. Exclude it to avoid recursion.
        run([ctest, "--test-dir", str(engine), "--output-on-failure", "-E", "^build_boundaries$"])
        if blocked.exists():
            raise RuntimeError("engine-only build attempted to discover a system audio dependency")

        audio = root / "audio"
        run([cmake, "-S", str(source), "-B", str(audio), "-G", "Ninja",
             "-DVOICE_ENGINE_BUILD_ENGINE=OFF", "-DVOICE_ENGINE_BUILD_AUDIO=ON",
             "-DVOICE_ENGINE_BUILD_PULSE_AUDIO=OFF",
             "-DVOICE_ENGINE_BUILD_ENV_CHECK=OFF", "-DBUILD_TESTING=ON",
             "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE",
             "-DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=TRUE",
             "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE",
             "-DCMAKE_DISABLE_FIND_PACKAGE_Threads=TRUE",
             "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE"])
        run([cmake, "--build", str(audio), "--parallel", "2"])
        run([ctest, "--test-dir", str(audio), "--output-on-failure"])

        native_wrapper = root / "pkg-config-without-application-deps"
        native_blocked = root / "blocked-application-dependency"
        native_wrapper.write_text(
            f"#!{sys.executable}\n"
            "import os, pathlib, sys\n"
            "if any(name in arg for arg in sys.argv[1:] "
            "for name in ('yaml', 'sndfile')):\n"
            f"    pathlib.Path({str(native_blocked)!r}).touch()\n"
            "    sys.exit(1)\n"
            f"os.execv({pkg_config!r}, [{pkg_config!r}, *sys.argv[1:]])\n",
            encoding="utf-8",
        )
        native_wrapper.chmod(0o755)
        native = root / "native-audio"
        run([cmake, "-S", str(source), "-B", str(native), "-G", "Ninja",
             "-DVOICE_ENGINE_BUILD_ENGINE=OFF", "-DVOICE_ENGINE_BUILD_AUDIO=ON",
             "-DVOICE_ENGINE_BUILD_PULSE_AUDIO=ON",
             "-DVOICE_ENGINE_BUILD_ENV_CHECK=OFF", "-DBUILD_TESTING=ON",
             "-DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=TRUE",
             "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE",
             "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE",
             f"-DPKG_CONFIG_EXECUTABLE={native_wrapper}"])
        run([cmake, "--build", str(native), "--parallel", "2"])
        run([ctest, "--test-dir", str(native), "--output-on-failure"])
        if native_blocked.exists():
            raise RuntimeError("native-audio-only build attempted to discover yaml or libsndfile")

    print("BUILD_BOUNDARIES_OK: engine-only, mock-audio-only, and native-audio-only "
          "build and test independently")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"build boundary test failed: {error}", file=sys.stderr)
        sys.exit(1)
