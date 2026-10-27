#!/usr/bin/env python3
"""Fail fast through the real CLI before any model or device is initialized.

Only model-enabled builds register this test. Temporary configs either leave
models disabled or use synthetic IDs with explicitly missing manifests, and
point Pulse at a nonexistent private socket. No private models can load.
This checks preflight ordering, not active-model exceptional cleanup.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def require(ok, why):
    if not ok:
        raise AssertionError(why)


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="ve-realtime-preflight-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        env["PULSE_SERVER"] = f"unix:{root}/no-server"
        env["PULSE_CLIENTCONFIG"] = str(root / "pulse-client.conf")
        Path(env["PULSE_CLIENTCONFIG"]).write_text("autospawn = no\n", encoding="utf-8")

        def config(name, capture=2000, playback=2000, enabled=False):
            path = root / f"{name}.yaml"
            model_settings = ""
            if enabled:
                model_settings = "models:\n" + "".join(
                    f"  {name}: {{enabled: true, id: preflight-{name}}}\n"
                    for name in ("vad", "asr", "llm", "tts"))
                model_settings += (
                    "assets:\n  models_manifest: intentionally-absent-models.json\n"
                    "  deps_manifest: intentionally-absent-deps.json\n")
            path.write_text(
                "audio:\n  source: private_preflight_input\n  sink: private_preflight_output\n"
                "  profile: none\nvad:\n  pre_roll_ms: 0\n"
                f"queues:\n  capture_ms: {capture}\n  playback_ms: {playback}\n" + model_settings,
                encoding="utf-8")
            return path

        def invoke(name, settings, output, expected, diagnostic):
            result = subprocess.run(
                [executable, "--config", str(settings), "--control", "stdin",
                 "--output-dir", str(output), "--run-id", name],
                input="quit\n", capture_output=True, text=True, env=env, timeout=8)
            require(result.returncode == expected,
                    f"{name}: exit {result.returncode}, expected {expected}: {result.stdout} {result.stderr}")
            require(diagnostic in result.stderr,
                    f"{name}: missing diagnostic {diagnostic!r}: {result.stderr}")
            require("ENGINE_READY" not in result.stdout and "models_ready" not in result.stdout,
                    f"{name}: unexpectedly initialized real models")
            return result

        for name, capture, playback in (("capture-one-frame", 20, 60),
                                         ("playback-one-frame", 40, 20),
                                         ("playback-two-frames", 40, 40)):
            output = root / f"{name}-runs"
            result = invoke(name, config(name, capture, playback), output, 2,
                            "realtime requires 20 ms frames, capture_ms >= 40 and playback_ms >= 60")
            require("ENGINE_STARTING" not in result.stdout and not output.exists(),
                    f"{name}: allocated run/model state before queue preflight rejection")

        valid = config("minimum", 40, 60)
        output = root / "minimum-runs"
        result = invoke("disabled-models", valid, output, 2,
                        "realtime requires VAD/ASR/LLM/TTS enabled")
        require("ENGINE_STARTING" not in result.stdout and not output.exists(),
                "invalid realtime model configuration created a run directory or began initialization")

        for name, extra, diagnostic in (
            ("wake-no-keyword-file", "  kws: {enabled: true, id: preflight-kws}\ndialogue:\n  wake_enabled: true\n",
             "wake mode requires dialogue.keywords_file"),
            ("kws-without-wake", "  kws: {enabled: true, id: preflight-kws}\n",
             "realtime KWS and wake_enabled"),
            ("barge-in-oversized-confirmation", "dialogue:\n  barge_in: true\n  barge_in_min_ms: 5000\n",
             "barge-in capture_ms must cover")):
            path = config(name, enabled=True)
            text = path.read_text()
            if extra.startswith("  kws:"):
                text = text.replace("assets:\n", extra + "assets:\n")
            else:
                text += extra
            path.write_text(text)
            invoke(name, path, root / (name + "-runs"), 2, diagnostic)

        # Legal structure reaches logger construction, but intentionally missing
        # asset manifests prevent any real model load even if ordering regresses.
        valid = config("logging-valid-structure", 40, 60, enabled=True)
        blocked = root / "output-is-file"
        blocked.write_text("keep existing data\n", encoding="utf-8")
        barge = config("barge-valid-structure", 2000, 60, enabled=True)
        barge.write_text(barge.read_text() + "dialogue:\n  barge_in: true\n")
        invoke("barge-logging-rejection", barge, blocked, 1, "LOG_ERROR:")
        result = invoke("logging-rejection", valid, blocked, 1, "LOG_ERROR:")
        require("ENGINE_STARTING" not in result.stdout and
                blocked.read_text(encoding="utf-8") == "keep existing data\n",
                "log path failure initialized models or changed an unrelated file")
        existing = root / "existing-runs"
        (existing / "preserved").mkdir(parents=True)
        marker = existing / "preserved" / "keep.txt"
        marker.write_text("existing run", encoding="utf-8")
        result = invoke("preserved", valid, existing, 1, "LOG_ERROR:")
        require("ENGINE_STARTING" not in result.stdout and marker.read_text(encoding="utf-8") == "existing run",
                "existing run protection failed before model initialization")

    print("REALTIME_PREFLIGHT_OK: queue minima, disabled models, unusable/existing log destinations; no models/devices run")


if __name__ == "__main__":
    try:
        main()
    except (OSError, AssertionError, subprocess.TimeoutExpired) as error:
        print(f"realtime preflight failed: {error}", file=sys.stderr)
        sys.exit(1)
