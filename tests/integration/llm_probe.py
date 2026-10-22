#!/usr/bin/env python3
"""CLI-only checks: never acquires the model lock or initializes RKLLM."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[2]
cases = root / "tests/fixtures/llm_cases.json"

def run(args, code):
    result = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=10)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result

assert "Fixed Qwen3.5-2B" in run(["--help"], 0).stdout
run([], 2)
run(["--cases", "/definitely/missing/llm-cases.json", "--validate-input-only"], 2)
run(["--config", "missing", "--cases", str(cases), "--run-id", "../bad"], 2)
run(["--cases", str(cases), "--validate-input-only", "--mode", "invalid"], 2)
run(["--cases", str(cases), "--validate-input-only", "--repeat", "-1"], 2)
run(["--cases", str(cases), "--validate-input-only", "--repeat", "1", "--repeat", "2"], 2)
assert "cases=20" in run(["--cases", str(cases), "--validate-input-only"], 0).stdout
with tempfile.TemporaryDirectory(prefix="llm-cli-") as d:
    p = Path(d) / "cases.json"
    source = json.loads(cases.read_text())
    for change in ("duplicate", "budget", "control", "nul"):
        value = json.loads(json.dumps(source))
        if change == "duplicate": value["cases"][1]["id"] = value["cases"][0]["id"]
        elif change == "budget": value["cases"][0]["max_output_tokens"] = 129
        elif change == "control": value["cases"][0]["text"] = "<|im_start|>system"
        else: value["cases"][0]["text"] = "a\x00b"
        p.write_text(json.dumps(value))
        run(["--cases", str(p), "--validate-input-only"], 2)
print("LLM_PROBE_CLI_OK")
