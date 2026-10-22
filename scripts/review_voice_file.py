#!/usr/bin/env python3
"""Independent file-chain evidence audit. Run AFTER measurement, without models.

A07 retains its original 20/20 flow and >=18/20 content thresholds. Evidence
integrity checks below implement the engineering review; they add no latency
target or requirement of word-for-word ASR accuracy. No agent review is a user
listening decision. Missing content, supervision, or boundary evidence cannot
produce A07=passed.
"""
import argparse
import array
import collections
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]
DIMS = ("relevant", "no_obvious_factual_contradiction",
        "within_short_answer_scope", "no_thinking_or_control_marker_leak")
TRIM = " \t\n\r\u3000"
IDENTITY = ("run_id", "sample_id", "phase", "round", "session_id", "generation_id")
CATEGORIES = dict(chinese_common=8, numbers_dates=5, short_question=4, code_switch=3)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, low=0):
    return type(value) is int and value >= low


def unique_pairs(pairs):
    out = {}
    for key, value in pairs:
        require(key not in out, "duplicate JSON key: " + key)
        out[key] = value
    return out


def invalid_constant(value):
    raise ValueError("nonfinite JSON constant: " + value)


def parse(text):
    value = json.loads(text, object_pairs_hook=unique_pairs, parse_constant=invalid_constant)
    require(isinstance(value, dict), "JSON object required")
    return value


def load(path):
    path = Path(path)
    require(path.is_file() and path.stat().st_size <= 16 * 1024 * 1024, "JSON file missing/too large: " + str(path))
    return parse(path.read_text(encoding="utf-8"))


def lines(path, limit=100000):
    path = Path(path)
    require(path.is_file() and path.stat().st_size <= 64 * 1024 * 1024, "JSONL missing/too large: " + str(path))
    result = []
    with path.open(encoding="utf-8") as stream:
        for text in stream:
            require(len(result) < limit and len(text) <= 1024 * 1024, "JSONL bounds exceeded")
            require(text.strip(), "blank JSONL record")
            result.append(parse(text))
    return result


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def text_sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def safe_file(root, name):
    require(isinstance(name, str) and name and "\\" not in name and "\x00" not in name,
            "invalid relative evidence path")
    relative = Path(name)
    require(not relative.is_absolute() and ".." not in relative.parts, "escaping evidence path")
    path = (root / relative).resolve()
    require(path.is_relative_to(root.resolve()) and path.is_file(), "missing/escaping evidence file: " + name)
    return path


def normalized(text):
    # Exact policy of src/asr/text_metrics.hpp, not general Unicode NFKC.
    require(isinstance(text, str), "ASR text must be a string")
    result = []
    for char in text:
        c = ord(char)
        if 0xff01 <= c <= 0xff5e:
            c -= 0xfee0
        if ord("A") <= c <= ord("Z"):
            c += 32
        if c in (32, 9, 13, 10, 0x3000) or 0x2000 <= c <= 0x200b:
            continue
        if (33 <= c <= 47 or 58 <= c <= 64 or 91 <= c <= 96 or
                123 <= c <= 126 or 0x3001 <= c <= 0x303f or 0x2010 <= c <= 0x2027 or c == 0xffe5):
            continue
        result.append(chr(c))
    return "".join(result)


def distance(a, b):
    row = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        new = [i]
        for j, y in enumerate(b, 1):
            new.append(min(row[j] + 1, new[-1] + 1, row[j - 1] + (x != y)))
        row = new
    return row[-1]


def fnv1a64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return format(value, "016x")


def wav_data(path, max_samples=1920000):
    with wave.open(str(path), "rb") as wav:
        require((wav.getframerate(), wav.getnchannels(), wav.getsampwidth(), wav.getcomptype()) ==
                (16000, 1, 2, "NONE"), "WAV is not mono 16kHz PCM16: " + str(path))
        n = wav.getnframes()
        require(n <= max_samples, "WAV exceeds bounded sample limit")
        data = wav.readframes(n)
        require(len(data) == n * 2, "truncated WAV PCM payload")
    return n, data


def match_identity(row, parent):
    require(all(row.get(key) == parent.get(key) for key in IDENTITY), "packet/frame identity mismatch")


