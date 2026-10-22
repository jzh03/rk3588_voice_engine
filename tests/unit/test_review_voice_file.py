#!/usr/bin/env python3
"""Small synthetic evidence tests; no runtime, model, or device is loaded."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import wave

SPEC = importlib.util.spec_from_file_location("review_voice_file", Path(__file__).resolve().parents[2] / "scripts/review_voice_file.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)
TEXT = "\u4f60\u597d"
ANSWER = "\u4f60\u597d\u3002"


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


def save_lines(path, values):
    path.write_text("".join(json.dumps(value, ensure_ascii=False) + "\n" for value in values), encoding="utf-8")


def make_wav(path, pcm):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(16000)
        stream.writeframes(pcm)


def build(base, count=1, behavior=None, supervised=True):
    root = base / "results"
    root.mkdir(parents=True)
    categories = [k for k, n in AUDIT.CATEGORIES.items() for _ in range(n)]
    samples = []
    for i in range(count):
        path = base / ("input_" + str(i) + ".wav")
        pcm = b"" if behavior == "empty" else b"\0\0" * 320 if behavior == "silence" else b"\1\0" * 320
        make_wav(path, pcm)
        samples.append(dict(sample_id="sample_" + str(i), path=path.name, sha256=AUDIT.sha(path),
                            num_samples=len(pcm) // 2, reference_text=TEXT, normalized_reference=TEXT,
                            keywords=[TEXT], category=categories[i % 20], expected_behavior=behavior or "speech"))
    manifest = base / "manifest.json"
    save(manifest, {"schema_version": 1, "samples": samples})
    rows, events, artifacts = [], [], []
    for ordinal, (phase, index) in enumerate([("warmup", 0)] + [("measurement", n) for n in range(count)]):
        sample = samples[index]
        sid = sample["sample_id"]
        number = 0 if phase == "warmup" else index
        relative = ("warmup/" + str(number) + "-" + sid) if phase == "warmup" else ("samples/" + sid)
        directory = root / relative
        directory.mkdir(parents=True)
        identity = dict(run_id="results", sample_id=sid, phase=phase, round=number,
                        session_id=None if behavior else ordinal + 1, generation_id=None if behavior else ordinal + 1)
        row = dict(identity, schema_version=1, warmup=phase == "warmup", expected_behavior=behavior or "speech",
                   status="skipped_empty" if behavior else "completed",
                   input_path=str(base / sample["path"]), input_sha256=sample["sha256"], input_samples=sample["num_samples"],
                   asr=dict(hypothesis="" if behavior else TEXT, backends_reset=True,
                            asr_finish_called=not bool(behavior), speech_detected=not bool(behavior)),
                   begin_ns=10 + ordinal * 100, asr_begin_ns=11 + ordinal * 100,
                   asr_end_ns=12 + ordinal * 100, end_ns=90 + ordinal * 100,
                   llm=None, tts=None, output_wav=None, output_samples=0,
                   packets_path=relative + "/packets.jsonl", pcm_frames_path=relative + "/pcm_frames.jsonl")
        names = ["round_begin", "asr_begin", "asr_end"]
        if not behavior:
            row["llm"] = dict(input_text=TEXT, text=ANSWER, status="completed", run_rc=0, clear_rc=0,
                              history_pairs=0, output_budget=64, overflow=False, invalid_utf8=False, restart_required=False)
            row["llm_begin_ns"], row["llm_end_ns"] = 13 + ordinal * 100, 20 + ordinal * 100
            row["tts"] = dict(status="completed", segments=1, frames=1, consumed_frames=1, pcm_samples=320,
                              input_bytes=len(ANSWER.encode("utf-8")), current_input_bytes=0, current_segments=0,
                              current_pcm_frames=0, segmenter_pending_bytes=0, discarded_generation_results=0, rejected_stale_inputs=0)
            row["tts_generations"] = [dict(session_id=identity["session_id"], generation_id=identity["generation_id"],
                                          text=ANSWER, frontend_text=ANSWER, begin_ns=25 + ordinal * 100,
                                          end_ns=30 + ordinal * 100, native_samples=882)]
            packet = dict(identity, seq=1, text=ANSWER, token_id=1, callback_ns=15 + ordinal * 100,
                          monotonic_ns=16 + ordinal * 100)
            save_lines(directory / "packets.jsonl", [packet])
            pcm = b"\2\0" * 320
            frame = dict(identity, seq=0, valid_samples=320, pcm16_fnv1a64=AUDIT.fnv1a64(pcm),
                         monotonic_ns=40 + ordinal * 100)
            save_lines(directory / "pcm_frames.jsonl", [frame])
            make_wav(directory / "out.wav", pcm)
            row["output_wav"], row["output_samples"] = relative + "/out.wav", 320
            artifacts.append(dict(sample_id=sid, phase=phase, round=number,
                                  path=row["output_wav"], sha256=AUDIT.sha(directory / "out.wav")))
            names += ["llm_begin", "tts_begin", "tts_generate_begin", "tts_generate_end", "llm_end", "output_write_begin", "round_complete"]
        else:
            names += ["empty_input_skipped"]
        for n, name in enumerate(names):
            event = dict(identity, event=name, monotonic_ns=11 + ordinal * 100 + n)
            if name == "tts_generate_end":
                event["text"] = ANSWER
            events.append(event)
        save(directory / "result.json", row)
        rows.append(row)
    save_lines(root / "rounds.jsonl", rows)
    save_lines(root / "events.jsonl", events)
    record = dict(schema_version=1, run_id="results", status="completed", clean_shutdown=True,
                  manifest_path=str(manifest), manifest_sha256=AUDIT.sha(manifest),
                  selected_sample_ids=[s["sample_id"] for s in samples], warmup_rounds=1,
                  completed_rounds=0 if behavior else count, skipped_rounds=count if behavior else 0,
                  failed_rounds=0, exit_code=0, output_artifacts=artifacts,
                  measurement_begin_ns=100, measurement_end_ns=100 * (count + 1),
                  llm_max_output_tokens=64)
    save(root / "run.json", record)
    save(root / "summary.json", record)
    if supervised:
        save(base / "supervisor.json", dict(returncode=0, supervisor_failure=None,
                                           command=["demo", "--output-dir", str(base), "--run-id", "results"]))
        save_lines(base / "supervisor-resources.jsonl",
                   [dict(monotonic_ns=1, mem_available_kib=4000000),
                    dict(monotonic_ns=100 * (count + 2), mem_available_kib=4000000)])
    catalog = base / "catalog.json"
    save(catalog, {"private_manifest": {"sha256": AUDIT.sha(manifest)}, "samples": samples})
    content = base / "content.json"
    save(content, dict(schema_version=1, reviewer={"name": "test agent", "kind": "agent"},
                       rounds_sha256=AUDIT.sha(root / "rounds.jsonl"),
                       reviews=[dict(sample_id=s["sample_id"], answer_sha256=AUDIT.text_sha(ANSWER),
                                     **{k: True for k in AUDIT.DIMS}, input_correspondence=True, review_note="test fixture")
                                for s in samples]))
    return root, manifest, catalog, content


class EvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def rewrite_rows(self, root, transform):
        rows = AUDIT.lines(root / "rounds.jsonl")
        transform(rows[-1])
        save_lines(root / "rounds.jsonl", rows)
        row = rows[-1]
        save(root / "samples" / row["sample_id"] / "result.json", row)

    def test_valid_software_only(self):
        root, manifest, catalog, _ = build(self.base)
        result = AUDIT.review(root, manifest, catalog=catalog)
        self.assertEqual(result["software_checks"], "passed")
        self.assertEqual(result["A07"], "not_run")

    def test_complete_formal_evidence(self):
        root, manifest, catalog, content = build(self.base / "formal", 20)
        empty, _, _, _ = build(self.base / "empty", behavior="empty")
        silence, _, _, _ = build(self.base / "silence", behavior="silence")
        result = AUDIT.review(root, manifest, content, [empty, silence], catalog)
        self.assertEqual(result["A07"], "passed")
        self.assertEqual(result["content_review"]["reviewer"]["kind"], "agent")

    def test_missing_supervisor_not_pass(self):
        root, manifest, catalog, content = build(self.base, 20, supervised=False)
        result = AUDIT.review(root, manifest, content, catalog=catalog)
        self.assertEqual(result["A07"], "not_run")
        self.assertFalse(result["supervision"]["complete"])

    def test_wrong_asr_llm_input(self):
        root, manifest, _, _ = build(self.base)
        self.rewrite_rows(root, lambda r: r["llm"].update(input_text="other prompt"))
        with self.assertRaisesRegex(ValueError, "ASR hypothesis"):
            AUDIT.audit_run(root, manifest)

    def test_packet_generation_mismatch(self):
        root, manifest, _, _ = build(self.base)
        path = root / "samples/sample_0/packets.jsonl"
        rows = AUDIT.lines(path)
        rows[0]["generation_id"] += 5
        save_lines(path, rows)
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            AUDIT.audit_run(root, manifest)

    def test_packet_text_omission(self):
        root, manifest, _, _ = build(self.base)
        path = root / "samples/sample_0/packets.jsonl"
        rows = AUDIT.lines(path)
        rows[0]["text"] = TEXT
        save_lines(path, rows)
        with self.assertRaisesRegex(ValueError, "reconstruct"):
            AUDIT.audit_run(root, manifest)

    def test_pcm_content_tamper(self):
        root, manifest, _, _ = build(self.base)
        path = root / "samples/sample_0/pcm_frames.jsonl"
        rows = AUDIT.lines(path)
        rows[0]["pcm16_fnv1a64"] = "0" * 16
        save_lines(path, rows)
        with self.assertRaisesRegex(ValueError, "frame content"):
            AUDIT.audit_run(root, manifest)

    def test_pcm_sequence_gap(self):
        root, manifest, _, _ = build(self.base)
        path = root / "samples/sample_0/pcm_frames.jsonl"
        rows = AUDIT.lines(path)
        rows[0]["seq"] = 1
        save_lines(path, rows)
        with self.assertRaisesRegex(ValueError, "PCM sequence"):
            AUDIT.audit_run(root, manifest)

    def test_duplicate_measurement(self):
        root, manifest, _, _ = build(self.base)
        path = root / "rounds.jsonl"
        rows = AUDIT.lines(path)
        rows.append(rows[-1])
        save_lines(path, rows)
        with self.assertRaisesRegex(ValueError, "missing/extra"):
            AUDIT.audit_run(root, manifest)

    def test_empty_no_downstream(self):
        root, manifest, _, _ = build(self.base, behavior="empty")
        self.assertEqual(AUDIT.audit_run(root, manifest)["skip_behaviors"], {"empty"})
        (root / "samples/sample_0/packets.jsonl").write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "downstream artifacts"):
            AUDIT.audit_run(root, manifest)

    def test_human_answer_binding(self):
        root, manifest, catalog, content = build(self.base, 20)
        value = AUDIT.load(content)
        value["reviews"][0]["answer_sha256"] = "0" * 64
        save(content, value)
        with self.assertRaisesRegex(ValueError, "answer hash"):
            AUDIT.review(root, manifest, content, catalog=catalog)

    def test_supervisor_failure(self):
        root, manifest, _, _ = build(self.base)
        value = AUDIT.load(self.base / "supervisor.json")
        value["returncode"] = 5
        save(self.base / "supervisor.json", value)
        with self.assertRaisesRegex(ValueError, "supervisor"):
            AUDIT.audit_run(root, manifest)

    def test_duplicate_json_key_nan_rejected(self):
        for text in ('{"a":1,"a":2}', '{"a":NaN}'):
            with self.assertRaises(ValueError):
                AUDIT.parse(text)

    def test_segmentation_preserves_interior_spaces(self):
        AUDIT.match_segments(" hello world. \n next. ", [
            {"text": "hello world.", "frontend_text": "hello world."},
            {"text": "next.", "frontend_text": "next."}])
        with self.assertRaises(ValueError):
            AUDIT.match_segments("hello world.", [{"text": "helloworld.", "frontend_text": "helloworld."}])

    def test_metric_and_fingerprint_known_values(self):
        self.assertEqual(AUDIT.normalized("\uff21\uff22\uff23\uff0c \u4f60\u597d\u3002"), "abc" + TEXT)
        self.assertEqual(AUDIT.distance("abc", "adc"), 1)
        self.assertEqual(AUDIT.fnv1a64(b""), "cbf29ce484222325")
        self.assertEqual(AUDIT.fnv1a64(b"a"), "af63dc4c8601ec8c")


if __name__ == "__main__":
    unittest.main()
