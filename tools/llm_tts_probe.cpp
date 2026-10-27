#include "voice_engine/assets.hpp"
#include "voice_engine/config.hpp"
#include "voice_engine/llm.hpp"
#include "voice_engine/probe_support.hpp"
#include "voice_engine/tts_stream.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sndfile.h>
#include <sstream>
#include <thread>
namespace {
namespace fs=std::filesystem;
using namespace voice_engine;
using J=nlohmann::json;
using Clock=std::chrono::steady_clock;
std::int64_t ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(
    Clock::now().time_since_epoch()).count();}
struct Options{
    std::string config,prompt,replay,output,delivery="stream";
    int cancel_ms=0;
    bool validate=false;
};
const char* help=
"llm_tts_probe --config PATH --output NEW_DIRECTORY\n"
"  (--prompt TEXT | --replay packets.jsonl) [--delivery stream|batch]\n"
"  [--cancel-ms 1..30000] [--validate-input-only]\n"
"Local LLM -> bounded TTS worker -> 16 kHz WAV. Never opens audio devices.\n"
"Replay compares the same token timeline, with TTS only; not four-model coexistence.\n"
"Run under flock -x /tmp/rk3588_voice_engine_model_validation.lock and timeout 180.\n"
"Exit 0 local checks pass, 2 input, 4 model/stream failure, 5 resource/deadline.\n"
"Speech content and A05/A06/A08 require separate acceptance.\n";
Options parse(int argc,char**argv){
    Options o;std::set<std::string> seen;
    for(int i=1;i<argc;++i){
        std::string key=argv[i];
        if(!seen.insert(key).second)throw std::invalid_argument("duplicate argument");
        if(key=="--validate-input-only"){o.validate=true;continue;}
        if(i+1>=argc)throw std::invalid_argument("missing value");
        const std::string value=argv[++i];
        if(key=="--config")o.config=value;
        else if(key=="--prompt")o.prompt=value;
        else if(key=="--replay")o.replay=value;
        else if(key=="--output")o.output=value;
        else if(key=="--delivery")o.delivery=value;
        else if(key=="--cancel-ms"){
            if(value.empty()||value.find_first_not_of("0123456789")!=std::string::npos)
                throw std::invalid_argument("invalid cancel time");
            const auto n=std::stoll(value);
            if(n<1||n>30000)throw std::invalid_argument("cancel time out of range");
            o.cancel_ms=static_cast<int>(n);
        }else throw std::invalid_argument("unknown argument");
    }
    if(o.prompt.empty()==o.replay.empty() || (o.delivery!="stream"&&o.delivery!="batch") ||
       (!o.validate&&(o.config.empty()||o.output.empty())))
        throw std::invalid_argument("config/output and exactly one prompt or replay required");
    if(o.prompt.size()>1024 || (!o.prompt.empty()&&(!llm_valid_utf8(o.prompt)||
       o.prompt.find('<')!=std::string::npos || o.prompt.find('\0')!=std::string::npos)))
        throw std::invalid_argument("invalid plain prompt");
    return o;
}
std::vector<J> read_replay(const std::string& path){
    if(path.empty())return {};
    if(fs::file_size(path)>1024*1024)throw std::invalid_argument("replay exceeds 1 MiB");
    std::ifstream f(path);std::vector<J> rows;std::string line;std::int64_t last=-1;std::size_t total=0;
    while(std::getline(f,line)){
        auto j=J::parse(line);
        if(!j.at("offset_ns").is_number_integer()||!j.at("text").is_string())
            throw std::invalid_argument("invalid replay row");
        const auto t=j["offset_ns"].get<std::int64_t>();const auto text=j["text"].get<std::string>();
        total+=text.size();
        if(t<last||t<0||t>60000000000LL||!llm_valid_utf8(text)||total>8192||rows.size()>=4096)
            throw std::invalid_argument("invalid replay timing/text/bounds");
        last=t;rows.push_back(j);
    }
    if(rows.empty()||total==0)throw std::invalid_argument("empty replay");
    return rows;
}
void json(const fs::path&p,const J&j){
    std::ofstream f(p);f<<j.dump(2)<<'\n';if(!f)throw std::runtime_error("write failed");
}
long mem(const char*file,const char*key){
    std::ifstream f(file);std::string line;
    while(std::getline(f,line))if(line.rfind(key,0)==0){
        std::istringstream s(line.substr(std::string(key).size()));long v=0;s>>v;return v;
    }
    return 0;
}
struct Monitor{
    std::atomic<bool> stop{false};std::thread thread;
    explicit Monitor(fs::path root){
        if(mem("/proc/meminfo","MemAvailable:")<1572864)
            throw std::runtime_error("memory below 1.5 GiB floor");
        thread=std::thread([this,root]{
            std::ofstream log(root/"resources.jsonl");int low=0;auto start=Clock::now();
            while(!stop){
                auto available=mem("/proc/meminfo","MemAvailable:");
                low=available<1572864?low+1:0;
                log<<J{{"monotonic_ns",ns()},{"mem_available_kib",available},
                    {"rss_kib",mem("/proc/self/status","VmRSS:")}}.dump()<<'\n';log.flush();
                if(!log||low>=30||Clock::now()-start>std::chrono::seconds(175)){
                    json(root/"guard-failure.json",J{{"status","failed"},{"low_memory",low>=30},
                        {"reason","memory, monitor I/O, or process deadline"}});
                    std::_Exit(5);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    ~Monitor(){stop=true;if(thread.joinable())thread.join();}
};
J stats(const TtsStreamStats&r){
    return J{{"status",r.status},{"error",r.error},{"input_bytes",r.input_bytes},
      {"segments",r.segments},{"frames",r.frames},{"consumed_frames",r.consumed_frames},
      {"peak_input_bytes",r.peak_input_bytes},{"peak_pending_segments",r.peak_segments},
      {"peak_pcm_queue_frames",r.peak_pcm_frames},{"discarded_generation_results",r.discarded_generation_results},
      {"begin_ns",r.begin_ns},{"first_segment_ns",r.first_segment_ns},{"first_pcm_ns",r.first_pcm_ns},
      {"cancel_ns",r.cancel_ns},{"producer_done_ns",r.producer_done_ns},
      {"first_generated_audio_ns",r.first_generated_audio_ns},{"playback_release_ns",r.playback_release_ns},
      {"peak_native_slots",r.peak_native_slots},{"peak_native_samples",r.peak_native_samples},
      {"generate_s",r.generate_seconds}};
}
void wav(const fs::path&p,const std::vector<float>&samples){
    SF_INFO info{};info.samplerate=16000;info.channels=1;info.format=SF_FORMAT_WAV|SF_FORMAT_PCM_16;
    SNDFILE* f=sf_open(p.c_str(),SFM_WRITE,&info);
    if(!f)throw std::runtime_error("cannot create WAV");
    const auto n=sf_write_float(f,samples.data(),static_cast<sf_count_t>(samples.size()));
    const int rc=sf_close(f);
    if(n!=static_cast<sf_count_t>(samples.size())||rc)throw std::runtime_error("WAV write failed");
}
}
int main(int argc,char**argv){
    fs::path root;
    try{
        if(argc==2&&std::string(argv[1])=="--help"){std::cout<<help;return 0;}
        auto o=parse(argc,argv);auto replay=read_replay(o.replay);
        if(o.validate){std::cout<<"LLM_TTS_INPUT_OK\n";return 0;}
        auto config=load_config(o.config);
        probe::require_model_validation_lock();
        const auto requested=fs::absolute(o.output);
        if(fs::exists(requested)||!fs::create_directories(requested))
            throw std::invalid_argument("output must be new");
        root=fs::canonical(requested);
        Monitor monitor(root);
        J record{{"schema_version",1},{"scope","local file experiment; no devices or A06/A08 certification"},
          {"delivery",o.delivery},{"config_sha256",file_sha256(o.config)},
          {"executable_sha256",file_sha256(fs::canonical("/proc/self/exe"))},
          {"source","real LLM stream"},{"cancel_ms",o.cancel_ms},
          {"tts_threads",config.threads.tts},{"tts_min_chars",config.tts.min_chars},
          {"tts_max_chars",config.tts.max_chars},{"tts_flush_ms",config.tts.flush_ms},
          {"tts_prebuffer_ms",config.tts.prebuffer_ms},
          {"tts_target_chars",config.tts.target_chars},{"tts_speed",config.tts.speed},{"tts_sentence_mode",config.tts.sentence_mode},{"tts_join_short_sentences",config.tts.join_short_sentences},
          {"tts_boundary_lookahead_chars",config.tts.boundary_lookahead_chars}};
        record["tts_first_segment"]=nullptr;
        if(config.tts.first_segment) {
            const auto& first=*config.tts.first_segment;
            record["tts_first_segment"]={{"min_chars",first.min_chars},{"target_chars",first.target_chars},
                                        {"boundary_lookahead_chars",first.boundary_lookahead_chars}};
        }
        record["tts_frontend_policy"]=config.tts.join_short_sentences ?
            "melo-bounded-sentence-context-v2" : "melo-fullwidth-comma-context-v1";
        record["tts_max_num_sentences"]=1;
        if(!o.replay.empty())record["source"]="frozen token-time replay, TTS only";
        record["input_sha256"]=o.replay.empty()?"":file_sha256(o.replay);
        record["prompt"]=o.prompt;
        json(root/"run.json",record);
        MeloTts model(config,config.threads.tts);
        std::unique_ptr<LlmAdapter> llm;
        if(o.replay.empty()){
            auto options=llm_options_from_config(config);
            auto assets=resolve_rkllm_assets(config);
            llm=std::make_unique<LlmAdapter>(make_rkllm_backend(assets,options),options);
            record["llm_initialization_ms"]=llm->initialization_ms();
        }
        record["tts_initialization_s"]=model.model_initialization_seconds();
        std::vector<J> generated;std::mutex generated_mutex;
        TtsStreamWorker worker([&](const std::string&s){
            const auto begin=ns();auto a=model.generate(s);
            std::lock_guard<std::mutex> l(generated_mutex);
            generated.push_back(J{{"text",s},{"frontend_text",melo_frontend_text(s,config.tts.join_short_sentences)},
                                 {"begin_ns",begin},{"end_ns",ns()},
                                 {"native_samples",a.samples.size()}});
            return a;
        },config.tts,config.queues,config.timeouts.tts_segment_ms);
        const auto begin=ns();
        LlmTurn turn;
        TtsStreamId id{1,1};
        if(llm){turn=llm->start(o.prompt);id={turn.session_id,turn.generation_id};}
        worker.begin(id);
        std::ofstream tokens(root/"packets.jsonl");
        std::vector<float> audio;audio.reserve(16000*120);
        std::string accumulated;std::size_t index=0;
        bool input_end=false,cancelled=false;
        std::int64_t end_input=0,first_text=0,cancel_request=0,first_polled_pcm=0;
        J llm_result;
        const auto deliver=[&](const std::string&text,std::int64_t offset){
            tokens<<J{{"offset_ns",offset},{"text",text}}.dump()<<'\n';tokens.flush();
            if(!tokens)throw std::runtime_error("packet log write failed");
            if(text.empty())return;
            if(!first_text)first_text=ns();
            if(accumulated.size()+text.size()>8192)throw std::runtime_error("total text bound exceeded");
            accumulated+=text;
            if(o.delivery=="stream"&&!worker.submit(id,text))
                throw std::runtime_error("TTS rejected streamed text");
        };
        const auto consume=[&](const TtsStreamFrame& f){
            if(cancelled)throw std::runtime_error("PCM observed after cancellation");
            if(!first_polled_pcm)first_polled_pcm=ns();
            if(audio.size()+f.audio.valid_samples>16000*120)
                throw std::runtime_error("diagnostic WAV exceeds 120 seconds");
            if(!worker.commit_if_current(id,[&]{
                audio.insert(audio.end(),f.audio.samples.begin(),
                             f.audio.samples.begin()+f.audio.valid_samples);
            }))throw std::runtime_error("stale PCM commit");
        };
        while(true){
            if(llm&&llm->restart_required()){
                const auto failure=llm->snapshot(turn);
                json(root/"llm-restart-required.json",J{{"status",failure.status},
                    {"restart_required",true},{"cancel_requested_ns",failure.cancel_requested_ns},
                    {"idle_ns",failure.idle_ns}});
                worker.cancel(id);
                throw std::runtime_error("LLM handle requires process restart; model idle not confirmed");
            }
            if(!cancelled&&o.cancel_ms&&ns()-begin>=static_cast<std::int64_t>(o.cancel_ms)*1000000){
                cancel_request=ns();cancelled=true;worker.cancel(id);if(llm)llm->cancel();
            }
            if(!input_end&&!cancelled){
                if(llm){
                    auto p=llm->poll(turn);
                    // Re-read after observing idle: the last callback may have
                    // arrived between the first empty poll and that observation.
                    const bool observed_idle=llm->idle();
                    if(!p&&observed_idle)p=llm->poll(turn);
                    if(p){
                        bool accepted=llm->commit_if_current(turn,[&]{
                            if(o.delivery=="stream"&&!p->text.empty()&&!worker.submit(id,p->text))
                                throw std::runtime_error("TTS rejected LLM text");
                        });
                        if(!accepted)throw std::runtime_error("LLM generation invalidated");
                        tokens<<J{{"offset_ns",ns()-begin},{"text",p->text},
                            {"callback_ns",p->monotonic_ns},{"token_id",p->token_id}}.dump()<<'\n';
                        if(!tokens)throw std::runtime_error("packet log write failed");
                        if(!p->text.empty()){
                            if(!first_text)first_text=ns();
                            if(accumulated.size()+p->text.size()>8192)throw std::runtime_error("text overflow");
                            accumulated+=p->text;
                        }
                    }else if(observed_idle){
                        auto r=llm->wait(turn,1);
                        if(!r)throw std::runtime_error("LLM idle without result");
                        llm_result={{"status",r->status},{"text",r->text},{"first_token_ns",r->first_token_ns},
                          {"first_text_ns",r->first_text_ns},{"submit_ns",r->submit_ns},
                          {"finish_ns",r->finish_ns},{"run_return_ns",r->run_return_ns}};
                        if(r->status!="completed")throw std::runtime_error("LLM failed: "+r->status);
                        if(accumulated!=r->text)throw std::runtime_error("LLM packet/result text mismatch");
                        input_end=true;
                    }
                }else{
                    while(index<replay.size()&&replay[index]["offset_ns"].get<std::int64_t>()<=ns()-begin){
                        deliver(replay[index]["text"].get<std::string>(),replay[index]["offset_ns"].get<std::int64_t>());
                        ++index;
                    }
                    input_end=index==replay.size();
                }
                if(input_end){
                    end_input=ns();
                    if(o.delivery=="batch"&&!worker.submit(id,accumulated))
                        throw std::runtime_error("TTS rejected batch text");
                    if(!worker.finish(id))throw std::runtime_error("TTS finish rejected");
                }
            }
            while(auto f=worker.poll(id))consume(*f);
            if(worker.wait_idle(0)){
                // Once idle is observed, no producer can add more frames.
                while(auto f=worker.poll(id))consume(*f);
                auto s=worker.stats();
                if(s.status=="failed")throw std::runtime_error(s.error);
                if((input_end||cancelled)&&(!llm||llm->idle()))break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(cancelled&&llm){
            auto r=llm->wait(turn,2000);
            if(r)llm_result={{"status",r->status},{"cancel_requested_ns",r->cancel_requested_ns},
                            {"idle_ns",r->idle_ns}};
        }
        const auto end=ns();
        tokens.close();
        if(!cancelled&&audio.empty())throw std::runtime_error("completed without PCM");
        record["text"]=accumulated;record["begin_ns"]=begin;record["input_end_ns"]=end_input;
        record["first_speakable_text_ns"]=first_text;record["first_polled_pcm_ns"]=first_polled_pcm;
        record["end_ns"]=end;record["cancel_request_ns"]=cancel_request;
        record["cancelled"]=cancelled;record["tts"]=stats(worker.stats());record["llm"]=llm_result;
        record["segments"]=generated;record["samples"]=audio.size();
        record["first_pcm_s"]=first_polled_pcm?J((first_polled_pcm-begin)/1e9):J();
        record["complete_s"]=(end-begin)/1e9;
        record["cancel_to_idle_ms"]=cancel_request?J((end-cancel_request)/1e6):J();
        record["no_post_cancel_pcm"]=true;
        record["history_committed"]=false;
        if(!audio.empty()){
            wav(root/"output-16k.wav",audio);
            record["wav_sha256"]=file_sha256(root/"output-16k.wav");
        }
        // A file-mode output is successful only after writing and validating it.
        if(llm&&!cancelled)record["history_committed"]=llm->commit_history(turn);
        if(llm&&!llm->close())throw std::runtime_error("LLM did not close; restart required");
        record["packets_sha256"]=file_sha256(root/"packets.jsonl");
        record["status"]=cancelled?"cancelled":"completed";
        json(root/"summary.json",record);std::cout<<record.dump()<<'\n';return 0;
    }catch(const std::exception&e){
        std::cerr<<e.what()<<'\n';
        if(!root.empty()&&fs::exists(root))json(root/"failure.json",J{{"error",e.what()}});
        return dynamic_cast<const std::invalid_argument*>(&e)?2:4;
    }
}
