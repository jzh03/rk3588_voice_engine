#!/usr/bin/env python3
"""Offline integrity and coverage audit for frozen file fixtures; loads no model."""
import argparse,array,collections,hashlib,json,math,pathlib,re,sys,unicodedata,wave
ROOT=pathlib.Path(__file__).resolve().parents[2]
EXPECTED=collections.Counter(chinese_common=8,numbers_dates=5,short_question=4,code_switch=3)
BOUNDARIES={'empty','silence','short_audio','long_audio','tail_speech','noise'}

def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for b in iter(lambda:f.read(1048576),b''):h.update(b)
 return h.hexdigest()
def unique_pairs(pairs):
 out={}
 for k,v in pairs:
  if k in out:raise ValueError('Duplicate JSON key: '+k)
  out[k]=v
 return out
def load(path):
 if path.stat().st_size>4*1024*1024:raise ValueError('Manifest exceeds 4MiB: '+str(path))
 return json.loads(path.read_text(encoding='utf-8'),object_pairs_hook=unique_pairs)
def require(ok,message):
 if not ok:raise ValueError(message)
def normalize(text):
 return ''.join(c for c in unicodedata.normalize('NFKC',text).casefold() if not c.isspace() and not unicodedata.category(c).startswith('P'))
def safe_file(base,name):
 require(isinstance(name,str) and name!='' and '\\' not in name,'Invalid relative path')
 rel=pathlib.PurePosixPath(name)
 require(not rel.is_absolute() and '..' not in rel.parts,'Unsafe relative path: '+name)
 resolved=(base/name).resolve()
 require(resolved.is_relative_to(base.resolve()) and resolved.is_file(),'Missing/escaping fixture: '+name)
 return resolved
