#!/usr/bin/env python3
"""Exercise audio_probe against a private, hardware-free PipeWire instance.

The custom graph contains only audiotestsrc and a null sink. Every client gets
explicit temporary sockets; no user daemon, sound card, Bluetooth device, or
global route is queried or changed. This is an integration regression, not
the physical-headset A01/A02 acceptance test.
"""

from contextlib import AbstractContextManager
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
import wave


CORE_CONFIG = """
context.properties = {
    core.daemon = true
    core.name = pipewire-0
    support.dbus = false
    default.clock.rate = 48000
    default.clock.quantum = 480
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
    audiotestsrc = audiotestsrc/libspa-audiotestsrc
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-access args = { access.force = unrestricted } }
]
context.objects = [
    { factory = metadata args = { metadata.name = default } }
    { factory = spa-node-factory args = {
        factory.name = support.node.driver
        node.name = Dummy-Driver
        node.group = pipewire.dummy
        priority.driver = 20000
    } }
    { factory = adapter args = {
        factory.name = audiotestsrc
        node.name = ve_test_source
        node.description = Test-source
        media.class = Audio/Source
        audio.rate = 48000
        audio.channels = 1
        audio.position = [ MONO ]
        node.always-process = true
        adapter.auto-port-config = { mode = dsp monitor = false position = preserve }
    } }
    { factory = adapter args = {
        factory.name = support.null-audio-sink
        node.name = ve_test_sink
        node.description = Test-sink
        media.class = Audio/Sink
        audio.rate = 48000
        audio.channels = 1
        audio.position = [ MONO ]
        adapter.auto-port-config = { mode = dsp monitor = false position = preserve }
    } }
]
"""

PULSE_CONFIG = """
context.properties = { support.dbus = false }
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-protocol-pulse }
]
pulse.properties = {
    server.address = [ "unix:native" ]
    pulse.min.quantum = 480/48000
}
"""

POLICY_CONFIG = """
context.properties = {
    application.name = Private-Audio-Policy
    support.dbus = false
    wireplumber.script-engine = lua-scripting
    wireplumber.export-core = false
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-client-device }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-session-manager }
]
wireplumber.components = [
    { name = libwireplumber-module-lua-scripting type = module }
    { name = libwireplumber-module-si-node type = module }
    { name = libwireplumber-module-si-audio-adapter type = module }
    { name = libwireplumber-module-si-standard-link type = module }
    { name = libwireplumber-module-si-audio-endpoint type = module }
    { name = libwireplumber-module-default-nodes-api type = module }
    { name = libwireplumber-module-default-nodes type = module
      args = { use-persistent-storage = false } }
    { name = libwireplumber-module-mixer-api type = module }
    { name = create-item.lua type = script/lua args = { audio.no-dsp = false } }
    { name = policy-node.lua type = script/lua args = { move = false follow = false } }
]
"""


def wait_for(predicate, description, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.03)
    raise RuntimeError(f"timed out waiting for {description}")


