#!/usr/bin/env python3
"""Exercise the config-only CLI without accessing audio devices or models."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Optional, Tuple


@dataclass(frozen=True)
class Case:
    name: str
    yaml_text: Optional[str]
    expected_code: int = 2
    error_terms: Tuple[str, ...] = ()
    arguments: Optional[Tuple[str, ...]] = None
    stdout_terms: Tuple[str, ...] = ()


def audio_yaml(overrides=None, omitted=(), extra=""):
    values = {
        "source": '"bluez_input.example.0"',
        "sink": '"bluez_output.example.1"',
        "profile": '"headset-head-unit-msbc"',
        "sample_rate": "16000",
        "channels": "1",
        "frame_ms": "20",
    }
    values.update(overrides or {})
    fields = ["audio:"]
    fields.extend(
        "  {}: {}".format(key, value)
        for key, value in values.items()
        if key not in omitted
    )
    return "\n".join(fields) + "\n" + extra


def cases():
    valid = audio_yaml()
    large_integer = "9" * 500
    large_hex_integer = "0x" + "f" * 500
    result = [
        Case(
            "explicit audio values",
            valid,
            0,
            stdout_terms=(
                "audio.source: bluez_input.example.0",
                "audio.sink: bluez_output.example.1",
                "audio.profile: headset-head-unit-msbc",
            ),
        ),
        Case(
            "numeric defaults",
            audio_yaml(omitted=("sample_rate", "channels", "frame_ms")),
            0,
        ),
        Case(
            "non-Bluetooth names and other profile",
            audio_yaml({
                "source": '"alsa_input.example"',
                "sink": '"alsa_output.example"',
                "profile": '"custom-profile"',
            }),
            0,
            stdout_terms=(
                "audio.source: alsa_input.example",
                "audio.sink: alsa_output.example",
                "audio.profile: custom-profile",
            ),
        ),
        Case(
            "check-config before config",
            valid,
            0,
            arguments=("--check-config", "--config", "{config}"),
        ),
        Case("help", None, 0, arguments=("--help",)),
        Case(
            "help does not read missing config",
            None,
            0,
            arguments=("--config", "{config}", "--help"),
        ),
        Case("missing file", None, error_terms=("missing-file.yaml",)),
        Case("malformed YAML", "audio: [\n", error_terms=("malformed-yaml.yaml",)),
        Case("empty document", ""),
        Case("scalar root", "hello\n"),
        Case("sequence root", "- audio\n"),
        Case("missing audio", "{}\n", error_terms=("audio",)),
        Case("null audio", "audio: null\n", error_terms=("audio",)),
        Case("scalar audio", "audio: hello\n", error_terms=("audio",)),
        Case("sequence audio", "audio: []\n", error_terms=("audio",)),
        Case(
            "monitor source",
            audio_yaml({"source": '"alsa_output.example.monitor"'}),
            error_terms=("source",),
        ),
        Case(
            "unknown root field",
            valid + "unexpected_root: true\n",
            error_terms=("unexpected_root",),
        ),
        Case(
            "unknown audio field",
            audio_yaml(extra="  unexpected_audio: true\n"),
            error_terms=("unexpected_audio",),
        ),
        Case(
            "duplicate root key",
            valid + valid,
            error_terms=("audio",),
        ),
        Case(
            "duplicate audio key",
            audio_yaml(extra='  source: "second_input"\n'),
            error_terms=("source",),
        ),
        Case("multiple YAML documents", valid + "---\n" + valid),
        Case("no arguments", None, arguments=()),
        Case("unknown argument", None, arguments=("--unknown",)),
        Case("config missing value", None, arguments=("--config",)),
        Case(
            "option instead of config value",
            None,
            arguments=("--config", "--check-config"),
        ),
        Case(
            "missing check-config",
            valid,
            arguments=("--config", "{config}"),
        ),
        Case("missing config option", None, arguments=("--check-config",)),
        Case(
            "duplicate config option",
            valid,
            arguments=(
                "--config", "{config}", "--config", "{config}", "--check-config",
            ),
        ),
        Case(
            "duplicate check-config option",
            valid,
            arguments=("--config", "{config}", "--check-config", "--check-config"),
        ),
    ]
    for field in ("source", "sink", "profile"):
        result.append(Case(
            "missing " + field,
            audio_yaml(omitted=(field,)),
            error_terms=(field,),
        ))
        result.append(Case(
            "whitespace " + field,
            audio_yaml({field: '"   "'}),
            error_terms=(field,),
        ))
    for label, field, value in (
        ("empty", "source", '""'),
        ("null", "source", "null"),
        ("sequence", "sink", "[]"),
        ("mapping", "profile", "{}"),
    ):
        result.append(Case(
            label + " " + field,
            audio_yaml({field: value}),
            error_terms=(field,),
        ))
    for field, unsupported in (
        ("sample_rate", "48000"),
        ("channels", "2"),
        ("frame_ms", "10"),
    ):
        result.append(Case(
            "unsupported " + field,
            audio_yaml({field: unsupported}),
            error_terms=(field,),
        ))
    for label, field, value in (
        ("decimal", "sample_rate", "16000.0"),
        ("text", "channels", '"not-an-integer"'),
        ("boolean", "frame_ms", "true"),
        ("null", "sample_rate", "null"),
        ("overflow", "sample_rate", "999999999999999999999999999999"),
    ):
        result.append(Case(
            label + " " + field,
            audio_yaml({field: value}),
            error_terms=(field,),
        ))
    for label, field, value in (
        ("plain integer device", "source", "123"),
        ("plain boolean device", "sink", "true"),
        ("plain float device", "profile", "1.5"),
        ("plain overflowing exponent device", "source", "1e9999"),
        ("plain overflowing integer device", "sink", large_integer),
        ("plain overflowing hex device", "profile", large_hex_integer),
        ("explicit integer device", "source", "!!int 123"),
        ("quoted numeric parameter", "sample_rate", '"16000"'),
        ("explicit string parameter", "channels", "!!str 1"),
    ):
        result.append(Case(
            label,
            audio_yaml({field: value}),
            error_terms=(field,),
        ))
    for label, field, value in (
        ("quoted numeric device string", "source", '"123"'),
        ("explicit numeric device string", "source", "!!str 123"),
        ("quoted overflowing exponent device string", "source", '"1e9999"'),
        (
            "quoted overflowing integer device string", "sink",
            '"' + large_integer + '"',
        ),
        (
            "explicit overflowing hex device string", "profile",
            "!!str " + large_hex_integer,
        ),
        ("explicit integer parameter", "sample_rate", "!!int 16000"),
    ):
        result.append(Case(
            label,
            audio_yaml({field: value}),
            0,
        ))
    return result


def run_case(executable, directory, case):
    filename = case.name.replace(" ", "-").lower() + ".yaml"
    config_path = directory / filename
    if case.yaml_text is not None:
        config_path.write_text(case.yaml_text, encoding="utf-8")
    arguments = case.arguments
    if arguments is None:
        arguments = ("--config", "{config}", "--check-config")
    command = [str(executable)] + [
        argument.replace("{config}", str(config_path)) for argument in arguments
    ]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return "{}: {}".format(case.name, error)

    problems = []
    if completed.returncode != case.expected_code:
        problems.append("exit {}, expected {}".format(
            completed.returncode, case.expected_code,
        ))
    if case.expected_code == 0:
        if "--help" in arguments:
            if not (completed.stdout.strip() or completed.stderr.strip()):
                problems.append("help produced no output")
        else:
            if "CONFIG_OK" not in completed.stdout:
                problems.append("stdout missing CONFIG_OK")
            expected_lines = (
                "audio.sample_rate: 16000 Hz",
                "audio.channels: 1",
                "audio.frame_ms: 20 ms",
            ) + case.stdout_terms
            output_lines = completed.stdout.splitlines()
            for line in expected_lines:
                if line not in output_lines:
                    problems.append("stdout missing line {!r}".format(line))
    else:
        if not completed.stderr.strip():
            problems.append("stderr is empty")
        for term in case.error_terms:
            if term.casefold() not in completed.stderr.casefold():
                problems.append("stderr missing {!r}".format(term))
    if problems:
        return "{}: {}\n  stdout: {!r}\n  stderr: {!r}".format(
            case.name, "; ".join(problems), completed.stdout, completed.stderr,
        )
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="path to voice_engine")
    arguments = parser.parse_args()
    executable = arguments.executable.resolve()
    if not executable.is_file():
        parser.error("executable does not exist: {}".format(executable))

    test_cases = cases()
    failures = []
    with tempfile.TemporaryDirectory(prefix="voice-engine-config-test-") as temp:
        directory = Path(temp)
        for case in test_cases:
            failure = run_case(executable, directory, case)
            if failure:
                failures.append(failure)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("{} of {} config checks failed".format(
            len(failures), len(test_cases),
        ), file=sys.stderr)
        return 1
    print("All {} config checks passed".format(len(test_cases)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
