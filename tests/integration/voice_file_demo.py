#!/usr/bin/env python3
"""P3 CLI/header preflight regression; deliberately missing assets prevent model use."""
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
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True,
                            text=True, timeout=10)
    assert result.returncode == code, (
        args, result.returncode, result.stdout, result.stderr
    )
    checks += 1
    return result


def without_lock_ancestor(args):
    """Leave the outer CTest flock ancestry without acquiring/releasing its lock.

    The short launcher exits; its orphan waits for reparenting before starting the
    probe. An inherited stdout pipe carries the actual probe status back. Neither
    process can load models because the fixture's asset manifests do not exist.
    """
    global checks
    script = r"""
import json, os, subprocess, sys, time
launcher = os.getpid()
if os.fork():
    os._exit(0)
os.setsid()
deadline = time.monotonic() + 3
while os.getppid() == launcher:
    if time.monotonic() >= deadline:
        raise RuntimeError("launcher was not reparented")
    time.sleep(0.01)
result = subprocess.run(sys.argv[1:], capture_output=True, text=True, timeout=5)
print(json.dumps({"returncode": result.returncode, "stdout": result.stdout,
                  "stderr": result.stderr}), flush=True)
"""
    result = subprocess.run([sys.executable, "-c", script, str(exe), *map(str, args)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (result.stdout, result.stderr)
    report = json.loads(result.stdout)
    assert report["returncode"] == 2, report
    assert "exclusive lock" in report["stderr"], report
    checks += 1


help_text = run(["--help"], 0).stdout
for flag in ("--config", "--manifest", "--output-dir", "--run-id", "--sample",
             "--warmup-rounds", "--tts-mode", "--validate-input-only", "flock"):
    assert flag in help_text, (flag, help_text)
run([], 2)
run(["--unknown", "x"], 2)

with tempfile.TemporaryDirectory(prefix="voice-file-cli-") as directory:
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
        "audio": {"source": "file-input", "sink": "file-output",
                  "profile": "file-only", "sample_rate": 16000,
                  "channels": 1, "frame_ms": 20},
        "models": {key: {"enabled": True, "id": value}
                   for key, value in model_ids.items()},
        # Intentionally nonexistent: preflight must neither read nor hash assets.
        "assets": {"models_manifest": "missing-model-assets.json",
                   "deps_manifest": "missing-runtime-assets.json"},
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

    def write_manifest(value=None):
        manifest.write_text(json.dumps(dataset if value is None else value),
                            encoding="utf-8")

    def args(extra=(), validate=True):
        request = ["--config", config, "--manifest", manifest,
                   "--output-dir", output, "--run-id", "cli-only"]
        return request + (["--validate-input-only"] if validate else []) + list(extra)

    def replace(request, flag, value):
        request[request.index(flag) + 1] = value
        return request

    def preflight(extra=(), count=2, rounds=1, mode="stream", request=None):
        result = run(args(extra) if request is None else request, 0)
        marker = "VOICE_FILE_INPUT_OK"
        assert marker in result.stdout, result.stdout
        report = json.loads(result.stdout.split(marker, 1)[1].strip())
        assert report["A07"] == "not_run", report
        assert report["selected_samples"] == count, report
        assert report["warmup_rounds"] == rounds, report
        assert report["tts_mode"] == mode, report
        assert not output.exists(), "preflight created a runtime output directory"

    write_config()
    write_wav(root / "speech.wav")
    sample = {
        "sample_id": "speech_a", "path": "speech.wav",
        "sha256": "0" * 64,  # Valid shape, deliberately incorrect file digest.
        "sample_rate": 16000, "channels": 1, "sample_width_bytes": 2,
        "num_samples": 1600, "duration_sec": 0.1,
        "reference_text": "hello", "normalized_reference": "hello",
        "keywords": ["hello"], "category": "common",
        "expected_behavior": "speech",
    }
    dataset = {"schema_version": 1, "samples": [
        sample, dict(sample, sample_id="speech_b")
    ]}
    write_manifest()
    preflight()
    preflight(["--warmup-rounds", "1"])
    preflight(["--warmup-rounds", "20"], rounds=20)
    preflight(["--tts-mode", "stream"])
    preflight(["--tts-mode", "batch"], mode="batch")
    preflight(["--sample", "speech_b"], count=1)
    preflight(["--sample", "speech_b", "--sample", "speech_a"])
    request = args()
    index = request.index("--run-id")
    del request[index:index + 2]
    preflight(request=request)

    for value in ("0", "-1", "21", "1.5", "+1", "1x", "nan", "9" * 30):
        run(args(["--warmup-rounds", value]), 2)
    for extra in (
        ["--warmup-rounds"], ["--tts-mode"], ["--sample"],
        ["--tts-mode", "unknown"], ["--tts-mode", "STREAM"],
        ["--sample", "missing"], ["--sample", "speech_a", "--sample", "speech_a"],
        ["--warmup-rounds", "1", "--warmup-rounds", "2"],
        ["--tts-mode", "stream", "--tts-mode", "batch"],
        ["--validate-input-only"], ["--config", config], ["--manifest", manifest],
        ["--output-dir", output], ["--run-id", "duplicate"], ["--unknown", "x"],
    ):
        run(args(extra), 2)
    for flag in ("--config", "--manifest", "--run-id", "--output-dir", "--sample",
                 "--tts-mode", "--warmup-rounds"):
        request = args()
        if flag in request:
            index = request.index(flag)
            del request[index:index + 2]
        run([*request, flag, ""], 2)
    for invalid in ("../escape", "/absolute", ".hidden", "a/b", "a b", "x" * 65):
        run(replace(args(), "--run-id", invalid), 2)
    for missing in ("--config", "--manifest"):
        request = args()
        index = request.index(missing)
        del request[index:index + 2]
        run(request, 2)
    request = args(validate=False)
    index = request.index("--run-id")
    del request[index:index + 2]
    run(request, 2)
    run(replace(args(), "--config", root / "absent.yaml"), 2)
    run(replace(args(), "--manifest", root / "absent.json"), 2)

    for key in model_ids:
        value = copy.deepcopy(config_data)
        value["models"][key]["enabled"] = False
        write_config(value)
        run(args(), 2)
        value = copy.deepcopy(config_data)
        value["models"][key]["id"] = "unexpected-model"
        write_config(value)
        run(args(), 2)
    for section, key, setting in (
        ("models", "kws", {"enabled": True, "id": "unexpected-kws"}),
        ("threads", "tts", 1), ("audio", "profile", "default"),
        ("dialogue", "wake_enabled", True), ("dialogue", "barge_in", True),
    ):
        value = copy.deepcopy(config_data)
        value.setdefault(section, {})[key] = setting
        write_config(value)
        run(args(), 2)
    config.write_text("audio: [malformed\n", encoding="utf-8")
    run(args(), 2)
    write_config(dict(config_data, unknown_field=1))
    run(args(), 2)
    write_config()

    for value in ({}, [], {"schema_version": 2, "samples": [sample]},
                  {"schema_version": True, "samples": [sample]},
                  {"schema_version": 1, "samples": []},
                  dict(dataset, samples=[sample, sample]),
                  dict(dataset, samples=[dict(sample, sample_id=str(i)) for i in range(257)])):
        write_manifest(value)
        run(args(), 2)
    manifest.write_text("{malformed", encoding="utf-8")
    run(args(), 2)
    for key, value in (
        ("sample_id", ""), ("sample_id", "../escape"), ("sample_id", "x" * 65),
        ("path", "../escape.wav"), ("path", "/etc/passwd"),
        ("path", "missing.wav"), ("path", "speech\u0000.wav"), ("path", "."),
        ("sample_rate", 8000), ("sample_rate", 16000.0),
        ("channels", 2), ("channels", True), ("sample_width_bytes", 1),
        ("num_samples", 1601), ("num_samples", -1), ("num_samples", 0),
        ("num_samples", 240001), ("num_samples", 1600.0),
        ("duration_sec", -0.1), ("duration_sec", 0.2), ("duration_sec", "0.1"),
        ("expected_behavior", "noise"), ("sha256", "not-a-hash"),
        ("sha256", "A" * 64),
    ):
        write_manifest({"schema_version": 1, "samples": [dict(sample, **{key: value})]})
        run(args(), 2)

    # Duplicate IDs must be rejected even if that ID is not selected.
    write_manifest({"schema_version": 1, "samples": [
        sample, dict(sample, sample_id="speech_b"), dict(sample, sample_id="speech_b")
    ]})
    run(args(["--sample", "speech_a"]), 2)
    # Explicit selection must not require nonselected WAVs to exist.
    write_manifest({"schema_version": 1, "samples": [
        sample, dict(sample, sample_id="omitted", path="not-present.wav")
    ]})
    preflight(["--sample", "speech_a"], count=1)

    outside = root / "outside"
    outside.mkdir()
    write_wav(outside / "real.wav")
    fixtures = root / "fixtures"
    fixtures.mkdir()
    (fixtures / "escape.wav").symlink_to(outside / "real.wav")
    escaped = fixtures / "manifest.json"
    escaped.write_text(json.dumps({"schema_version": 1, "samples": [
        dict(sample, path="escape.wav")
    ]}), encoding="utf-8")
    run(replace(args(), "--manifest", escaped), 2)

    write_manifest()
    for rate, channels, width in ((8000, 1, 2), (16000, 2, 2), (16000, 1, 1)):
        write_wav(root / "speech.wav", rate, channels, width)
        run(args(), 2)
    (root / "speech.wav").write_bytes(b"not a RIFF WAV")
    run(args(), 2)
    write_wav(root / "speech.wav")
    original = (root / "speech.wav").read_bytes()
    (root / "speech.wav").write_bytes(original[:-2])
    run(args(), 2)
    (root / "speech.wav").write_bytes(original)

    # Header-only zero-frame RIFF WAVs and silent PCM are valid boundary inputs.
    write_wav(root / "empty.wav", frames=0)
    boundary = dict(sample, sample_id="empty", path="empty.wav",
                    expected_behavior="empty", num_samples=0, duration_sec=0)
    write_manifest({"schema_version": 1, "samples": [
        sample, dict(sample, sample_id="silence", expected_behavior="silence"),
        dict(sample, sample_id="no_speech", expected_behavior="no_speech"), boundary
    ]})
    preflight(count=4)
    preflight(["--sample", "empty"], count=1)
    preflight(["--sample", "silence"], count=1)
    write_manifest()

    # This remains a no-model test when CTest itself has a flock ancestor.
    without_lock_ancestor(args(validate=False))
    assert not output.exists(), "lock rejection created a runtime output directory"

    existing_root = root / "existing"
    existing_run = existing_root / "cli-only"
    existing_run.mkdir(parents=True)
    sentinel = existing_run / "keep.txt"
    sentinel.write_text("keep original evidence\n", encoding="utf-8")
    request = replace(args(), "--output-dir", existing_root)
    preflight(request=request)
    rejected = run(replace(args(validate=False), "--output-dir", existing_root), 2)
    # Under the documented outer flock this reaches output collision detection;
    # a standalone invocation may reject earlier at the lock requirement.
    assert ("exclusive lock" in rejected.stderr or
            "run directory must be new" in rejected.stderr), rejected.stderr
    assert sentinel.read_text(encoding="utf-8") == "keep original evidence\n"
    assert list(existing_run.iterdir()) == [sentinel], "existing evidence overwritten"
    assert not output.exists(), "invalid/preflight input created a model run"

print(f"VOICE_FILE_DEMO_CLI_OK checks={checks}; no models or inference")
