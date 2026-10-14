#!/usr/bin/env python3
"""Exercise mock stdin control and JSONL logging without devices or models."""

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time


TIMEOUT_SECONDS = 5
CONFIG = """audio:
  source: "test_input"
  sink: "test_output"
  profile: "test_profile"
  sample_rate: 16000
  channels: 1
  frame_ms: 20
"""
REQUIRED_FIELDS = {
    "timestamp_utc", "monotonic_ns", "run_id", "seq", "level", "event",
    "session_id", "generation_id", "state", "mode", "data",
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def write_config(directory, extra=""):
    path = directory / "config.yaml"
    path.write_text(CONFIG + extra, encoding="utf-8")
    return path


def invoke(executable, directory, arguments, text="", expected_code=0):
    result = subprocess.run(
        [str(executable)] + [str(argument) for argument in arguments],
        cwd=directory,
        input=text,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        timeout=TIMEOUT_SECONDS,
        check=False,
    )
    require(result.returncode == expected_code,
            "exit {}, expected {}; stdout={!r}; stderr={!r}".format(
                result.returncode, expected_code, result.stdout, result.stderr))
    if expected_code:
        require(result.stderr.strip(), "failed command must explain the error")
    return result


def mock_arguments(config, output_root=None, run_id=None):
    arguments = ["--config", str(config), "--mock", "--control", "stdin"]
    if output_root is not None:
        arguments += ["--output-dir", str(output_root)]
    if run_id is not None:
        arguments += ["--run-id", run_id]
    return arguments


def read_events(run_directory, run_id, rotated=False):
    current = run_directory / "events.jsonl"
    require(current.is_file(), "events.jsonl was not created")
    paths = list(run_directory.iterdir()) if rotated else [current]
    records = []
    for path in paths:
        require(path.is_file(), "unexpected directory in run output: {}".format(path))
        content = path.read_bytes()
        require(content.endswith(b"\n"), "log must end with a complete JSONL line")
        for line in content.splitlines():
            event = json.loads(line)
            require(isinstance(event, dict), "JSONL record must be an object")
            require(REQUIRED_FIELDS <= event.keys(), "missing log fields: {}".format(event))
            require(event["run_id"] == run_id, "wrong run_id in log")
            require(event["mode"] == "mock", "mock mode missing from log")
            for key in ("monotonic_ns", "seq", "session_id", "generation_id"):
                require(type(event[key]) is int and event[key] >= 0,
                        "{} must be a nonnegative integer".format(key))
            require(event["session_id"] >= 1, "session IDs start at 1")
            for key in ("level", "event", "state"):
                require(isinstance(event[key], str) and event[key],
                        "{} must be a nonempty string".format(key))
            require(isinstance(event["data"], dict), "data must be an object")
            timestamp = event["timestamp_utc"]
            require(isinstance(timestamp, str), "timestamp_utc must be a string")
            parsed = datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
            require(parsed.utcoffset() is not None and
                    parsed.utcoffset().total_seconds() == 0,
                    "timestamp_utc must contain the UTC offset")
            if event["event"] == "state_changed":
                require(event["data"].get("to") == event["state"],
                        "state_changed.to differs from current state")
                require(isinstance(event["data"].get("from"), str),
                        "state_changed.from missing")
            records.append(event)
    if rotated:
        records.sort(key=lambda event: event["seq"])
    require(records, "log is empty")
    for previous, event in zip(records, records[1:]):
        require(previous["seq"] < event["seq"], "seq is not strictly increasing")
        require(previous["monotonic_ns"] <= event["monotonic_ns"],
                "monotonic_ns moved backwards")
    return records


def shutdown_reason(events, reason):
    require(events[-1]["event"] == "shutdown", "last event must be shutdown")
    require(events[-1]["state"] == "STOPPING", "shutdown must be in STOPPING")
    require(events[-1]["data"].get("reason") == reason,
            "wrong shutdown reason: {}".format(events[-1]["data"]))


def command_segments(events):
    indices = [index for index, event in enumerate(events)
               if event["event"] == "command_received"]
    return [(events[start]["data"].get("command"), events[start:end])
            for start, end in zip(indices, indices[1:] + [len(events)])]


def control_protocol(executable, directory):
    config = write_config(directory)
    output_root = directory / "runs"
    run_id = "protocol"
    run_directory = output_root / run_id
    steps = [
        ("status", "IDLE", 1, 0),
        ("start", "LISTENING", 1, 1),
        ("start", "LISTENING", 1, 2),
        ("stop", "IDLE", 1, 2),
        ("stop", "IDLE", 1, 2),
        ("cancel", "IDLE", 1, 3),
        ("new-session", "IDLE", 2, 4),
        ("start", "LISTENING", 2, 5),
        ("new-session", "IDLE", 3, 6),
        ("start", "LISTENING", 3, 7),
        ("cancel", "IDLE", 3, 8),
        ("help", "IDLE", 3, 8),
        ("status", "IDLE", 3, 8),
        ("quit", "STOPPING", 3, 8),
    ]
    process = subprocess.Popen(
        [str(executable)] + mock_arguments(config, output_root, run_id),
        cwd=directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    prefix = b""
    try:
        # No command is sent until startup output arrives, so a missing flush
        # cannot be hidden by process termination or the next command.
        deadline = time.monotonic() + TIMEOUT_SECONDS
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while not all(value in prefix for value in (
                    b"MOCK_READY", run_id.encode(), str(run_directory).encode())):
                remaining = deadline - time.monotonic()
                require(remaining > 0, "startup output was not flushed")
                ready = selector.select(remaining)
                require(ready, "startup output was not flushed")
                block = os.read(process.stdout.fileno(), 4096)
                require(block, "process exited before MOCK_READY: {!r}".format(prefix))
                prefix += block
        stdout, stderr = process.communicate(
            ("\n".join(step[0] for step in steps) + "\n").encode(),
            timeout=TIMEOUT_SECONDS,
        )
        stdout = (prefix + stdout).decode("utf-8", errors="replace")
        stderr = stderr.decode("utf-8", errors="replace")
        require(process.returncode == 0,
                "control exit {}; stdout={!r}; stderr={!r}".format(
                    process.returncode, stdout, stderr))
        require("OK stop" in stdout, "successful stop was not acknowledged")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()

    events = read_events(run_directory, run_id)
    require(events[0]["event"] == "startup", "first event must be startup")
    initial = next(event for event in events if event["event"] == "state_changed")
    require((initial["data"].get("from"), initial["data"].get("to")) ==
            ("STARTING", "IDLE"),
            "initial transition must be STARTING -> IDLE")
    require((initial["session_id"], initial["generation_id"]) == (1, 0),
            "wrong initial session/generation")
    segments = command_segments(events)
    require([command for command, _ in segments] == [step[0] for step in steps],
            "received commands differ from input")
    for (command, segment), (_, state, session, generation) in zip(segments, steps):
        last = segment[-1]
        require((last["state"], last["session_id"], last["generation_id"]) ==
                (state, session, generation),
                "wrong state/session/generation after {}: {}".format(command, last))
    for index in (2, 8, 10):
        transitions = [event["state"] for event in segments[index][1]
                       if event["event"] == "state_changed"]
        require("CANCELLING" in transitions,
                "active {} must enter CANCELLING".format(steps[index][0]))
    stop_segment = segments[3][1]
    require([event["state"] for event in stop_segment
             if event["event"] == "state_changed"] == ["FINALIZING", "IDLE"],
            "stop must go through FINALIZING -> IDLE")
    require(any(event["event"] == "mock_input_finished" for event in stop_segment),
            "stop did not record mock_input_finished")
    require(any(event["event"] == "command_rejected" for event in segments[4][1]),
            "stop while idle must be rejected")
    for index in (6, 8):
        require(any(event["event"] == "session_started" for event in segments[index][1]),
                "new-session did not record session_started")
    allowed_states = {"STARTING", "IDLE", "LISTENING", "FINALIZING", "CANCELLING", "STOPPING"}
    require(all(event["state"] in allowed_states for event in events),
            "mock run entered a model processing state")
    audio_backend = events[0]["data"].get("audio_backend")
    require(audio_backend in ("mock", "none"), "unknown mock audio boundary")
    require(events[0]["data"].get("asset_checks") == "skipped_mock",
            "mock asset validation boundary is not explicit")
    frames = [event for event in events if event["event"] == "mock_audio_frame"]
    if audio_backend == "mock":
        starts = [segment for command, segment in segments if command == "start"]
        require(len(frames) == len(starts), "each mock start must exercise one audio frame")
        for frame in frames:
            require(frame["data"].get("samples") == 320 and
                    frame["data"].get("sample_rate") == 16000 and
                    frame["data"].get("channels") == 1 and
                    frame["data"].get("synthetic") is True,
                    "mock audio frame must describe synthetic 20 ms mono audio")
            require("pcm" not in frame["data"], "raw PCM must not be logged")
    else:
        require(not frames, "engine-only control must not produce audio frames")
    shutdown_reason(events, "quit")


def eof_shutdown(executable, directory, active, trailing_newline=True):
    config = write_config(directory)
    output_root = directory / "runs"
    run_id = "eof"
    invoke(executable, directory, mock_arguments(config, output_root, run_id),
           ("start\n" if trailing_newline else "start") if active else "")
    events = read_events(output_root / run_id, run_id)
    shutdown_reason(events, "eof")
    require(events[-1]["generation_id"] >= int(active), "generation was lost at EOF")
    if active:
        require(any(event["event"] == "state_changed" and event["state"] == "LISTENING"
                    for event in events), "final start command was not processed")


def signal_shutdown(executable, directory, signal_number, reason):
    config = write_config(directory)
    output_root = directory / "runs"
    run_id = reason
    process = subprocess.Popen(
        [str(executable)] + mock_arguments(config, output_root, run_id),
        cwd=directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    output = bytearray()

    def wait_for_output(markers):
        deadline = time.monotonic() + TIMEOUT_SECONDS
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while not all(marker in output for marker in markers):
                remaining = deadline - time.monotonic()
                require(remaining > 0, "timed out waiting for {!r}".format(markers))
                require(selector.select(remaining),
                        "timed out waiting for {!r}".format(markers))
                block = os.read(process.stdout.fileno(), 4096)
                require(block, "process exited before {!r}: {!r}".format(markers, output))
                output.extend(block)

    try:
        wait_for_output((b"MOCK_READY",))
        process.stdin.write(b"start\n")
        process.stdin.flush()
        wait_for_output((b"OK start", b"LISTENING"))
        process.send_signal(signal_number)
        # Keep stdin open until exit, so EOF cannot mask a failure to respond
        # to the signal while poll() is waiting for another command.
        process.wait(timeout=TIMEOUT_SECONDS)
        stdout, stderr = process.communicate(timeout=TIMEOUT_SECONDS)
        require(process.returncode == 128 + signal_number,
                "signal exit {}, expected {}; stdout={!r}; stderr={!r}".format(
                    process.returncode, 128 + signal_number, bytes(output) + stdout, stderr))
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    events = read_events(output_root / run_id, run_id)
    shutdown_reason(events, reason)


def input_privacy(executable, directory):
    config = write_config(directory)
    output_root = directory / "runs"
    run_id = "privacy"
    unknown = "secret-unknown-command-65d79ea4"
    oversized = "secret-overlong-command-7b94064a" + "x" * 50000
    commands = ("\n\t \r\n  status  \r\n" + "status" + " " * 122 + "\n" +
                "status" + " " * 123 + "\n\tstart\t\r\n" + unknown + "\n" +
                oversized + "\nstop\nquit\n")
    invoke(executable, directory, mock_arguments(config, output_root, run_id), commands)
    events = read_events(output_root / run_id, run_id)
    require([command for command, _ in command_segments(events)] ==
            ["status", "status", "start", "stop", "quit"],
            "whitespace, line limit, or recovery after invalid input is incorrect")
    require(sum(event["event"] == "command_rejected" for event in events) == 3,
            "expected one rejection per unknown/overlong input line")
    serialized = json.dumps(events)
    require(unknown not in serialized and oversized[:31] not in serialized,
            "raw rejected input leaked into JSONL logs")
    shutdown_reason(events, "quit")


def no_overwrite(executable, directory):
    config = write_config(directory)
    output_root = directory / "runs"
    run_id = "existing"
    arguments = mock_arguments(config, output_root, run_id)
    invoke(executable, directory, arguments, "quit\n")
    run_directory = output_root / run_id
    before = {path.name: path.read_bytes() for path in run_directory.iterdir()}
    invoke(executable, directory, arguments, "quit\n", expected_code=1)
    require(before == {path.name: path.read_bytes() for path in run_directory.iterdir()},
            "existing run files were overwritten")


def automatic_ids(executable, directory):
    config = write_config(directory)
    arguments = mock_arguments(config)
    first = invoke(executable, directory, arguments, "quit\n")
    second = invoke(executable, directory, arguments, "quit\n")
    output_root = directory / "output" / "runs"
    require(output_root.is_dir(), "default output/runs root was not created")
    directories = list(output_root.iterdir())
    require(len(directories) == 2 and all(path.is_dir() for path in directories),
            "automatic run IDs did not produce two separate runs")
    for path in directories:
        shutdown_reason(read_events(path, path.name), "quit")
        require(any(path.name in result.stdout for result in (first, second)),
                "automatic run ID was not reported")


def valid_id(executable, directory, run_id):
    config = write_config(directory)
    output_root = directory / "runs"
    invoke(executable, directory, mock_arguments(config, output_root, run_id), "quit\n")
    shutdown_reason(read_events(output_root / run_id, run_id), "quit")


def invalid_cli(executable, directory, options):
    config = write_config(directory)
    arguments = ["--config", str(config)] + options
    invoke(executable, directory, arguments, "quit\n", expected_code=2)
    require(not (directory / "output").exists(), "invalid CLI created run output")


def invalid_logging(executable, directory, logging_yaml):
    config = write_config(directory, logging_yaml)
    invoke(executable, directory, mock_arguments(config), "quit\n", expected_code=2)
    require(not (directory / "output").exists(), "invalid config created run output")


def log_rotation(executable, directory):
    config = write_config(directory, "logging:\n  max_file_bytes: 1024\n  max_files: 3\n")
    output_root = directory / "runs"
    run_id = "rotate"
    invoke(executable, directory, mock_arguments(config, output_root, run_id),
           "status\n" * 100 + "quit\n")
    run_directory = output_root / run_id
    paths = list(run_directory.iterdir())
    require(2 <= len(paths) <= 3, "rotation must keep at most 3 total log files")
    require(all(path.stat().st_size <= 1024 for path in paths),
            "a rotated log exceeds max_file_bytes")
    events = read_events(run_directory, run_id, rotated=True)
    require(events[0]["event"] != "startup", "oldest records were not rotated away")
    shutdown_reason(events, "quit")
    current_events = read_events(run_directory, run_id)
    require(current_events[-1]["seq"] == events[-1]["seq"],
            "events.jsonl is not the current log")


def config_has_no_side_effects(executable, directory):
    config = write_config(directory, "logging:\n  max_file_bytes: 10485760\n  max_files: 10\n")
    before = sorted(path.relative_to(directory) for path in directory.rglob("*"))
    result = invoke(executable, directory, ["--config", str(config), "--check-config"])
    require("CONFIG_OK" in result.stdout, "check-config did not succeed")
    require(before == sorted(path.relative_to(directory) for path in directory.rglob("*")),
            "check-config created files or directories")


def output_io_failure(executable, directory):
    config = write_config(directory)
    output_root = directory / "regular-file"
    output_root.write_text("preserve me", encoding="utf-8")
    invoke(executable, directory, mock_arguments(config, output_root, "failed"),
           "quit\n", expected_code=1)
    require(output_root.read_text(encoding="utf-8") == "preserve me",
            "output failure modified an existing file")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="path to voice_engine")
    arguments = parser.parse_args()
    executable = arguments.executable.resolve()
    if not executable.is_file():
        parser.error("executable does not exist: {}".format(executable))

    cases = [
        ("control protocol and flushed startup", control_protocol, ()),
        ("EOF while idle", eof_shutdown, (False,)),
        ("EOF while listening", eof_shutdown, (True,)),
        ("EOF processes command without final newline", eof_shutdown, (True, False)),
        ("SIGINT while waiting for input", signal_shutdown, (signal.SIGINT, "sigint")),
        ("SIGTERM while waiting for input", signal_shutdown, (signal.SIGTERM, "sigterm")),
        ("input limits, privacy, and recovery", input_privacy, ()),
        ("existing run is preserved", no_overwrite, ()),
        ("unique automatic IDs and default output root", automatic_ids, ()),
        ("one-character run ID", valid_id, ("A",)),
        ("64-character run ID", valid_id, ("a" + "b" * 60 + "_.-",)),
        ("bounded log rotation", log_rotation, ()),
        ("check-config has no output side effects", config_has_no_side_effects, ()),
        ("output I/O failure", output_io_failure, ()),
    ]
    cli_cases = [
        ("control without mock", ["--control", "stdin"]),
        ("mock without control", ["--mock"]),
        ("unsupported control", ["--mock", "--control", "socket"]),
        ("missing control value", ["--mock", "--control"]),
        ("check-config with mock control", ["--check-config", "--mock", "--control", "stdin"]),
        ("check-config with run ID", ["--check-config", "--run-id", "unused"]),
        ("duplicate mock", ["--mock", "--mock", "--control", "stdin"]),
        ("duplicate control", ["--mock", "--control", "stdin", "--control", "stdin"]),
        ("missing run ID value", ["--mock", "--control", "stdin", "--run-id"]),
        ("missing output directory value", ["--mock", "--control", "stdin", "--output-dir"]),
    ]
    for run_id in ("../escape", "a/b", "a\\b", ".hidden", "-bad", "中文", "a" * 65, ""):
        cli_cases.append(("invalid run ID {!r}".format(run_id),
                          ["--mock", "--control", "stdin", "--run-id", run_id]))
    cases.extend((name, invalid_cli, (options,)) for name, options in cli_cases)
    logging_cases = [
        ("logging is a mapping", "logging: []\n"),
        ("max_file_bytes lower bound", "logging:\n  max_file_bytes: 1023\n"),
        ("max_file_bytes upper bound", "logging:\n  max_file_bytes: 10485761\n"),
        ("max_files lower bound", "logging:\n  max_files: 0\n"),
        ("max_files upper bound", "logging:\n  max_files: 11\n"),
        ("max_file_bytes must be integer", 'logging:\n  max_file_bytes: "1024"\n'),
        ("max_files must be integer", "logging:\n  max_files: true\n"),
    ]
    cases.extend((name, invalid_logging, (yaml_text,)) for name, yaml_text in logging_cases)
    failures = []
    with tempfile.TemporaryDirectory(prefix="voice-engine-control-test-") as temp:
        for index, (name, test, values) in enumerate(cases):
            directory = Path(temp) / str(index)
            directory.mkdir()
            try:
                test(executable, directory, *values)
            except (AssertionError, OSError, ValueError, KeyError,
                    subprocess.TimeoutExpired, StopIteration) as error:
                failures.append("{}: {}".format(name, error))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("{} of {} control checks failed".format(len(failures), len(cases)), file=sys.stderr)
        return 1
    print("All {} control checks passed".format(len(cases)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
