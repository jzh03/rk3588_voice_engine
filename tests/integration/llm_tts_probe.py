#!/usr/bin/env python3
"""No model or device work: validate CLI and untrusted replay bounds."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
exe=sys.argv[1]
def run(args,rc):
    p=subprocess.run([exe,*args],capture_output=True,text=True,timeout=5)
    assert p.returncode==rc,(args,p.returncode,p.stdout,p.stderr)
run(["--help"],0)
run(["--prompt","你好","--validate-input-only"],0)
run(["--prompt","<think>bad","--validate-input-only"],2)
run(["--prompt","你好","--replay","missing","--validate-input-only"],2)
run(["--prompt","你好","--cancel-ms","0","--validate-input-only"],2)
with tempfile.TemporaryDirectory() as temp:
    path=Path(temp)/"replay.jsonl"
    def check(rows,rc):
        path.write_text("".join(json.dumps(r)+"\n" for r in rows))
        run(["--replay",str(path),"--validate-input-only"],rc)
    check([dict(offset_ns=0,text="你好。")],0)
    check([dict(offset_ns=2,text="你"),dict(offset_ns=1,text="好")],2)
    check([dict(offset_ns=0,text="x"*8193)],2)
    check([dict(offset_ns=60000000001,text="你好")],2)
print("LLM_TTS_CLI_OK")
