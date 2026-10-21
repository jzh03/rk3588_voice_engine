#!/usr/bin/env python3
"""CLI rejection checks; these never initialize model runtimes."""
import subprocess,sys,tempfile,pathlib,json,hashlib,wave
exe=sys.argv[1]
def run(args,expected):
 p=subprocess.run([exe]+args,text=True,capture_output=True,timeout=10)
 assert p.returncode==expected,(args,p.returncode,p.stdout,p.stderr)
 return p
assert 'flock' in run(['--help'],0).stdout
run([],2)
run(['--unknown','x'],2)
run(['--config','missing','--official','--run-id','../escape'],2)
run(['--config','missing','--official','--run-id','test','--chunk-ms','21'],2)
run(['--config','missing','--official','--run-id','test','--repeat','0'],2)
run(['--config','missing','--official','--manifest','missing','--run-id','test'],2)
run(['--config','missing','--official','--run-id','test'],2)
# These run before lock/model initialization. The optional probe build already
# requires the pinned local dependencies; asset hashing is read-only.
root=pathlib.Path(__file__).resolve().parents[2]
config=root/'config/file-probes.yaml'
if config.is_file():
 with tempfile.TemporaryDirectory(prefix='asr-probe-rejections-') as directory:
  tmp=pathlib.Path(directory)
  args=['--config',str(config),'--manifest',str(tmp/'missing.json'),
        '--output-dir',str(tmp/'runs'),'--run-id','negative']
  result=run(args,2)
  assert 'JSON' in result.stderr and 'model validation' not in result.stderr
  wav=tmp/'wrong-rate.wav'
  with wave.open(str(wav),'wb') as out:
   out.setnchannels(1);out.setsampwidth(2);out.setframerate(8000);out.writeframes(b'\0\0'*80)
  sample={'sample_id':'bad','path':wav.name,'sha256':hashlib.sha256(wav.read_bytes()).hexdigest(),
          'sample_rate':16000,'channels':1,'num_samples':80,'duration_sec':.005,
          'reference_text':'','normalized_reference':'','keywords':[],
          'category':'boundary','expected_behavior':'short_audio'}
  manifest=tmp/'manifest.json'
  manifest.write_text(json.dumps({'schema_version':1,'samples':[sample]}))
  args[3]=str(manifest)
  result=run(args,2)
  assert 'fixture WAV must be PCM16 mono 16 kHz RIFF' in result.stderr,result.stderr
  sample['path']='../escape.wav'
  manifest.write_text(json.dumps({'schema_version':1,'samples':[sample]}))
  result=run(args,2)
  assert 'asset traversal' in result.stderr,result.stderr
  assert not (tmp/'runs').exists(),'negative preflight must not create model run'
print('ASR_PROBE_CLI_OK')
