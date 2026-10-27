#!/usr/bin/env python3
"""Opt-in A14: ten uninterrupted completed turns, one cancel, one service recovery.
Private audio and frozen production assets; no headset, desktop or reboot operations.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from deployment_runtime import PROJECT, RunProcess, deploy, systemctl, wait_for
from audio_probe import PrivatePulse
from wake_runtime import Feeder, Evidence, pcm, save


class Telemetry:
    """Read-only process/host samples. Swap is deliberately not a pass gate."""
    def __init__(self, pid, output):
        self.pid, self.output = pid, output
        self.done = threading.Event()
        self.rows = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        previous = None
        with self.output.open("w") as stream:
            while not self.done.is_set():
                now = time.monotonic()
                row = {"monotonic_ns": time.monotonic_ns(), "pid": self.pid}
                try:
                    proc = Path("/proc") / str(self.pid)
                    values = {}
                    for line in (proc / "status").read_text().splitlines():
                        if ":" in line:
                            k, v = line.split(":", 1)
                            if k in ("VmRSS", "Threads"):
                                values[k] = int(v.split()[0])
                    row.update(rss_kib=values["VmRSS"], threads=values["Threads"],
                               fds=len(list((proc / "fd").iterdir())))
                    try:
                        row["pss_kib"] = next(int(s.split()[1]) for s in
                            (proc / "smaps_rollup").read_text().splitlines() if s.startswith("Pss:"))
                    except (OSError, StopIteration):
                        row["pss_kib"] = None
                    stat = (proc / "stat").read_text().rsplit(")", 1)[1].split()
                    ticks = int(stat[11]) + int(stat[12])
                    if previous:
                        row["cpu_percent"] = 100 * (ticks - previous[1]) / os.sysconf("SC_CLK_TCK") / (now - previous[0])
                    previous = (now, ticks)
                    row["mem_available_kib"] = next(int(s.split()[1]) for s in
                        Path("/proc/meminfo").read_text().splitlines() if s.startswith("MemAvailable:"))
                    row["disk_free_bytes"] = shutil.disk_usage(PROJECT).free
                    row["temperatures_millic"] = {p.parent.name: int(p.read_text())
                        for p in Path("/sys/class/thermal").glob("thermal_zone*/temp")}
                    row["cpu_khz"] = {p.parent.name: int(p.read_text())
                        for p in Path("/sys/devices/system/cpu/cpufreq").glob("policy*/scaling_cur_freq")}
                except (OSError, ValueError, KeyError) as error:
                    row["observation_error"] = str(error)
                self.rows.append(row)
                stream.write(json.dumps(row) + "\n")
                stream.flush()
                self.done.wait(1)

    def close(self):
        self.done.set()
        self.thread.join(timeout=3)


def play(feeder, data):
    with feeder.lock:
        pending = len(feeder.frames)
    if pending:
        time.sleep(pending * .02 + .1)
    feeder.play(data)


def ready(root, release, old_id=None):
    def started():
        path = root / "status.json"
        if not path.exists():
            return None
        state = json.loads(path.read_text())
        return state if (state.get("status") == "running" and state.get("release") == release
                         and state.get("run_id") != old_id) else None
    state = wait_for(started, 180)
    ev = Evidence(root / "runs" / state["run_id"], RunProcess(root, state["run_id"]))
    armed = ev.wait("wake_listening", seconds=180)
    time.sleep(1)
    ev.refresh()
    waits = [e for e in ev.events if e["event"] == "device_wait" and e["seq"] > armed["seq"]]
    if waits:
        armed = ev.wait("wake_listening", waits[-1]["seq"], 15)
    else:
        armed = max((e for e in ev.events if e["event"] == "wake_listening"), key=lambda e: e["seq"])
    return state, ev, armed


def wake_window(ev, armed, feeder, wake):
    play(feeder, wake)
    detected = ev.wait("wake_detected", armed["seq"], 15)
    return ev.wait("dialogue_window_opened", detected["seq"], 5)


def answer(ev, opened, feeder, query):
    play(feeder, query)
    completed = ev.wait("turn_completed", opened["seq"], 120, lambda e: not e["data"]["empty_input"])
    assert completed["data"]["history_committed"], completed
    selected = [e for e in ev.events if opened["seq"] <= e["seq"] <= completed["seq"]]
    assert not any(e["event"] in ("model_error", "restart_required", "barge_in_detected") for e in selected)
    timing = next(e["data"] for e in selected if e["event"] == "playback_drain_requested")
    assert timing["tts_speed"] == .85 and timing["tts_prebuffer_ms"] == 1500
    assert not timing["tts_first_segment_override"] and not timing["tts_sentence_mode"]
    assert all((x["min_chars"], x["target_chars"], x["boundary_lookahead_chars"]) == (12, 12, 2)
               for x in timing["tts_segment_timings"])
    return completed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=deploy.DEFAULT_ROOT)
    parser.add_argument("--wake-wav", type=Path, required=True)
    parser.add_argument("--query-wav", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--batch-id", required=True)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt("SIGTERM")))
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.root.resolve()
    production, info = deploy.release_info(root)
    pointers = {p: (root / p).resolve().name for p in ("current", "previous")}
    suffix = deploy.checked_id(args.batch_id)
    release_id = "p7-a14-private-" + suffix
    unit = "rk3588-voice-engine-a14-" + suffix
    report = {"status": "running", "scope": "10 completed continuous turns + 1 cancellation; separate fault recovery",
              "production_release": info["id"], "binary_sha256": info["files"]["bin/voice_engine"]["sha256"],
              "asset_pool": info["asset_pool"], "rounds": [], "excluded": [
                  "headset", "headless_login", "desktop_logout", "system_reboot", "boot_autostart",
                  "acoustic_timing", "swap_gate", "one_hour_soak"]}
    save(args.output / "review.json", report)
    feeder = monitor = None
    launched = False
    try:
        assert "ActiveState=inactive" in systemctl("show", "rk3588-voice-engine", "-p", "ActiveState")
        assert hashlib.sha256((PROJECT / "scripts/deploy.py").read_bytes()).hexdigest() == info["files"]["deploy.py"]["sha256"]
        config = production / "config/engine.yaml"
        original = deploy.read_config(config)
        settings = json.loads(json.dumps(original))
        settings["audio"].update(source="ve_a14_source", sink="ve_test_sink", profile="none")
        for key in ("models_manifest", "deps_manifest"):
            settings["assets"][key] = str((config.parent / settings["assets"][key]).resolve())
        settings["dialogue"]["keywords_file"] = str((config.parent / settings["dialogue"]["keywords_file"]).resolve())
        restored = json.loads(json.dumps(settings))
        restored["assets"] = original["assets"]
        restored["dialogue"]["keywords_file"] = original["dialogue"]["keywords_file"]
        for key in ("source", "sink", "profile"):
            restored["audio"][key] = original["audio"][key]
        assert restored == original
        private = args.output / "private-config.yaml"
        save(private, settings)
        report["staged"] = deploy.stage(root, PROJECT, production / "bin/voice_engine", private, release_id)
        report["input_hashes"] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.wake_wav, args.query_wav)}
        wake, query = pcm(args.wake_wav), pcm(args.query_wav)
        with tempfile.TemporaryDirectory(prefix="ve-p7-a14-") as temporary:
            with PrivatePulse(Path(temporary)) as server:
                fifo = Path(temporary) / "input.fifo"
                server.command(["pactl", "load-module", "module-pipe-source", "source_name=ve_a14_source",
                    "file=" + str(fifo), "format=s16le", "rate=16000", "channels=1", "channel_map=mono"], timeout=5)
                feeder = Feeder(fifo)
                # Use the shipped restart/sandbox settings; omit only device-service ordering and ExecStart.
                template = (PROJECT / "packaging/rk3588-voice-engine.service.in").read_text()
                keys = ("Type", "StartLimitIntervalSec", "StartLimitBurst", "UMask", "NoNewPrivileges",
                        "RestrictAddressFamilies", "Restart", "RestartPreventExitStatus", "RestartSec",
                        "TimeoutStopSec", "KillMode")
                properties = [s for s in template.splitlines() if s.split("=", 1)[0] in keys]
                command = ["systemd-run", "--user", "--unit=" + unit] + ["--property=" + s for s in properties]
                for key, value in server.env.items():
                    if key.startswith(("XDG_", "PULSE_", "PIPEWIRE_", "WIREPLUMBER_", "DBUS_")):
                        command.append("--setenv=" + key + "=" + value)
                command += [sys.executable, str(root / "current/deploy.py"), "--root", str(root),
                            "serve", "--release", release_id]
                launch = subprocess.run(command, capture_output=True, text=True, timeout=20)
                save(args.output / "launch.json", {"command": command, "returncode": launch.returncode,
                     "stdout": launch.stdout, "stderr": launch.stderr})
                assert launch.returncode == 0, launch.stderr
                launched = True
                try:
                    state, ev, armed = ready(root, release_id)
                    report["continuous_run"] = state
                    monitor = Telemetry(state["engine_pid"], args.output / "resources.jsonl")
                    report["continuous_started_monotonic_ns"] = time.monotonic_ns()
                    opened = wake_window(ev, armed, feeder, wake)
                    for index in range(10):
                        completed = answer(ev, opened, feeder, query)
                        report["rounds"].append({"round": index + 1, "session_id": completed["session_id"],
                            "generation_id": completed["generation_id"], "seq": completed["seq"],
                            "monotonic_ns": completed["monotonic_ns"], "completed": True})
                        print("A14 continuous turn %d/10 completed" % (index + 1), flush=True)
                        save(args.output / "review.json", report)
                        if index < 9:
                            opened = ev.wait("dialogue_window_opened", completed["seq"], 10)
                        if index == 4:
                            play(feeder, query)
                            played = ev.wait("audio_event", opened["seq"], 90,
                                lambda e: e["data"]["type"] == "playback_started")
                            deploy.send(root, "cancel")
                            requested = ev.wait("cancel_requested", played["seq"], 5)
                            barrier = ev.wait("cancel_completed", requested["seq"], 5)
                            armed = ev.wait("wake_listening", barrier["seq"], 5)
                            report["cancellation"] = {"generation_id": played["generation_id"],
                                "requested_seq": requested["seq"], "barrier_seq": barrier["seq"],
                                "completed": True, "counted_as_completed_turn": False}
                            print("A14 cancellation 1/1 completed", flush=True)
                            opened = wake_window(ev, armed, feeder, wake)
                    report["continuous_finished_monotonic_ns"] = time.monotonic_ns()
                    assert "NRestarts=0" in systemctl("show", unit, "-p", "NRestarts")
                    ev.refresh()
                    before_fault = list(ev.events)
                    cancel = report["cancellation"]
                    assert not [e for e in before_fault if e["seq"] > cancel["barrier_seq"]
                        and e["generation_id"] == cancel["generation_id"]
                        and e["event"] in ("playback_submit", "turn_completed")]
                    report["cancellation"]["old_generation_commits_after_barrier"] = 0
                    assert not any(e["event"] in ("model_error", "restart_required") for e in before_fault)
                    samples = [e["data"] for e in before_fault if e["event"] == "resource_sample"]
                    report["queues"] = {key: max(s[key] for s in samples) for key in
                        ("capture_overflows", "playback_overflows", "playback_underruns", "capture_holes",
                         "dropped_events", "capture_peak_frames", "playback_peak_frames")}
                    assert all(report["queues"][key] == 0 for key in
                        ("capture_overflows", "playback_overflows", "playback_underruns", "capture_holes", "dropped_events"))
                    monitor.close()
                    rows = [r for r in monitor.rows if "observation_error" not in r]
                    assert rows and min(r["mem_available_kib"] for r in rows) >= 1572864
                    assert min(r["disk_free_bytes"] for r in rows) >= 3 * 1024 ** 3
                    report["resources"] = {"samples": len(rows),
                        "rss_min_kib": min(r["rss_kib"] for r in rows), "rss_max_kib": max(r["rss_kib"] for r in rows),
                        "rss_first_kib": rows[0]["rss_kib"], "rss_last_kib": rows[-1]["rss_kib"],
                        "mem_available_min_kib": min(r["mem_available_kib"] for r in rows),
                        "threads_max": max(r["threads"] for r in rows), "fds_max": max(r["fds"] for r in rows),
                        "temperature_max_millic": max((v for r in rows for v in r["temperatures_millic"].values()), default=None),
                        "scope": "short observation only; no one-hour leak claim"}
                    save(args.output / "review.json", report)
                    # Explicitly separate the one injected process failure from the continuous interval.
                    report["fault_injection"] = {"signal": "SIGKILL", "engine_pid": state["engine_pid"],
                                               "monotonic_ns": time.monotonic_ns()}
                    print("A14 continuous segment passed; injecting one separate engine exit", flush=True)
                    os.kill(state["engine_pid"], signal.SIGKILL)
                    recovery, recovered_ev, recovered_armed = ready(root, release_id, state["run_id"])
                    outcome = json.loads((root / "runs" / state["run_id"] / "exit.json").read_text())
                    assert outcome["exit_code"] == -signal.SIGKILL, outcome
                    assert recovery["engine_pid"] != state["engine_pid"]
                    props = systemctl("show", unit, "-p", "NRestarts", "-p", "Restart", "-p", "RestartUSec",
                                      "-p", "StartLimitIntervalUSec", "-p", "StartLimitBurst")
                    assert "NRestarts=1" in props
                    opened = wake_window(recovered_ev, recovered_armed, feeder, wake)
                    complete = answer(recovered_ev, opened, feeder, query)
                    report["recovery"] = {"run": recovery, "injected_exit": outcome, "properties": props,
                                          "completed_turns": 1, "generation_id": complete["generation_id"],
                                          "counted_in_continuous_turns": False}
                    print("A14 automatic restart 1/1 and recovery answer 1/1 completed", flush=True)
                    deploy.send(root, "quit")
                    exit_path = root / "runs" / recovery["run_id"] / "exit.json"
                    wait_for(lambda: exit_path.exists(), 20)
                    report["recovery"]["exit"] = json.loads(exit_path.read_text())
                    assert report["recovery"]["exit"]["exit_code"] == 0
                    report["production_pointers_unchanged"] = pointers == {p: (root / p).resolve().name for p in pointers}
                    assert report["production_pointers_unchanged"]
                    report["managed_logs_bytes"] = sum(p.stat().st_size for p in (root / "runs").rglob("*") if p.is_file())
                    assert report["managed_logs_bytes"] <= info["log_limit_bytes"]
                    report["status"] = "passed"
                    report["total_continuous_completed_turns"] = 10
                    save(args.output / "review.json", report)
                    print("A14_AUTOMATION_PASSED", flush=True)
                finally:
                    if monitor:
                        monitor.close()
                    if launched:
                        subprocess.run(["systemctl", "--user", "stop", unit], capture_output=True, timeout=25)
                    feeder.close()
                    feeder = None
    except BaseException as error:
        report.update(status="failed", error=repr(error))
        save(args.output / "review.json", report)
        raise
    finally:
        if monitor:
            monitor.close()
        if feeder:
            try:
                feeder.close()
            except RuntimeError:
                pass
        journal = subprocess.run(["journalctl", "--user", "-u", unit, "--no-pager", "-n", "160"],
                                 capture_output=True, text=True, timeout=20)
        (args.output / "service-journal.log").write_text(journal.stdout + journal.stderr)


if __name__ == "__main__":
    main()
