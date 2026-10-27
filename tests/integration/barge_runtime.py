#!/usr/bin/env python3
"""Opt-in P5-02 smoke: one real-model insertion, one completed replacement.
Private audio server and existing fixed WAV only; no headset/acoustic acceptance.
Run under the project model-validation lock with an external bounded timeout.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import yaml
from audio_probe import PrivatePulse
from wake_runtime import Feeder, Evidence, pcm, save, ROOT

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--config", type=Path, required=True)
    p.add_argument("--query-wav", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt("signal " + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    args.output.mkdir(parents=True, exist_ok=False)
    settings = yaml.safe_load(args.config.read_text())
    frozen = yaml.safe_load((ROOT / "config/realtime-local.yaml").read_text())
    for key in ("tts", "llm", "threads"):
        assert settings[key] == frozen[key], "existing tuning changed"
    settings["audio"].update(source="ve_barge_source", sink="ve_test_sink", profile="none")
    for key in ("models_manifest", "deps_manifest"):
        settings["assets"][key] = str((args.config.parent / settings["assets"][key]).resolve())
    settings["dialogue"]["keywords_file"] = str((args.config.parent / settings["dialogue"]["keywords_file"]).resolve())
    settings["dialogue"]["barge_in"] = True
    assert settings["dialogue"].get("barge_in_min_ms", 250) == 250
    assert settings["dialogue"]["window_ms"] == 15000
    save(args.output / "config.yaml", settings)
    question = pcm(args.query_wav)
    report = {"status": "running", "scope": "one software playback insertion and completed replacement",
        "physical_A12": "not_run", "external_acoustic_timing": "removed_from_scope",
        "input_sha256": hashlib.sha256(args.query_wav.read_bytes()).hexdigest(),
        "binary_sha256": hashlib.sha256((ROOT / "build/model-probes/bin/voice_engine").read_bytes()).hexdigest()}
    proc = feeder = evidence = None
    try:
        with tempfile.TemporaryDirectory(prefix="ve-p5-barge-") as temporary:
            with PrivatePulse(Path(temporary)) as server:
                fifo = Path(temporary) / "input.fifo"
                server.command(["pactl", "load-module", "module-pipe-source",
                    "source_name=ve_barge_source", "file=" + str(fifo), "format=s16le",
                    "rate=16000", "channels=1", "channel_map=mono"], timeout=5)
                feeder = Feeder(fifo)
                with (args.output / "stdout.log").open("wb") as out, (args.output / "stderr.log").open("wb") as err:
                    command = [str(ROOT / "build/model-probes/bin/voice_engine"), "--config",
                        str(args.output / "config.yaml"), "--control", "stdin",
                        "--run-id", "engine", "--output-dir", str(args.output)]
                    report["command"] = command
                    proc = subprocess.Popen(command, cwd=ROOT, env=server.env, stdin=subprocess.PIPE,
                        stdout=out, stderr=err, start_new_session=True)
                    evidence = Evidence(args.output, proc)
                    def send(command):
                        proc.stdin.write((command + "\n").encode()); proc.stdin.flush()
                    try:
                        ready = evidence.wait("wake_listening", seconds=150)
                        # Reuse explicit start; KWS detection was completed in P5-01.
                        send("start")
                        opened = evidence.wait("dialogue_window_opened", ready["seq"], 6)
                        old = (opened["session_id"], opened["generation_id"])
                        feeder.play(question)
                        played = evidence.wait("audio_event", opened["seq"], 70,
                            lambda e: e["data"]["type"] == "playback_started" and
                            e["data"]["audio_generation_id"] == old[1])
                        report["injected_ns"] = time.monotonic_ns()
                        feeder.play(question)
                        detected = evidence.wait("barge_in_detected", played["seq"], 10)
                        assert (detected["session_id"], detected["generation_id"]) == old
                        cancelled = evidence.wait("cancel_requested", detected["seq"], 3,
                            lambda e: e["data"]["reason"] == "barge_in")
                        barrier = evidence.wait("cancel_completed", cancelled["seq"], 4)
                        resumed = evidence.wait("turn_started", barrier["seq"], 4)
                        assert resumed["session_id"] == old[0] and resumed["generation_id"] > old[1]
                        complete = evidence.wait("turn_completed", resumed["seq"], 90)
                        assert complete["data"]["history_committed"] and not complete["data"]["empty_input"]
                        reopened = evidence.wait("dialogue_window_opened", complete["seq"], 5)
                        evidence.refresh()
                        assert len([e for e in evidence.events if e["event"] == "barge_in_detected"]) == 1
                        assert not [e for e in evidence.events if e["seq"] > cancelled["seq"] and
                            e["event"] == "playback_submit" and e["generation_id"] == old[1]]
                        assert not [e for e in evidence.events if e["event"] == "turn_completed" and
                            e["generation_id"] == old[1]]
                        assert not [e for e in evidence.events if e["event"] in
                            ("turn_failed", "pipeline_error", "restart_required", "repeat_required")]
                        assert not [e for e in evidence.events if detected["seq"] < e["seq"] < reopened["seq"] and
                            e["event"] == "audio_event" and e["data"]["type"] in ("capture_stopped", "capture_stop_requested")]
                        resources = evidence.resources()
                        assert resources["capture_overflows"] == resources["playback_overflows"] == 0
                        assert resources["capture_queue_peak_frames"] <= 55
                        assert resources["playback_queue_peak_frames"] <= 50
                        assert resources["mem_available_min_kib"] >= 1572864
                        report.update(status="passed", detection=detected["data"],
                            cancelled_generation=old[1], replacement_generation=resumed["generation_id"],
                            old_generation_submissions_after_cancel=0,
                            replacement_history_committed=True, continuous_capture_handoff=True,
                            next_dialogue_window_opened=True,
                            resources={k:v for k,v in resources.items() if "swap" not in k})
                        print("BARGE_RUNTIME_OK: one insertion, old-generation isolation, replacement and continuation", flush=True)
                    finally:
                        if proc.poll() is None:
                            send("quit")
                            try:
                                proc.wait(timeout=8)
                            except subprocess.TimeoutExpired:
                                os.killpg(proc.pid, signal.SIGTERM)
                                try:
                                    proc.wait(timeout=5)
                                except subprocess.TimeoutExpired:
                                    os.killpg(proc.pid, signal.SIGKILL); proc.wait()
                        report["exit_code"] = proc.returncode
                        if evidence:
                            evidence.refresh()
                        feeder.close(); feeder = None
                        if report["status"] == "passed":
                            assert proc.returncode == 0, "engine did not exit normally"
    except BaseException as error:
        report.update(status="failed", error=repr(error))
        raise
    finally:
        if feeder:
            feeder.close()
        save(args.output / "review.json", report)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
