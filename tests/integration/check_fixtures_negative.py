#!/usr/bin/env python3
"""Exercise fixture integrity failures on temporary copies; never load a model."""
import array
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'tests/fixtures'
CHECKER = ROOT / 'tests/integration/check_fixtures.py'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def edit_json(base, name, change):
    path = base / name
    data = json.loads(path.read_text(encoding='utf-8'))
    replacement = change(data)
    if replacement is not None:
        data = replacement
    path.write_text(json.dumps(data, ensure_ascii=False), encoding='utf-8')
    if name == 'speech_cases.json':
        edit_json(base, 'manifest.json', lambda manifest:
                  manifest.update(speech_cases_sha256=digest(path)))


def sample(manifest, sample_id):
    return next(row for row in manifest['samples'] if row['sample_id'] == sample_id)


def change_sample(base, row_id, **changes):
    edit_json(base, 'manifest.json', lambda manifest:
              sample(manifest, row_id).update(changes))


def append_reference(base, sample_id):
    def change(manifest):
        row = sample(manifest, sample_id)
        row['reference_text'] += 'x'
        row['normalized_reference'] += 'x'
    edit_json(base, 'manifest.json', change)


def drop_keyword(base, sample_id):
    def change(manifest):
        sample(manifest, sample_id)['keywords'].pop()
    edit_json(base, 'manifest.json', change)


def change_pcm(base, sample_id, index):
    manifest = json.loads((base / 'manifest.json').read_text())
    path = base / sample(manifest, sample_id)['path']
    with wave.open(str(path), 'rb') as stream:
        params = stream.getparams()
        pcm = array.array('h', stream.readframes(stream.getnframes()))
    # Toggle a byte-level sample without exceeding the frozen noise range.
    pcm[index] = 0 if pcm[index] != 0 else 1
    with wave.open(str(path), 'wb') as stream:
        stream.setparams(params)
        stream.writeframes(pcm.tobytes())
    change_sample(base, sample_id, sha256=digest(path), size_bytes=path.stat().st_size)


def corrupt_pcm(base, truncate=False):
    path = base / 'audio/zh_01.wav'
    data = bytearray(path.read_bytes())
    if truncate:
        data = data[:-2]
    else:
        data[-1] ^= 1
    path.write_bytes(data)
    if truncate:
        change_sample(base, 'zh_01', sha256=digest(path), size_bytes=path.stat().st_size)


def symlink_escape(base):
    path = base / 'audio/zh_01.wav'
    path.unlink()
    path.symlink_to(FIXTURES / 'audio/zh_01.wav')


def duplicate_key(base):
    path = base / 'manifest.json'
    text = path.read_text(encoding='utf-8')
    path.write_text(text.replace('"schema_version": 1',
                                '"schema_version": 1, "schema_version": 1', 1),
                    encoding='utf-8')


def run_case(base, expected_exit, label, expected_diagnostic=None):
    command = [sys.executable, str(CHECKER), '--manifest', str(base / 'manifest.json')]
    result = subprocess.run(command, text=True, capture_output=True, timeout=15)
    if result.returncode != expected_exit:
        raise RuntimeError(f'{label}: expected exit {expected_exit}, got {result.returncode}\n'
                           f'{result.stdout}{result.stderr}')
    if expected_exit == 2:
        if not result.stderr.startswith('FIXTURE_CHECK_FAILED:') or 'Traceback' in result.stderr:
            raise RuntimeError(f'{label}: failure must use the documented diagnostic\n{result.stderr}')
        if expected_diagnostic and expected_diagnostic not in result.stderr:
            raise RuntimeError(f'{label}: wrong validation failure\n{result.stderr}')
    else:
        report = json.loads(result.stdout)
        if not (report['status'] == 'passed' and report['samples'] == 26 and
                report['model_loaded'] is False and report['audio_played'] is False and
                report['natural_speech_acceptance'] is False):
            raise RuntimeError(f'{label}: unexpected positive report: {report}')
    return {'case': label, 'exit_code': result.returncode,
            'diagnostic': result.stderr.strip()}


