#!/usr/bin/env python3
"""Recalculate the published snapshot using only its JSON event excerpts."""

import argparse
import json
from decimal import Decimal
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify(snapshot):
    require(snapshot["schema_version"] == 1, "Unsupported snapshot schema")
    continuous = snapshot["continuous"]
    rounds = continuous["rounds"]
    require(
        len(rounds) == continuous["completed_turns"] == continuous["expected_turns"],
        "Continuous turn count mismatch",
    )
    require([r["round"] for r in rounds] == list(range(1, len(rounds) + 1)),
            "Continuous round order mismatch")
    contexts = set()
    previous_seq = 0
    for row in rounds:
        event = row["completion"]
        require(event["event"] == "turn_completed", "Invalid completion event")
        require(event["data"]["history_committed"] is True
                and event["data"]["empty_input"] is False, "Incomplete turn")
        require(continuous["observed_start_ns"] <= event["monotonic_ns"]
                <= continuous["observed_end_ns"], "Completion outside observation")
        require(event["seq"] > previous_seq, "Continuous event order mismatch")
        previous_seq = event["seq"]
        context = (event["session_id"], event["generation_id"])
        require(context not in contexts, "Duplicate completed turn")
        contexts.add(context)

    headset = snapshot["headset"]
    cases = headset["cases"]
    require(len(cases) == headset["passed_cases"] == headset["expected_cases"],
            "Headset case count mismatch")
    require({c["disconnect_state"] for c in cases} == {"idle", "recording", "playback"},
            "Missing reconnect state")
    states = {"idle": "IDLE", "recording": "LISTENING", "playback": "RESPONDING"}
    latencies = []
    for case in cases:
        require(case["status"] == "passed" and case["complete_answer_heard"] is True,
                "Missing successful listener confirmation")
        events = case["events"]
        waiting, ready, wake = (events[k] for k in
                               ["device_wait", "device_ready", "wake_listening"])
        asr, first, completed = (events[k] for k in
                                ["asr_final", "first_playback_submit", "turn_completed"])
        require(waiting["event"] == ready["event"] == "audio_event"
                and waiting["data"]["type"] == "device_wait"
                and ready["data"]["type"] == "device_ready"
                and wake["event"] == "wake_listening", "Invalid reconnect events")
        require(waiting["state"] == states[case["disconnect_state"]],
                "Disconnect state mismatch")
        require(asr["event"] == "asr_final" and first["event"] == "playback_submit"
                and completed["event"] == "turn_completed", "Invalid response events")
        sequence = [waiting, ready, wake, asr, first, completed]
        require(all(a["seq"] < b["seq"] and a["monotonic_ns"] < b["monotonic_ns"]
                    for a, b in zip(sequence, sequence[1:])), "Event order mismatch")
        require(len({(e["session_id"], e["generation_id"])
                     for e in [wake, asr, first, completed]}) == 1,
                "Response context mismatch")
        require(first["data"]["seq"] == 0 and first["data"]["valid_samples"] > 0,
                "Expected the first nonempty response frame")
        require(completed["data"]["history_committed"] is True
                and completed["data"]["empty_input"] is False, "Incomplete response")
        start_ns = asr["data"]["asr_final_ns"]
        end_ns = first["data"]["playback_submit_ns"]
        require(wake["monotonic_ns"] <= start_ns <= asr["monotonic_ns"]
                < end_ns <= first["monotonic_ns"], "Invalid timing endpoints")
        milliseconds = Decimal(end_ns - start_ns) / Decimal(1000000)
        require(milliseconds == Decimal(str(case["response_latency_ms"])),
                "Stored latency differs from event timestamps")
        if case["disconnect_state"] == "playback":
            require(case["old_answer_resumed"] is False
                    and case["old_generation_commits_after_disconnect"] == 0,
                    "Old playback was not isolated")
        latencies.append((case["id"], milliseconds))

    print("Snapshot:", snapshot["snapshot_id"])
    print("Release:", snapshot["release"]["id"])
    print("Continuous turns: {}/{}".format(len(rounds), continuous["expected_turns"]))
    print("Headset reconnects: {}/{}".format(len(cases), headset["expected_cases"]))
    for case_id, milliseconds in latencies:
        print("{}: {} ms ({:.3f} s)".format(case_id, milliseconds, milliseconds / 1000))
    print("OK: published counts and software timing values are consistent.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "snapshot", nargs="?", type=Path,
        default=Path(__file__).resolve().parents[1]
        / "docs/evidence/performance-snapshot-20261027.json",
    )
    args = parser.parse_args()
    try:
        verify(json.loads(args.snapshot.read_text(encoding="utf-8")))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, "Snapshot verification failed: {}\n".format(error))


if __name__ == "__main__":
    main()
