#!/usr/bin/env python3
"""Validate the complete configuration contract without hardware or model assets."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import subprocess
import sys
import tempfile


AUDIO = """audio:
  source: "test_input"
  sink: "test_output"
  profile: "test_profile"
"""

DEFAULTS = {
    "logging": {"max_file_bytes": "10485760", "max_files": "10"},
    "models": {
        name: '{enabled: false, id: ""}'
        for name in ("vad", "asr", "llm", "tts", "kws")
    },
    "assets": {
        "models_manifest": '"../models/manifest.json"',
        "deps_manifest": '"../deps/manifest.json"',
    },
    "vad": {
        "pre_roll_ms": "500", "min_speech_ms": "200", "end_silence_ms": "600",
        "start_timeout_ms": "5000", "max_speech_ms": "15000",
    },
    "threads": {"vad": "1", "asr": "1", "llm": "1", "tts": "2", "kws": "1"},
    "llm": {"context_tokens": "2048", "max_output_tokens": "128", "margin_tokens": "64"},
    "tts": {"min_chars": "8", "max_chars": "40", "flush_ms": "300", "prebuffer_ms": "0", "target_chars": "0", "speed": "1.0", "boundary_lookahead_chars": "0", "sentence_mode": "false", "join_short_sentences": "false", "first_segment": "null"},
    "queues": {
        "capture_ms": "2000", "pending_turns": "1", "llm_text_bytes": "8192",
        "tts_segments": "4", "playback_ms": "2000", "max_tts_audio_ms": "20000",
    },
    "timeouts": {
        "asr_final_ms": "3000", "llm_first_token_ms": "10000", "llm_total_ms": "30000",
        "tts_segment_ms": "10000", "cancel_ms": "2000",
    },
    "dialogue": {
        "wake_enabled": "false", "barge_in": "false", "wake_word": '"你好小派"',
        "window_ms": "15000", "barge_in_min_ms": "250",
    },
}

# Field boundaries are public configuration behavior, including inactive modules.
BOUNDS = {
    "logging.max_file_bytes": (1024, 10485760), "logging.max_files": (1, 10),
    "vad.pre_roll_ms": (0, 10000), "vad.min_speech_ms": (20, 30000),
    "vad.end_silence_ms": (20, 10000), "vad.start_timeout_ms": (20, 60000),
    "vad.max_speech_ms": (20, 60000),
    **{"threads." + name: (1, 16) for name in ("vad", "asr", "llm", "tts", "kws")},
    "llm.context_tokens": (128, 2048), "llm.max_output_tokens": (1, 128),
    "llm.margin_tokens": (0, 1024),
    "tts.min_chars": (1, 256), "tts.max_chars": (1, 1024), "tts.flush_ms": (1, 5000), "tts.prebuffer_ms": (0, 2000), "tts.target_chars": (0, 1024),
    "queues.capture_ms": (20, 10000), "queues.pending_turns": (1, 1),
    "queues.llm_text_bytes": (1, 1048576), "queues.tts_segments": (1, 64),
    "queues.playback_ms": (20, 10000), "queues.max_tts_audio_ms": (20, 60000),
    "timeouts.asr_final_ms": (1, 120000), "timeouts.llm_first_token_ms": (1, 120000),
    "timeouts.llm_total_ms": (1, 300000), "timeouts.tts_segment_ms": (1, 120000),
    "timeouts.cancel_ms": (1, 10000),
    "dialogue.window_ms": (20, 60000), "dialogue.barge_in_min_ms": (20, 5000),
}


@dataclass(frozen=True)
class Case:
    name: str
    text: str
    error: str = ""


def yaml_config(changes=None):
    groups = {name: values.copy() for name, values in DEFAULTS.items()}
    for group, values in (changes or {}).items():
        groups[group].update(values)
    return AUDIO + "".join(
        group + ":\n" + "".join("  {}: {}\n".format(key, value) for key, value in values.items())
        for group, values in groups.items()
    )


def changed(field, value):
    group, key = field.split(".")
    return yaml_config({group: {key: value}})


def cases():
    yield Case("all explicit defaults", yaml_config())
    first = "{min_chars: 8, target_chars: 15, boundary_lookahead_chars: 2}"
    yield Case("dynamic first override", yaml_config({"tts": {
        "first_segment": first, "min_chars": "10", "target_chars": "24",
        "boundary_lookahead_chars": "4", "sentence_mode": "false"}}))
    for value in ("true", "[]", "15", '"override"', "{}",
                  "{min_chars: 8, target_chars: 15}",
                  "{min_chars: 8, target_chars: 15, boundary_lookahead_chars: 2, typo: 1}",
                  "{min_chars: 16, target_chars: 15, boundary_lookahead_chars: 2}",
                  "{min_chars: 8, target_chars: 39, boundary_lookahead_chars: 2}"):
        yield Case("invalid first override "+value, changed("tts.first_segment",value), "tts.first_segment")
    for key,values in {
        "min_chars":("0","257","1.5","true",'"8"',"null"),
        "target_chars":("0","1025","1.5","true",'"15"',"null"),
        "boundary_lookahead_chars":("-1","33","1.5","true",'"2"',"null")
    }.items():
        for value in values:
            parts={"min_chars":"8","target_chars":"15","boundary_lookahead_chars":"2"}
            parts[key]=value
            mapping="{"+", ".join(k+": "+v for k,v in parts.items())+"}"
            yield Case("invalid first "+key+" "+value,changed("tts.first_segment",mapping),"tts.first_segment."+key)
    yield Case("first override conflicts with sentence mode",
               yaml_config({"tts":{"first_segment":first,"sentence_mode":"true"}}),"tts.first_segment")

    yield Case("join short sentences", changed("tts.join_short_sentences", "true"))
    for value in ("1", "0", '"true"', "null", "[]", "{}"):
        yield Case("invalid join short sentences " + value,
                   changed("tts.join_short_sentences", value), "tts.join_short_sentences")
    yield Case("join with bounded realtime segments", yaml_config({"tts": {
        "join_short_sentences": "true", "min_chars": "12", "target_chars": "12",
        "speed": "0.9", "prebuffer_ms": "1000"}}))
    yield Case("sentence policy", changed("tts.sentence_mode", "true"))
    for value in ("1", "0", "null", '"true"', "[]", "{}"):
        yield Case("invalid sentence policy " + value, changed("tts.sentence_mode", value), "tts.sentence_mode")
    yield Case("sentence policy conflicts with target", yaml_config({"tts": {"sentence_mode": "true", "target_chars": "12"}}), "tts.sentence_mode")
    yield Case("sentence policy conflicts with lookahead", yaml_config({"tts": {"sentence_mode": "true", "target_chars": "12", "boundary_lookahead_chars": "6"}}), "tts.sentence_mode")
    yield Case("punctuation lookahead", yaml_config({"tts": {"target_chars": "12", "boundary_lookahead_chars": "6"}}))
    yield Case("maximum punctuation lookahead", yaml_config({"tts": {"target_chars": "8", "boundary_lookahead_chars": "32"}}))
    for value in ("-1", "33", "1.5", "true", '"6"', "null"):
        yield Case("invalid punctuation lookahead " + value,
                   yaml_config({"tts": {"target_chars": "12", "boundary_lookahead_chars": value}}),
                   "tts.boundary_lookahead_chars")
    yield Case("lookahead needs target", changed("tts.boundary_lookahead_chars", "6"), "tts.boundary_lookahead_chars")
    yield Case("lookahead exceeds maximum", yaml_config({"tts": {"target_chars": "38", "boundary_lookahead_chars": "6"}}),
               "tts.boundary_lookahead_chars")
    yield Case("legacy audio only", AUDIO)
    yield Case("empty optional mappings", AUDIO + "".join(group + ": {}\n" for group in DEFAULTS))
    yield Case("inactive assets need no manifests", changed("assets.models_manifest", '"missing.json"'))
    yield Case("inactive model may carry an id", changed("models.asr", '{id: "future-asr"}'))
    yield Case("zero pre roll and margin", yaml_config({"vad": {"pre_roll_ms": "0"}, "llm": {"margin_tokens": "0"}}))
    yield Case("tight context budget", yaml_config({"llm": {"context_tokens": "128", "max_output_tokens": "63", "margin_tokens": "64"}}))
    yield Case("equal VAD and ring limits", yaml_config({"vad": {"pre_roll_ms": "2000", "min_speech_ms": "15000"}}))
    yield Case("soft target at minimum", changed("tts.target_chars", "8"))
    yield Case("soft target at maximum", changed("tts.target_chars", "40"))
    yield Case("equal first token and total timeout", changed("timeouts.llm_first_token_ms", "30000"))
    yield Case("empty inactive wake word", changed("dialogue.wake_word", '""'))
    yield Case("one capture frame and two playback frames", yaml_config({"vad": {"pre_roll_ms": "0"}, "queues": {"capture_ms": "20", "playback_ms": "40"}}))
    yield Case("two capture frames and one playback frame", yaml_config({"vad": {"pre_roll_ms": "0"}, "queues": {"capture_ms": "40", "playback_ms": "20"}}))

    for value in ("0.5", "0.95", "1", "1.0", "2.0", "!!float 0.95", "!!int 1"):
        yield Case("TTS speed valid " + value, changed("tts.speed", value))
    for value in ("0.499", "2.001", "0", "-1", ".nan", ".inf", "-.inf",
                  "1e309", "true", "null", "[]", "{}", '"0.95"', "!!str 1"):
        yield Case("TTS speed invalid " + value, changed("tts.speed", value), "tts.speed")

    for field, (minimum, maximum) in BOUNDS.items():
        for label, value in (
            ("below range", str(minimum - 1)),
            ("above range", str(maximum + 1)),
        ):
            yield Case(field + " " + label, changed(field, value), field)

    for field, invalid in (
        ("threads.tts", '"2"'),
        ("vad.pre_roll_ms", "1.5"),
        ("llm.margin_tokens", "true"),
        ("tts.flush_ms", "null"),
        ("tts.target_chars", "true"),
        ("tts.target_chars", "12.5"),
        ("tts.target_chars", '"12"'),
        ("tts.prebuffer_ms", "true"),
        ("tts.prebuffer_ms", "0.5"),
        ("tts.prebuffer_ms", '"2000"'),
        ("queues.llm_text_bytes", "99999999999999999999999999"),
        ("timeouts.cancel_ms", "[]"),
        ("dialogue.window_ms", "!!str 1000"),
    ):
        yield Case(field + " invalid type", changed(field, invalid), field)

    for group in DEFAULTS:
        yield Case(group + " unknown", AUDIO + group + ": {unexpected: 1}\n", group + ".unexpected")
        key, default = next(iter(DEFAULTS[group].items()))
        yield Case(group + " duplicate", AUDIO + group + ":\n  " + key + ": " + default + "\n  " + key + ": " + default + "\n", group + "." + key)

    for group, invalid in (("vad", "null"), ("queues", "[]"), ("models", "true")):
        yield Case(group + " invalid mapping", AUDIO + group + ": " + invalid + "\n", group)

    for field in ("dialogue.wake_enabled", "dialogue.barge_in"):
        for invalid in ('"true"', "1", "null"):
            yield Case(field + " invalid " + invalid, changed(field, invalid), field)
    for field, invalid in (
        ("assets.models_manifest", "123"),
        ("assets.deps_manifest", "false"),
        ("dialogue.wake_word", "[]"),
        ("assets.models_manifest", "null"),
        ("dialogue.wake_word", "!!int 42"),
        ("assets.models_manifest", '"bad\\0path"'),
    ):
        yield Case(field + " invalid " + invalid, changed(field, invalid), field)
    yield Case("source NUL", AUDIO.replace('"test_input"', '"bad\\0source"'), "audio.source")
    for field in ("assets.models_manifest", "assets.deps_manifest"):
        yield Case(field + " blank", changed(field, '"   "'), field)

    for model in ("vad", "asr", "llm", "tts", "kws"):
        section = "models." + model
        for label, value, field in (
            ("missing enabled id", "{enabled: true}", ".id"),
            ("blank enabled id", '{enabled: true, id: "   "}', ".id"),
        ):
            yield Case(section + " " + label, changed(section, value), section + field)

    for label, value, field in (
        ("null mapping", "null", "models.vad"),
        ("sequence mapping", "[]", "models.vad"),
        ("scalar mapping", "false", "models.vad"),
        ("unknown", "{unexpected: 1}", "models.vad.unexpected"),
        ("duplicate", "{enabled: false, enabled: false}", "models.vad.enabled"),
        ("quoted boolean", '{enabled: "false"}', "models.vad.enabled"),
        ("numeric boolean", "{enabled: 0}", "models.vad.enabled"),
        ("numeric id", "{id: 123}", "models.vad.id"),
        ("null id", "{id: null}", "models.vad.id"),
    ):
        yield Case("model " + label, changed("models.vad", value), field)

    for name, changes, field in (
        ("fractional capture frame", {"vad": {"pre_roll_ms": "0"}, "queues": {"capture_ms": "21"}}, "queues.capture_ms"),
        ("fractional playback frame", {"queues": {"playback_ms": "21"}}, "queues.playback_ms"),
        ("pre roll exceeds ring", {"vad": {"pre_roll_ms": "2001"}}, "vad.pre_roll_ms"),
        ("speech minimum exceeds maximum", {"vad": {"min_speech_ms": "16000"}}, "vad.min_speech_ms"),
        ("TTS target below minimum", {"tts": {"target_chars": "7"}}, "tts.target_chars"),
        ("TTS target exceeds maximum", {"tts": {"target_chars": "41"}}, "tts.target_chars"),
        ("TTS minimum exceeds maximum", {"tts": {"min_chars": "41"}}, "tts.min_chars"),
        ("context exhausted", {"llm": {"context_tokens": "192"}}, "llm.context_tokens"),
        ("first token after total", {"timeouts": {"llm_total_ms": "9999"}}, "timeouts.llm_first_token_ms"),
        ("wake without KWS", {"dialogue": {"wake_enabled": "true"}}, "models.kws.enabled"),
        ("wake with empty word", {"dialogue": {"wake_enabled": "true", "wake_word": '" "'}, "models": {"kws": '{enabled: true, id: "test-kws"}'}}, "dialogue.wake_word"),
        ("barge in without models", {"dialogue": {"barge_in": "true"}}, "dialogue.barge_in"),
        ("barge in without ASR", {"dialogue": {"barge_in": "true"}, "models": {"vad": '{enabled: true, id: "test-vad"}'}}, "models.asr.enabled"),
    ):
        yield Case(name, yaml_config(changes), field)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error("executable does not exist: {}".format(executable))
    failures = []
    test_cases = list(cases())
    with tempfile.TemporaryDirectory(prefix="voice-engine-full-config-") as directory:
        path = Path(directory) / "config.yaml"
        for case in test_cases:
            path.write_text(case.text, encoding="utf-8")
            try:
                result = subprocess.run(
                    [str(executable), "--config", str(path), "--check-config"],
                    capture_output=True, text=True, timeout=5, check=False,
                )
            except (OSError, subprocess.TimeoutExpired) as error:
                failures.append("{}: {}".format(case.name, error))
                continue
            expected = 2 if case.error else 0
            valid = result.returncode == expected
            if case.error:
                valid = valid and case.error in result.stderr and "CONFIG_OK" not in result.stdout
            else:
                valid = valid and "CONFIG_OK" in result.stdout
            if not valid:
                failures.append("{}: exit {}, expected {}; stdout={!r}; stderr={!r}".format(
                    case.name, result.returncode, expected, result.stdout, result.stderr,
                ))
    if failures:
        print("\n".join("FAIL: " + failure for failure in failures), file=sys.stderr)
        return 1
    print("All {} full config checks passed".format(len(test_cases)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
