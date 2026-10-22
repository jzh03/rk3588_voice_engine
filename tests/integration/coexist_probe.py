#!/usr/bin/env python3
"""CLI/config/fixture preflight only; no model lock, hashes, or inference."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

exe = Path(sys.argv[1]).resolve()
checks = 0


def run(args, code):
    global checks
    result = subprocess.run(
        [str(exe), *map(str, args)], capture_output=True, text=True, timeout=10
    )
    assert result.returncode == code, (
        args, result.returncode, result.stdout, result.stderr
    )
    checks += 1
    return result


help_text = run(["--help"], 0).stdout
for flag in ("--duration-sec", "--min-rounds", "--warmup-rounds", "--swap-policy", "--smoke",
             "--validate-input-only", "flock"):
    assert flag in help_text, (flag, help_text)
run([], 2)
run(["--unknown", "x"], 2)

with tempfile.TemporaryDirectory(prefix="coexist-cli-") as directory:
    root = Path(directory)
    config = root / "config.yaml"
    manifest = root / "manifest.json"
    output = root / "must-not-be-created"
    model_ids = {
        "vad": "silero-vad-v5.0",
        "asr": "zipformer-bilingual-zh-en-rk3588-fp16",
        "llm": "qwen3.5-2b-rk3588-w8a8",
        "tts": "vits-melo-tts-zh-en-fp32",
    }
    config_data = {
        "audio": {
            "source": "file-input", "sink": "file-output", "profile": "file-only",
            "sample_rate": 16000, "channels": 1, "frame_ms": 20,
        },
        "models": {key: {"enabled": True, "id": value}
                   for key, value in model_ids.items()},
        # Deliberately absent. Preflight must not resolve/hash any model assets.
        "assets": {
            "models_manifest": "missing-model-assets.json",
            "deps_manifest": "missing-runtime-assets.json",
        },
        "threads": {"vad": 1, "asr": 1, "llm": 1, "tts": 4},
    }
    config_data["models"]["kws"] = {"enabled": False, "id": ""}

    def write_config(value=None):
        config.write_text(json.dumps(config_data if value is None else value),
                          encoding="utf-8")

    def write_wav(path, rate=16000, channels=1, width=2, frames=1600):
        with wave.open(str(path), "wb") as stream:
            stream.setframerate(rate)
            stream.setnchannels(channels)
            stream.setsampwidth(width)
            stream.writeframes(bytes(frames * channels * width))

    write_config()
    write_wav(root / "speech.wav")
    sample = {
        "sample_id": "speech_a", "path": "speech.wav",
        "sha256": "0" * 64,  # Valid shape; intentionally no content hash.
        "sample_rate": 16000, "channels": 1, "sample_width_bytes": 2,
        "num_samples": 1600, "duration_sec": 0.1,
        "reference_text": "\u4f60\u597d", "normalized_reference": "\u4f60\u597d",
        "keywords": ["\u4f60\u597d"], "category": "chinese_common",
        "expected_behavior": "speech",
    }
    dataset = {"schema_version": 1, "samples": [
        sample, dict(sample, sample_id="speech_b")
    ]}

    def write_manifest(value=None):
        manifest.write_text(json.dumps(dataset if value is None else value),
                            encoding="utf-8")

    def args(extra=()):
        return ["--config", config, "--manifest", manifest,
                "--output-dir", output, "--run-id", "cli-only",
                "--validate-input-only", *extra]

    def preflight(extra=(), formal=True, warmup_rounds=5, swap_policy="review"):
        result = run(args(extra), 0)
        marker = "COEXIST_INPUT_OK"
        assert marker in result.stdout, result.stdout
        report = json.loads(result.stdout.split(marker, 1)[1].strip())
        assert report["A06"] == "not_run", report
        assert report["formal_eligible"] is formal, report
        assert report["warmup_rounds"] == warmup_rounds, report
        assert report["swap_policy"] == swap_policy, report
        assert not output.exists(), "preflight created a runtime evidence directory"

    write_manifest()
    preflight()
    preflight(["--swap-policy", "zero"], swap_policy="zero")
    preflight(["--swap-policy", "review"], swap_policy="review")
    preflight(["--warmup-rounds", "1"], warmup_rounds=1)
    preflight(["--warmup-rounds", "20"], warmup_rounds=20)
    preflight(["--sample", "speech_a", "--sample", "speech_b"])
    preflight(["--smoke", "--duration-sec", "1", "--min-rounds", "1"],
              formal=False)
    # Even a 300-second smoke request can never label itself a formal A06 pass.
    preflight(["--smoke", "--duration-sec", "300", "--min-rounds", "5"],
              formal=False)

    for flag, values in {
        "--duration-sec": ("0", "-1", "299", "3601", "1.5", "+300",
                           "300x", "999999999999999999999999"),
        "--min-rounds": ("0", "-1", "4", "1001", "5.5", "+5", "nan"),
        "--warmup-rounds": ("0", "-1", "21", "5.5", "+5", "nan"),
    }.items():
        for value in values:
            run(args([flag, value]), 2)
    for extra in (
        ["--smoke", "--duration-sec", "0"],
        ["--smoke", "--duration-sec", "3601"],
        ["--smoke", "--min-rounds", "0"],
        ["--smoke", "--min-rounds", "1001"],
        ["--duration-sec"], ["--min-rounds"], ["--warmup-rounds"], ["--swap-policy"], ["--sample"],
        ["--swap-policy", "unknown"], ["--swap-policy", "0"],
        ["--swap-policy", "zero", "--swap-policy", "review"],
        ["--sample", "missing"], ["--sample", "speech_a", "--sample", "speech_a"],
        ["--duration-sec", "300", "--duration-sec", "301"],
        ["--min-rounds", "5", "--min-rounds", "6"],
        ["--warmup-rounds", "5", "--warmup-rounds", "6"],
        ["--smoke", "--smoke"], ["--validate-input-only"],
        ["--config", config], ["--manifest", manifest],
        ["--output-dir", output], ["--run-id", "duplicate"],
        ["--unknown", "x"],
    ):
        run(args(extra), 2)

    for invalid in ("../escape", "/absolute", ".hidden", "a/b", "a b", "x" * 65):
        request = args()
        request[request.index("--run-id") + 1] = invalid
        run(request, 2)
    for missing_flag in ("--config", "--manifest"):
        request = args()
        index = request.index(missing_flag)
        del request[index:index + 2]
        run(request, 2)
    request = args()
    request[request.index("--config") + 1] = root / "absent.yaml"
    run(request, 2)

    for key in model_ids:
        value = copy.deepcopy(config_data)
        value["models"][key]["enabled"] = False
        write_config(value)
        run(args(), 2)
        value = copy.deepcopy(config_data)
        value["models"][key]["id"] = "unexpected-model"
        write_config(value)
        run(args(), 2)
    value = copy.deepcopy(config_data)
    value["models"]["kws"] = {"enabled": True, "id": "unexpected-kws"}
    write_config(value)
    run(args(), 2)
    config.write_text("audio: [malformed\n", encoding="utf-8")
    run(args(), 2)
    value = copy.deepcopy(config_data)
    value["unknown_field"] = 1
    write_config(value)
    run(args(), 2)
    write_config()

    for value in ({}, {"schema_version": 1, "samples": []},
                  dict(dataset, samples=[sample, sample])):
        write_manifest(value)
        run(args(), 2)
    for key, value in (
        ("path", "../escape.wav"), ("path", "/etc/passwd"),
        ("path", "missing.wav"), ("path", "speech\u0000.wav"),
        ("sample_rate", 8000), ("channels", 2),
        ("num_samples", 1601), ("num_samples", -1),
        ("duration_sec", -0.1), ("duration_sec", 0.2),
        ("duration_sec", 16), ("expected_behavior", "noise"),
        ("sha256", "not-a-hash"),
    ):
        write_manifest({"schema_version": 1, "samples": [dict(sample, **{key: value})]})
        run(args(), 2)
    # Exercise canonical confinement, not only a literal ".." check.
    outside = root / "outside"
    outside.mkdir()
    write_wav(outside / "real.wav")
    fixture_root = root / "fixtures"
    fixture_root.mkdir()
    (fixture_root / "escape.wav").symlink_to(outside / "real.wav")
    escaped = fixture_root / "manifest.json"
    escaped.write_text(json.dumps({"schema_version": 1, "samples": [
        dict(sample, path="escape.wav")
    ]}), encoding="utf-8")
    request = args()
    request[request.index("--manifest") + 1] = escaped
    run(request, 2)

    write_manifest()
    for rate, channels, width in ((8000, 1, 2), (16000, 2, 2), (16000, 1, 1)):
        write_wav(root / "speech.wav", rate, channels, width)
        run(args(), 2)
    assert not output.exists(), "invalid/preflight input created a model run"

print(f"COEXIST_PROBE_CLI_OK checks={checks}; no models or inference")