def match_segments(text, segments):
    cursor = 0
    for segment in segments:
        while cursor < len(text) and text[cursor] in TRIM:
            cursor += 1
        part = segment["text"]
        require(isinstance(part, str) and part and part.strip(TRIM) == part,
                "empty/untrimmed TTS segment")
        require(text.startswith(part, cursor), "TTS text is not a consecutive LLM output segment")
        cursor += len(part)
        require(segment["frontend_text"] == part.replace("\uff0c", "\u3001"), "unexpected TTS frontend rewrite")
    require(not text[cursor:].strip(TRIM), "TTS omitted LLM output tail")


def supervision(root, summary):
    path = root.parent / "supervisor.json"
    resources = root.parent / "supervisor-resources.jsonl"
    if not path.exists() or not resources.exists():
        return {"complete": False, "reason": "supervisor or resource evidence absent"}
    record = load(path)
    require(record.get("returncode") == 0 and record.get("supervisor_failure") is None,
            "supervisor reports child failure")
    rows = lines(resources)
    require(len(rows) >= 2, "insufficient supervisor resource samples")
    times = [row["monotonic_ns"] for row in rows]
    require(all(integer(t, 1) for t in times) and all(a < b for a, b in zip(times, times[1:])),
            "invalid supervisor sample timestamps")
    memory = [row["mem_available_kib"] for row in rows]
    require(all(integer(m, 1) for m in memory), "invalid memory evidence")
    require(min(memory) >= 1572864, "supervisor memory floor breached")
    begin, end = summary["measurement_begin_ns"], summary["measurement_end_ns"]
    require(times[0] <= begin <= end <= times[-1], "resource samples do not cover measured run")
    command = record.get("command")
    require(isinstance(command, list) and "--run-id" in command and "--output-dir" in command,
            "supervisor command association absent")
    output = Path(command[command.index("--output-dir") + 1]) / command[command.index("--run-id") + 1]
    require(output.resolve() == root, "supervisor command belongs to a different run")
    return {"complete": True, "samples": len(rows), "min_mem_available_kib": min(memory),
            "supervisor_sha256": sha(path), "resources_sha256": sha(resources)}


