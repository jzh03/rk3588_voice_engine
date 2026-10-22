#!/usr/bin/env python3
"""CLI/file isolation checks. Never initialize a model or open an audio device."""
import json
import pathlib
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())
def invoke(args, code=0):
    result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=20)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result

invoke(['--help'])
invoke(['--unknown'], 2)
with tempfile.TemporaryDirectory(prefix='tts-probe-cli-') as tmp:
    root = pathlib.Path(tmp)
    cases = root/'cases.json'
    cases.write_text(json.dumps({'schema_version': 1, 'cases': [{'id': 'short', 'text': '好。', 'category': 'short'}, {'id': 'date', 'text': '今天是2026年10月20日，温度23.5度。', 'category': 'number_date'}]}, ensure_ascii=False))
    args = ['--validate-text-only', '--cases', str(cases), '--output-dir', str(root/'result'), '--run-id', 'cli-test']
    invoke(args)
    before = (root/'result'/'cli-test'/'summary.json').read_bytes()
    invoke(args, 2)
    assert (root/'result'/'cli-test'/'summary.json').read_bytes() == before
    assert not (root/'result'/'cli-test'/'failure.json').exists()
    for extra in [['--debug-frontend'], ['--pronunciations', 'missing.json'], ['--threads', '3'], ['--repeat', '0'], ['--warmup', '-1'], ['--repeat', '1x'], ['--case-id', 'missing'], ['--run-id', '../../escape']]:
        invoke([*args, *extra], 2)
    cases.write_text(json.dumps({'schema_version': 1, 'cases': [{'id': 'reject', 'text': '<think>private</think>回答', 'category': 'control'}, {'id': 'good', 'text': '你好。', 'category': 'plain'}]}, ensure_ascii=False))
    invoke(['--validate-text-only', '--cases', str(cases), '--output-dir', str(root/'rejected'), '--run-id', 'reject'], 2)
    report=json.loads((root/'rejected'/'reject'/'summary.json').read_text())
    assert report['failed']==1 and len(report['results'])==2
    assert not report['results'][0]['success'] and report['results'][1]['success']
    assert not list(root.rglob('*.wav'))
    cases.write_text(json.dumps({'schema_version': 1, 'cases': [{'id': '../escape', 'text': '你好。', 'category': 'plain'}]}, ensure_ascii=False))
    invoke(args, 2)
print('TTS_PROBE_CLI_OK')
