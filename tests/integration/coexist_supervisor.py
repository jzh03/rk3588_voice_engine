#!/usr/bin/env python3
"""Supervisor regression checks with temporary fake processes, never models."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time

path = Path(__file__).with_name("coexist_validate.py")
spec = importlib.util.spec_from_file_location("coexist_supervisor_under_test", path)
supervisor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(supervisor)


def parse(extra, valid):
    common = ["--manifest", "unused.json", "--output", "unused-output"]
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            result = supervisor.parse_args([*common, *extra])
        except SystemExit as exc:
            assert not valid and exc.code == 2, (extra, exc.code)
            return
    assert valid, extra
    return result


assert parse([], True).duration_sec == 300
assert parse([], True).min_rounds == 5
assert parse([], True).swap_policy == "review"
assert parse(["--swap-policy", "zero"], True).swap_policy == "zero"
assert parse([], True).warmup_rounds == 5
assert parse(["--warmup-rounds", "1"], True).warmup_rounds == 1
assert parse(["--warmup-rounds", "20"], True).warmup_rounds == 20
for extra in (
    ["--duration-sec", "299"], ["--min-rounds", "4"],
    ["--duration-sec", "0"], ["--duration-sec", "3601"],
    ["--min-rounds", "0"], ["--min-rounds", "1001"],
    ["--timeout", "300"], ["--timeout", "7201"],
    ["--smoke", "--duration-sec", "0"],
    ["--warmup-rounds", "0"], ["--warmup-rounds", "21"],
    ["--warmup-rounds", "-1"], ["--warmup-rounds", "1.5"],
    ["--swap-policy", "unknown"], ["--swap-policy", "0"],
):
    parse(extra, False)
parse(["--smoke", "--duration-sec", "1", "--min-rounds", "1",
       "--timeout", "2"], True)

sample = supervisor.process_sample(os.getpid())
assert sample["threads"] >= 1 and sample["vmrss_kib"] > 0, sample
assert sample["fd_count"] >= 3 and sample["starttime_ticks"] > 0, sample
assert sample["cpu_ticks"] >= 0, sample
assert sample["pss_kib"] is None or sample["pss_kib"] > 0, sample

# Stable isolated resource inputs; no host load or model/library dependency.
supervisor.system_sample = lambda: {
    "mem_available_kib": 4 * 1024 * 1024,
    "swap_total_kib": 0, "swap_free_kib": 0,
    "pswpin_pages": 0, "pswpout_pages": 0,
}
supervisor.sensor_paths = lambda: {}


def alive(pid):
    try:
        fields = (Path("/proc") / str(pid) / "stat").read_text()
        # A zombie has exited and cannot continue any inference/work.
        return fields[fields.rfind(")") + 2:].split()[0] != "Z"
    except FileNotFoundError:
        return False


with tempfile.TemporaryDirectory(prefix="coexist-supervisor-test-") as directory:
    root = Path(directory)
    fake = root / "fake_probe.py"
    fake.write_text(
        "#!/usr/bin/env python3\n"
        "import pathlib, subprocess, sys, time\n"
        "args=sys.argv[1:]\n"
        "root=pathlib.Path(args[args.index('--output-dir')+1])\n"
        "mode=pathlib.Path(args[args.index('--config')+1]).name\n"
        "if mode=='timeout':\n"
        "    child=subprocess.Popen([sys.executable, '-c', "
        "'import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(60)'])\n"
        "    (root/'child-pid').write_text(str(child.pid))\n"
        "    time.sleep(60)\n"
        "if mode=='failure':\n"
        "    raise SystemExit(4)\n"
        "time.sleep(0.1)\n",
        encoding="utf-8",
    )
    fake.chmod(0o700)

    def invoke(mode, destination):
        argv = ["--executable", str(fake), "--config", mode,
                "--manifest", "unused.json", "--output", str(destination),
                "--smoke", "--duration-sec", "1", "--min-rounds", "1",
                "--warmup-rounds", "3", "--swap-policy", "zero", "--timeout", "2"]
        with contextlib.redirect_stdout(io.StringIO()):
            return supervisor.main(argv)

    good = root / "good"
    assert invoke("success", good) == 0
    record = json.loads((good / "supervisor.json").read_text())
    assert record["returncode"] == 0 and record["supervisor_failure"] is None
    assert record["smoke"] is True and record["child_starttime_ticks"] > 0
    assert not record.get("a06_pass", False), record
    command = json.loads((good / "command.json").read_text())
    assert command[command.index("--duration-sec") + 1] == "1"
    assert command[command.index("--min-rounds") + 1] == "1"
    assert command[command.index("--warmup-rounds") + 1] == "3"
    assert command[command.index("--swap-policy") + 1] == "zero"
    assert record["swap_policy"] == "zero"
    assert "--smoke" in command
    before = (good / "supervisor.json").read_bytes()
    try:
        invoke("success", good)
    except FileExistsError:
        pass
    else:
        raise AssertionError("existing run output was accepted")
    assert (good / "supervisor.json").read_bytes() == before

    failed = root / "model-failure"
    assert invoke("failure", failed) == 4
    record = json.loads((failed / "supervisor.json").read_text())
    assert record["returncode"] == 4 and record["supervisor_failure"] is None

    timed = root / "timeout"
    try:
        assert invoke("timeout", timed) == 5
        record = json.loads((timed / "supervisor.json").read_text())
        assert record["supervisor_failure"] == "process deadline", record
        child = int((timed / "child-pid").read_text())
        deadline = time.monotonic() + 2
        while alive(child) and time.monotonic() < deadline:
            time.sleep(0.01)
        assert not alive(child), "SIGTERM-ignoring descendant survived supervisor cleanup"
    finally:
        # Test failures must not leave the deliberately stubborn fake child.
        report = timed / "supervisor.json"
        if report.exists():
            group = json.loads(report.read_text()).get("process_group")
            if group:
                try:
                    os.killpg(group, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    supervisor.system_sample = lambda: {
        "mem_available_kib": supervisor.MEMORY_FLOOR_KIB - 1
    }
    low = root / "low-memory"
    assert invoke("success", low) == 5
    record = json.loads((low / "supervisor.json").read_text())
    assert record["status"] == "not_launched" and record["a06_pass"] is False
    assert not (low / "stdout.log").exists(), "child launched below memory floor"

print("COEXIST_SUPERVISOR_OK; temporary fake processes only")
