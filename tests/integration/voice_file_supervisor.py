#!/usr/bin/env python3
"""Exercise the P3 supervisor with tiny fake executables; never load model assets."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

supervisor = Path(__file__).with_name("voice_file_validate.py").resolve()
checks = 0


def run(args, expected):
    global checks
    result = subprocess.run([sys.executable, str(supervisor), *map(str, args)],
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == expected, (
        args, result.returncode, result.stdout, result.stderr
    )
    checks += 1
    return result


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def process_running(pid):
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
        return stat[stat.rfind(")") + 2:].split()[0] != "Z"
    except FileNotFoundError:
        return False


with tempfile.TemporaryDirectory(prefix="voice-file-supervisor-") as directory:
    root = Path(directory)
    # Nothing is read from these paths: these tests do not initialize adapters.
    config = root / "missing.yaml"
    manifest = root / "missing.json"

    def fake(name, code):
        path = root / name
        path.write_text(f"#!{sys.executable}\n" + code, encoding="utf-8")
        path.chmod(0o700)
        return path

    common = r"""
import json, os, pathlib, sys
args = sys.argv[1:]
output = pathlib.Path(args[args.index("--output-dir") + 1])
(output / "fake-args.json").write_text(json.dumps(args))
print("FAKE_STDOUT", flush=True)
print("FAKE_STDERR", file=sys.stderr, flush=True)
"""
    orphan = fake("orphan", common + r"""
import subprocess
child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
(output / "fake-pids.json").write_text(json.dumps([os.getpid(), child.pid]))
raise SystemExit(0)
""")
    success = fake("success", common + "\nraise SystemExit(0)\n")
    failure = fake("failure", common + "\nraise SystemExit(6)\n")
    timeout = fake("timeout", common + r"""
import signal, subprocess, time
child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
(output / "fake-pids.json").write_text(json.dumps([os.getpid(), child.pid]))
def terminate(signum, frame):
    # Group termination must reach both processes; reap to avoid orphan zombies.
    child.wait(timeout=2)
    raise SystemExit(128 + signum)
signal.signal(signal.SIGTERM, terminate)
time.sleep(30)
""")

    def args(executable, output, extra=()):
        return ["--executable", executable, "--config", config,
                "--manifest", manifest, "--output", output, "--timeout", "10", *extra]

    good_output = root / "success-run"
    run(args(success, good_output, ["--tts-mode", "batch", "--warmup-rounds", "2",
                                   "--sample", "speech_b", "--sample", "speech_a"]), 0)
    report = read_json(good_output / "supervisor.json")
    assert report["returncode"] == 0 and report["supervisor_failure"] is None, report
    assert report["samples"] >= 1 and report["elapsed_s"] >= 0, report
    assert report["warmup_rounds"] == 2 and report["tts_mode"] == "batch", report
    command = read_json(good_output / "command.json")
    captured = read_json(good_output / "fake-args.json")
    assert command[1:] == captured, (command, captured)
    assert captured[-4:] == ["--sample", "speech_b", "--sample", "speech_a"], captured
    assert captured[captured.index("--run-id") + 1] == "results", captured
    assert "FAKE_STDOUT" in (good_output / "stdout.log").read_text()
    assert "FAKE_STDERR" in (good_output / "stderr.log").read_text()
    assert (good_output / "supervisor-resources.jsonl").read_text().strip()
    assert (good_output / "sensor-sources.json").is_file()

    failed_output = root / "failure-run"
    run(args(failure, failed_output), 6)
    report = read_json(failed_output / "supervisor.json")
    assert report["returncode"] == 6 and report["supervisor_failure"] is None, report
    assert "FAKE_STDERR" in (failed_output / "stderr.log").read_text()

    timed_output = root / "timeout-run"
    try:
        request = args(timeout, timed_output)
        request[request.index("--timeout") + 1] = "1"
        run(request, 5)
        report = read_json(timed_output / "supervisor.json")
        assert report["supervisor_failure"] == "process deadline", report
        assert report["returncode"] is not None and report["returncode"] != 0, report
        assert 1 <= report["elapsed_s"] < 15, report
        pids = read_json(timed_output / "fake-pids.json")
        assert report["process_group"] == pids[0], report
        deadline = time.monotonic() + 2
        while any(process_running(pid) for pid in pids) and time.monotonic() < deadline:
            time.sleep(0.02)
        assert not any(process_running(pid) for pid in pids), pids
    finally:
        pids_file = timed_output / "fake-pids.json"
        if pids_file.exists():
            pids = read_json(pids_file)
            if any(process_running(pid) for pid in pids):
                try:
                    os.killpg(pids[0], signal.SIGKILL)
                except ProcessLookupError:
                    pass

    # A successful leader exit must not let an orphaned child survive the
    # supervisor or release the lock while its work continues.
    orphan_output = root / "orphan-run"
    unrelated = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(30)"],
        start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        run(args(orphan, orphan_output), 5)
        report = read_json(orphan_output / "supervisor.json")
        assert report["returncode"] == 0, report
        assert report["supervisor_failure"] == (
            "child exited with surviving process-group members"), report
        pids = read_json(orphan_output / "fake-pids.json")
        assert report["process_group"] == pids[0], report
        deadline = time.monotonic() + 2
        while any(process_running(pid) for pid in pids) and time.monotonic() < deadline:
            time.sleep(0.02)
        assert not any(process_running(pid) for pid in pids), pids
        assert unrelated.poll() is None, "supervisor killed an unrelated process"
    finally:
        pids_file = orphan_output / "fake-pids.json"
        if pids_file.exists():
            pids = read_json(pids_file)
            if any(process_running(pid) for pid in pids):
                try:
                    os.killpg(pids[0], signal.SIGKILL)
                except ProcessLookupError:
                    pass
        if unrelated.poll() is None:
            unrelated.terminate()
            try:
                unrelated.wait(timeout=5)
            except subprocess.TimeoutExpired:
                unrelated.kill()
                unrelated.wait(timeout=5)

    existing_output = root / "existing-run"
    existing_output.mkdir()
    sentinel = existing_output / "keep.txt"
    sentinel.write_text("original evidence\n", encoding="utf-8")
    run(args(success, existing_output), 1)
    assert sentinel.read_text(encoding="utf-8") == "original evidence\n"
    assert list(existing_output.iterdir()) == [sentinel], "existing output was modified"

    missing_output = root / "missing-executable-run"
    run(args(root / "absent-executable", missing_output), 5)
    report = read_json(missing_output / "supervisor.json")
    assert report["returncode"] is None, report
    assert "FileNotFoundError" in report["supervisor_failure"], report

    for flag, value in (("--warmup-rounds", "0"), ("--warmup-rounds", "21"),
                        ("--timeout", "0"), ("--timeout", "7201"),
                        ("--tts-mode", "invalid")):
        invalid_output = root / "invalid-run"
        request = args(success, invalid_output)
        if flag in request:
            request[request.index(flag) + 1] = value
        else:
            request.extend([flag, value])
        run(request, 2)
        assert not invalid_output.exists(), "invalid CLI created evidence directory"

print(f"VOICE_FILE_SUPERVISOR_OK checks={checks}; fake executables only")