def audit_run(root, manifest_path):
    root, manifest_path = Path(root).resolve(), Path(manifest_path).resolve()
    run, summary = load(root / "run.json"), load(root / "summary.json")
    require(run.get("schema_version") == 1 and summary.get("schema_version") == 1, "unsupported run schema")
    require(summary.get("status") == "completed" and summary.get("clean_shutdown") is True,
            "run did not complete cleanly; stage=" + str(summary.get("failure_stage", "unknown")) +
            "; error=" + str(summary.get("error", "missing clean shutdown")))
    require(run.get("run_id") == summary.get("run_id"), "run/summary identity mismatch")
    require(run.get("manifest_sha256") == sha(manifest_path) == summary.get("manifest_sha256"),
            "manifest evidence hash mismatch")
    manifest = load(manifest_path)
    samples = manifest["samples"]
    by_id = {sample["sample_id"]: sample for sample in samples}
    require(len(by_id) == len(samples), "duplicate manifest sample IDs")
    selected = run["selected_sample_ids"]
    require(isinstance(selected, list) and selected and len(set(selected)) == len(selected)
            and set(selected) <= set(by_id), "invalid selected sample identities")
    require(selected == summary.get("selected_sample_ids"), "run/summary selection mismatch")
    rounds = lines(root / "rounds.jsonl", 300)
    events = lines(root / "events.jsonl")
    measured = [r for r in rounds if r.get("phase") == "measurement"]
    warm = [r for r in rounds if r.get("phase") == "warmup"]
    require(len(measured) == len(selected) and len(warm) == run["warmup_rounds"]
            and len(rounds) == len(measured) + len(warm), "missing/extra measured or warmup rows")
    require([r.get("sample_id") for r in measured] == selected, "measurement samples/order differ from selection")
    for phase_rows in (warm, measured):
        require([r.get("round") for r in phase_rows] == list(range(len(phase_rows))), "round numbering not contiguous")
    artifacts = summary["output_artifacts"]
    artifact_map = {(r["phase"], r["round"], r["sample_id"]): r for r in artifacts}
    require(len(artifact_map) == len(artifacts), "duplicate output artifacts")
    used_artifacts, used_paths, identities = set(), set(), set()
    metrics, skip_behaviors = [], set()
    for row in rounds:
        sid, phase = row["sample_id"], row["phase"]
        require(sid in by_id and row["run_id"] == run["run_id"], "round identity outside manifest/run")
        require(row.get("warmup") is (phase == "warmup"), "incorrect warmup marker")
        sample = by_id[sid]
        require(row["expected_behavior"] == sample["expected_behavior"], "expected behavior changed")
        require(row["input_sha256"] == sample["sha256"], "input identity changed")
        path = safe_file(manifest_path.parent, sample["path"])
        require(Path(row["input_path"]).resolve() == path and sha(path) == sample["sha256"], "input WAV association/hash mismatch")
        frames, input_pcm = wav_data(path, 240000)
        require(frames == sample["num_samples"] == row["input_samples"], "input sample count mismatch")
        directory = ("warmup/" + str(row["round"]) + "-" + sid) if phase == "warmup" else ("samples/" + sid)
        require(load(safe_file(root, directory + "/result.json")) == row, "per-sample result differs from rounds evidence")
        asr = row["asr"]
        require(asr.get("backends_reset") is True, "ASR reset missing")
        row_events = [e for e in events if e.get("phase") == phase and e.get("round") == row["round"]
                      and e.get("sample_id") == sid]
        require(row_events, "no events for round")
        require(all(integer(e.get("monotonic_ns"), 1) and row["begin_ns"] <= e["monotonic_ns"] <= row["end_ns"]
                    for e in row_events), "event timestamp outside round")
        for event_name in ("round_begin", "asr_begin", "asr_end"):
            require(sum(e.get("event") == event_name for e in row_events) == 1, "missing/duplicate event: " + event_name)
        event_times = {e["event"]: e["monotonic_ns"] for e in row_events}
        require(event_times["round_begin"] <= event_times["asr_begin"] <= event_times["asr_end"],
                "ASR event order violated")
        require(all(e.get("run_id") == row["run_id"] for e in row_events), "event run identity mismatch")
        names = [e["event"] for e in row_events]
        require("round_begin" in names and "asr_begin" in names and "asr_end" in names, "incomplete ASR events")
        for key in ("begin_ns", "asr_begin_ns", "asr_end_ns", "end_ns"):
            require(integer(row.get(key), 1), "invalid round timestamp: " + key)
        require(row["begin_ns"] <= row["asr_begin_ns"] <= row["asr_end_ns"] <= row["end_ns"], "ASR timing order violated")
        if row["status"] == "failed":
            require(row.get("failure_stage") and row.get("error"), "failed row lacks stage/error")
            raise ValueError("stage failure: " + str(row["failure_stage"]) + ": " + str(row["error"]))
        if row["status"] == "skipped_empty":
            require(sample["expected_behavior"] in ("empty", "silence") and not any(input_pcm),
                    "skip evidence is not frozen empty/silence")
            require(asr["hypothesis"] == "" and asr["speech_detected"] is False and
                    row.get("llm") is None and row.get("tts") is None and
                    row.get("session_id") is None and row.get("generation_id") is None,
                    "empty ASR triggered downstream state")
            require(not row.get("output_wav") and row.get("output_samples") == 0, "empty ASR created output")
            for field in ("packets_path", "pcm_frames_path"):
                require(not row.get(field) or not (root / row[field]).exists(), "empty ASR created downstream artifacts")
            require(not any("llm" in n or "tts" in n or "pcm" in n for n in names), "empty ASR triggered downstream events")
            if phase == "measurement":
                skip_behaviors.add(sample["expected_behavior"])
            continue
        require(row["status"] == "completed" and sample["expected_behavior"] == "speech", "unexpected round status/behavior")
        require(asr.get("asr_finish_called") is True and asr["speech_detected"] is True and isinstance(asr["hypothesis"], str) and asr["hypothesis"].strip(TRIM),
                "empty/silent ASR generated an answer")
        pair = (row["session_id"], row["generation_id"])
        require(all(integer(n, 1) for n in pair) and pair not in identities, "reused/invalid session generation")
        identities.add(pair)
        llm, tts = row["llm"], row["tts"]
        require(llm["input_text"] == asr["hypothesis"], "LLM input is not this round's ASR hypothesis")
        require(llm["status"] == "completed" and llm["run_rc"] == 0 and llm["clear_rc"] == 0,
                "LLM completion/KV reset failed")
        require(llm.get("history_pairs") == 0, "LLM retained previous history")
        text = llm["text"]
        require(isinstance(text, str) and text.strip(TRIM) and len(text.encode("utf-8")) <= 8192, "invalid LLM text")
        packets = lines(safe_file(root, row["packets_path"]), 8192)
        require(packets and [p["seq"] for p in packets] == list(range(1, len(packets) + 1)), "LLM packet sequence gap/duplicate")
        for packet in packets:
            match_identity(packet, row)
            require(isinstance(packet["text"], str) and integer(packet["monotonic_ns"], 1), "invalid LLM packet")
        require("".join(p["text"] for p in packets) == text, "LLM chunks do not reconstruct answer")
        require(all(a["monotonic_ns"] <= b["monotonic_ns"] for a, b in zip(packets, packets[1:])),
                "LLM packet timing regressed")
        segments = row["tts_generations"]
        require(isinstance(segments, list) and segments and len(segments) == tts["segments"], "TTS segment count mismatch")
        for segment in segments:
            require(segment["session_id"] == pair[0] and segment["generation_id"] == pair[1],
                    "TTS segment from another generation")
            require(integer(segment["native_samples"], 1) and integer(segment["begin_ns"], 1)
                    and segment["begin_ns"] <= segment["end_ns"], "invalid generated TTS segment")
        match_segments(text, segments)
        require(tts["status"] == "completed", "TTS did not complete")
        require(tts["input_bytes"] == len(text.encode("utf-8")), "TTS did not receive exact LLM byte count")
        require(all(tts.get(k) == 0 for k in ("current_input_bytes", "current_segments", "current_pcm_frames",
                "segmenter_pending_bytes", "discarded_generation_results", "rejected_stale_inputs")),
                "TTS retained queues/stale-generation output")
        require(all(llm.get(k) is False for k in ("overflow", "invalid_utf8", "restart_required")),
                "LLM integrity flags failed")
        require(integer(llm["output_budget"], 1) and llm["output_budget"] == run["llm_max_output_tokens"],
                "LLM output budget mismatch")
        token_count = sum(integer(p.get("token_id")) for p in packets)
        require(token_count <= llm["output_budget"], "observed token IDs exceeded budget")
        pcm_rows = lines(safe_file(root, row["pcm_frames_path"]), 6001)
        require(pcm_rows and [p["seq"] for p in pcm_rows] == list(range(len(pcm_rows))), "PCM sequence gap/duplicate")
        require(tts["frames"] == tts["consumed_frames"] == len(pcm_rows), "PCM frame counts disagree")
        wav_path = safe_file(root, row["output_wav"])
        require(wav_path not in used_paths, "same WAV reused for different rounds")
        used_paths.add(wav_path)
        n, pcm = wav_data(wav_path)
        require(n > 0 and any(pcm) and n == row["output_samples"] == tts["pcm_samples"], "empty or inconsistent output PCM")
        offset = 0
        for frame in pcm_rows:
            match_identity(frame, row)
            count = frame["valid_samples"]
            require(integer(count, 1) and count <= 320, "invalid PCM valid_samples")
            payload = pcm[offset * 2:(offset + count) * 2]
            require(len(payload) == count * 2 and frame["pcm16_fnv1a64"] == fnv1a64(payload), "PCM frame content differs from written WAV")
            offset += count
        require(offset == n, "PCM frames do not cover output WAV")
        key = (phase, row["round"], sid)
        require(key in artifact_map, "output WAV absent from manifest")
        artifact = artifact_map[key]
        require(artifact["path"] == row["output_wav"] and artifact["sha256"] == sha(wav_path), "output WAV artifact hash mismatch")
        used_artifacts.add(key)
        require(row["asr_end_ns"] <= row["llm_begin_ns"] <= row["llm_end_ns"] <= row["end_ns"],
                "ASR/LLM sequencing or round timing violated")
        require("llm_begin" in names and "llm_end" in names and "round_complete" in names, "missing completion events")
        for event_name in ("llm_begin", "tts_begin", "llm_end", "output_write_begin", "round_complete"):
            require(names.count(event_name) == 1, "missing/duplicate downstream event: " + event_name)
        require(event_times["asr_end"] <= event_times["llm_begin"] <= event_times["tts_begin"] <=
                event_times["llm_end"] <= event_times["output_write_begin"] <= event_times["round_complete"],
                "downstream event order violated")
        generated_events = [e for e in row_events if e["event"] == "tts_generate_end"]
        require(len(generated_events) == len(segments) and names.count("tts_generate_begin") == len(segments),
                "TTS generation event count mismatch")
        require([e.get("text") for e in generated_events] == [part["text"] for part in segments],
                "TTS event text mismatch")
        require(row["begin_ns"] >= summary["measurement_begin_ns"] and row["end_ns"] <= summary["measurement_end_ns"]
                if phase == "measurement" else row["end_ns"] <= summary["measurement_begin_ns"],
                "warmup/measurement timing contaminated")
        require(all(pair == (e.get("session_id"), e.get("generation_id")) for e in row_events
                    if e["event"].startswith(("llm_", "tts_", "pcm_"))), "downstream event generation mismatch")
        if phase == "measurement":
            hyp, ref = normalized(asr["hypothesis"]), sample["normalized_reference"]
            hits = [normalized(k) in hyp for k in sample["keywords"]]
            require(hits and normalized(sample["reference_text"]) == ref, "invalid frozen reference/keywords")
            metrics.append({"sample_id": sid, "category": sample["category"], "hypothesis": asr["hypothesis"],
                            "all_keywords_match": all(hits), "character_errors": distance(ref, hyp),
                            "reference_characters": len(ref), "cer_eligible": sample["category"] != "code_switch"})
    require(used_artifacts == set(artifact_map), "unclaimed/extra output artifacts")
    require(summary["completed_rounds"] == sum(r["status"] == "completed" for r in measured)
            and summary["skipped_rounds"] == sum(r["status"] == "skipped_empty" for r in measured)
            and summary["failed_rounds"] == 0, "summary counts differ from raw rounds")
    require(all(e.get("run_id") == run["run_id"] for e in events), "foreign run event")
    known = {(r["phase"], r["round"], r["sample_id"]) for r in rounds}
    require(all((e.get("phase"), e.get("round"), e.get("sample_id")) in known for e in events if "sample_id" in e),
            "orphan event without round")
    require(summary.get("exit_code") == 0, "run exit code not successful")
    return {"root": root, "run": run, "summary": summary, "manifest": manifest, "manifest_path": manifest_path,
            "measured": measured, "metrics": metrics, "skip_behaviors": skip_behaviors,
            "supervision": supervision(root, summary)}


