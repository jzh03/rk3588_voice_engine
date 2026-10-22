#include "voice_engine/asr.hpp"
#include "voice_engine/vad.hpp"
#include "voice_engine/assets.hpp"
#include "voice_engine/probe_support.hpp"
#include "../src/asr/text_metrics.hpp"
#include <nlohmann/json.hpp>
#include <sndfile.h>
#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
using json=nlohmann::json;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
using voice_engine::ConfigError;
namespace {
struct Options {std::string config,manifest,output="output/runs",id;
int chunk=20,repeat=1,pre_roll_ms=0;
bool official=false,listening=false;
std::set<std::string> samples;
};
Options options(int argc,char** argv){
    Options o;
    for(int i=1;i<argc;++i){std::string a=argv[i];
    if(a=="--help"){std::cout<<"asr_probe --config PATH (--manifest FIXTURES_JSON | --official) --output-dir ROOT --run-id ID [--chunk-ms 10|20|37] [--repeat 1..20] [--sample ID ...] [--listening] [--pre-roll-ms N]\nFile-only 16 kHz mono PCM16; no audio device or LLM access. Run complete process under flock -x /tmp/rk3588_voice_engine_model_validation.lock. Exit: 0 applicable checks pass, 2 CLI/config/asset/lock error, 4 model/runtime failure, 6 quality/reset failure. External watchdog handles hard timeouts.\n";
    std::exit(0);
    }if(a=="--official"){o.official=true;
    continue;
    }if(a=="--listening"){o.listening=true;continue;}if(i+1==argc)throw ConfigError("missing value: "+a);
    std::string v=argv[++i];
        if(a=="--config")o.config=v;
        else if(a=="--manifest")o.manifest=v;
        else if(a=="--output-dir")o.output=v;
        else if(a=="--run-id")o.id=v;
        else if(a=="--sample")o.samples.insert(v);
        else if(a=="--chunk-ms"||a=="--repeat"||a=="--pre-roll-ms"){if(!std::regex_match(v,std::regex("[0-9]+")))throw ConfigError("invalid integer: "+a);
        try{int n=std::stoi(v);
        if(a=="--chunk-ms")o.chunk=n;
        else if(a=="--repeat")o.repeat=n;
        else o.pre_roll_ms=n;
        }catch(...){throw ConfigError("invalid integer: "+a);
        }}else throw ConfigError("unknown option: "+a);
    }
    if(o.config.empty()||o.id.empty()||(!o.official&&o.manifest.empty())||(o.official&&!o.manifest.empty()))throw ConfigError("required --config, --run-id, and exactly one of --manifest/--official");
    if(!std::regex_match(o.id,std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,63}")))throw ConfigError("invalid run-id");
    if(o.chunk!=10&&o.chunk!=20&&o.chunk!=37)throw ConfigError("chunk-ms must be 10, 20, or 37");
    if(o.repeat<1||o.repeat>20)throw ConfigError("repeat must be 1..20");
    if(o.pre_roll_ms&&!o.listening)throw ConfigError("pre-roll-ms requires --listening");
    return o;
}
json read_json(const fs::path& path) {
    try {
        if (!fs::is_regular_file(path) || fs::file_size(path) > 4 * 1024 * 1024) {
            throw ConfigError("JSON must be a regular file no larger than 4 MiB: " + path.string());
        }
        std::ifstream input(path);
        if (!input) throw ConfigError("cannot open JSON: " + path.string());
        json value;
        input >> value;
        return value;
    } catch (const fs::filesystem_error& e) {
        throw ConfigError(std::string("JSON path error: ") + e.what());
    }
}

fs::path confined(const fs::path& base, const std::string& name) {
    try {
        fs::path path(name);
        if (name.find('\0') != std::string::npos || path.empty() || path.is_absolute()) {
            throw ConfigError("asset path must be relative and contain no NUL");
        }
        for (const auto& part : path) {
            if (part == "..") throw ConfigError("asset traversal");
        }
        const auto resolved = fs::canonical(base / path);
        const auto root = fs::canonical(base);
        auto a = root.begin();
        auto b = resolved.begin();
        for (; a != root.end(); ++a, ++b) {
            if (b == resolved.end() || *a != *b) throw ConfigError("asset symlink escape");
        }
        if (!fs::is_regular_file(resolved)) throw ConfigError("asset must be regular file");
        return resolved;
    } catch (const fs::filesystem_error& e) {
        throw ConfigError(std::string("asset path error: ") + e.what());
    }
}

void validate_wav_header(const fs::path& path, const json& sample) {
    SF_INFO info{};
    std::unique_ptr<SNDFILE, decltype(&sf_close)> wav(
        sf_open(path.c_str(), SFM_READ, &info), sf_close);
    if (!wav) throw ConfigError(std::string("fixture WAV open failed: ") + sf_strerror(nullptr));
    if (info.samplerate != 16000 || info.channels != 1 ||
        (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16 ||
        (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV) {
        throw ConfigError("fixture WAV must be PCM16 mono 16 kHz RIFF");
    }
    if (info.frames < 0 || info.frames > 120 * 16000 ||
        info.frames != sample.at("num_samples").get<sf_count_t>()) {
        throw ConfigError("fixture WAV frame count mismatch or 120 s bound exceeded");
    }
    static const std::set<std::string> behaviors{
        "speech", "empty", "silence", "noise", "short_audio", "long_audio", "tail_speech"};
    if (!behaviors.count(sample.at("expected_behavior").get<std::string>())) {
        throw ConfigError("unknown fixture expected_behavior");
    }
}

json select_model(const json& manifest,const std::string& id){for(auto& m:manifest.at("models"))if(m.at("id")==id)return m;
throw ConfigError("selected model missing: "+id);
}
fs::path model_path(const fs::path& base,const json& m,const std::string& name){bool listed=false;
for(auto& f:m.at("files"))if(f.at("path")==name)listed=true;
if(!listed)throw ConfigError("role path is not declared in selected model files: "+name);
return confined(base,name);
}
json memory(){
    struct rusage r{};
    getrusage(RUSAGE_SELF,&r);
    json j={{"process_peak_rss_kib",r.ru_maxrss},{"scope","Linux RUSAGE_SELF ru_maxrss; RSS excludes total NPU/system allocation; PSS is point sample"}};
    for(auto path:{"/proc/self/status","/proc/self/smaps_rollup","/proc/meminfo"}){std::ifstream in(path);
    std::string line;
    while(std::getline(in,line)){std::istringstream s(line);
    std::string key;
    long value=0;
    s>>key>>value;
    if(key=="VmRSS:"||key=="Pss:"||key=="MemAvailable:"||key=="SwapFree:")j[key.substr(0,key.size()-1)+"_kib"]=value;
    }}
    return j;
}
json libraries(){std::ifstream in("/proc/self/maps");
std::set<std::string> paths;
std::string line;
while(std::getline(in,line)){auto p=line.find('/');
if(p==std::string::npos)continue;
auto path=line.substr(p);
if(path.find("librknnrt")!=std::string::npos||path.find("libsherpa-onnx")!=std::string::npos||path.find("libonnxruntime")!=std::string::npos)paths.insert(path);
}json out=json::array();
for(auto& p:paths)out.push_back({{"path",p},{"sha256",voice_engine::file_sha256(p)}});
return out;
}
json verified_libraries(const voice_engine::Config& config,const json& asr_model,const json& vad_model){
    auto dep_path=fs::path(config.assets.deps_manifest);
    if(dep_path.is_relative())dep_path=config.source_path.parent_path()/dep_path;
    auto deps=read_json(dep_path);
    std::set<std::string> required;
    for(const auto& model:{asr_model,vad_model})for(const auto& dep:model.at("dependencies"))required.insert(dep.at("id").get<std::string>());
    std::map<std::string,std::string> allowed;
    for(const auto& dep:deps.at("dependencies"))if(required.count(dep.at("id").get<std::string>()))for(const auto& file:dep.at("files")){
        auto name=file.at("path").get<std::string>();
        if(name.find(".so")!=std::string::npos)allowed[confined(dep_path.parent_path(),name).string()]=file.at("sha256").get<std::string>();
    }
    auto observed=libraries();
    for(const auto& lib:observed){auto canonical=fs::canonical(lib.at("path").get<std::string>()).string();
    auto found=allowed.find(canonical);
        if(found==allowed.end()||found->second!=lib.at("sha256").get<std::string>())throw ConfigError("loaded model library is not a validated dependency: "+canonical);
    }
    if(observed.size()!=3)throw ConfigError("expected exactly RKNN, sherpa C and ORT libraries");
    return observed;
}
double seconds(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double>(b-a).count();
}
void save(const fs::path& p,const json& j){std::ofstream f(p);
if(!f)throw std::runtime_error("cannot create report: "+p.string());
f<<j.dump(2)<<'\n';
if(!f)throw std::runtime_error("report write failed");
}
double percentile(std::vector<double> x,double p){if(x.empty())return 0;
std::sort(x.begin(),x.end());
return x[static_cast<std::size_t>(std::ceil(p*x.size()))-1];
}
}
int main(int argc,char** argv){
    fs::path run;
    try{
        auto o=options(argc,argv);
        auto config=voice_engine::load_config(o.config);
        if(o.pre_roll_ms>config.vad.pre_roll_ms)throw ConfigError("pre-roll-ms exceeds configured pre_roll_ms");
        config.models.llm.enabled=false;
        config.models.tts.enabled=false;
        config.models.kws.enabled=false;
        if(!config.models.asr.enabled||!config.models.vad.enabled)throw ConfigError("asr_probe requires enabled ASR and VAD ids");
        auto assets=voice_engine::validate_assets(config);
        auto manifest_path=fs::path(config.assets.models_manifest);
        if(manifest_path.is_relative())manifest_path=config.source_path.parent_path()/manifest_path;
        auto manifest=read_json(manifest_path);
        auto asr_model=select_model(manifest,config.models.asr.id),vad_model=select_model(manifest,config.models.vad.id);
        auto model_base=manifest_path.parent_path();
        auto role=[&](const std::string& k){return model_path(model_base,asr_model,asr_model.at("roles").at(k).get<std::string>());
        };
        voice_engine::ZipformerFiles files{role("encoder"),role("decoder"),role("joiner"),role("vocabulary"),role("feature_config")};
        auto vad_file=model_path(model_base,vad_model,vad_model.at("model_path"));
        json samples=json::array();
        fs::path fixture_base;
        if(o.official){auto wav=role("official_wav");
        fixture_base=wav.parent_path();
        samples.push_back({{"sample_id","official"},{"path",wav.filename().string()},{"sha256",voice_engine::file_sha256(wav)},{"sample_rate",16000},{"channels",1},{"num_samples",89784},{"duration_sec",5.6115},{"reference_text","对我做了介绍那么我想说的是大家如果对我的研究感兴趣呢"},{"normalized_reference","对我做了介绍那么我想说的是大家如果对我的研究感兴趣呢"},{"keywords",json::array({"介绍","研究","感兴趣"})},{"category","official"},{"expected_behavior","speech"}});
        }
        else {auto f=read_json(o.manifest);
        if(f.at("schema_version")!=1||!f.at("samples").is_array())throw ConfigError("invalid fixtures manifest");
        samples=f.at("samples");
        fixture_base=fs::absolute(o.manifest).parent_path();
        }
        if(samples.size()>256)throw ConfigError("too many fixtures");
        std::vector<json> selected;
        std::set<std::string> seen;
        for(auto sample:samples){auto id=sample.at("sample_id").get<std::string>();
        if(!seen.insert(id).second)throw ConfigError("duplicate fixture id");
        if(!o.samples.empty()&&!o.samples.count(id))continue;
        auto path=confined(fixture_base,sample.at("path"));
        if(voice_engine::file_sha256(path)!=sample.at("sha256"))throw ConfigError("fixture checksum mismatch: "+id);
        if(sample.at("sample_rate")!=16000||sample.at("channels")!=1)throw ConfigError("fixture format must be 16 kHz mono");
        validate_wav_header(path,sample);
        sample["resolved_path"]=path.string();
        selected.push_back(sample);
        }
        for(auto& id:o.samples)if(!seen.count(id))throw ConfigError("unknown sample selection: "+id);
        if(selected.empty())throw ConfigError("no selected samples");
        voice_engine::probe::require_model_validation_lock();
        const auto loaded_libraries=verified_libraries(config,asr_model,vad_model);
        auto before=memory();
        if(before.at("MemAvailable_kib").get<long>()<1536*1024)throw std::runtime_error("MemAvailable below 1.5 GiB before model initialization");
        run=fs::path(o.output)/o.id;
        fs::create_directories(run.parent_path());
        if(!fs::create_directory(run))throw ConfigError("run directory already exists");
        const auto begin=Clock::now();
        auto init_start=Clock::now();
        voice_engine::ZipformerAsr asr(files,config.queues.llm_text_bytes);
        voice_engine::SileroVad vad(vad_file,config.vad,config.threads.vad);
        voice_engine::ListeningAsr listening(asr,vad,config.vad);
        auto init_end=Clock::now();
        json metadata={{"schema_version",1},{"run_id",o.id},{"mode","asr_probe"},{"backend",{{"asr","RKNN NPU encoder/decoder/joiner"},{"vad","sherpa-onnx CPU"}}},{"config_sha256",voice_engine::file_sha256(config.source_path)},{"models_manifest_sha256",voice_engine::file_sha256(manifest_path)},{"chunk_ms",o.chunk},{"repeat",o.repeat},{"assets",{{"models",assets.models},{"dependencies",assets.dependencies},{"files",assets.files}}},{"initialization_sec",seconds(init_start,init_end)},{"runtime",json::parse(asr.runtime_info_json())},{"vad_runtime",json::parse(vad.runtime_info_json())},{"loaded_libraries",loaded_libraries},{"model_runtime_versions",{{"asr",asr_model.at("runtime_version")},{"vad",vad_model.at("runtime_version")}}},{"memory_before",before},{"memory_after_init",memory()},{"vad_config",{{"pre_roll_ms",config.vad.pre_roll_ms},{"min_speech_ms",config.vad.min_speech_ms},{"end_silence_ms",config.vad.end_silence_ms},{"start_timeout_ms",config.vad.start_timeout_ms},{"max_speech_ms",config.vad.max_speech_ms}}},{"pcm_policy","incremental original PCM to ASR and VAD; publish empty when VAD finds no speech; pre-roll metadata only, no VAD trimming"},{"normalization","fixture normalized_reference; hypothesis ASCII case fold, fullwidth ASCII conversion, CJK/ASCII punctuation and whitespace removal"},{"acceptance_complete",false}};
        metadata["mode"]=o.listening?"single_turn_listening":"whole_file";
        metadata["pre_roll_ms"]=o.pre_roll_ms;
        if(o.listening)metadata["pcm_policy"]="one turn: actual prefix PCM once; incremental ASR and VAD; first VAD/timeout/max endpoint stops consumption; snapshots final then resets; EOF flush preserves valid tail";
        auto deps_path=fs::path(config.assets.deps_manifest);
        if(deps_path.is_relative())deps_path=config.source_path.parent_path()/deps_path;
        metadata["deps_manifest_sha256"]=voice_engine::file_sha256(deps_path);
        if(!o.official)metadata["fixtures_manifest_sha256"]=voice_engine::file_sha256(o.manifest);
        metadata["source_sha256"]=json::object();
        for(auto p:{"tools/asr_probe.cpp","src/asr/zipformer.cpp","src/asr/silero_vad.cpp","include/voice_engine/asr.hpp","include/voice_engine/vad.hpp","src/asr/text_metrics.hpp","src/asr/listening_asr.cpp","src/asr/listening_backend.cpp"})metadata["source_sha256"][p]=voice_engine::file_sha256(p);
        save(run/"metadata.json",metadata);
        // Exercise production cancellation once, outside all measured sample runs.
        // A full encoder block dirties real caches; cancel then begin/empty-finish
        // must clear them before the subsequent normal cases use the same handles.
        if(o.listening){
            json cancel_check={{"executed",false}};
            for(const auto& sample:selected){
                if(sample.at("num_samples").get<std::uint64_t>()<19200)continue;
                SF_INFO ci{};
                std::unique_ptr<SNDFILE,decltype(&sf_close)> cw(
                    sf_open(sample.at("resolved_path").get<std::string>().c_str(),SFM_READ,&ci),sf_close);
                if(!cw)throw std::runtime_error("cancel-check WAV open failed");
                std::vector<float> cp(19200);
                if(sf_readf_float(cw.get(),cp.data(),cp.size())!=19200)throw std::runtime_error("cancel-check WAV incomplete");
                listening.begin();
                for(std::size_t pos=0;pos<cp.size()&&listening.state()==voice_engine::ListeningState::listening;){
                    const auto take=std::min<std::size_t>(320,cp.size()-pos);
                    pos+=listening.accept(cp.data()+pos,take);
                }
                const auto cancelled_chunks=asr.stats().encoder_chunks;
                const auto cancel_start=Clock::now();
                listening.cancel();
                const auto cancel_end=Clock::now();
                const bool cleared=asr.partial().empty()&&asr.stats().input_samples==0&&
                    vad.stats().input_samples==0&&vad.segments().empty()&&listening.buffered_samples()==0;
                listening.begin();
                const auto empty=listening.finish();
                const bool isolated=empty.hypothesis.empty()&&empty.asr.input_samples==0&&empty.backends_reset;
                cancel_check={{"executed",true},{"sample_id",sample.at("sample_id")},
                    {"encoder_chunks_before_cancel",cancelled_chunks},{"cancel_reset_sec",seconds(cancel_start,cancel_end)},
                    {"cleared",cleared},{"new_empty_round_isolated",isolated},
                    {"scope","synchronous worker boundary cancellation; not preemption of an in-flight SDK call"}};
                if(!cleared||!isolated)throw std::runtime_error("LISTENING cancellation/reset failed");
                break;
            }
            save(run/"cancel-check.json",cancel_check);
        }
        std::ofstream rows(run/"results.jsonl");
        if(!rows)throw std::runtime_error("cannot open results");
        json all=json::array(),batches=json::array();
        std::map<std::string,std::string> first;
        std::vector<double> warm_rtf;
        bool failures=false,quality_failure=false;
        for(int rep=0;rep<o.repeat;++rep){std::size_t main_count=0,keyword_pass=0,cer_errors=0,cer_chars=0;
        bool batch_reset=true;
            for(auto& sample:selected){
                auto id=sample.at("sample_id").get<std::string>();
                json row={{"sample_id",id},{"repeat",rep+1},{"chunk_ms",o.chunk},{"reference",sample.at("reference_text")},{"normalized_reference",sample.at("normalized_reference")},{"category",sample.at("category")},{"expected_behavior",sample.at("expected_behavior")},{"input_sha256",sample.at("sha256")}};
                try{
                    asr.reset();
                    vad.reset();
                    bool reset_ok=asr.partial().empty()&&asr.stats().input_samples==0&&asr.stats().encoder_chunks==0&&vad.segments().empty()&&!vad.stats().speech_detected;
                    SF_INFO info{};
                    std::unique_ptr<SNDFILE,decltype(&sf_close)> wav(sf_open(sample.at("resolved_path").get<std::string>().c_str(),SFM_READ,&info),sf_close);
                    if(!wav)throw std::runtime_error(std::string("WAV open failed: ")+sf_strerror(nullptr));
                    if(info.samplerate!=16000||info.channels!=1||(info.format&SF_FORMAT_SUBMASK)!=SF_FORMAT_PCM_16||(info.format&SF_FORMAT_TYPEMASK)!=SF_FORMAT_WAV)throw std::runtime_error("WAV must be PCM16 mono 16 kHz RIFF");
                    if(info.frames!=sample.at("num_samples").get<sf_count_t>()||info.frames>120*16000||info.frames<0)throw std::runtime_error("WAV frame count mismatch or bound exceeded");
                    std::vector<float> pcm(static_cast<std::size_t>(o.chunk)*16);
                    auto start=Clock::now();
                    sf_count_t read=0,total=0;
                    std::uint64_t first_chunk_samples=0;
                    auto last_guard=start;
                    if(o.listening){
                        const auto prefix_count=std::min<sf_count_t>(info.frames,static_cast<sf_count_t>(o.pre_roll_ms)*16);
                        std::vector<float> prefix(static_cast<std::size_t>(prefix_count));
                        if(prefix_count&&sf_readf_float(wav.get(),prefix.data(),prefix_count)!=prefix_count)
                            throw std::runtime_error("pre-roll WAV read incomplete");
                        listening.begin(prefix.data(),prefix.size());
                        total=prefix_count;
                    }
                    while((!o.listening||listening.state()==voice_engine::ListeningState::listening)&&
                          (read=sf_readf_float(wav.get(),pcm.data(),pcm.size()))>0){
                        if(o.listening)total+=static_cast<sf_count_t>(listening.accept(pcm.data(),read));
                        else {asr.accept(pcm.data(),read);vad.accept(pcm.data(),read);total+=read;}
                        const auto streamed_chunks=o.listening&&listening.state()==voice_engine::ListeningState::final?
                            listening.result().chunks_before_finish:asr.stats().encoder_chunks;
                        if(!first_chunk_samples&&streamed_chunks)first_chunk_samples=total;
                        auto now=Clock::now();
                        if(seconds(last_guard,now)>1){
                            if(memory().at("MemAvailable_kib").get<long>()<1536*1024)
                                throw std::runtime_error("MemAvailable below 1.5 GiB during inference");
                            last_guard=now;
                        }
                    }
                    if((!o.listening&&total!=info.frames)||sf_error(wav.get())!=SF_ERR_NO_ERROR)
                        throw std::runtime_error("WAV read incomplete");
                    std::uint64_t chunks_before=asr.stats().encoder_chunks;
                    const auto endpoint=Clock::now();
                    std::string raw,hypothesis;
                    voice_engine::AsrStats stat;
                    voice_engine::VadStats vs;
                    std::vector<voice_engine::VadSegment> segments;
                    bool stable_finish=false;
                    double endpoint_to_final=0;
                    if(o.listening){
                        const auto& result=listening.finish();
                        raw=result.unfiltered_hypothesis;hypothesis=result.hypothesis;
                        stat=result.asr;vs=result.vad;segments=result.segments;
                        chunks_before=result.chunks_before_finish;
                        endpoint_to_final=result.endpoint_to_final_sec;
                        stable_finish=listening.finish().hypothesis==hypothesis&&
                            listening.finish().asr.encoder_chunks==stat.encoder_chunks;
                        reset_ok=reset_ok&&result.backends_reset;
                        row["listening"]={{"reason",voice_engine::listening_end_name(result.reason)},
                            {"pre_roll_samples",result.pre_roll_samples},{"fresh_input_samples",result.input_samples},
                            {"endpoint_sample",result.endpoint_sample},{"asr_finish_called",result.asr_finish_called},
                            {"backends_reset",result.backends_reset},{"unconsumed_wav_samples",info.frames-total},
                            {"timing_scope","VAD confirmation or manual finish call to final/reset; file-driven, no acoustic time"}};
                    }else{
                        vad.finish();raw=asr.finish();vs=vad.stats();stat=asr.stats();
                        hypothesis=vs.speech_detected?raw:std::string();
                        segments=vad.segments();stable_finish=asr.finish()==raw;
                        endpoint_to_final=seconds(endpoint,Clock::now());
                    }
                    const auto end=Clock::now();
                    auto normalized=voice_engine::asr_metrics::normalize(hypothesis);
                    auto reference=sample.at("normalized_reference").get<std::string>();
                    auto chars=voice_engine::asr_metrics::codepoints(reference).size();
                    auto errors=voice_engine::asr_metrics::edit_distance(reference,normalized);
                    json hits=json::array();
                    bool keywords=true;
                    for(auto& k:sample.at("keywords")){bool hit=normalized.find(voice_engine::asr_metrics::normalize(k.get<std::string>()))!=std::string::npos;
                    keywords=keywords&&hit;
                    hits.push_back({{"keyword",k},{"hit",hit}});
                    }
                    const double duration=info.frames/16000.0,elapsed=seconds(start,end);
                    bool same=!first.count(id)||first[id]==hypothesis;
                    if(!first.count(id))first[id]=hypothesis;
                    auto category=sample.at("category").get<std::string>();
                    bool main=category=="chinese_common"||category=="numbers_dates"||category=="short_question"||category=="code_switch";
                    bool chinese=main&&category!="code_switch";
                    if(main){++main_count;
                    if(keywords)++keyword_pass;
                    }if(chinese){cer_chars+=chars;
                    cer_errors+=errors;
                    }
                    json spans=json::array();
                    for(auto span:segments)spans.push_back({{"start_sample",span.start_sample},{"end_sample",span.end_sample},{"pre_roll_start_sample",span.pre_roll_start_sample}});
                    row.update({{"hypothesis",hypothesis},{"asr_unfiltered_hypothesis",raw},{"normalized_hypothesis",normalized},{"exact_match",normalized==reference},{"keyword_results",hits},{"all_keywords_match",keywords},{"cer_eligible",chinese},{"character_errors",errors},{"reference_characters",chars},{"cer",chars?json(static_cast<double>(errors)/chars):json(nullptr)},{"elapsed_sec",elapsed},{"endpoint_to_final_sec",endpoint_to_final},{"input_samples",stat.input_samples},{"consumed_audio_sec",stat.input_samples/16000.0},{"rtf_consumed_audio",stat.input_samples?json(elapsed/(stat.input_samples/16000.0)):json(nullptr)},{"audio_sec",duration},{"rtf_original_wav",duration?json(elapsed/duration):json(nullptr)},{"encoder_chunks",stat.encoder_chunks},{"chunks_before_finish",chunks_before},{"first_encoder_chunk_input_samples",first_chunk_samples},{"tail_padding_samples",stat.tail_padding_samples},{"peak_buffered_feature_frames",stat.peak_feature_frames},{"vad",{{"speech_detected",vs.speech_detected},{"start_timed_out",vs.start_timed_out},{"input_samples",vs.input_samples},{"windows",vs.windows},{"tail_valid_samples",vs.tail_valid_samples},{"max_duration_endpoints",vs.max_duration_endpoints},{"max_duration_start_sample",vs.max_duration_start_sample},{"segments",spans}}},{"result",hypothesis.empty()?"empty":"text"},{"repeat_consistent",same},{"finish_idempotent",stable_finish},{"memory",memory()}});
                    // A genuinely empty new stream after nonempty inference tests both
                    // model-state reset and output isolation without another model load.
                    if(o.listening){
                        listening.begin();
                        const auto empty=listening.finish();
                        reset_ok=reset_ok&&empty.hypothesis.empty()&&empty.asr.encoder_chunks==0&&
                            !empty.vad.speech_detected&&empty.segments.empty()&&empty.backends_reset;
                    }else{
                        asr.reset();vad.reset();asr.finish();vad.finish();
                        reset_ok=reset_ok&&asr.partial().empty()&&asr.stats().encoder_chunks==0&&!vad.stats().speech_detected&&vad.segments().empty();
                    }
                    row["reset_empty_stream_ok"]=reset_ok;
                    batch_reset=batch_reset&&reset_ok&&same&&stable_finish;
                    if(rep>0&&duration>0&&vs.speech_detected&&(main||o.official))warm_rtf.push_back(elapsed/(o.listening?stat.input_samples/16000.0:duration));
                    auto expected=sample.at("expected_behavior").get<std::string>();
                    bool behavior=false;
                    std::string behavior_check;
                    if(expected=="empty"||expected=="silence"||expected=="noise"){
                        behavior=hypothesis.empty()&&!vs.speech_detected;
                        behavior_check="no_speech_and_empty_final";
                    }else if(expected=="short_audio"){
                        behavior=stat.input_samples==static_cast<std::uint64_t>(info.frames);
                        behavior_check="completed_without_crash_or_hang; text_quality_not_evaluated";
                    }else if(expected=="speech"){
                        behavior=vs.speech_detected&&!hypothesis.empty();
                        behavior_check="speech_detected_and_nonempty_final";
                    }else if(expected=="tail_speech"){
                        behavior=vs.speech_detected&&!hypothesis.empty()&&keywords&&normalized==reference;
                        behavior_check="EOF_flush_full_reference_and_keywords";
                    }else if(expected=="long_audio"){
                        behavior=vs.speech_detected&&!hypothesis.empty()&&keywords&&
                            (o.listening?stat.input_samples==static_cast<std::uint64_t>(total):
                             stat.input_samples==static_cast<std::uint64_t>(info.frames));
                        behavior_check=o.listening?"single_turn_consumed_prefix_and_speech_and_keywords; full_file_long_behavior_not_applicable":
                            "complete_input_and_speech_and_keywords";
                    }else throw ConfigError("unknown expected_behavior: "+expected);
                    row["behavior_check"]=behavior_check;
                    row["expected_behavior_ok"]=behavior;
                    row["failure"]=nullptr;
                    if(!behavior||!reset_ok||!same||!stable_finish)quality_failure=true;
                    if(o.official&&hypothesis!=sample.at("reference_text").get<std::string>())quality_failure=true;
                }catch(const std::exception& e){row["failure"]=e.what();
                failures=true;
                }
                rows<<row.dump()<<'\n';
                rows.flush();
                if(!rows)throw std::runtime_error("results write failed");
                all.push_back(row);
                std::cout<<id<<" repeat="<<rep+1<<" "<<(row.contains("hypothesis")?row["hypothesis"].get<std::string>():"FAILED")<<'\n';
            }
            if(main_count==20&&(keyword_pass<18||(cer_chars&&static_cast<double>(cer_errors)/cer_chars>.15)))quality_failure=true;
            batches.push_back({{"repeat",rep+1},{"main_samples",main_count},{"all_keyword_match_samples",keyword_pass},{"keyword_target_18_of_20_met",main_count==20?json(keyword_pass>=18):json(nullptr)},{"chinese_character_errors",cer_errors},{"chinese_reference_characters",cer_chars},{"chinese_cer",cer_chars?json(static_cast<double>(cer_errors)/cer_chars):json(nullptr)},{"reset_and_repeat_consistent",batch_reset}});
        }
        if(!warm_rtf.empty()&&percentile(warm_rtf,.95)>=1.0)quality_failure=true;
        json summary={{"schema_version",1},{"run_id",o.id},{"rows",all.size()},{"batches",batches},{"runtime_failure",failures},{"quality_failure",quality_failure},{"warm_rtf_count",warm_rtf.size()},{"performance_gate_applicable",!warm_rtf.empty()},{"warm_rtf_p95_below_1",warm_rtf.empty()?json(nullptr):json(percentile(warm_rtf,.95)<1.0)},{"warm_rtf_p95",warm_rtf.empty()?json(nullptr):json(percentile(warm_rtf,.95))},{"memory_end",memory()},{"elapsed_after_initialization_sec",seconds(init_end,Clock::now())},{"elapsed_including_initialization_sec",seconds(begin,Clock::now())},{"A03_acceptance_complete",false},{"limitations",json::array({"File diagnostic only; no microphone, Bluetooth, LLM or engine state-machine integration", "Synthetic fixtures do not establish natural/noisy speech quality", "Endpoint timing is EOF-to-final wall time; not acoustic last-speech timing", "Warm statistics exclude repeat 1; library initialization measured separately", "RUSAGE_SELF peak RSS does not measure all NPU/system memory"})}};
        summary["mode"]=o.listening?"single_turn_listening":"whole_file";
        summary["warm_rtf_basis"]=o.listening?"consumed original PCM including pre-roll":"complete original WAV";
        if(o.listening)summary["limitations"][2]="Endpoint timing is VAD confirmation or manual finish call to final/reset; file-driven, not acoustic time";
        save(run/"summary.json",summary);
        std::cout<<"ASR_PROBE_DONE "<<run<<'\n';
        return failures?4:(quality_failure?6:0);
    }catch(const ConfigError& e){std::cerr<<"ASR_PROBE_CONFIG_ERROR "<<e.what()<<'\n';
    return 2;
    }catch(const json::exception& e){std::cerr<<"ASR_PROBE_CONFIG_ERROR "<<e.what()<<'\n';
    return 2;
    }catch(const std::exception& e){std::cerr<<"ASR_PROBE_ERROR "<<e.what()<<'\n';
    if(!run.empty()&&fs::exists(run))try{save(run/"failure.json",{{"error",e.what()},{"memory",memory()}});
    }catch(...){}return 4;
    }
}
