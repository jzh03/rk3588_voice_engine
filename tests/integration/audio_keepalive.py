#!/usr/bin/env python3
"""Exercise backend-lifetime silence using an isolated Pulse/PipeWire graph.

Checks actual sink and stream state, bounded queues, independent speech drain,
cancel/new generation, service reconnection, disabled/capture-only and cleanup.
This cannot establish Bluetooth headset acoustic readiness.
"""
import json
import math
import os
from pathlib import Path
import shutil
import signal
import struct
import sys
import tempfile
import time
import wave

from audio_probe import PrivatePulse, RunningProbe, check_summary, read_events, require, wait_for


def main():
    binary = str(Path(sys.argv[1]).resolve())
    evidence = os.environ.get("VE_KEEPALIVE_TEST_OUTPUT")
    root = Path(evidence) if evidence else Path(tempfile.mkdtemp(prefix="ve-keepalive-"))
    if evidence:
        root.mkdir(parents=True, exist_ok=False)
    private = Path(tempfile.mkdtemp(prefix="ve-keep-pulse-"))
    (root / "private-runtime-path.txt").write_text(str(private))
    completed = False
    observations = []
    try:
        for dep in ("pipewire", "pipewire-pulse", "wireplumber", "pactl"):
            require(shutil.which(dep), "missing dependency: " + dep)
        wav = root / "payload.wav"
        with wave.open(str(wav), "wb") as out:
            out.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
            out.writeframes(b"".join(struct.pack("<h", round(3000 * math.sin(i * 2 * math.pi * 440 / 16000)))
                                     for i in range(3851)))  # 13 frames including tail
        configs = {}
        for enabled in (False, True):
            config = root / ("enabled.yaml" if enabled else "disabled.yaml")
            config.write_text("audio:\n  source: ve_test_source\n  sink: ve_test_sink\n  profile: none\n"
                              "  playback_keepalive: " + str(enabled).lower() +
                              "\n  playback_lead_in_ms: 0\nqueues:\n  playback_ms: 20\n")
            configs[enabled] = config
        runs = root / "runs"
        with PrivatePulse(private) as server:
            def query(kind):
                return json.loads(server.command(["pactl", "--format=json", "list", kind]).stdout)

            def launch(name, enabled=True, capture=False):
                args = [binary, "--config", str(configs[enabled]),
                        "--capture-only" if capture else "--playback-only",
                        "--control", "stdin", "--duration-sec", "60", "--wait-device-sec", "3",
                        "--output-dir", str(runs), "--run-id", name]
                if not capture:
                    args += ["--play-wav", str(wav)]
                return RunningProbe(args, server.env)

            def keeper_inputs():
                return [i for i in query("sink-inputs")
                        if i.get("properties", {}).get("media.name") == "explicit earphone keepalive"]

            def assert_running(probe, label, speech_count=None):
                state = probe.status()
                require(state["playback_keepalive_active"], label + ": keepalive inactive")
                require(state["playback_keepalive_underruns"] == 0, label + ": keepalive underrun")
                require(state["playback_queue_peak_frames"] <= 1, label + ": speech queue exceeded")
                if speech_count is not None:
                    require(state["playback_frames"] == speech_count, label + ": unexpected speech")
                sink = next(s for s in query("sinks") if s["name"] == "ve_test_sink")
                require(sink["state"] == "RUNNING", label + ": actual sink not RUNNING")
                inputs = keeper_inputs()
                require(len(inputs) == 1 and not inputs[0]["corked"],
                        label + ": silent stream missing, duplicated or corked")
                require(inputs[0]["sink"] == sink["index"], label + ": wrong sink")
                # Requested bound is 100 ms at native 48 kHz mono float32.
                require(0 < state["playback_keepalive_buffer"]["maxlength"] <= 19200,
                        label + ": unbounded keepalive server buffer")
                observations.append({"label": label, "sink_state": sink["state"],
                                     "keepalive_input_index": inputs[0]["index"], "status": state})
                return state, inputs[0]["index"]

            for name, enabled, capture in (("disabled", False, False), ("capture-only", True, True)):
                probe = launch(name, enabled, capture)
                try:
                    probe.wait_text("AUDIO_READY")
                    state = probe.status()
                    require(not state["playback_keepalive_active"] and
                            state["playback_keepalive_native_frames"] == 0 and not keeper_inputs(),
                            name + ": unwanted output keepalive")
                    if capture:
                        require(not query("sink-inputs"), "capture-only opened a playback stream")
                        probe.send("start")
                        probe.wait_capture_progress()
                        probe.send("cancel")
                    probe.send("quit")
                    probe.finish()
                finally:
                    probe.close()
                    (root / (name + ".log")).write_bytes(probe.output)
                check_summary(runs / name)
                wait_for(lambda: not query("sink-inputs"), name + " stream cleanup")

            run = runs / "lifecycle"
            probe = launch(run.name)
            try:
                probe.wait_text("AUDIO_READY")
                wait_for(lambda: probe.status()["playback_keepalive_active"], "keepalive active")
                initial, keeper = assert_running(probe, "initial-idle", 0)
                for i in range(8):
                    time.sleep(1)
                    state, current = assert_running(probe, "idle-" + str(i + 1), 0)
                    require(current == keeper, "idle keepalive recreated")
                    require(not state["session_active"] and state["playback_queued_frames"] == 0,
                            "idle keepalive became a speech session")
                require(state["playback_keepalive_native_frames"] > initial["playback_keepalive_native_frames"],
                        "idle keepalive stopped supplying PCM")
                probe.send("start")
                wait_for(lambda: sum(e["event"] == "playback_drain_complete"
                                     for e in read_events(run)) == 1, "speech drain with keepalive")
                wait_for(lambda: not probe.status()["reset_pending"], "post-drain flush")
                for i in range(6):
                    time.sleep(1)
                    state, current = assert_running(probe, "after-drain-" + str(i + 1), 13)
                    require(current == keeper and not state["session_active"],
                            "drain stopped/recreated keepalive or left session active")

                probe.send("start")
                wait_for(lambda: sum(e["event"] == "playback_started" for e in read_events(run)) == 2,
                         "speech before cancel")
                probe.send("cancel")
                wait_for(lambda: not probe.status()["session_active"] and not probe.status()["reset_pending"],
                         "cancel acknowledgement")
                cancelled, current = assert_running(probe, "cancelled")
                require(current == keeper, "cancel restarted keepalive")
                time.sleep(1)
                after, _ = assert_running(probe, "cancelled-idle", cancelled["playback_frames"])
                require(after["playback_keepalive_native_frames"] > cancelled["playback_keepalive_native_frames"],
                        "cancel stopped silent PCM")

                probe.send("start")
                wait_for(lambda: sum(e["event"] == "playback_drain_complete"
                                     for e in read_events(run)) == 2, "fresh generation drain")
                expected = cancelled["playback_frames"] + 13
                assert_running(probe, "fresh-drain", expected)
                probe.send("start\nstop")
                wait_for(lambda: sum(e["event"] == "playback_drain_complete"
                                     for e in read_events(run)) == 3, "empty drain")
                assert_running(probe, "empty-drain", expected)

                starts_before = sum(e["event"] == "session_start" for e in read_events(run))
                server.stop()
                probe.wait_text("AUDIO_DEVICE_WAIT")
                state = probe.status()
                require(not state["playback_keepalive_active"] and not state["session_active"],
                        "disconnect retained active transport")
                server.start()
                wait_for(lambda: probe.status()["playback_keepalive_active"], "reconnected keepalive", timeout=12)
                state, _ = assert_running(probe, "reconnected-idle", expected)
                require(not state["session_active"] and state["playback_queued_frames"] == 0 and
                        state["reconnects"] >= 1, "reconnection replayed old session")
                require(sum(e["event"] == "session_start" for e in read_events(run)) == starts_before,
                        "reconnection auto-started speech")
                probe.send("start")
                wait_for(lambda: sum(e["event"] == "playback_drain_complete"
                                     for e in read_events(run)) == 4, "post-reconnect drain")
                assert_running(probe, "post-reconnect", expected + 13)
                probe.send("quit")
                probe.finish()
            finally:
                probe.close()
                (root / "lifecycle.log").write_bytes(probe.output)
            summary, events = check_summary(run)
            require(not any(e["event"].startswith("playback_lead_in_") for e in events),
                    "keepalive inserted a per-turn prefix")
            require(sum(e["event"] == "playback_keepalive_started" for e in events) == 2,
                    "keepalive lifetime did not match the two server connections")
            require(summary["status"]["playback_underruns"] == 0, "speech underrun hidden by silence")
            wait_for(lambda: not query("sink-inputs"), "normal exit stream release")
            wait_for(lambda: next(s for s in query("sinks") if s["name"] == "ve_test_sink")["state"] != "RUNNING",
                     "sink returned to idle after exit")

            probe = launch("signal-cleanup")
            try:
                probe.wait_text("AUDIO_READY")
                wait_for(lambda: probe.status()["playback_keepalive_active"], "keepalive before SIGTERM")
                probe.process.send_signal(signal.SIGTERM)
                probe.finish(expected_code=143)
            finally:
                probe.close()
                (root / "signal-cleanup.log").write_bytes(probe.output)
            check_summary(runs / "signal-cleanup", expected_code=143)
            wait_for(lambda: not query("sink-inputs"), "signal exit stream release")
        completed = True
        print("audio_keepalive: actual idle RUNNING, bounded silent buffer, independent drain/cancel,"
              " reconnect without old speech, defaults/capture-only and exit cleanup passed")
    finally:
        (root / "observations.json").write_text(json.dumps(observations, indent=2) + "\n")
        for source in private.rglob("*"):
            if source.is_file():
                target = root / "private" / source.relative_to(private)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        if completed:
            shutil.rmtree(private)
        if completed and not evidence:
            shutil.rmtree(root)
        else:
            print("audio_keepalive evidence: " + str(root))


if __name__ == "__main__":
    main()
