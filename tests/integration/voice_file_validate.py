#!/usr/bin/env python3
"""Explicit P3 model supervisor, invoked under the shared flock; not a CTest model run."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from coexist_validate import (MEMORY_FLOOR_KIB, write_json, system_sample, process_sample,
                              sensor_paths, read_int, stop_group, group_exists)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", default="build/model-probes/bin/voice_file_demo")
    parser.add_argument("--config", default="config/voice-file-demo.yaml")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--sample", action="append", default=[])
    parser.add_argument("--output", required=True)
    parser.add_argument("--warmup-rounds", type=int, default=1)
    parser.add_argument("--tts-mode", choices=("stream", "batch"), default="stream")
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args(argv)
    if not 1 <= args.warmup_rounds <= 20:
        parser.error("warmup-rounds must be 1..20")
    if not 1 <= args.timeout <= 7200:
        parser.error("timeout must be 1..7200")
    return args


def main(argv=None):
    args = parse_args(argv)
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False)
    command = [str(Path(args.executable).resolve()), "--config", str(Path(args.config).resolve()),
               "--manifest", str(Path(args.manifest).resolve()), "--output-dir", str(root),
               "--run-id", "results", "--warmup-rounds", str(args.warmup_rounds),
               "--tts-mode", args.tts_mode]
    for sample in args.sample:
        command += ["--sample", sample]
    write_json(root / "command.json", command)
    paths = sensor_paths()
    ticks = os.sysconf("SC_CLK_TCK")
    write_json(root / "sensor-sources.json", {"sample_interval_sec": 1,
        "memory_guard_interval_sec": 0.05, "clock_ticks_per_sec": ticks,
        "cpu_percent_basis": "100 percent = one fully busy logical CPU", "sensors": paths,
        "npu_memory": {"readable": False, "reason": "No validated unprivileged NPU memory counter"}})
    interrupted = []
    handlers = {sig: signal.signal(sig, lambda value, frame: interrupted.append(value))
                for sig in (signal.SIGINT, signal.SIGTERM)}
    begin = time.monotonic_ns()
    proc = None
    failure = None
    count = 0
    minimum = None
    identity = None
    last_cpu = None
    next_sample = begin
    try:
        if system_sample()["mem_available_kib"] < MEMORY_FLOOR_KIB:
            raise RuntimeError("MemAvailable below 1.5 GiB before launch")
        with (root / "stdout.log").open("wb") as out, (root / "stderr.log").open("wb") as err, \
             (root / "supervisor-resources.jsonl").open("w", encoding="utf-8") as samples:
            proc = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
            while True:
                now = time.monotonic_ns()
                system = system_sample()
                available = system["mem_available_kib"]
                minimum = available if minimum is None else min(minimum, available)
                if available < MEMORY_FLOOR_KIB:
                    failure = "MemAvailable below 1.5 GiB"
                if now - begin > args.timeout * 1_000_000_000:
                    failure = "process deadline"
                if interrupted:
                    failure = f"supervisor interrupted by signal {interrupted[0]}"
                code = proc.poll()
                if now >= next_sample or code is not None or failure:
                    ps = process_sample(proc.pid)
                    if "starttime_ticks" in ps:
                        identity = ps["starttime_ticks"] if identity is None else identity
                        if identity != ps["starttime_ticks"]:
                            failure = "child PID identity changed"
                        dt = (now-last_cpu[0])/1e9 if last_cpu else 0
                        ps["cpu_percent"] = 100*(ps["cpu_ticks"]-last_cpu[1])/ticks/dt if dt > 0 else None
                        last_cpu = (now, ps["cpu_ticks"])
                    row = {"monotonic_ns": now, "elapsed_s": (now-begin)/1e9, "pid": proc.pid,
                           **system, **ps, "sensors": {key: read_int(path) for key,path in paths.items()}}
                    samples.write(json.dumps(row, ensure_ascii=False)+"\n")
                    samples.flush()
                    count += 1
                    next_sample = now + 1_000_000_000
                if failure:
                    stop_group(proc)
                    break
                if code is not None:
                    # A reaped leader may leave asset hashers or other children
                    # alive. They remain in the session created for this probe;
                    # clean only that group before releasing the outer lock.
                    if group_exists(proc.pid):
                        failure = "child exited with surviving process-group members"
                        stop_group(proc)
                    break
                time.sleep(0.05)
    except BaseException as exc:
        failure = f"supervisor exception: {type(exc).__name__}: {exc}"
        if proc is not None:
            stop_group(proc)
    finally:
        record = {"command": command, "returncode": proc.returncode if proc else None,
                  "begin_ns": begin, "end_ns": time.monotonic_ns(), "supervisor_failure": failure,
                  "process_group": proc.pid if proc else None, "child_starttime_ticks": identity,
                  "samples": count, "min_mem_available_kib": minimum,
                  "warmup_rounds": args.warmup_rounds, "tts_mode": args.tts_mode,
                  "scope": "P3 file pipeline; software timing, no audio device or A10 acoustic acceptance"}
        record["elapsed_s"] = (record["end_ns"]-begin)/1e9
        write_json(root / "supervisor.json", record)
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    print(json.dumps(record, ensure_ascii=False))
    return 5 if failure else (record["returncode"] if record["returncode"] is not None else 5)


if __name__ == "__main__":
    raise SystemExit(main())
