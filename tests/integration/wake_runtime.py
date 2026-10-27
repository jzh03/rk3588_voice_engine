#!/usr/bin/env python3
"""Opt-in five-model P5 check on private audio. Not registered in CTest.
Run under the project model lock; no headset or user audio service is touched.
Requires a prepared positive wake WAV and an existing fixed question WAV.
"""
import argparse
import collections
import errno
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading
import time
import wave
import yaml
from audio_probe import PrivatePulse
from coexist_validate import system_sample, process_sample, sensor_paths, read_int, MEMORY_FLOOR_KIB

ROOT = Path(__file__).resolve().parents[2]

def save(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

def pcm(path):
    with wave.open(str(path), "rb") as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (16000, 1, 2)
        assert 0 < w.getnframes() <= 16000 * 30
        return w.readframes(w.getnframes())

class Feeder:
    def __init__(self, path):
        self.path = path
        self.frames = collections.deque()
        self.lock = threading.Lock()
        self.done = threading.Event()
        self.error = None
        self.pause_until = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
    def play(self, data):
        with self.lock:
            if self.frames:
                raise RuntimeError("previous clip still queued")
            self.frames.extend(data[n:n+640].ljust(640, b"\0") for n in range(0, len(data), 640))
    def pause(self, seconds):
        with self.lock:
            if self.frames:
                raise RuntimeError("cannot inject capture gap while speech is queued")
            self.pause_until = time.monotonic() + seconds
    def run(self):
        fd = None
        try:
            while not self.done.is_set() and fd is None:
                try:
                    fd = os.open(self.path, os.O_WRONLY | os.O_NONBLOCK)
                except OSError as e:
                    if e.errno != errno.ENXIO:
                        raise
                    self.done.wait(.01)
            next_tick = time.monotonic()
            while not self.done.is_set():
                with self.lock:
                    remaining = self.pause_until - time.monotonic()
                if remaining > 0:
                    self.done.wait(min(.02, remaining))
                    continue
                with self.lock:
                    data = self.frames.popleft() if self.frames else b"\0" * 640
                offset = 0
                while offset < len(data) and not self.done.is_set():
                    try:
                        offset += os.write(fd, data[offset:])
                    except BlockingIOError:
                        self.done.wait(.003)
                next_tick = max(next_tick + .02, time.monotonic())
                self.done.wait(max(0, next_tick - time.monotonic()))
        except BaseException as e:
            self.error = repr(e)
        finally:
            if fd is not None:
                os.close(fd)
    def close(self):
        self.done.set()
        self.thread.join(3)
        if self.thread.is_alive() or self.error:
            raise RuntimeError(self.error or "feeder stop timeout")

class ResourceSupervisor:
    """Independent one-second system/process/sensor observations; no tuning."""
    def __init__(self, root):
        self.root, self.process = root, None
        self.done = threading.Event()
        self.error = None
        self.count, self.minimum = 0, None
        self.starttime = None
        self.paths = sensor_paths()
        self.begin_ns = time.monotonic_ns()
        save(root / "sensor-sources.json", {
            "interval_seconds": 1, "sensors": self.paths,
            "process_faults": "major faults also include file faults; do not equate with swap-in",
            "process_swap": "VmSwap is current swapped anonymous memory, not cumulative I/O",
            "npu_memory": "No validated unprivileged per-process NPU memory counter"})
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
    @staticmethod
    def processes():
        rows = []
        for directory in Path("/proc").iterdir():
            if not directory.name.isdecimal():
                continue
            try:
                fields = {}
                for line in (directory / "status").read_text().splitlines():
                    key, _, tail = line.partition(":")
                    if key in ("Name", "VmSwap", "VmRSS"):
                        fields[key] = tail.strip() if key == "Name" else int(tail.split()[0])
                stat = (directory / "stat").read_text()
                values = stat[stat.rfind(")") + 2:].split()
                major = int(values[9])
                if fields.get("VmSwap", 0) or major:
                    rows.append({"pid": int(directory.name), "name": fields.get("Name"),
                        "starttime_ticks": int(values[19]), "major_faults": major,
                        "vm_swap_kib": fields.get("VmSwap", 0), "rss_kib": fields.get("VmRSS", 0)})
            except (OSError, ValueError, IndexError):
                continue
        return rows
    def sample(self):
        now = time.monotonic_ns()
        system = system_sample()
        self.minimum = system["mem_available_kib"] if self.minimum is None else min(
            self.minimum, system["mem_available_kib"])
        proc = self.process
        process = process_sample(proc.pid) if proc else {}
        if "starttime_ticks" in process:
            if self.starttime is None:
                self.starttime = process["starttime_ticks"]
            if process["starttime_ticks"] != self.starttime:
                raise RuntimeError("engine PID identity changed")
        try:
            pressure = {"readable": True, "value": Path("/proc/pressure/memory").read_text().strip()}
        except OSError as error:
            pressure = {"readable": False, "reason": type(error).__name__}
        return {"monotonic_ns": now, "engine_pid": proc.pid if proc else None,
            "system": system, "engine": process, "processes": self.processes(),
            "memory_pressure": pressure,
            "sensors": {name: read_int(path) for name, path in self.paths.items()}}
    def run(self):
        try:
            with (self.root / "supervisor-resources.jsonl").open("w") as out:
                next_sample = time.monotonic()
                while True:
                    row = self.sample()
                    out.write(json.dumps(row) + "\n"); out.flush(); self.count += 1
                    if row["system"]["mem_available_kib"] < MEMORY_FLOOR_KIB:
                        raise RuntimeError("independent supervisor memory floor breached")
                    if time.monotonic_ns() - self.begin_ns > 600_000_000_000:
                        raise RuntimeError("independent supervisor deadline")
                    if self.done.is_set():
                        break
                    next_sample += 1
                    self.done.wait(max(0, next_sample-time.monotonic()))
        except BaseException as error:
            self.error = repr(error)
            proc = self.process
            if proc and proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
    def close(self):
        self.done.set(); self.thread.join(5)
        if self.thread.is_alive():
            self.error = "resource supervisor stop timeout"
        result = {"error": self.error, "samples": self.count,
            "min_mem_available_kib": self.minimum, "engine_starttime_ticks": self.starttime,
            "begin_ns": self.begin_ns, "end_ns": time.monotonic_ns()}
        save(self.root / "supervisor.json", result)
        return result

class Evidence:
    def __init__(self, root, process):
        self.root, self.process = root, process
        self.events, self.readers = [], {}
        self.seq = 0
    def refresh(self):
        new = []
        for p in (self.root / "engine").glob("events*.jsonl"):
            key = p.stat().st_ino
            offset, pending = self.readers.get(key, (0, ""))
            with p.open() as f:
                f.seek(offset)
                pending += f.read()
                offset = f.tell()
            while "\n" in pending:
                line, pending = pending.split("\n", 1)
                if line:
                    e = json.loads(line)
                    if e["seq"] > self.seq:
                        new.append(e)
            self.readers[key] = offset, pending
        for e in sorted(new, key=lambda e: e["seq"]):
            if e["seq"] > self.seq:
                self.events.append(e)
                self.seq = e["seq"]
    def wait(self, name, after=0, seconds=90, predicate=None):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.refresh()
            for e in self.events:
                if e["seq"] > after and e["event"] == name and (predicate is None or predicate(e)):
                    return e
            if self.process.poll() is not None:
                raise RuntimeError("engine exited: " + str(self.process.returncode))
            time.sleep(.025)
        raise TimeoutError(name)
    def resources(self):
        self.refresh()
        rows = [e for e in self.events if e["event"] == "resource_sample"]
        warm = next((e["monotonic_ns"] for e in self.events if e["event"] == "turn_completed"
                     and not e["data"]["empty_input"]), 0)
        warm_rows = [e for e in rows if e["monotonic_ns"] > warm]
        deltas = [{"monotonic_ns": b["monotonic_ns"],
                   "in": b["data"]["swap_in_pages"] - a["data"]["swap_in_pages"],
                   "out": b["data"]["swap_out_pages"] - a["data"]["swap_out_pages"]}
                  for a, b in zip(warm_rows, warm_rows[1:])]
        return {"sample_count": len(rows),
            "mem_available_min_kib": min(e["data"]["mem_available_kib"] for e in rows),
            "rss_peak_kib": max(e["data"]["rss_kib"] for e in rows),
            "post_first_turn_swap_deltas": deltas,
            "capture_overflows": max(e["data"]["capture_overflows"] for e in rows),
            "playback_overflows": max(e["data"]["playback_overflows"] for e in rows),
            "playback_underruns": max(e["data"]["playback_underruns"] for e in rows),
            "playback_keepalive_underruns": max(e["data"]["playback_keepalive_underruns"] for e in rows),
            "capture_queue_peak_frames": max(e["data"]["capture_peak_frames"] for e in rows),
            "playback_queue_peak_frames": max(e["data"]["playback_peak_frames"] for e in rows)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--wake-wav", type=Path, required=True)
    parser.add_argument("--query-wav", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--formal-a06", action="store_true",
        help="one warmup, five measured answers over >=300s, independent resource supervisor")
    args = parser.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt("signal " + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    args.output.mkdir(parents=True, exist_ok=False)
    wake, question = pcm(args.wake_wav), pcm(args.query_wav)
    settings = yaml.safe_load(args.config.read_text())
    frozen = yaml.safe_load((ROOT / "config/realtime-local.yaml").read_text())
    for key in ("tts", "llm", "threads"):
        assert settings[key] == frozen[key], "existing tuning changed"
    original_audio = dict(settings["audio"])
    settings["audio"].update(source="ve_wake_source", sink="ve_test_sink", profile="none")
    for key in ("models_manifest", "deps_manifest"):
        settings["assets"][key] = str((args.config.parent / settings["assets"][key]).resolve())
    settings["dialogue"]["keywords_file"] = str((args.config.parent / settings["dialogue"]["keywords_file"]).resolve())
    assert settings["dialogue"]["window_ms"] == 15000 and not settings["dialogue"]["barge_in"]
    save(args.output / "config.yaml", settings)
    report = {"status": "running", "mode": "private_audio_real_models_not_physical_A11",
              "input_hashes": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in (args.wake_wav, args.query_wav)},
              "scope": "one >=300s five-model run, five answers, two wakes, two silent windows",
              "results": []}
    save(args.output / "result.json", report)
    proc = feeder = evidence = supervisor = None
    logs = []
    try:
        if args.formal_a06:
            report["scope"] = "one complete warmup; >=300s measurement with five answers; two silent windows, one 1.75s private capture gap"
            report["warmup_results"] = []
            supervisor = ResourceSupervisor(args.output)
            time.sleep(10)  # Predeclared background baseline, no engine resident.
            if supervisor.error:
                raise RuntimeError(supervisor.error)
        with tempfile.TemporaryDirectory(prefix="ve-p5-") as temporary:
            with PrivatePulse(Path(temporary)) as server:
                fifo = Path(temporary) / "input.fifo"
                server.command(["pactl", "load-module", "module-pipe-source",
                    "source_name=ve_wake_source", "file=" + str(fifo), "format=s16le",
                    "rate=16000", "channels=1", "channel_map=mono"], timeout=5)
                feeder = Feeder(fifo)
                logs = [(args.output / name).open("wb") for name in ("stdout.log", "stderr.log")]
                command = [str(ROOT / "build/model-probes/bin/voice_engine"), "--config",
                    str(args.output / "config.yaml"), "--control", "stdin", "--run-id", "engine",
                    "--output-dir", str(args.output)]
                report["command"] = command
                proc = subprocess.Popen(command, cwd=ROOT, env=server.env, stdin=subprocess.PIPE,
                    stdout=logs[0], stderr=logs[1], start_new_session=True)
                if supervisor:
                    supervisor.process = proc
                report["engine_pid"] = proc.pid
                evidence = Evidence(args.output, proc)
                try:
                    armed = evidence.wait("wake_listening", seconds=150)
                    resident_start = time.monotonic()
                    report["measurement_start_ns"] = time.monotonic_ns()
                    (args.output / "loaded-maps.txt").write_text(Path(f"/proc/{proc.pid}/maps").read_text())
                    wake_sessions = []
                    after = armed["seq"]
                    for index in range(6 if args.formal_a06 else 5):
                        warmup = args.formal_a06 and index == 0
                        measured_index = index - 1 if args.formal_a06 else index
                        if index == 0 or measured_index in (0, 2):
                            feeder.play(wake)
                            detected = evidence.wait("wake_detected", after, 15)
                            wake_sessions.append(detected["session_id"])
                            opened = evidence.wait("dialogue_window_opened", detected["seq"], 5)
                        else:
                            opened = evidence.wait("dialogue_window_opened", after, 5)
                        # Allow any unused synthetic wake suffix to leave the FIFO;
                        # real interaction begins after the LISTENING cue.
                        with feeder.lock:
                            pending = len(feeder.frames)
                        if pending:
                            time.sleep(pending * .02 + .1)
                        feeder.play(question)
                        complete = evidence.wait("turn_completed", opened["seq"], 90)
                        assert complete["data"]["history_committed"] and not complete["data"]["empty_input"]
                        selected = [e for e in evidence.events if opened["seq"] <= e["seq"] <= complete["seq"]]
                        timing = next(e["data"] for e in selected if e["event"] == "playback_drain_requested")
                        assert timing["tts_speed"] == .85 and timing["tts_prebuffer_ms"] == 1500
                        assert not timing["tts_first_segment_override"] and not timing["tts_sentence_mode"]
                        assert all((t["min_chars"], t["target_chars"], t["boundary_lookahead_chars"]) == (12,12,2)
                                   for t in timing["tts_segment_timings"])
                        result = {"round": measured_index + 1, "session_id": complete["session_id"],
                            "generation_id": complete["generation_id"], "completed": True}
                        report["warmup_results" if warmup else "results"].append(result)
                        print("warmup completed" if warmup else "completed " + str(measured_index + 1), flush=True)
                        after = complete["seq"]
                        if warmup:
                            proc.stdin.write(b"cancel\n"); proc.stdin.flush()
                            cancelled = evidence.wait("cancel_completed", after, 5)
                            armed = evidence.wait("wake_listening", cancelled["seq"], 5)
                            after = armed["seq"]
                            resident_start = time.monotonic()
                            report["measurement_start_ns"] = time.monotonic_ns()
                            continue
                        if measured_index in (1, 4):
                            wait_start = evidence.wait("dialogue_window_opened", after, 5)
                            if args.formal_a06 and measured_index == 4:
                                report["capture_gap_injection"] = {
                                    "requested_seconds": 1.75, "monotonic_ns": time.monotonic_ns(),
                                    "generation_id": wait_start["generation_id"]}
                                feeder.pause(1.75)
                            end = evidence.wait("dialogue_window_closed", wait_start["seq"], 25)
                            elapsed = (end["monotonic_ns"] - wait_start["monotonic_ns"]) / 1e9
                            assert 14.5 <= elapsed <= (15.25 if args.formal_a06 else 18), elapsed
                            endpoint = next(e for e in evidence.events
                                if e["event"] == "endpoint" and
                                e["generation_id"] == wait_start["generation_id"])
                            report.setdefault("silent_window_details", []).append({
                                "generation_id": wait_start["generation_id"],
                                "opened_ns": wait_start["data"]["opened_ns"],
                                "closed_ns": end["monotonic_ns"],
                                "endpoint_samples": endpoint["data"].get("endpoint_sample"),
                                "wall_seconds": elapsed})
                            report.setdefault("silent_window_seconds", []).append(elapsed)
                            armed = evidence.wait("wake_listening", end["seq"], 5)
                            after = armed["seq"]
                    assert len(wake_sessions) == (3 if args.formal_a06 else 2)
                    assert all(a < b for a, b in zip(wake_sessions, wake_sessions[1:]))
                    while time.monotonic() - resident_start < 300:
                        if proc.poll() is not None or feeder.error or (supervisor and supervisor.error):
                            raise RuntimeError("engine/feeder stopped during residency")
                        evidence.refresh()
                        time.sleep(.2)
                    report["resident_seconds"] = time.monotonic() - resident_start
                    report["measurement_end_ns"] = time.monotonic_ns()
                    report["wake_session_ids"] = wake_sessions
                    report["resources"] = evidence.resources()
                    assert report["resources"]["mem_available_min_kib"] >= 1572864
                    assert report["resources"]["capture_overflows"] == 0
                    assert report["resources"]["playback_overflows"] == 0
                    assert report["resources"]["capture_queue_peak_frames"] <= 99
                    assert report["resources"]["playback_queue_peak_frames"] <= 50
                    assert len([e for e in evidence.events if e["event"] == "wake_detected"]) == (3 if args.formal_a06 else 2)
                    assert not [e for e in evidence.events if e["event"] in ("turn_failed", "restart_required", "pipeline_error")]
                    report["status"] = "passed"
                finally:
                    if proc.poll() is None:
                        proc.stdin.write(b"quit\n"); proc.stdin.flush()
                        try:
                            proc.wait(timeout=8)
                        except subprocess.TimeoutExpired:
                            os.killpg(proc.pid, signal.SIGTERM)
                            try:
                                proc.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                os.killpg(proc.pid, signal.SIGKILL); proc.wait()
                    feeder.close(); feeder = None
                    report["exit_code"] = proc.returncode
                    if report["status"] == "passed" and proc.returncode != 0:
                        raise RuntimeError("nonzero clean shutdown")
    except BaseException as error:
        report["status"] = "failed"
        report["error"] = repr(error)
        raise
    finally:
        if feeder:
            feeder.close()
        if supervisor:
            report["supervisor"] = supervisor.close()
            if report["supervisor"]["error"]:
                report["status"] = "failed"
        for f in logs:
            f.close()
        if evidence:
            evidence.refresh()
            if any(e["event"] == "resource_sample" for e in evidence.events):
                report["resources"] = evidence.resources()
        save(args.output / "result.json", report)
    if report["status"] != "passed":
        raise RuntimeError(report.get("error", "supervisor failure"))
    print("WAKE_RUNTIME_OK", args.output, flush=True)

if __name__ == "__main__":
    main()