class PrivatePulse(AbstractContextManager):
    """Own all test daemons and a minimal private-graph link policy."""

    def __init__(self, root):
        self.root = Path(root)
        self.root.chmod(0o700)
        self.processes = []
        self.logs = []
        # Discard inherited server/config selectors before installing private
        # paths. XDG overrides also prevent user configuration/drop-ins loading.
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("PIPEWIRE_", "PULSE_", "WIREPLUMBER_"))}
        self.env.update({
            "XDG_RUNTIME_DIR": str(self.root),
            "PIPEWIRE_RUNTIME_DIR": str(self.root),
            "PIPEWIRE_REMOTE": "pipewire-0",
            "PIPEWIRE_CONFIG_DIR": str(self.root),
            "XDG_CONFIG_HOME": str(self.root / "config"),
            "XDG_CONFIG_DIRS": str(self.root / "config"),
            "XDG_DATA_HOME": str(self.root / "data"),
            "XDG_STATE_HOME": str(self.root / "state"),
            "PULSE_SERVER": f"unix:{self.root}/pulse/native",
            "PULSE_CLIENTCONFIG": str(self.root / "pulse-client.conf"),
            "DBUS_SESSION_BUS_ADDRESS": f"unix:path={self.root}/no-session-bus",
            "DBUS_SYSTEM_BUS_ADDRESS": f"unix:path={self.root}/no-system-bus",
        })
        for directory in ("config", "data", "state"):
            (self.root / directory).mkdir(exist_ok=True)
        (self.root / "core.conf").write_text(CORE_CONFIG, encoding="utf-8")
        (self.root / "pulse.conf").write_text(PULSE_CONFIG, encoding="utf-8")
        # Only node/link policy is enabled: no hardware monitors, device route
        # policy, Bluetooth profile policy, or persistent user preferences.
        (self.root / "policy.conf").write_text(POLICY_CONFIG, encoding="utf-8")
        # PipeWire clients need only the native protocol and the client node
        # factory. The installed client config is independent of device probes.
        (self.root / "client.conf").write_text(
            "context.properties = { support.dbus = false }\n"
            "context.modules = [\n"
            " { name = libpipewire-module-protocol-native }\n"
            " { name = libpipewire-module-client-node }\n"
            "]\n", encoding="utf-8")
        (self.root / "pulse-client.conf").write_text(
            "autospawn = no\ndaemon-binary = /bin/false\n"
            f"cookie-file = {self.root}/private-cookie\n", encoding="utf-8")

    def command(self, args, timeout=2, check=True):
        result = subprocess.run(args, env=self.env, text=True,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f"private command failed: {args!r}\n{result.stderr}")
        return result

    def start(self):
        for binary, config in (("pipewire", "core.conf"),
                               ("pipewire-pulse", "pulse.conf"),
                               ("wireplumber", "policy.conf")):
            log = open(self.root / f"{binary}.log", "a+", encoding="utf-8")
            self.logs.append(log)
            self.processes.append(subprocess.Popen(
                [binary, "-c", str(self.root / config)], env=self.env,
                stdout=log, stderr=log))

        def available():
            if any(process.poll() is not None for process in self.processes):
                raise RuntimeError("private PipeWire process terminated unexpectedly")
            result = self.command(["pactl", "--format=json", "list", "sources"],
                                  check=False)
            if result.returncode:
                return False
            sources = json.loads(result.stdout)
            return any(source["name"] == "ve_test_source" for source in sources)

        wait_for(available, "private PulseAudio source")

    def stop(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
        for process in reversed(self.processes):
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        self.processes.clear()

    def __enter__(self):
        try:
            self.start()
            return self
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def __exit__(self, *_):
        self.stop()
        for log in self.logs:
            log.close()
        return False


def main():
    binary = str(Path(sys.argv[1]).resolve())
    for dependency in ("pipewire", "pipewire-pulse", "wireplumber", "pactl"):
        if not shutil.which(dependency):
            raise RuntimeError(f"private audio integration requires {dependency}")

    with tempfile.TemporaryDirectory(prefix="ve-audio-test-") as directory:
        root = Path(directory)
        config = root / "test.yaml"
        config.write_text(
            "audio:\n  source: ve_test_source\n  sink: ve_test_sink\n"
            "  profile: none\n  sample_rate: 16000\n  channels: 1\n"
            "  frame_ms: 20\n", encoding="utf-8")
        output = root / "runs"
        common = ["--config", str(config), "--duration-sec", "1",
                  "--wait-device-sec", "2", "--output-dir", str(output)]
        # Even CLI failures are isolated from the user's audio environment.
        invalid_env = os.environ.copy()
        invalid_env["PULSE_SERVER"] = f"unix:{root}/missing-server"
        invalid_env["PULSE_CLIENTCONFIG"] = str(root / "no-autospawn.conf")
        Path(invalid_env["PULSE_CLIENTCONFIG"]).write_text("autospawn = no\n")
        cases = [
            (["--help"], 0),
            ([], 2),
            (["--unknown"], 2),
            (["--config", str(config), "--duplex", "--duration-sec", "0"], 2),
            (["--config", str(config), "--duplex", "--duration-sec", "3601"], 2),
            ([*common, "--duplex", "--capture-only"], 2),
            ([*common, "--duplex", "--control", "invalid"], 2),
            ([*common, "--playback-only", "--record-wav"], 2),
            ([*common, "--capture-only", "--play-wav", str(config)], 2),
            ([*common, "--playback-only", "--play-wav", str(config)], 2),
            ([*common, "--playback-only", "--play-wav", str(root / "missing.wav")], 2),
        ]
        for arguments, expected in cases:
            result = subprocess.run([binary, *arguments], env=invalid_env,
                                    capture_output=True, text=True, timeout=6)
            require(result.returncode == expected,
                    f"CLI {arguments!r}: expected {expected}, got "
                    f"{result.returncode}: {result.stdout}\n{result.stderr}")

        started = time.monotonic()
        result = subprocess.run(
            [binary, *common, "--duplex", "--run-id", "unavailable"],
            env=invalid_env, capture_output=True, text=True, timeout=7)
        require(result.returncode == 3, f"missing server did not fail: {result}")
        require(time.monotonic() - started < 6, "missing server failure was unbounded")
        check_summary(output / "unavailable", expected_code=3)

        private_root = root / "private"
        private_root.mkdir()
        with PrivatePulse(private_root) as server:
            sources = json.loads(server.command(
                ["pactl", "--format=json", "list", "sources"]).stdout)
            sinks = json.loads(server.command(
                ["pactl", "--format=json", "list", "sinks"]).stdout)
            cards = json.loads(server.command(
                ["pactl", "--format=json", "list", "cards"]).stdout)
            require(not cards, "private graph unexpectedly contains hardware cards")
            require({source["name"] for source in sources} ==
                    {"ve_test_source", "ve_test_sink.monitor"},
                    "private graph contains unexpected capture devices")
            require({sink["name"] for sink in sinks} == {"ve_test_sink"},
                    "private graph contains unexpected playback devices")

            for mode in ("duplex", "capture-only", "playback-only"):
                run_id = mode
                args = [binary, *common, f"--{mode}", "--run-id", run_id]
                if mode == "capture-only":
                    args.append("--record-wav")
                result = subprocess.run(args, env=server.env, capture_output=True,
                                        text=True, timeout=10)
                require(result.returncode == 0,
                        f"{mode} failed: {result.stdout}\n{result.stderr}\n"
                        f"{run_diagnostics(output / run_id)}")
                summary, events = check_summary(output / run_id)
                status = summary["status"]
                require(status["source"] == "ve_test_source" and
                        status["sink"] == "ve_test_sink" and
                        status["profile"] == "none", "incorrect device binding")
                require(status["capture_overflows"] == 0 and
                        status["capture_holes"] == 0 and
                        status["playback_overflows"] == 0,
                        f"{mode} lost audio: {status}")
                if mode != "playback-only":
                    require(summary["capture_frames_consumed"] >= 20,
                            f"{mode} captured too few frames")
                    require(0 < summary["capture_samples"] <=
                            summary["capture_frames_consumed"] * 320,
                            "capture sample count exceeds its framed capacity")
                    require(summary["capture_frames_consumed"] * 320 -
                            summary["capture_samples"] < 320,
                            "capture contains more than one partial final frame")
                    expected_samples = (status["capture_native_frames"] * 16000 /
                                        status["capture_rate"])
                    require(abs(summary["capture_samples"] - expected_samples) <= 1,
                            "normal capture stop lost buffered samples or its resampler tail")
                    require(any(event["event"] == "capture_stopped" for event in events),
                            "normal capture stop did not acknowledge its final tail")
                    require(0 < summary["rms"] <= summary["peak"] <= 1,
                            "synthetic source did not produce normalized audio")
                else:
                    require(summary["capture_samples"] == 0,
                            "playback-only unexpectedly captured audio")
                if mode != "capture-only":
                    require(status["playback_frames"] >= 20,
                            f"{mode} submitted too few playback frames")
                else:
                    with wave.open(str(output / run_id / "capture.wav"), "rb") as wav:
                        require((wav.getnchannels(), wav.getframerate(),
                                 wav.getsampwidth(), wav.getnframes()) ==
                                (1, 16000, 2, summary["capture_samples"]),
                                "recorded WAV format or sample count was incorrect")
                require(events, "missing audio events")

            # No missing name/profile may fall back to a default, and capture
            # must reject a sink's monitor even when that monitor exists.
            for run_id, original, replacement, expected in (
                    ("monitor", "source: ve_test_source", "source: ve_test_sink.monitor", 2),
                    ("wrong-source", "source: ve_test_source", "source: ve_missing_source", 3),
                    ("wrong-sink", "sink: ve_test_sink", "sink: ve_missing_sink", 3),
                    ("wrong-profile", "profile: none", "profile: headset-head-unit", 3)):
                rejected = root / f"{run_id}.yaml"
                rejected.write_text(config.read_text(encoding="utf-8").replace(
                    original, replacement), encoding="utf-8")
                result = subprocess.run(
                    [binary, "--config", str(rejected), "--duplex", "--duration-sec", "1",
                     "--wait-device-sec", "1", "--output-dir", str(output), "--run-id", run_id],
                    env=server.env, capture_output=True, text=True, timeout=6)
                require(result.returncode == expected,
                        f"{run_id} was not rejected: {result.stdout}\n{result.stderr}")
                if expected == 3:
                    summary, events = check_summary(output / run_id, expected_code=3)
                    require(summary["capture_samples"] == 0 and
                            not any(event["event"] in ("device_ready", "session_start")
                                    for event in events),
                            f"{run_id} used an unconfigured fallback device")

            wav_path = root / "stereo-48k.wav"
            native_frames = 11531  # Intentionally not a 20 ms or resampling boundary.
            with wave.open(str(wav_path), "wb") as wav:
                wav.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
                wav.writeframes(b"".join(
                    struct.pack("<hh", round(3000 * math.sin(index * 2 * math.pi * 440 / 48000)),
                                round(1500 * math.sin(index * 2 * math.pi * 440 / 48000)))
                    for index in range(native_frames)))
            result = subprocess.run(
                [binary, *common, "--playback-only", "--play-wav", str(wav_path),
                 "--run-id", "wav-playback"], env=server.env,
                capture_output=True, text=True, timeout=8)
            require(result.returncode == 0,
                    f"WAV playback failed: {result.stdout}\n{result.stderr}")
            summary, events = check_summary(output / "wav-playback")
            require(summary["reason"] == "playback_file_complete" and
                    summary["capture_samples"] == 0 and
                    summary["status"]["playback_frames"] == 13 and
                    summary["status"]["drained"],
                    f"WAV conversion/tail/drain failed: {summary}")
            metadata = json.loads((output / "wav-playback" / "run.json").read_text())
            require(metadata["play_wav_sha256"] == hashlib.sha256(wav_path.read_bytes()).hexdigest(),
                    "played WAV identity was not recorded")
            require(any(event["event"] == "playback_file_end" for event in events) and
                    any(event["event"] == "playback_drain_complete" for event in events),
                    "WAV playback did not finish and drain normally")

            controlled = ["--config", str(config), "--duration-sec", "30",
                          "--wait-device-sec", "2", "--output-dir", str(output)]
            probe = RunningProbe([binary, *controlled, "--duplex", "--control", "stdin",
                                  "--run-id", "control"], server.env)
            try:
                probe.wait_text("AUDIO_READY")
                probe.send("unrecognized-private-command")
                probe.send("x" * 129)
                probe.send("help")
                probe.wait_text("Commands: start stop cancel status help quit")
                probe.send("start")
                probe.wait_text("AUDIO_SESSION_STARTED", count=1)
                probe.wait_capture_progress()
                probe.send("cancel")
                probe.send("start")
                probe.wait_text("AUDIO_SESSION_STARTED", count=2)
                probe.wait_capture_progress()
                probe.send("stop")
                wait_for(lambda: any(event["event"] == "playback_drain_complete"
                                     for event in read_events(output / "control")),
                         "controlled playback drain")
                probe.send("status")
                probe.send("quit")
                probe.finish()
            finally:
                probe.close()
            summary, events = check_summary(output / "control")
            require(summary["capture_frames_consumed"] > 0,
                    "controlled sessions never captured audio")
            starts = [event for event in events if event["event"] == "session_start"]
            require(len(starts) == 2 and
                    starts[1]["generation_id"] > starts[0]["generation_id"],
                    "cancel/start did not advance the generation")
            rejected = {event["data"]["reason"] for event in events
                        if event["event"] == "command_rejected"}
            require({"unknown_command", "input_too_long"}.issubset(rejected),
                    "control input validation did not report bounded rejections")
            require("unrecognized-private-command" not in json.dumps(events) and
                    "x" * 129 not in json.dumps(events),
                    "rejected control input leaked into logs")

            probe = RunningProbe([binary, *controlled, "--duplex", "--control", "stdin",
                                  "--run-id", "reconnect"], server.env)
            try:
                probe.wait_text("AUDIO_READY")
                probe.send("start")
                probe.wait_text("AUDIO_SESSION_STARTED", count=1)
                probe.wait_capture_progress()
                server.stop()
                probe.wait_text("AUDIO_DEVICE_WAIT")
                server.start()
                run = output / "reconnect"
                wait_for(lambda: sum(event["event"] == "device_ready"
                                     for event in read_events(run)) >= 2,
                         "private audio reconnection", timeout=10)
                require(sum(event["event"] == "session_start"
                            for event in read_events(run)) == 1,
                        "reconnection automatically resumed the old session")
                idle = probe.status()
                require(not idle["session_active"] and not idle["capturing"] and
                        idle["capture_queued_frames"] == 0 and
                        idle["playback_queued_frames"] == 0,
                        "reconnected device retained old session audio")
                probe.send("start")
                probe.wait_text("AUDIO_SESSION_STARTED", count=2)
                probe.wait_capture_progress()
                probe.send("quit")
                probe.finish()
            finally:
                probe.close()
            summary, events = check_summary(output / "reconnect")
            require(summary["status"]["reconnects"] >= 1,
                    "backend did not report a successful reconnection")
            require(sum(event["event"] == "session_start" for event in events) == 2,
                    "new generation could not start after reconnection")
            starts = [event for event in events if event["event"] == "session_start"]
            require(starts[1]["generation_id"] > starts[0]["generation_id"],
                    "reconnection reused the disconnected generation")

            for number, expected in ((signal.SIGINT, 130), (signal.SIGTERM, 143)):
                run_id = f"signal-{number}"
                probe = RunningProbe(
                    [binary, *controlled, "--duplex", "--record-wav", "--run-id", run_id],
                    server.env)
                try:
                    probe.wait_text("AUDIO_SESSION_STARTED")
                    run = output / run_id
                    wait_for(lambda: any(event["event"] == "audio_metrics" and
                                         event["data"]["capture_frames"] > 0
                                         for event in read_events(run)),
                             "capture before signal")
                    probe.process.send_signal(number)
                    probe.finish(expected_code=expected)
                finally:
                    probe.close()
                summary, events = check_summary(output / run_id, expected_code=expected)
                require(summary["reason"] == "signal" and
                        summary["capture_samples"] > 0 and
                        events[-1]["event"] == "shutdown" and
                        events[-1]["data"]["exit_code"] == expected,
                        "signal did not close the report and event stream")
                with wave.open(str(output / run_id / "capture.wav"), "rb") as wav:
                    require(wav.getnframes() == summary["capture_samples"],
                            "signal did not finalize the WAV header")

    print("AUDIO_PROBE_OK: CLI/WAV rejection, bounded unavailable server, private PipeWire "
          "duplex/capture/playback/WAV conversion+drain, explicit-device refusal, "
          "cancel, reconnection isolation, SIGINT/SIGTERM; headset A01/A02 not run")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_events(run):
    events = []
    for path in sorted(run.glob("events*.jsonl")):
        for line in path.read_text(encoding="utf-8").splitlines(keepends=True):
            # The active probe may be between its JSON write and newline.
            if line.endswith("\n"):
                events.append(json.loads(line))
    return sorted(events, key=lambda event: event["seq"])


def run_diagnostics(run):
    summary = run / "summary.json"
    details = [event for event in read_events(run)
               if event["event"] not in ("audio_metrics", "status")]
    return f"summary={summary.read_text() if summary.exists() else 'missing'} events={details}"


def check_summary(run, expected_code=0):
    summary = json.loads((run / "summary.json").read_text(encoding="utf-8"))
    require(summary["exit_code"] == expected_code, "summary disagrees with exit status")
    require(summary["acceptance"].get("A01") == "not_run" and
            summary["acceptance"].get("A02") == "not_run" and
            summary["acceptance"].get("requires_physical_device_review") is True,
            "software integration falsely claims hardware acceptance")
    events = read_events(run)
    require(all(event["mode"] == "audio_probe" for event in events),
            "audio logs use an incorrect mode")
    require(len({event["seq"] for event in events}) == len(events),
            "duplicate audio event sequence")
    require(all("samples" not in event.get("data", {}) and
                "pcm" not in event.get("data", {}) for event in events),
            "audio logs contain raw samples")
    return summary, events


class RunningProbe:
    def __init__(self, args, env):
        self.process = subprocess.Popen(args, env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.output = b""

    def wait_text(self, token, count=1, timeout=8):
        wanted = token.encode()
        deadline = time.monotonic() + timeout
        while self.output.count(wanted) < count:
            if time.monotonic() >= deadline:
                raise RuntimeError(f"probe did not emit {token!r}: {self.output!r}")
            if select.select([self.process.stdout], [], [], 0.05)[0]:
                chunk = os.read(self.process.stdout.fileno(), 8192)
                if chunk:
                    self.output += chunk
                elif self.process.poll() is not None:
                    stderr = self.process.stderr.read().decode(errors="replace")
                    raise RuntimeError(f"probe exited before {token!r}: "
                                       f"{self.output!r}\n{stderr}")

    def send(self, command):
        self.process.stdin.write((command + "\n").encode())
        self.process.stdin.flush()

    def status(self):
        count = self.output.count(b"AUDIO_STATUS ") + 1
        self.send("status")
        self.wait_text("AUDIO_STATUS ", count=count)
        # A read can end between the status prefix and its trailing newline.
        wait_for(lambda: self._complete_statuses() >= count,
                 "complete status response", timeout=2)
        lines = [line for line in self.output.splitlines()
                 if line.startswith(b"AUDIO_STATUS ")]
        return json.loads(lines[-1][len(b"AUDIO_STATUS "):])

    def _complete_statuses(self):
        if select.select([self.process.stdout], [], [], 0)[0]:
            self.output += os.read(self.process.stdout.fileno(), 8192)
        return sum(line.startswith(b"AUDIO_STATUS ")
                   for line in self.output.split(b"\n")[:-1])

    def wait_capture_progress(self, timeout=5):
        initial = self.status()["capture_frames"]
        wait_for(lambda: self.status()["capture_frames"] > initial,
                 "new capture frames", timeout=timeout)

    def finish(self, expected_code=0):
        output, error = self.process.communicate(timeout=8)
        self.output += output
        require(self.process.returncode == expected_code,
                f"controlled probe failed: {self.output!r}\n{error!r}")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.communicate(timeout=2)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"audio probe test failed: {error}", file=sys.stderr)
        sys.exit(1)