def main():
    cases = [
        ('long_reference_not_from_source', lambda base: append_reference(base, 'boundary_long15')),
        ('tail_reference_not_from_source', lambda base: append_reference(base, 'boundary_tail')),
        ('long_keywords_not_from_source', lambda base: drop_keyword(base, 'boundary_long15')),
        ('tail_keywords_not_from_source', lambda base: drop_keyword(base, 'boundary_tail')),
        ('different_noise_with_updated_hash', lambda base: change_pcm(base, 'boundary_noise', 0)),
        ('padding_count_mismatch', lambda base: change_sample(base, 'q_02', trailing_padding_samples=3208)),
        ('padding_duration_mismatch', lambda base: change_sample(base, 'q_02', unpadded_duration_sec=1.8)),
        ('padding_is_not_zero_pcm', lambda base: change_pcm(base, 'q_02', -1)),
        ('padding_bool_rejected', lambda base: change_sample(base, 'zh_01', trailing_padding_samples=False)),
        ('short_declared_length_mismatch', lambda base: change_sample(base, 'boundary_short', source_length_samples=1)),
        ('long_padding_mismatch', lambda base: change_sample(base, 'boundary_long15', trailing_padding_samples=0)),
        ('tail_stop_outside_source', lambda base: change_sample(base, 'boundary_tail', source_stop_samples=1000000)),
        ('tail_leading_padding_mismatch', lambda base: change_sample(base, 'boundary_tail', leading_padding_samples=0)),
        ('tail_threshold_mismatch', lambda base: change_sample(base, 'boundary_tail', tail_threshold_pcm=0)),
        ('wav_hash_corruption', lambda base: corrupt_pcm(base)),
        ('wav_truncation_with_updated_hash', lambda base: corrupt_pcm(base, truncate=True)),
        ('path_escape', lambda base: change_sample(base, 'zh_01', path='../outside.wav')),
        ('symlink_escape', symlink_escape),
        ('duplicate_json_key', duplicate_key),
        ('invalid_json_root', lambda base: edit_json(base, 'manifest.json', lambda data: [])),
        ('invalid_normalization_object', lambda base: edit_json(base, 'manifest.json', lambda data: data.update(normalization=[]))),
        ('invalid_sample_object', lambda base: edit_json(base, 'manifest.json', lambda data: data['samples'].__setitem__(0, []))),
        ('duplicate_sample_id', lambda base: change_sample(base, 'zh_02', sample_id='zh_01')),
        ('missing_sample', lambda base: edit_json(base, 'manifest.json', lambda data: data.update(samples=data['samples'][:-1]))),
        ('frozen_reference_changed', lambda base: append_reference(base, 'zh_01')),
        ('frozen_duplicate_sample_id', lambda base: edit_json(base, 'speech_cases.json', lambda data: data['samples'].__setitem__(1, data['samples'][0]))),
        ('format_bool_rejected', lambda base: change_sample(base, 'zh_01', channels=True)),
        ('nonfinite_json_number', lambda base: edit_json(base, 'manifest.json', lambda data: data.update(extension=float('nan')))),
        ('tts_category_counts_changed', lambda base: edit_json(base, 'tts_cases.json', lambda data: data['cases'][0].update(category='numbers_dates'))),
        ('text_schema_bool_rejected', lambda base: edit_json(base, 'tts_cases.json', lambda data: data.update(schema_version=True))),
        ('text_case_empty_id', lambda base: edit_json(base, 'llm_cases.json', lambda data: data['cases'][0].update(id=''))),
        ('text_case_invalid_object', lambda base: edit_json(base, 'llm_cases.json', lambda data: data['cases'].__setitem__(0, []))),
        ('llm_execution_disclosure_removed', lambda base: edit_json(base, 'llm_cases.json', lambda data: data.update(execution_prohibited_in_this_round=False))),
    ]
    results = []
    with tempfile.TemporaryDirectory(prefix='voice-fixture-negatives-') as directory:
        root = Path(directory)
        valid = root / 'valid'
        shutil.copytree(FIXTURES, valid)
        results.append(run_case(valid, 0, 'unchanged_relocated_fixtures'))
        # Consumers must preserve extension fields and need not preserve row order.
        def add_extension_and_reverse(manifest):
            manifest['review_extension'] = {'comment': 'offline test only'}
            manifest['samples'].reverse()
        edit_json(valid, 'manifest.json', add_extension_and_reverse)
        results.append(run_case(valid, 0, 'extensions_and_sample_order'))
        for index, (label, mutate) in enumerate(cases):
            base = root / f'case-{index:02d}'
            shutil.copytree(FIXTURES, base)
            mutate(base)
            expected_diagnostic = ('Invalid or duplicate frozen sample ID'
                                   if label == 'frozen_duplicate_sample_id' else None)
            results.append(run_case(base, 2, label, expected_diagnostic))
    print(json.dumps({'status': 'passed', 'positive_cases': 2,
                      'negative_cases': len(cases), 'model_loaded': False,
                      'audio_played': False, 'checks': results}, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'FIXTURE_NEGATIVE_TEST_FAILED: {error}', file=sys.stderr)
        sys.exit(1)
