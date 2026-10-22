#!/usr/bin/env python3
"""Supervise the real four-model coexistence probe (not a CTest model run).

Run this whole script under the project's shared flock. The child checks lock
ownership before asset hashing or initialization. Only its process group is
terminated on timeout, a memory-floor breach, or a requested interruption.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

MEMORY_FLOOR_KIB = 1572864


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def numeric_lines(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        name, tail = line.split(":", 1)
        values[name] = int(tail.split()[0])
    return values


def system_sample():
    memory = numeric_lines("/proc/meminfo")
    swap = {}
    for line in Path("/proc/vmstat").read_text().splitlines():
        key, value = line.split()
        if key in ("pswpin", "pswpout"):
            swap[key + "_pages"] = int(value)
    return {
        "mem_available_kib": memory["MemAvailable"],
        "swap_total_kib": memory["SwapTotal"],
        "swap_free_kib": memory["SwapFree"],
        **swap,
    }


def read_int(path):
    try:
        return {"value": int(Path(path).read_text().strip()), "readable": True}
    except (OSError, ValueError) as exc:
        return {"value": None, "readable": False, "reason": type(exc).__name__}


def sensor_paths():
    paths = {}
    for directory in sorted(Path("/sys/class/thermal").glob("thermal_zone*")):
        try:
            kind = (directory / "type").read_text().strip()
        except OSError:
            kind = "unknown"
        paths[f"thermal:{directory.name}:{kind}:millidegC"] = str(directory / "temp")
    for directory in sorted(Path("/sys/devices/system/cpu/cpufreq").glob("policy*")):
        paths[f"cpu:{directory.name}:kHz"] = str(directory / "scaling_cur_freq")
    for directory in sorted(Path("/sys/class/devfreq").glob("*")):
        paths[f"devfreq:{directory.name}:Hz"] = str(directory / "cur_freq")
    return paths


def process_sample(pid):
    base = Path("/proc") / str(pid)
    try:
        status = {}
        for line in (base / "status").read_text().splitlines():
            key, _, tail = line.partition(":")
            if key in ("VmRSS", "VmHWM", "VmSwap"):
                status[key.lower() + "_kib"] = int(tail.split()[0])
            elif key == "Threads":
                status["threads"] = int(tail.strip())
        stat = (base / "stat").read_text()
        fields = stat[stat.rfind(")") + 2:].split()
        status["starttime_ticks"] = int(fields[19])
        status["cpu_ticks"] = int(fields[11]) + int(fields[12])
        try:
            with os.scandir(base / "fd") as entries:
                status["fd_count"] = sum(1 for _ in entries)
        except OSError as exc:
            status["fd_count"] = None
            status["fd_error"] = type(exc).__name__
        try:
            pss = None
            for line in (base / "smaps_rollup").read_text().splitlines():
                if line.startswith("Pss:"):
                    pss = int(line.split()[1])
            status["pss_kib"] = pss
            status["pss_readable"] = pss is not None
        except OSError as exc:
            status.update(pss_kib=None, pss_readable=False, pss_error=type(exc).__name__)
        return status
    except (OSError, ValueError, IndexError):
        return {"process_exited_or_unreadable": True}


def group_exists(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False


def stop_group(proc):
    # A reaped group leader does not imply its children have exited.
    if not group_exists(proc.pid):
        proc.poll()
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        proc.poll()  # Reap the leader without losing track of the process group.
        if not group_exists(proc.pid):
            return
        time.sleep(0.05)
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    proc.wait(timeout=5)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", default="build/model-probes/bin/coexist_probe")
    parser.add_argument("--config", default="config/coexist-probe.yaml")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--sample", action="append", default=[])
    parser.add_argument("--output", required=True)
    parser.add_argument("--duration-sec", type=int, default=300)
    parser.add_argument("--min-rounds", type=int, default=5)
    parser.add_argument("--warmup-rounds", type=int, default=5)
    parser.add_argument("--swap-policy", choices=("zero", "review"), default="review")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args(argv)
    if not 1 <= args.duration_sec <= 3600 or not 1 <= args.min_rounds <= 1000:
        parser.error("duration-sec must be 1..3600 and min-rounds 1..1000")
    if not 1 <= args.warmup_rounds <= 20:
        parser.error("warmup-rounds must be 1..20")
    if not args.smoke and (args.duration_sec < 300 or args.min_rounds < 5):
        parser.error("formal A06 requires duration-sec >=300 AND min-rounds >=5")
    if not 1 <= args.timeout <= 7200 or args.timeout <= args.duration_sec:
        parser.error("timeout must exceed duration-sec and be <=7200")
    return args


def main(argv=None):
    args = parse_args(argv)
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False)
    command = [str(Path(args.executable).resolve()), "--config", str(Path(args.config).resolve()),
               "--manifest", str(Path(args.manifest).resolve()), "--output-dir", str(root),
               "--run-id", "results", "--duration-sec", str(args.duration_sec),
               "--min-rounds", str(args.min_rounds),
               "--warmup-rounds", str(args.warmup_rounds),
               "--swap-policy", args.swap_policy]
    for sample_id in args.sample:
        command += ["--sample", sample_id]
    if args.smoke:
        command += ["--smoke"]
    write_json(root / "command.json", command)
    paths = sensor_paths()
    clock_ticks = os.sysconf("SC_CLK_TCK")
    write_json(root / "sensor-sources.json", {
        "sample_interval_sec": 1,
        "clock_ticks_per_sec": clock_ticks,
        "cpu_percent_basis": "100 percent = one fully busy logical CPU",
        "sensors": paths,
        "npu_memory": {"readable": False, "reason": "No validated unprivileged NPU memory counter; system headroom is recorded"},
        "swap_policy": args.swap_policy,
        "swap_review": "review records all activity for independent no-sustained-swap A06 review; zero fails on any measured page delta",
    })
    if system_sample()["mem_available_kib"] < MEMORY_FLOOR_KIB:
        write_json(root / "supervisor.json", {
            "command": command, "returncode": None,
            "supervisor_failure": "MemAvailable below 1.5 GiB before launch",
            "status": "not_launched", "a06_pass": False,
        })
        return 5
    interrupted = []
    previous_handlers = {}
    for sig in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[sig] = signal.signal(sig, lambda value, frame: interrupted.append(value))
    proc = None
    failure = None
    begin_ns = time.monotonic_ns()
    last_cpu = None
    identity = None
    next_sample_ns = begin_ns
    count = 0
    min_available = None
    record = None
    try:
        with (root / "stdout.log").open("wb") as out, (root / "stderr.log").open("wb") as err, \
             (root / "supervisor-resources.jsonl").open("w", encoding="utf-8") as samples:
            proc = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
            while True:
                now = time.monotonic_ns()
                system = system_sample()
                min_available = system["mem_available_kib"] if min_available is None else min(
                    min_available, system["mem_available_kib"])
                if system["mem_available_kib"] < MEMORY_FLOOR_KIB:
                    failure = "MemAvailable below 1.5 GiB"
                if now - begin_ns > args.timeout * 1_000_000_000:
                    failure = "process deadline"
                if interrupted:
                    failure = f"supervisor interrupted by signal {interrupted[0]}"
                code = proc.poll()
                if now >= next_sample_ns or code is not None or failure:
                    ps = process_sample(proc.pid)
                    if "starttime_ticks" in ps:
                        identity = identity if identity is not None else ps["starttime_ticks"]
                        if identity != ps["starttime_ticks"]:
                            failure = "child PID identity changed"
                        if last_cpu:
                            dt = (now - last_cpu[0]) / 1e9
                            ps["cpu_percent"] = 100 * (ps["cpu_ticks"] - last_cpu[1]) / clock_ticks / dt if dt > 0 else None
                        else:
                            ps["cpu_percent"] = None
                        last_cpu = (now, ps["cpu_ticks"])
                    row = {"monotonic_ns": now, "elapsed_s": (now-begin_ns)/1e9,
                           "pid": proc.pid, **system, **ps,
                           "sensors": {name: read_int(path) for name, path in paths.items()}}
                    samples.write(json.dumps(row, ensure_ascii=False) + "\n")
                    samples.flush()
                    count += 1
                    next_sample_ns = now + 1_000_000_000
                if failure:
                    stop_group(proc)
                    break
                if code is not None:
                    break
                time.sleep(0.05)
    except BaseException as exc:
        failure = f"supervisor exception: {type(exc).__name__}: {exc}"
        if proc is not None:
            stop_group(proc)
    finally:
        record = {
            "command": command, "returncode": proc.returncode if proc is not None else None,
            "begin_ns": begin_ns, "end_ns": time.monotonic_ns(),
            "supervisor_failure": failure, "process_group": proc.pid if proc is not None else None,
            "child_starttime_ticks": identity, "samples": count,
            "min_mem_available_kib": min_available,
            "scope": "A06 four-model coexistence only; no P3 full-chain or audio-device acceptance",
            "smoke": args.smoke,
            "warmup_rounds": args.warmup_rounds,
            "swap_policy": args.swap_policy,
        }
        record["elapsed_s"] = (record["end_ns"]-begin_ns)/1e9
        write_json(root / "supervisor.json", record)
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)
    print(json.dumps(record, ensure_ascii=False))
    return 5 if failure else (record["returncode"] if record["returncode"] is not None else 5)


if __name__ == "__main__":
    raise SystemExit(main())