def content_review(path, data):
    if path is None:
        return {"complete": False, "reason": "independent content review absent"}
    review = load(path)
    require(review.get("schema_version") == 1, "unsupported content-review schema")
    reviewer = review.get("reviewer")
    require(isinstance(reviewer, dict) and reviewer.get("kind") in ("agent", "user") and reviewer.get("name"),
            "reviewer name and agent/user provenance required")
    require(review.get("rounds_sha256") == sha(data["root"] / "rounds.jsonl"), "content review belongs to different rounds")
    reviews = review["reviews"]
    by_id = {r["sample_id"]: r for r in reviews}
    require(len(by_id) == len(reviews) and set(by_id) == {r["sample_id"] for r in data["measured"]},
            "content review omitted/duplicated/added samples")
    passed, correspondence = 0, 0
    for row in data["measured"]:
        item = by_id[row["sample_id"]]
        require(item["answer_sha256"] == text_sha(row["llm"]["text"]), "content review answer hash mismatch")
        require(all(type(item.get(k)) is bool for k in DIMS), "four content dimensions must be explicit booleans")
        require(type(item.get("input_correspondence")) is bool and isinstance(item.get("review_note"), str)
                and item["review_note"].strip(), "content review requires input correspondence and reason")
        score = all(item[k] for k in DIMS)
        if "passed" in item:
            require(item["passed"] is score, "content passed flag contradicts dimensions")
        passed += score
        correspondence += item["input_correspondence"]
    return {"complete": True, "reviewer": reviewer, "count": len(reviews), "passed": passed,
            "input_correspondence": correspondence, "quality_pass": passed >= 18,
            "user_audio_listening": "not_asserted_by_this_checker", "sha256": sha(path)}


