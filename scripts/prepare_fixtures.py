#!/usr/bin/env python3
"""Create synthetic file fixtures. No playback, device access or LLM execution.
Frozen WAV hashes define the corpus. New TTS synthesis need not be byte-identical;
--reuse-native-run reconstructs from archived native WAVs without loading a model.
"""
import argparse,array,datetime,hashlib,json,pathlib,re,shutil,subprocess,sys,wave,unicodedata
ROOT=pathlib.Path(__file__).resolve().parents[1]
FIXTURES=ROOT/'tests/fixtures'
LOCK='/tmp/rk3588_voice_engine_model_validation.lock'
CPP=r'''
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "sherpa-onnx/c-api/c-api.h"
using J=nlohmann::json; namespace fs=std::filesystem; using Clock=std::chrono::steady_clock;
int main(int argc,char **argv) {
 try {
  if(argc!=4) throw std::runtime_error("usage: synth SPEC MODEL_DIR OUTPUT_DIR");
  std::ifstream in(argv[1]); J spec; in>>spec; fs::path m(argv[2]),out(argv[3]);
  std::string model=(m/"model.onnx").string(),tokens=(m/"tokens.txt").string(),lexicon=(m/"lexicon.txt").string(),dict=(m/"dict").string(),fsts;
  SherpaOnnxOfflineTtsConfig c{};
  c.model.vits.model=model.c_str(); c.model.vits.tokens=tokens.c_str();
  c.model.vits.lexicon=lexicon.c_str(); c.model.vits.dict_dir=dict.c_str();
  c.model.vits.noise_scale=0.667f; c.model.vits.noise_scale_w=0.8f; c.model.vits.length_scale=1.0f;
  c.model.num_threads=2; c.model.provider="cpu"; c.model.debug=1; c.max_num_sentences=1; c.silence_scale=1.0f;
  for(const auto &f:{"date.fst","number.fst","phone.fst","new_heteronym.fst"}) {
   if(!fsts.empty()) fsts+=","; fsts+=(m/f).string(); }
  c.rule_fsts=fsts.c_str();
  auto start=Clock::now();
  std::unique_ptr<const SherpaOnnxOfflineTts,decltype(&SherpaOnnxDestroyOfflineTts)> tts(SherpaOnnxCreateOfflineTts(&c),&SherpaOnnxDestroyOfflineTts);
  if(!tts) throw std::runtime_error("TTS initialization failed");
  std::cout<<J{{"event","initialized"},{"load_sec",std::chrono::duration<double>(Clock::now()-start).count()},
   {"sherpa_version",SherpaOnnxGetVersionStr()},{"sherpa_git",SherpaOnnxGetGitSha1()},
   {"sample_rate",SherpaOnnxOfflineTtsSampleRate(tts.get())}}.dump()<<std::endl;
  for(const auto &s:spec.at("samples")) {
   auto id=s.at("sample_id").get<std::string>(); fs::path p=out/(id+".wav");
   if(fs::exists(p)) throw std::runtime_error("Refusing existing output: "+p.string());
   auto text=s.at("input_text").get<std::string>();start=Clock::now();
   std::unique_ptr<const SherpaOnnxGeneratedAudio,decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio)> a(SherpaOnnxOfflineTtsGenerate(tts.get(),text.c_str(),0,1.0f),&SherpaOnnxDestroyOfflineTtsGeneratedAudio);
   double elapsed=std::chrono::duration<double>(Clock::now()-start).count();
   if(!a||a->n<=0||a->sample_rate!=44100) throw std::runtime_error("Unexpected empty or non-44100 output");
   if(!SherpaOnnxWriteWave(a->samples,a->n,a->sample_rate,p.string().c_str())) throw std::runtime_error("WAV write failed");
   std::cout<<J{{"event","generated"},{"sample_id",id},{"num_samples",a->n},
    {"sample_rate",a->sample_rate},{"generate_sec",elapsed}}.dump()<<std::endl;
  }
  return 0;
 } catch(const std::exception &e) {std::cerr<<e.what()<<std::endl;return 1;}
}
'''
def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for block in iter(lambda:f.read(1048576),b''):h.update(block)
 return h.hexdigest()
def save(path,obj):
 with path.open('x',encoding='utf-8') as f:json.dump(obj,f,ensure_ascii=False,indent=2);f.write('\n')
def norm(t):
 return ''.join(c for c in unicodedata.normalize('NFKC',t).casefold() if not c.isspace() and not unicodedata.category(c).startswith('P'))
def info(path):
 with wave.open(str(path),'rb') as w:
  if w.getcomptype()!='NONE' or w.getsampwidth()!=2:raise ValueError('Expected PCM16: '+str(path))
  return dict(sha256=sha(path),size_bytes=path.stat().st_size,sample_rate=w.getframerate(),channels=w.getnchannels(),num_samples=w.getnframes(),duration_sec=w.getnframes()/w.getframerate(),sample_width_bytes=2,encoding='PCM_S16LE')
