#!/usr/bin/env python3
"""Offline integrity and coverage audit for frozen file fixtures; loads no model."""
import argparse,array,collections,hashlib,json,math,pathlib,re,sys,unicodedata,wave
ROOT=pathlib.Path(__file__).resolve().parents[2]
EXPECTED=collections.Counter(chinese_common=8,numbers_dates=5,short_question=4,code_switch=3)
TTS_EXPECTED=collections.Counter(chinese_common=3,numbers_dates=3,punctuation=2,code_switch=2)
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
def invalid_constant(value):
 raise ValueError('Non-finite JSON number: '+value)
def load(path):
 if path.stat().st_size>4*1024*1024:raise ValueError('Manifest exceeds 4MiB: '+str(path))
 value=json.loads(path.read_text(encoding='utf-8'),object_pairs_hook=unique_pairs,parse_constant=invalid_constant)
 require(isinstance(value,dict),'JSON root must be an object: '+str(path))
 return value
def require(ok,message):
 if not ok:raise ValueError(message)
def integer(value,minimum=0):
 return type(value) is int and value>=minimum
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
 normalization=manifest.get('normalization')
 require(isinstance(normalization,dict) and normalization.get('id')=='nfkc-casefold-strip-punctuation-space-v1','Unknown normalization version')
 spec_path=safe_file(base,'speech_cases.json');lock_path=safe_file(base,'generator-lock.json')
 require(sha(spec_path)==manifest['speech_cases_sha256'],'Frozen text specification hash mismatch')
 require(sha(lock_path)==manifest['generator_lock_sha256'],'Generator lock hash mismatch')
 spec=load(spec_path)
 require(type(spec.get('schema_version')) is int and spec['schema_version']==1,'Frozen text schema_version must be integer 1')
 frozen_rows=spec.get('samples')
 require(isinstance(frozen_rows,list) and len(frozen_rows)==20,'Frozen text must contain exactly 20 samples')
 frozen={}
 for row in frozen_rows:
  require(isinstance(row,dict),'Frozen text sample must be an object')
  sid=row.get('sample_id')
  require(isinstance(sid,str) and re.fullmatch(r'[a-z0-9_]+',sid) and sid not in frozen,'Invalid or duplicate frozen sample ID')
  frozen[sid]=row
 samples=manifest.get('samples');require(isinstance(samples,list) and len(samples)==26,'Require exactly 20 normal + 6 boundary WAVs')
 ids=set();paths=set();counts=collections.Counter();behaviors=set();pcm_by_id={};total=0;durations=[]
 for s in samples:
  require(isinstance(s,dict),'Sample must be an object')
  sid=s.get('sample_id');require(isinstance(sid,str) and re.fullmatch(r'[a-z0-9_]+',sid) and sid not in ids,'Invalid or duplicate sample ID')
  ids.add(sid);path=safe_file(base,s.get('path'))
  require(path not in paths,'Same WAV used by multiple samples');paths.add(path)
  require(s.get('synthetic') is True and s.get('human_audio_review')=='not_run','Origin/review metadata missing')
  category=s.get('category')
  require(isinstance(category,str) and category in {*EXPECTED,'boundary'},'Invalid sample category: '+sid)
  behavior=s.get('expected_behavior')
  require(isinstance(behavior,str),'Expected behavior must be a string: '+sid)
  source=s.get('source');source_type='procedural_synthetic' if category=='boundary' else 'synthetic'
  require(isinstance(source,dict) and source.get('type')==source_type,'Missing/inconsistent source recipe: '+sid)
  require(s.get('reference_kind')=='intended_spoken_text_not_audited_transcript','Reference review limitation missing: '+sid)
  require(isinstance(s.get('reference_text'),str) and s.get('normalized_reference')==normalize(s['reference_text']),'Reference normalization mismatch: '+sid)
  require(isinstance(s.get('keywords'),list) and all(isinstance(k,str) and normalize(k) and normalize(k) in s['normalized_reference'] for k in s['keywords']),'Invalid keywords: '+sid)
  require(isinstance(s.get('sha256'),str) and re.fullmatch('[0-9a-f]{64}',s['sha256']) is not None and sha(path)==s['sha256'],'WAV SHA256 mismatch: '+sid)
  require(integer(s.get('size_bytes')) and path.stat().st_size==s['size_bytes'],'WAV byte size mismatch: '+sid)
  with wave.open(str(path),'rb') as w:
   n=w.getnframes();rate=w.getframerate()
   require((rate,w.getnchannels(),w.getsampwidth(),w.getcomptype())==(16000,1,2,'NONE'),'WAV must be mono 16k PCM16: '+sid)
   require(0<=n<=240000,'WAV exceeds fixed corpus frame limit: '+sid)
   payload=w.readframes(n)
   require(len(payload)==n*2,'Truncated PCM data: '+sid)
   require(integer(s.get('num_samples')) and s['num_samples']==n,'Frame count mismatch: '+sid)
   require(all(type(s.get(k)) is int and s[k]==v for k,v in [('sample_rate',rate),('channels',1),('sample_width_bytes',2)]) and s.get('encoding')=='PCM_S16LE','Format metadata mismatch: '+sid)
   require(type(s.get('duration_sec')) in (int,float) and 0<=s['duration_sec']<=15 and math.isfinite(s['duration_sec']) and abs(s['duration_sec']-n/rate)<1e-10,'Duration metadata mismatch: '+sid)
  data=array.array('h',payload)
  if sys.byteorder!='little':data.byteswap()
  pcm_by_id[sid]=data;total+=len(payload)
  if category=='boundary':
   require(behavior in BOUNDARIES and behavior not in behaviors,'Invalid/duplicate boundary behavior')
   require(source.get('base_sample_id')==frozen_rows[0]['sample_id'],'Boundary must use declared first frozen speech sample: '+sid)
   behaviors.add(behavior)
  else:
   counts[category]+=1;durations.append(n/rate)
   require(behavior=='speech' and 32000<=n<=128000,'Normal WAV duration/behavior invalid: '+sid)
   require(sid in frozen and all(s[k]==frozen[sid][k] for k in ('category','input_text','reference_text','normalized_reference','keywords','expected_behavior')),'Reference changed after text freeze: '+sid)
   require(any(v!=0 for v in data) and len(s['keywords'])>0,'Normal speech is empty or has no keywords: '+sid)
   native_hash=source.get('native_sha256')
   require(isinstance(native_hash,str) and re.fullmatch('[0-9a-f]{64}',native_hash) is not None,'Native provenance hash missing')
   unpadded=s.get('unpadded_duration_sec');padding=s.get('trailing_padding_samples')
   require(type(unpadded) in (int,float) and 0<unpadded<=8 and math.isfinite(unpadded) and integer(padding),'Invalid padding metadata: '+sid)
   frames=round(unpadded*rate)
   require(abs(unpadded-frames/rate)<1e-10 and frames+padding==n and padding==max(0,32000-frames),'Padding duration/count mismatch: '+sid)
   require(not any(data[frames:]),'Declared trailing padding is not zero PCM: '+sid)
 require(counts==EXPECTED,'Category coverage mismatch: '+str(dict(counts)))
 require(behaviors==BOUNDARIES,'Boundary coverage mismatch')
 by_behavior={s['expected_behavior']:s for s in samples if s['category']=='boundary'}
 def data_for(k):return pcm_by_id[by_behavior[k]['sample_id']]
 base_id=frozen_rows[0]['sample_id'];source=pcm_by_id[base_id];source_case=frozen[base_id]
 require(len(data_for('empty'))==0,'Empty WAV contains samples')
 require(len(data_for('silence'))==48000 and not any(data_for('silence')),'Silence WAV is not exactly zero/3s')
 short=by_behavior['short_audio'];offset=short.get('source_offset_samples')
 require(integer(offset) and offset==len(source)//2 and type(short.get('source_length_samples')) is int and short['source_length_samples']==1280,'Short crop offset/length recipe mismatch')
 require(len(data_for('short_audio'))==1280 and data_for('short_audio')==source[offset:offset+1280],'Short crop is not the declared 80ms source slice')
 long=by_behavior['long_audio'];repeats=long.get('utterance_repeats');padding=long.get('trailing_padding_samples')
 require(integer(repeats,1) and repeats==240000//len(source) and integer(padding) and padding==240000-len(source)*repeats,'15s repeat/padding recipe mismatch')
 expected=list(source)*repeats+[0]*padding
 require(len(data_for('long_audio'))==240000 and list(data_for('long_audio'))==expected,'15s boundary does not match complete-utterance repetition recipe')
 require(long['reference_text']==source_case['reference_text']*repeats and long['keywords']==source_case['keywords'],'15s boundary reference/keywords differ from repeated source')
 tail=by_behavior['tail_speech'];stop=tail.get('source_stop_samples')
 active=[i for i,v in enumerate(source) if abs(v)>32]
 require(active,'Tail source has no signal above threshold')
 require(integer(stop,1) and stop==min(len(source),active[-1]+321),'Tail stop does not match signal threshold recipe')
 require(type(tail.get('leading_padding_samples')) is int and tail['leading_padding_samples']==32000 and type(tail.get('tail_threshold_pcm')) is int and tail['tail_threshold_pcm']==32,'Tail padding/threshold recipe mismatch')
 require(list(data_for('tail_speech'))==[0]*32000+list(source[:stop]),'Tail boundary source recipe mismatch')
 require(tail['reference_text']==source_case['reference_text'] and tail['keywords']==source_case['keywords'],'Tail boundary reference/keywords differ from source')
 require(any(abs(v)>32 for v in data_for('tail_speech')[-1600:]),'Tail has no signal within final 100ms')
 state=0x13579BDF;expected_noise=array.array('h')
 for _ in range(48000):
  state^=(state<<13)&0xffffffff;state^=state>>17;state^=(state<<5)&0xffffffff;state&=0xffffffff;expected_noise.append((state%1025)-512)
 require(data_for('noise')==expected_noise,'Noise does not match fixed xorshift32 seed/recipe')
 for behavior in ('empty','silence','short_audio','noise'):
  require(by_behavior[behavior]['reference_text']=='' and by_behavior[behavior]['keywords']==[],'Unscored boundary must not carry invented reference')
 for name,count in [('tts_cases.json',10),('llm_cases.json',20)]:
  cases=load(safe_file(base,name));require(type(cases.get('schema_version')) is int and cases['schema_version']==1 and isinstance(cases.get('cases'),list) and len(cases['cases'])==count,'Text case coverage mismatch: '+name)
  rows=cases['cases']
  require(all(isinstance(x,dict) and isinstance(x.get('id'),str) and re.fullmatch(r'[a-z0-9_]+',x['id']) and isinstance(x.get('text'),str) and x['text'].strip() and isinstance(x.get('category'),str) and x['category'].strip() for x in rows),'Invalid text case: '+name)
  require(len({x['id'] for x in rows})==count,'Duplicate text case: '+name)
  if name.startswith('tts'):
   require(collections.Counter(x['category'] for x in rows)==TTS_EXPECTED,'TTS category coverage mismatch')
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
 except (ValueError,OSError,KeyError,TypeError,RecursionError,wave.Error,EOFError) as exc:print('FIXTURE_CHECK_FAILED: '+str(exc),file=sys.stderr);sys.exit(2)
