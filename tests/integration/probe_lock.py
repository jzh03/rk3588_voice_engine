#!/usr/bin/env python3
"""Check lock ownership without loading any model or using the shared model lock."""
from pathlib import Path
import subprocess
import sys
import tempfile


def check(command, expected):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=10)
    if result.returncode != expected:
        raise AssertionError(f"{command}: rc={result.returncode}, expected={expected}\n"
                             f"{result.stdout}\n{result.stderr}")


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="probe-lock-") as directory:
        lock = str(Path(directory) / "validation.lock")
        other = str(Path(directory) / "unrelated.lock")
        check([binary, lock], 2)
        Path(lock).touch()
        check([binary, lock], 2)
        check(["flock", "-x", lock, binary, lock], 0)
        check(["flock", "-x", "-F", lock, binary, lock], 0)
        check([binary, lock, "--self-lock"], 0)
        check(["flock", "-s", lock, binary, lock], 2)
        check(["flock", "-x", other, binary, lock], 2)
        # A different process owning this lock must not authorize an unlocked
        # model runner. Handshake through stdout avoids a timing-dependent sleep.
        holder = subprocess.Popen(
            ["flock", "-x", lock, sys.executable, "-u", "-c",
             "import sys; print('LOCKED', flush=True); sys.stdin.read()"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            if holder.stdout.readline().strip() != "LOCKED":
                raise AssertionError("unrelated lock holder failed to start")
            check([binary, lock], 2)
        finally:
            holder.communicate(timeout=10)
        check([binary, lock], 2)
    print("PROBE_LOCK_OK: nine ownership/lock-mode checks; no model initialized")


if __name__ == "__main__":
    main()