def read_pcm(path):
 with wave.open(str(path),'rb') as w:
  if (w.getframerate(),w.getnchannels(),w.getsampwidth())!=(16000,1,2):raise ValueError('Expected 16k mono PCM16')
  a=array.array('h',w.readframes(w.getnframes()))
 if sys.byteorder!='little':a.byteswap()
 return a

def write_pcm(path,pcm):
 if path.exists():raise FileExistsError(path)
 a=array.array('h',pcm)
 if sys.byteorder!='little':a.byteswap()
 with wave.open(str(path),'wb') as w:w.setparams((1,2,16000,0,'NONE','not compressed'));w.writeframes(a.tobytes())

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('--destination',type=pathlib.Path,default=FIXTURES)
 ap.add_argument('--run-id',default='file-fixtures-'+datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).strftime('%Y%m%d-%H%M%S'))
 ap.add_argument('--reuse-native-run',type=pathlib.Path,help='Archived native-index.json and native/; no model is loaded')
 args=ap.parse_args()
 if not re.fullmatch(r'file-fixtures-[A-Za-z0-9_.-]+',args.run_id):ap.error('run-id must start file-fixtures- and use safe ASCII characters')
 dest=args.destination.resolve()
 if dest!=FIXTURES.resolve() and not dest.is_relative_to((ROOT/'output/runs').resolve()):ap.error('destination must be tests/fixtures or beneath output/runs')
 if (dest/'manifest.json').exists() or (dest/'audio').exists():ap.error('Refusing existing manifest/audio; choose a new --destination')
 spec_path=FIXTURES/'speech_cases.json';spec=json.loads(spec_path.read_text())
 pins=json.loads((FIXTURES/'generator-lock.json').read_text())
 for item in pins['files']:
  p=ROOT/item['path']
  if not p.is_file() or sha(p)!=item['sha256']:raise ValueError('Locked input missing or changed: '+str(p))
 run=ROOT/'output/runs'/args.run_id;run.mkdir(parents=True,exist_ok=False)
 native=run/'native';native.mkdir();audio=run/'audio';audio.mkdir()
 shutil.copy2(spec_path,run/'speech_cases.json');shutil.copy2(FIXTURES/'generator-lock.json',run/'generator-lock.json')
 if args.reuse_native_run:
  old=args.reuse_native_run.resolve();index=json.loads((old/'native-index.json').read_text())
  if index['speech_cases_sha256']!=sha(spec_path):raise ValueError('Native text specification mismatch')
  for s in spec['samples']:
   sid=s['sample_id'];origin=old/'native'/(sid+'.wav')
   if sha(origin)!=index['files'][sid]['sha256']:raise ValueError('Native WAV hash mismatch: '+sid)
   shutil.copy2(origin,native/origin.name)
  save(run/'reuse.json',dict(source_run=str(old),model_loaded=False))
 else:
  sdk=ROOT/'deps/model-prep/speech/sherpa-onnx-v1.12.14-linux-aarch64-shared-cpu'
  source=run/'batch_synthesize.cpp';source.write_text(CPP,encoding='utf-8');binary=run/'batch_synthesize'
  cmd=['g++','-std=c++17','-O2',str(source),'-I'+str(ROOT/'deps/model-prep/speech/include'),'-L'+str(sdk/'lib'),'-lsherpa-onnx-c-api','-Wl,-rpath,'+str(sdk/'lib'),'-o',str(binary)]
  save(run/'compile-command.json',cmd)
  with (run/'compile.log').open('x') as log:subprocess.run(cmd,check=True,stdout=log,stderr=subprocess.STDOUT)
  cmd=['flock','-x',LOCK,'python3',str(ROOT/'deps/model-prep/speech/measure.py'),str(run/'generation'),str(binary),str(run/'speech_cases.json'),str(ROOT/'models/tts/vits-melo-tts-zh_en'),str(native)]
  save(run/'generation-command.json',cmd)
  print('Waiting for shared model lock; one resident model generates the batch.',flush=True)
  subprocess.run(cmd,check=True,cwd=ROOT)
 index=dict(speech_cases_sha256=sha(spec_path),files={s['sample_id']:info(native/(s['sample_id']+'.wav')) for s in spec['samples']})
 save(run/'native-index.json',index)
 samples=[];commands=[]
 for case in spec['samples']:
  sid=case['sample_id'];out=audio/(sid+'.wav')
  cmd=['ffmpeg','-nostdin','-v','error','-n','-i',str(native/(sid+'.wav')),'-map_metadata','-1','-ac','1','-ar','16000','-c:a','pcm_s16le','-fflags','+bitexact','-flags:a','+bitexact',str(out)]
  subprocess.run(cmd,check=True);commands.append(cmd);pcm=read_pcm(out);frames=len(pcm)
  if frames>128000:raise ValueError('Normal utterance exceeds 8 seconds: '+sid)
  if frames<32000:
   padded=audio/(sid+'.padded.wav');write_pcm(padded,list(pcm)+[0]*(32000-frames));padded.replace(out)
  s=dict(case,path='audio/'+out.name,**info(out))
  s.update(synthetic=True,source=dict(type='synthetic',generator='MeloTTS FP32 via sherpa-onnx 1.12.14',native_path=str((native/(sid+'.wav')).relative_to(ROOT)),native_sha256=index['files'][sid]['sha256'],generation_conditions_ref='generator-lock.json'),unpadded_duration_sec=frames/16000,trailing_padding_samples=max(0,32000-frames),human_audio_review='not_run',reference_kind='intended_spoken_text_not_audited_transcript')
  samples.append(s)
 save(run/'conversion-commands.json',commands)
 base=samples[0];pcm=read_pcm(audio/(base['sample_id']+'.wav'))
 def boundary(sid,behavior,data,recipe,reference='',keywords=None,extra=None):
  p=audio/(sid+'.wav');write_pcm(p,data)
  row=dict(sample_id=sid,path='audio/'+p.name,category='boundary',expected_behavior=behavior,reference_text=reference,normalized_reference=norm(reference),keywords=keywords or [],synthetic=True,source=dict(type='procedural_synthetic',recipe=recipe,base_sample_id=base['sample_id']),human_audio_review='not_run',reference_kind='intended_spoken_text_not_audited_transcript',**info(p))
  if extra:row.update(extra)
  samples.append(row)
 boundary('boundary_empty','empty',[],'Valid zero-sample WAV. Finish/reset must not create speech text.')
 boundary('boundary_silence','silence',[0]*48000,'Exactly 3 seconds of zero PCM; expect no speech/final text.')
 mid=len(pcm)//2
 boundary('boundary_short','short_audio',pcm[mid:mid+1280],'80ms midpoint crop of zh_01; no complete-word reference; no crash/hang, excluded from CER.',extra=dict(source_offset_samples=mid,source_length_samples=1280))
 repeats=240000//len(pcm);repeated=list(pcm)*repeats
 boundary('boundary_long15','long_audio',repeated+[0]*(240000-len(repeated)),'Repeat complete zh_01 utterance as many times as fit, then zero-pad to exactly 15s; no word truncation.',reference=base['reference_text']*repeats,keywords=base['keywords'],extra=dict(utterance_repeats=repeats,trailing_padding_samples=240000-len(repeated)))
 active=[i for i,v in enumerate(pcm) if abs(v)>32];stop=min(len(pcm),active[-1]+321)
 boundary('boundary_tail','tail_speech',[0]*32000+list(pcm[:stop]),'2s leading silence + zh_01 through last abs(PCM)>32, retaining 20ms tail; EOF flush test.',reference=base['reference_text'],keywords=base['keywords'],extra=dict(leading_padding_samples=32000,source_stop_samples=stop,tail_threshold_pcm=32))
 state=0x13579BDF;noise=[]
 for _ in range(48000):
  state^=(state<<13)&0xffffffff;state^=state>>17;state^=(state<<5)&0xffffffff;state&=0xffffffff;noise.append((state%1025)-512)
 boundary('boundary_noise','noise',noise,'3s deterministic xorshift32 noise; seed=0x13579BDF, integer amplitude [-512,512]; no speech reference.')
 manifest={k:v for k,v in spec.items() if k!='samples'}
 manifest.update(dataset_kind='synthetic_engineering_regression',run_id=args.run_id,natural_speech_acceptance=False,tts_shared_source=True,generator_lock_sha256=sha(FIXTURES/'generator-lock.json'),speech_cases_sha256=sha(spec_path),samples=samples,limitations=['Single synthetic voice; same model family as TTS under test.','Reference is intended text, not human-audited audio transcription.','No listening review, natural-speech accuracy or device acceptance claimed.','New synthesis can differ; frozen WAV SHA256 defines the dataset.'])
 save(run/'manifest.json',manifest);dest.mkdir(parents=True,exist_ok=True)
 shutil.copytree(audio,dest/'audio');shutil.copy2(run/'manifest.json',dest/'manifest.json')
 if dest!=FIXTURES:
  for name in ['speech_cases.json','tts_cases.json','llm_cases.json','generator-lock.json']:shutil.copy2(FIXTURES/name,dest/name)
 save(run/'delivery.json',dict(destination=str(dest),manifest_sha256=sha(dest/'manifest.json'),sample_count=len(samples),normal_sample_count=20,audio_size_bytes=sum(s['size_bytes'] for s in samples),llm_execution='not_run',audio_playback='not_run'))
 print(json.dumps(dict(run=str(run),destination=str(dest),samples=len(samples)),ensure_ascii=False),flush=True)
if __name__=='__main__':
 try:main()
 except (ValueError,OSError,subprocess.SubprocessError) as e:print('FIXTURE_PREPARE_FAILED: '+str(e),file=sys.stderr);sys.exit(1)