def check(manifest_path):
 base=manifest_path.parent;manifest=load(manifest_path)
 require(type(manifest.get('schema_version')) is int and manifest['schema_version']==1,'schema_version must be integer 1')
 require(manifest.get('dataset_kind')=='synthetic_engineering_regression','Dataset kind must disclose synthetic origin')
 require(manifest.get('synthetic') is True and manifest.get('natural_speech_acceptance') is False,'Synthetic dataset must not claim natural-speech acceptance')
 require(manifest.get('tts_shared_source') is True,'TTS shared source disclosure missing')
 require(manifest.get('normalization',{}).get('id')=='nfkc-casefold-strip-punctuation-space-v1','Unknown normalization version')
 require(sha(base/'speech_cases.json')==manifest['speech_cases_sha256'],'Frozen text specification hash mismatch')
 require(sha(base/'generator-lock.json')==manifest['generator_lock_sha256'],'Generator lock hash mismatch')
 spec=load(base/'speech_cases.json');frozen={x['sample_id']:x for x in spec['samples']}
 samples=manifest.get('samples');require(isinstance(samples,list) and len(samples)==26,'Require exactly 20 normal + 6 boundary WAVs')
 ids=set();paths=set();counts=collections.Counter();behaviors=set();pcm_by_id={};total=0;durations=[]
 for s in samples:
  sid=s.get('sample_id');require(isinstance(sid,str) and re.fullmatch(r'[a-z0-9_]+',sid) and sid not in ids,'Invalid or duplicate sample ID')
  ids.add(sid);path=safe_file(base,s.get('path'))
  require(path not in paths,'Same WAV used by multiple samples');paths.add(path)
  require(s.get('synthetic') is True and s.get('human_audio_review')=='not_run','Origin/review metadata missing')
  require(isinstance(s.get('source'),dict) and s['source'].get('type') in ('synthetic','procedural_synthetic'),'Missing source recipe')
  require(isinstance(s.get('reference_text'),str) and s.get('normalized_reference')==normalize(s['reference_text']),'Reference normalization mismatch: '+sid)
  require(isinstance(s.get('keywords'),list) and all(isinstance(k,str) and normalize(k) and normalize(k) in s['normalized_reference'] for k in s['keywords']),'Invalid keywords: '+sid)
  require(re.fullmatch('[0-9a-f]{64}',s.get('sha256','')) is not None and sha(path)==s['sha256'],'WAV SHA256 mismatch: '+sid)
  require(type(s.get('size_bytes')) is int and path.stat().st_size==s['size_bytes'],'WAV byte size mismatch: '+sid)
  with wave.open(str(path),'rb') as w:
   n=w.getnframes();rate=w.getframerate();payload=w.readframes(n)
   require((rate,w.getnchannels(),w.getsampwidth(),w.getcomptype())==(16000,1,2,'NONE'),'WAV must be mono 16k PCM16: '+sid)
   require(len(payload)==n*2,'Truncated PCM data: '+sid)
   require(type(s.get('num_samples')) is int and s['num_samples']==n,'Frame count mismatch: '+sid)
   require(s.get('sample_rate')==rate and s.get('channels')==1 and s.get('sample_width_bytes')==2 and s.get('encoding')=='PCM_S16LE','Format metadata mismatch: '+sid)
   require(isinstance(s.get('duration_sec'),(int,float)) and math.isfinite(s['duration_sec']) and abs(s['duration_sec']-n/rate)<1e-10,'Duration metadata mismatch: '+sid)
  data=array.array('h',payload)
  if sys.byteorder!='little':data.byteswap()
  pcm_by_id[sid]=data;total+=len(payload)
  if s['category']=='boundary':
   require(s['expected_behavior'] in BOUNDARIES and s['expected_behavior'] not in behaviors,'Invalid/duplicate boundary behavior')
   behaviors.add(s['expected_behavior'])
  else:
   counts[s['category']]+=1;durations.append(n/rate)
   require(s['expected_behavior']=='speech' and 32000<=n<=128000,'Normal WAV duration/behavior invalid: '+sid)
   require(sid in frozen and all(s[k]==frozen[sid][k] for k in ('category','input_text','reference_text','normalized_reference','keywords','expected_behavior')),'Reference changed after text freeze: '+sid)
   require(any(v!=0 for v in data) and len(s['keywords'])>0,'Normal speech is empty or has no keywords: '+sid)
   require(re.fullmatch('[0-9a-f]{64}',s['source'].get('native_sha256','')) is not None,'Native provenance hash missing')
 require(counts==EXPECTED,'Category coverage mismatch: '+str(dict(counts)))
 require(behaviors==BOUNDARIES,'Boundary coverage mismatch')
 by_behavior={s['expected_behavior']:s for s in samples if s['category']=='boundary'}
 def data_for(k):return pcm_by_id[by_behavior[k]['sample_id']]
 require(len(data_for('empty'))==0,'Empty WAV contains samples')
 require(len(data_for('silence'))==48000 and not any(data_for('silence')),'Silence WAV is not exactly zero/3s')
 short=by_behavior['short_audio'];source=pcm_by_id[short['source']['base_sample_id']];offset=short['source_offset_samples']
 require(len(data_for('short_audio'))==1280 and data_for('short_audio')==source[offset:offset+1280],'Short crop is not the declared 80ms source slice')
 long=by_behavior['long_audio'];source=pcm_by_id[long['source']['base_sample_id']];expected=list(source)*long['utterance_repeats']
 expected += [0]*(240000-len(expected))
 require(len(data_for('long_audio'))==240000 and list(data_for('long_audio'))==expected,'15s boundary does not match complete-utterance repetition recipe')
 tail=by_behavior['tail_speech'];source=pcm_by_id[tail['source']['base_sample_id']]
 require(list(data_for('tail_speech'))==[0]*32000+list(source[:tail['source_stop_samples']]),'Tail boundary source recipe mismatch')
 require(any(abs(v)>32 for v in data_for('tail_speech')[-1600:]),'Tail has no signal within final 100ms')
 noise=data_for('noise');require(len(noise)==48000 and min(noise)>=-512 and max(noise)<=512 and any(noise),'Noise boundary invalid')
 for behavior in ('empty','silence','short_audio','noise'):
  require(by_behavior[behavior]['reference_text']=='' and by_behavior[behavior]['keywords']==[],'Unscored boundary must not carry invented reference')
 for name,count in [('tts_cases.json',10),('llm_cases.json',20)]:
  cases=load(base/name);require(cases.get('schema_version')==1 and isinstance(cases.get('cases'),list) and len(cases['cases'])==count,'Text case coverage mismatch: '+name)
  case_ids=[x['id'] for x in cases['cases']]
  require(len(set(case_ids))==count and all(isinstance(x.get('text'),str) and x['text'].strip() and x.get('category') for x in cases['cases']),'Invalid/duplicate text case: '+name)
  if name.startswith('tts'):
   require({x['category'] for x in cases['cases']}=={'chinese_common','numbers_dates','punctuation','code_switch'},'TTS category coverage incomplete')
  else:require(cases.get('execution_status')=='not_run' and cases.get('execution_prohibited_in_this_round') is True,'LLM execution prohibition missing')
 return dict(status='passed',schema_version=1,manifest_sha256=sha(manifest_path),samples=len(samples),normal_samples=sum(counts.values()),categories=dict(counts),boundaries=sorted(behaviors),normal_duration_min_sec=min(durations),normal_duration_max_sec=max(durations),pcm_size_bytes=total,tts_cases=10,llm_static_cases=20,model_loaded=False,audio_played=False,natural_speech_acceptance=False)
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--manifest',type=pathlib.Path,default=ROOT/'tests/fixtures/manifest.json');ap.add_argument('--json',type=pathlib.Path);args=ap.parse_args()
 result=check(args.manifest.resolve())
 if args.json:
  with args.json.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2);f.write('\n')
 print(json.dumps(result,ensure_ascii=False,indent=2))
if __name__=='__main__':
 try:main()
 except (ValueError,OSError,KeyError,TypeError,wave.Error,EOFError) as exc:print('FIXTURE_CHECK_FAILED: '+str(exc),file=sys.stderr);sys.exit(2)
