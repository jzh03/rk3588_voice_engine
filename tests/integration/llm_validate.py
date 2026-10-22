#!/usr/bin/env python3
"""Explicit real-model supervisor; NOT registered with CTest.

Run the entire invocation under the shared flock. The child adapter enforces
ownership again before loading/hashing assets. Only this child process group is
terminated on deadline or a sustained 1.5 GiB memory-floor breach.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

def memory():
    values = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        name, tail = line.split(":", 1)
        values[name] = int(tail.split()[0])
    return values

def process_stats(pid):
    values = {}
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            name, _, tail = line.partition(":")
            if name in ("VmRSS", "VmHWM", "VmSwap"):
                values[name + "_kib"] = int(tail.split()[0])
    except (FileNotFoundError, ProcessLookupError):
        pass
    return values

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--executable", default="build/model-probes/bin/llm_probe")
    p.add_argument("--config", default="config/llm-probe.yaml")
    p.add_argument("--cases", default="tests/fixtures/llm_cases.json")
    p.add_argument("--output", required=True)
    p.add_argument("--mode", choices=("cases", "lifecycle", "boundaries", "cancel"), default="cases")
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--repeat", type=int, default=3)
    p.add_argument("--warmup", type=int, default=1)
    p.add_argument("--init-cycles", type=int, default=10)
    p.add_argument("--case-id")
    a = p.parse_args()
    if not 1 <= a.timeout <= 3600:
        p.error("timeout must be 1..3600")
    root = Path(a.output).resolve()
    root.mkdir(parents=True, exist_ok=False)
    command = [str(Path(a.executable).resolve()), "--config", str(Path(a.config).resolve()),
               "--cases", str(Path(a.cases).resolve()), "--output-dir", str(root),
               "--run-id", "results", "--mode", a.mode, "--repeat", str(a.repeat),
               "--warmup", str(a.warmup), "--init-cycles", str(a.init_cycles)]
    if a.case_id:
        command += ["--case-id", a.case_id]
    (root / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    if memory()["MemAvailable"] < 1572864:
        raise RuntimeError("MemAvailable below 1.5 GiB before launch")
    low_since = None
    failure = None
    begin = time.monotonic()
    with (root / "stdout.log").open("wb") as out, (root / "stderr.log").open("wb") as err:
        proc = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
        with (root / "supervisor-resources.jsonl").open("w") as samples:
            while proc.poll() is None:
                elapsed = time.monotonic() - begin
                mem = memory()
                if mem["MemAvailable"] < 1572864:
                    low_since = low_since or time.monotonic()
                else:
                    low_since = None
                samples.write(json.dumps({"elapsed_s": elapsed, "pid": proc.pid,
                    "mem_available_kib": mem["MemAvailable"], "swap_free_kib": mem["SwapFree"],
                    **process_stats(proc.pid)}) + "\n")
                samples.flush()
                if low_since is not None and time.monotonic() - low_since >= 3:
                    failure = "MemAvailable below 1.5 GiB for 3 seconds"
                if elapsed > a.timeout:
                    failure = "process deadline"
                if failure:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait(timeout=5)
                    break
                time.sleep(0.25)
    record = {"command": command, "returncode": proc.returncode, "elapsed_s": time.monotonic()-begin,
              "supervisor_failure": failure, "process_group": proc.pid,
              "scope": "LLM model/file checks, not PCM/playback or acoustic cancellation"}
    (root / "supervisor.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, ensure_ascii=False))
    return 5 if failure else proc.returncode

if __name__ == "__main__":
    raise SystemExit(main())
