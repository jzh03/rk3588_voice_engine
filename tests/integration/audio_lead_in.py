#!/usr/bin/env python3
"""Real Pulse regression for lead-in timing, cancellation and tiny queues.

Only a private PipeWire graph is used. No models, user audio service or
physical devices are opened. Failures keep their temporary evidence.
"""
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

from audio_probe import PrivatePulse, RunningProbe, check_summary, read_events, require, wait_for


def main():
    binary = str(Path(sys.argv[1]).resolve())
    evidence = os.environ.get("VE_LEAD_IN_TEST_OUTPUT")
    root = Path(evidence) if evidence else Path(tempfile.mkdtemp(prefix="ve-lead-in-"))
    if evidence:
        root.mkdir(parents=True, exist_ok=False)
    completed = False
    private = None
    try:
        for dep in ("pipewire", "pipewire-pulse", "wireplumber", "pactl"):
            require(shutil.which(dep), "missing dependency: " + dep)
        wav = root / "payload.wav"
        samples = 3851  # 13 frames, including a short final frame.
        with wave.open(str(wav), "wb") as out:
            out.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
            out.writeframes(b"".join(struct.pack("<h", round(3000 * math.sin(i * 2 * math.pi * 440 / 16000)))
                                     for i in range(samples)))
        configs = {}
        for lead in (0, 500):
            config = root / ("lead-" + str(lead) + ".yaml")
            config.write_text(
                "audio:\n  source: ve_test_source\n  sink: ve_test_sink\n  profile: none\n"
                "  playback_lead_in_ms: " + str(lead) + "\nqueues:\n  playback_ms: 20\n")
            configs[lead] = config
        runs = root / "runs"
        # UNIX socket paths must fit sockaddr_un even when evidence lives
        # under a long project/run path. Keep only the daemon runtime short.
        private = Path(tempfile.mkdtemp(prefix="ve-lead-pulse-"))
        (root / "private-runtime-path.txt").write_text(str(private))
        with PrivatePulse(private) as server:
            for lead in (0, 500):
                run_id = "normal-" + str(lead)
                result = subprocess.run(
                    [binary, "--config", str(configs[lead]), "--playback-only",
                     "--play-wav", str(wav), "--duration-sec", "10",
                     "--wait-device-sec", "2", "--output-dir", str(runs), "--run-id", run_id],
                    env=server.env, text=True, capture_output=True, timeout=12)
                (root / (run_id + ".log")).write_text(result.stdout + result.stderr)
                require(result.returncode == 0, run_id + " failed: " + result.stderr)
                summary, events = check_summary(runs / run_id)
                require(summary["reason"] == "playback_file_complete" and
                        summary["status"]["playback_frames"] == 13,
                        "lead-in changed speech frame count or truncated file")
                require(summary["status"]["playback_queue_peak_frames"] <= 1,
                        "lead-in exceeded one-frame speech queue")
                starts = [e for e in events if e["event"] == "playback_lead_in_started"]
                queued = [e for e in events if e["event"] == "playback_lead_in_queued"]
                speech = [e for e in events if e["event"] == "playback_started"]
                require(len(speech) == 1, "speech-start event missing or duplicated")
                if lead:
                    require(len(starts) == len(queued) == 1, "lead-in did not run once")
                    dt = (speech[0]["data"]["backend_monotonic_ns"] -
                          starts[0]["data"]["backend_monotonic_ns"]) / 1e6
                    # Native server maxlength is 100 ms; at least 400 ms of
                    # the 500 ms prefix must pass before speech can be queued.
                    require(350 <= dt <= 3000, "lead-in was skipped or stalled: " + str(dt))
                    require(starts[0]["seq"] < queued[0]["seq"] < speech[0]["seq"],
                            "speech was submitted before the complete silence prefix")
                else:
                    require(not starts and not queued, "default path unexpectedly added silence")

            run = runs / "cancel-restart-empty"
            probe = RunningProbe(
                [binary, "--config", str(configs[500]), "--playback-only",
                 "--play-wav", str(wav), "--control", "stdin", "--duration-sec", "30",
                 "--wait-device-sec", "2", "--output-dir", str(runs), "--run-id", run.name],
                server.env)
            try:
                probe.wait_text("AUDIO_READY")
                probe.send("start")
                wait_for(lambda: any(e["event"] == "playback_lead_in_started" for e in read_events(run)),
                         "first lead-in")
                old = next(e["generation_id"] for e in read_events(run)
                           if e["event"] == "playback_lead_in_started")
                probe.send("cancel")
                wait_for(lambda: any(e["event"] == "cancel_flushed" for e in read_events(run)),
                         "cancel during lead-in")
                probe.send("start")
                wait_for(lambda: any(e["event"] == "playback_drain_complete" for e in read_events(run)),
                         "restarted playback drain")
                # Both commands arrive in one pipe write, so no speech frame
                # is produced between starting and requesting an empty drain.
                probe.send("start\nstop")
                wait_for(lambda: sum(e["event"] == "playback_drain_complete" for e in read_events(run)) >= 2,
                         "empty drain")
                probe.send("quit")
                probe.finish()
            finally:
                probe.close()
            summary, events = check_summary(run)
            require(not any(e["event"] == "playback_started" and e["generation_id"] == old
                            for e in events), "cancelled lead-in leaked old speech")
            lead_events = [e for e in events if e["event"] == "playback_lead_in_started"]
            require(len(lead_events) == 2 and lead_events[1]["generation_id"] > old,
                    "restart did not rearm prefix, or empty drain inserted silence")
            require(sum(e["event"] == "playback_started" for e in events) == 1,
                    "empty or cancelled session submitted speech")
            require(summary["status"]["playback_frames"] == 13 and
                    summary["status"]["playback_queue_peak_frames"] <= 1,
                    "cancel/empty session changed payload count or queue bound")
        completed = True
        print("audio_lead_in: disabled/default, 500 ms, one-frame queue, cancel/restart and empty drain passed")
    finally:
        if private is not None:
            # Preserve regular daemon diagnostics, not special socket files.
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
            print("audio_lead_in evidence: " + str(root))


if __name__ == "__main__":
    main()