def review(root, manifest, content=None, boundaries=(), catalog=None):
    data = audit_run(root, manifest)
    metrics = data["metrics"]
    eligible = [r for r in metrics if r["cer_eligible"]]
    errors = sum(r["character_errors"] for r in eligible)
    chars = sum(r["reference_characters"] for r in eligible)
    keyword_pass = sum(r["all_keywords_match"] for r in metrics)
    asr = {"samples": len(metrics), "keyword_pass": keyword_pass, "chinese_errors": errors,
           "chinese_reference_characters": chars, "chinese_cer": errors / chars if chars else None,
           "quality_pass": len(metrics) == 20 and keyword_pass >= 18 and chars > 0 and errors / chars <= 0.15,
           "per_sample": metrics,
           "category_counts": dict(collections.Counter(r["category"] for r in metrics))}
    fixed = load(catalog or ROOT / "tests/fixtures/natural-corpus.json")
    formal = (sha(manifest) == fixed["private_manifest"]["sha256"] and
              len(metrics) == 20 and asr["category_counts"] == CATEGORIES and
              {r["sample_id"] for r in metrics} == {r["sample_id"] for r in fixed["samples"]})
    if formal:
        original = {r["sample_id"]: r for r in fixed["samples"]}
        for sample in data["manifest"]["samples"]:
            require(all(sample[k] == original[sample["sample_id"]][k] for k in
                        ("sha256", "reference_text", "normalized_reference", "keywords", "category")),
                    "formal frozen sample metadata changed")
    content_result = content_review(content, data) if content else {"complete": False, "reason": "content review absent"}
    boundary_evidence, behaviors = [], set()
    for directory in boundaries:
        metadata = load(Path(directory) / "run.json")
        boundary = audit_run(directory, metadata["manifest_path"])
        require(all(r["status"] == "skipped_empty" for r in boundary["measured"]), "boundary run contains normal speech")
        if boundary["supervision"]["complete"]:
            behaviors |= boundary["skip_behaviors"]
        boundary_evidence.append({"run_dir": str(boundary["root"]), "behaviors": sorted(boundary["skip_behaviors"]),
                                  "supervision": boundary["supervision"], "rounds_sha256": sha(boundary["root"] / "rounds.jsonl")})
    complete = formal and data["supervision"]["complete"] and content_result["complete"] and behaviors >= {"empty", "silence"}
    status = "not_run"
    if complete:
        status = "passed" if asr["quality_pass"] and content_result["quality_pass"] else "failed"
    return {"schema_version": 1, "software_checks": "passed", "A07": status,
            "scope": "File-chain evidence and original A03/A04 content thresholds; no real-device latency or user listening claim",
            "formal_corpus_bound": formal, "run_dir": str(data["root"]), "manifest_sha256": sha(manifest),
            "rounds_sha256": sha(data["root"] / "rounds.jsonl"), "asr": asr, "content_review": content_result,
            "supervision": data["supervision"], "boundaries": boundary_evidence,
            "boundary_no_downstream_verified": behaviors >= {"empty", "silence"},
            "pcm_correspondence": "Each emitted PCM16 frame independently matched to WAV bytes with noncryptographic FNV1a64",
            "engineering_evidence_checks": "Evidence completeness checks do not alter original A07 quality/performance thresholds"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--content-review", type=Path)
    parser.add_argument("--boundary-run", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    require(not args.output.exists(), "review output must be new")
    try:
        result = review(args.run_dir, args.manifest, args.content_review, args.boundary_run)
        code = 6 if result["A07"] == "failed" else 0
    except (ValueError, KeyError, TypeError, IndexError, OSError, wave.Error) as exc:
        result = {"schema_version": 1, "software_checks": "failed", "A07": "failed",
                  "error": type(exc).__name__ + ": " + str(exc)}
        code = 6
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"software_checks": result["software_checks"], "A07": result["A07"],
                      "report": str(args.output)}, ensure_ascii=False))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
