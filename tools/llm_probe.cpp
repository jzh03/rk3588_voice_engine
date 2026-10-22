#include "voice_engine/llm.hpp"
#include "voice_engine/assets.hpp"
#include "voice_engine/config.hpp"
#include "voice_engine/probe_support.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <numeric>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
using namespace voice_engine;
class ProbeIoError:public std::runtime_error {public:using std::runtime_error::runtime_error;};
class ProbeTimeout:public std::runtime_error {public:using std::runtime_error::runtime_error;};
volatile std::sig_atomic_t interrupted=0;
void signal_handler(int value) { interrupted=value; }
struct Options {
    std::string config,cases,output="output/runs",run_id,mode="cases",only;
    int repeat=3,warmup=1,init_cycles=10;
    bool validate_only=false;
};
constexpr const char* usage=
    "llm_probe --config PATH --cases PATH --run-id ID [--output-dir ROOT]\n"
    "  [--mode cases|lifecycle|boundaries|cancel] [--repeat 1..100]\n"
    "  [--warmup 0..10] [--init-cycles 1..20] [--case-id ID]\n"
    "  [--validate-input-only]\n"
    "Fixed Qwen3.5-2B / RKLLM 1.3.0 selected-R profile; context 2048, default\n"
    "output 128; fixed cases retain their 64-token limits. No audio/network.\n"
    "All model execution: flock -x /tmp/rk3588_voice_engine_model_validation.lock ...\n"
    "Exit: 0 software checks passed (content review separate), 1 I/O/internal,\n"
    "2 config/assets/input, 4 model failure, 5 timeout, 6 acceptance checks failed.\n";
bool safe_id(const std::string& s) {
    return std::regex_match(s,std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,95}"));
}
int number(const std::string& s,int lo,int hi) {
    if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)
        throw std::invalid_argument("invalid integer argument");
    const auto n=std::stoll(s);
    if(n<lo || n>hi) throw std::invalid_argument("integer argument out of range");
    return static_cast<int>(n);
}
Options parse(int argc,char** argv) {
    Options o;std::set<std::string> seen;
    for(int i=1;i<argc;++i) {
        const std::string a=argv[i];
        if(!seen.insert(a).second) throw std::invalid_argument("duplicate argument: "+a);
        if(a=="--validate-input-only") {o.validate_only=true;continue;}
        if(i+1>=argc) throw std::invalid_argument("missing value: "+a);
        const std::string v=argv[++i];
        if(a=="--config") o.config=v;
        else if(a=="--cases") o.cases=v;
        else if(a=="--run-id") o.run_id=v;
        else if(a=="--output-dir") o.output=v;
        else if(a=="--mode") o.mode=v;
        else if(a=="--case-id") o.only=v;
        else if(a=="--repeat") o.repeat=number(v,1,100);
        else if(a=="--warmup") o.warmup=number(v,0,10);
        else if(a=="--init-cycles") o.init_cycles=number(v,1,20);
        else throw std::invalid_argument("unknown argument: "+a);
    }
    if(o.cases.empty() || (!o.validate_only && (o.config.empty() || !safe_id(o.run_id))))
        throw std::invalid_argument("--cases, --config, and safe --run-id are required");
    if(!std::set<std::string>{"cases","lifecycle","boundaries","cancel"}.count(o.mode))
        throw std::invalid_argument("unknown mode");
    if(!o.only.empty() && !safe_id(o.only)) throw std::invalid_argument("invalid case id");
    return o;
}
Json read_cases(const Options& o) {
    if(fs::file_size(o.cases)>1024*1024) throw std::invalid_argument("cases exceeds 1 MiB");
    std::ifstream f(o.cases);auto d=Json::parse(f);
    if(d.at("schema_version")!=1 || !d.at("cases").is_array() ||
       d.at("cases").empty() || d.at("cases").size()>1000)
        throw std::invalid_argument("invalid cases schema");
    Json result=Json::array();std::set<std::string> ids;
    for(const auto& c:d.at("cases")) {
        const auto id=c.at("id").get<std::string>(),text=c.at("text").get<std::string>();
        if(!safe_id(id) || !ids.insert(id).second || text.empty() || text.size()>8192 ||
           text.find('\0')!=std::string::npos || text.find("<|")!=std::string::npos ||
           text.find("<think")!=std::string::npos || text.find("</think")!=std::string::npos ||
           !llm_valid_utf8(text) || !c.at("max_output_tokens").is_number_integer() ||
           c.at("max_output_tokens").get<int>()<1 || c.at("max_output_tokens").get<int>()>128)
            throw std::invalid_argument("invalid case id, text, or output budget");
        if(o.only.empty() || o.only==id) result.push_back(c);
    }
    if(result.empty()) throw std::invalid_argument("case filter matched nothing");
    return result;
}
void write_json(const fs::path& p,const Json& j) {
    std::ofstream f(p);f<<j.dump(2)<<'\n';
    if(!f) throw ProbeIoError("cannot write "+p.string());
}
long memory_value(const std::string& file,const std::string& key) {
    std::ifstream f(file);std::string line;
    while(std::getline(f,line)) if(line.rfind(key,0)==0) {
        std::istringstream s(line.substr(key.size()));long n=0;s>>n;return n;
    }
    return 0;
}
long available() {return memory_value("/proc/meminfo","MemAvailable:");}
void check_memory() {
    if(available()<1572864) throw std::runtime_error("MemAvailable below 1.5 GiB floor");
}
std::string small_file(const fs::path& p) {
    std::ifstream f(p);std::string value;std::getline(f,value);return value;
}
Json libraries() {
    std::ifstream f("/proc/self/maps");std::string line;std::set<std::string> paths;
    while(std::getline(f,line)) {
        auto p=line.find('/');
        if(p!=std::string::npos && line.find(".so",p)!=std::string::npos)
            paths.insert(line.substr(p));
    }
    return paths;
}
struct Monitor {
    std::atomic<bool> stop{false},low{false};
    std::atomic<long> min_available{LONG_MAX},peak_rss{0};
    std::thread worker;
    explicit Monitor(const fs::path& path) {
        worker=std::thread([this,path] {
            std::ofstream out(path);int consecutive=0;
            while(!stop) {
                long m=available(),r=memory_value("/proc/self/status","VmRSS:");
                min_available=std::min(min_available.load(),m);
                peak_rss=std::max(peak_rss.load(),r);
                consecutive=m<1572864?consecutive+1:0;
                if(consecutive>=3) low=true;
                Json j{{"monotonic_ns",std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now().time_since_epoch()).count()},
                    {"mem_available_kib",m},{"rss_kib",r},
                    {"swap_free_kib",memory_value("/proc/meminfo","SwapFree:")},
                    {"cpu0_khz",small_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")},
                    {"cpu4_khz",small_file("/sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq")},
                    {"soc_millideg_c",small_file("/sys/class/thermal/thermal_zone0/temp")}};
                const fs::path devfreq="/sys/class/devfreq";
                if(fs::exists(devfreq)) for(const auto& e:fs::directory_iterator(devfreq))
                    if(e.path().filename().string().find("npu")!=std::string::npos)
                        j["npu_hz"]=small_file(e.path()/"cur_freq");
                out<<j.dump()<<'\n';out.flush();
                for(int i=0;i<20 && !stop;++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    ~Monitor() {stop=true;if(worker.joinable()) worker.join();}
};
Json json_result(const LlmResult& r) {
    Json j{{"session_id",r.session_id},{"generation_id",r.generation_id},{"status",r.status},
        {"text",r.text},{"raw_text",r.raw_text},{"budget_method","conservative_utf8_bytes_including_template_not_exact_tokens"},
        {"prompt_estimated_tokens",r.prompt_estimated_tokens},{"output_budget",r.output_budget},
        {"dropped_history_pairs",r.dropped_history_pairs},{"run_rc",r.run_rc},{"clear_rc",r.clear_rc},
        {"abort_rc",r.abort_rc},{"token_callbacks",r.token_callbacks},{"finish_callbacks",r.finish_callbacks},
        {"callback_errors",r.callback_errors},{"stale_callbacks",r.stale_callbacks},
        {"queue_peak_bytes",r.queue_peak_bytes},{"overflow",r.overflow},{"invalid_utf8",r.invalid_utf8},
        {"restart_required",r.restart_required},{"history_committed",r.history_committed},
        {"control_markers_seen",r.control_markers_seen},{"submit_ns",r.submit_ns},
        {"first_token_ns",r.first_token_ns},{"first_text_ns",r.first_text_ns},
        {"finish_ns",r.finish_ns},{"run_return_ns",r.run_return_ns},
        {"cancel_requested_ns",r.cancel_requested_ns},{"idle_ns",r.idle_ns},
        {"sdk_prefill_tokens",r.perf.prefill_tokens},{"sdk_generate_tokens",r.perf.generate_tokens},
        {"sdk_prefill_ms",r.perf.prefill_ms},{"sdk_generate_ms",r.perf.generate_ms},
        {"sdk_memory_mb",r.perf.memory_mb}};
    if(r.first_token_ns) j["ttft_ms"]=(r.first_token_ns-r.submit_ns)/1e6;
    if(r.first_text_ns) j["first_speakable_text_ms"]=(r.first_text_ns-r.submit_ns)/1e6;
    if(r.idle_ns) j["total_ms"]=(r.idle_ns-r.submit_ns)/1e6;
    if(r.cancel_requested_ns && r.idle_ns) j["cancel_to_idle_ms"]=(r.idle_ns-r.cancel_requested_ns)/1e6;
    if(r.perf.generate_ms>0) j["sdk_tokens_per_second"]=r.perf.generate_tokens*1000/r.perf.generate_ms;
    return j;
}
bool normal(const LlmResult& r) {
    return r.status=="completed" && r.run_rc==0 && r.clear_rc==0 &&
        r.finish_callbacks==1 && r.callback_errors==0 && !r.invalid_utf8 &&
        !r.overflow && !r.restart_required && !r.text.empty() &&
        r.perf.generate_tokens<=r.output_budget && r.token_callbacks<=r.output_budget &&
        r.perf.prefill_tokens+r.output_budget+64<=2048;
}
struct Runner {
    const Options& o;const fs::path run;Monitor& monitor;const int deadline_ms;
    std::ofstream tokens,metrics;
    Json results=Json::array(),checks=Json::array();
    bool failed=false;
    Runner(const Options& x,const fs::path& path,Monitor& m,int deadline)
        :o(x),run(path),monitor(m),deadline_ms(deadline),tokens(path/"tokens.jsonl"),metrics(path/"metrics.jsonl") {}
    void check(const std::string& name,bool ok,Json details=Json::object()) {
        checks.push_back({{"name",name},{"status",ok?"passed":"failed"},{"details",std::move(details)}});
        failed=failed||!ok;
    }
    LlmResult collect(LlmAdapter& a,const LlmTurn& turn,const std::string& id,int batch,
                      const std::string& phase,int cancel_after=0,int cancel_ms=0,
                      bool immediate_session=false) {
        const auto start=Clock::now();bool cancelled=false;
        int last_token_id=-1,observed_tokens=0,delivered_packets=0;std::string delivered_text;
        const auto decorate=[&](Json& j,const LlmResult& value) {
            const auto termination=llm_observed_termination(value,observed_tokens,last_token_id);
            j["last_token_id"]=last_token_id;
            j["observed_token_callbacks"]=observed_tokens;
            j["delivered_packets"]=delivered_packets;
            j["termination_reason"]=termination.reason;
            j["termination_basis"]=termination.basis;
            j["observed_output_budget_exceeded"]=termination.observed_budget_exceeded;
            if(value.status=="completed")
                check("observed_output_budget_"+phase+"_"+id+"_batch_"+std::to_string(batch),
                    !termination.observed_budget_exceeded,
                    {{"observed_token_callbacks",observed_tokens},{"output_budget",value.output_budget},
                     {"sdk_generate_tokens",value.perf.generate_tokens}});
        };
        while(true) {
            if(interrupted || monitor.low) a.cancel();
            // Observe idle BEFORE the final empty poll. A fresh idle read after
            // an empty poll can race the last callback and let wait() consume
            // unrecorded tail packets.
            const bool observed_idle=a.idle();
            const auto p=a.poll(turn,2);
            if(p) {
                ++delivered_packets;
                if(p->token_id>=0) {last_token_id=p->token_id;++observed_tokens;}
                delivered_text+=p->text;
                tokens<<Json{{"case_id",id},{"batch",batch},{"phase",phase},
                    {"session_id",p->session_id},{"generation_id",p->generation_id},
                    {"seq",p->seq},{"monotonic_ns",p->monotonic_ns},{"token_id",p->token_id},
                    {"text",p->text}}.dump()<<'\n';
            }
            const auto snap=a.snapshot(turn);
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();
            if(!cancelled && ((cancel_after>0 && snap.token_callbacks>=cancel_after) ||
                              (cancel_ms>0 && elapsed>=cancel_ms))) {
                a.cancel();cancelled=true;if(immediate_session) a.new_session();
            }
            // Do not call wait while tokens remain: it intentionally drains,
            // whereas this probe records every delivered token timestamp.
            if(!p && snap.status!="running" && observed_idle) {
                const auto result=a.wait(turn,0);
                if(result) {
                    if(result->status=="completed" &&
                       (delivered_text!=result->text || observed_tokens!=result->token_callbacks))
                        throw std::runtime_error("normal turn token trace differs from final text or token-ID count");
                    auto j=json_result(*result);decorate(j,*result);j["case_id"]=id;j["batch"]=batch;j["phase"]=phase;
                    metrics<<j.dump()<<'\n';metrics.flush();tokens.flush();
                    if(!metrics || !tokens) throw ProbeIoError("cannot write LLM evidence");
                    results.push_back(j);return *result;
                }
            }
            if(snap.restart_required) {
                const auto r=a.wait(turn,0);
                if(r) {
                    auto j=json_result(*r);decorate(j,*r);j["case_id"]=id;j["batch"]=batch;j["phase"]=phase;
                    metrics<<j.dump()<<'\n';metrics.flush();results.push_back(j);return *r;
                }
            }
            if(elapsed>deadline_ms) {a.cancel();throw ProbeTimeout("adapter supervision exceeded configured deadline plus shutdown grace");}
        }
    }
    LlmResult execute(LlmAdapter& a,const std::string& prompt,const std::string& id,
                      int batch=0,int max_tokens=64,const std::string& phase="case") {
        check_memory();return collect(a,a.start(prompt,max_tokens),id,batch,phase);
    }
};
Json quantiles(std::vector<double> a) {
    if(a.empty()) return {{"n",0}};
    std::sort(a.begin(),a.end());
    return {{"n",a.size()},{"p50",a[static_cast<std::size_t>(std::ceil(.5*a.size()))-1]},
        {"p95",a[static_cast<std::size_t>(std::ceil(.95*a.size()))-1]},{"max",a.back()}};
}
} // namespace
int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--help") {std::cout<<usage;return 0;}
    fs::path run;
    try {
        const auto o=parse(argc,argv);const auto cases=read_cases(o);
        if(o.validate_only) {std::cout<<"LLM_INPUT_OK cases="<<cases.size()<<'\n';return 0;}
        const auto config=load_config(o.config);const auto opts=llm_options_from_config(config);
        probe::require_model_validation_lock();check_memory();
        // Full hashes run once before the measured lifecycle/performance window.
        const auto assets=resolve_rkllm_assets(config);
        run=fs::path(o.output)/o.run_id;
        fs::create_directories(run.parent_path());
        if(!fs::create_directory(run)) throw std::invalid_argument("run directory already exists");
        std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
        Json meta{{"schema_version",1},{"mode",o.mode},{"profile","selected-R_repeat1"},
            {"model_id",assets.model_id},{"model",assets.model.string()},{"runtime",assets.runtime.string()},
            {"template",llm_system_template()},{"sdk_template","empty; application renders exactly once"},
            {"context_tokens",opts.context_tokens},{"default_output_tokens",opts.max_output_tokens},
            {"margin_tokens",opts.margin_tokens},
            {"first_token_timeout_ms",opts.first_token_timeout_ms},{"total_timeout_ms",opts.total_timeout_ms},
            {"cancel_timeout_ms",opts.cancel_timeout_ms},{"queue_bytes",opts.queue_bytes},{"keep_history",0},{"warmup",o.warmup},
            {"repeat",o.repeat},{"init_cycles",o.init_cycles},{"cases_sha256",file_sha256(o.cases)},
            {"config_sha256",file_sha256(o.config)},{"binary_sha256",file_sha256("/proc/self/exe")},
            {"input_budget_method","UTF-8 bytes of full rendered prompt, including literal special-token bytes; conservative admission estimate, not exact tokenization"},
            {"termination_method","status/EOS observation first; token_limit is an inference from observed valid token IDs reaching the requested limit, not an SDK internal stop reason"},
            {"output_count_methods",{{"sdk_generate_tokens","unmodified RKLLM generate-stage performance counter"},
                {"token_callbacks","adapter token callbacks with ID >= 0, including stale IDs; excludes FINISH/error"},
                {"observed_token_callbacks","delivered packets with token ID >= 0; includes empty-text IDs"},
                {"delivered_packets","all delivered packets, including no-ID text flushes; not a token count"}}},
            {"source_hashes",Json::object()},
            {"scope","P2 LLM file/model checks; no PCM/playback or acoustic A08/A10 measurement"}};
        for(const auto* f:{"include/voice_engine/llm.hpp","src/llm/adapter.cpp","src/llm/rkllm.cpp","tools/llm_probe.cpp"})
            meta["source_hashes"][f]=file_sha256(f);
        write_json(run/"run.json",meta);
        Monitor monitor(run/"resources.jsonl");Runner r(o,run,monitor,opts.total_timeout_ms+opts.cancel_timeout_ms+3000);
        std::vector<double> init_ms;
        if(o.mode=="lifecycle") {
            for(int cycle=0;cycle<o.init_cycles;++cycle) {
                check_memory();LlmAdapter a(make_rkllm_backend(assets,opts),opts);
                init_ms.push_back(a.initialization_ms());
                auto value=r.execute(a,"一加一等于多少？","lifecycle_"+std::to_string(cycle),cycle);
                r.check("lifecycle_request_"+std::to_string(cycle),normal(value));
                r.check("lifecycle_release_"+std::to_string(cycle),a.close());
                if(interrupted || monitor.low || value.restart_required) break;
            }
        } else {
            LlmAdapter a(make_rkllm_backend(assets,opts),opts);
            init_ms.push_back(a.initialization_ms());
            for(int n=0;n<o.warmup;++n) {
                a.new_session();
                const auto t=a.start(cases[0].at("text"),std::min(32,opts.max_output_tokens));
                auto value=r.collect(a,t,"warmup",n,"warmup");
                r.check("warmup_"+std::to_string(n),normal(value));
                if(value.restart_required) throw std::runtime_error("warmup requires restart");
            }
            if(o.mode=="cases") {
                for(int batch=1;batch<=o.repeat;++batch) {
                    std::vector<std::size_t> order(cases.size());
                    std::iota(order.begin(),order.end(),0);std::mt19937 rng(3588+batch);
                    std::shuffle(order.begin(),order.end(),rng);
                    for(const auto i:order) {
                        a.new_session();const auto& c=cases[i];
                        auto value=r.execute(a,c.at("text"),c.at("id"),batch,c.at("max_output_tokens"));
                        r.check(c.at("id").get<std::string>()+"_batch_"+std::to_string(batch),normal(value));
                        if(interrupted || monitor.low || value.restart_required) break;
                    }
                    if(interrupted || monitor.low || a.restart_required()) break;
                }
            } else if(o.mode=="boundaries") {
                const std::vector<std::string> fruits={"苹果","香蕉","橙子","葡萄","西瓜"};
                for(std::size_t n=0;n<fruits.size();++n) {
                    a.new_session();const auto id="multi_"+std::to_string(n+1);
                    auto t=a.start("请记住我喜欢的水果是"+fruits[n]+"，后面会问你。",64);
                    auto first=r.collect(a,t,id+"_remember",0,"context");
                    r.check(id+"_first",normal(first) && a.commit_history(t));
                    auto next=r.execute(a,"我刚才说喜欢哪种水果？",id+"_recall",0,64,"context");
                    r.check(id+"_recall",normal(next) && next.text.find(fruits[n])!=std::string::npos);
                    a.new_session();
                    r.check(id+"_reset_history",a.history_pairs()==0);
                    auto clean=r.execute(a,"请只回答：新会话。",id+"_new",0,64,"new_session");
                    r.check(id+"_new",normal(clean) &&
                        clean.prompt_estimated_tokens==llm_render_prompt({},"请只回答：新会话。").size());
                }
                bool rejected=false;try {a.start(std::string(8192,'x'));}catch(const std::length_error&){rejected=true;}
                r.check("overlong_input_rejected_before_sdk",rejected);
                a.new_session();
                auto ft=a.start("请依次写出从一到一百的整数，用逗号分隔。",128,5,30000);
                auto timeout=r.collect(a,ft,"forced_first_token",0,"timeout");
                r.check("first_token_timeout",timeout.status=="first_token_timeout" &&
                    timeout.run_rc!=-999 && timeout.idle_ns>0 && !timeout.restart_required);
                r.check("first_timeout_recovery",normal(r.execute(a,"请只回答：恢复成功。","first_timeout_recovery",0,64,"recovery")));
                a.new_session();
                auto tt=a.start("请依次写出从一到一百的整数，用逗号分隔。",128,700,800);
                auto total=r.collect(a,tt,"forced_total",0,"timeout");
                r.check("total_timeout",total.status=="total_timeout" &&
                    total.first_token_ns>0 && total.idle_ns>0 && !total.restart_required);
                r.check("total_timeout_recovery",normal(r.execute(a,"请只回答：恢复成功。","total_timeout_recovery",0,64,"recovery")));
            } else if(o.mode=="cancel") {
                for(const auto& stage:std::vector<std::string>{"before_first_token","during_generation"})
                    for(int n=1;n<=5;++n) {
                        a.new_session();
                        const auto id=stage+"_"+std::to_string(n);
                        auto t=a.start("请依次写出从一到一百的整数，用逗号分隔。",128);
                        auto value=r.collect(a,t,id,n,"cancel",stage=="during_generation"?3:0,
                            stage=="before_first_token"?5:0,true);
                        const bool before=stage!="before_first_token" || !value.first_token_ns ||
                            value.first_token_ns>=value.cancel_requested_ns;
                        r.check(id,value.cancel_requested_ns>0 && before && value.abort_rc==0 &&
                            value.run_rc!=-999 && value.idle_ns>=value.cancel_requested_ns &&
                            value.idle_ns-value.cancel_requested_ns<=2000000000LL &&
                            !value.restart_required && !a.commit_history(t) && a.history_pairs()==0);
                        bool old_commit=false;
                        r.check(id+"_old_commit",!a.commit_if_current(t,[&]{old_commit=true;}) && !old_commit);
                        r.check(id+"_recovery",normal(r.execute(a,"请只回答：恢复成功。",
                            id+"_recovery",n,64,"recovery")));
                        if(interrupted || monitor.low || a.restart_required()) break;
                    }
            }
            r.check("release",a.close());
        }
        write_json(run/"libraries.json",libraries());
        std::vector<double> ttft,speed,cancel_ms;
        int normal_count=0,failed_count=0;
        for(const auto& j:r.results) {
            if(j.at("phase")=="case") {
                if(j.at("status")=="completed") {
                    ++normal_count;
                    if(j.contains("ttft_ms")) ttft.push_back(j.at("ttft_ms"));
                    if(j.contains("sdk_tokens_per_second")) speed.push_back(j.at("sdk_tokens_per_second"));
                } else ++failed_count;
            }
            if(j.contains("cancel_to_idle_ms")) cancel_ms.push_back(j.at("cancel_to_idle_ms"));
        }
        struct rusage u{};getrusage(RUSAGE_SELF,&u);
        Json summary{{"software_status",r.failed?"failed":"passed"},{"content_review_status","not_run"},
            {"a04","not_run"},{"a08","not_run"},{"scope","model-side/file checks only"},
            {"normal_case_count",normal_count},{"failed_case_count",failed_count},
            {"initialization_ms",init_ms},{"ttft_ms_successes_only",quantiles(ttft)},
            {"sdk_tokens_per_second_successes_only",quantiles(speed)},
            {"cancel_to_idle_ms",quantiles(cancel_ms)},{"peak_rss_kib",u.ru_maxrss},
            {"sampled_peak_rss_kib",monitor.peak_rss.load()},
            {"min_mem_available_kib",monitor.min_available.load()},
            {"memory_floor_breached",monitor.low.load()},{"signal",static_cast<int>(interrupted)},
            {"checks",r.checks},{"results",r.results}};
        write_json(run/"summary.json",summary);
        if(interrupted) return 128+interrupted;
        if(monitor.low) return 4;
        std::cout<<"LLM_PROBE_REPORT "<<run.string()<<" software="<<(r.failed?"failed":"passed")<<'\n';
        int exit_code=r.failed?6:0;
        if(r.failed) for(const auto& row:r.results) {
            const auto status=row.at("status").get<std::string>();
            if(row.at("phase")!="timeout" && status.find("timeout")!=std::string::npos) exit_code=5;
            else if(exit_code==6 && status=="model_error") exit_code=4;
        }
        return exit_code;
    } catch(const ProbeIoError& e) {std::cerr<<"IO_ERROR "<<e.what()<<'\n';return 1;}
    catch(const ProbeTimeout& e) {std::cerr<<"TIMEOUT "<<e.what()<<'\n';return 5;}
    catch(const fs::filesystem_error& e) {std::cerr<<"PATH_ERROR "<<e.what()<<'\n';return 2;}
    catch(const ConfigError& e) {std::cerr<<"CONFIG_ERROR "<<e.what()<<'\n';return 2;}
    catch(const std::invalid_argument& e) {std::cerr<<"INPUT_ERROR "<<e.what()<<'\n';return 2;}
    catch(const Json::exception& e) {std::cerr<<"INPUT_ERROR "<<e.what()<<'\n';return 2;}
    catch(const std::exception& e) {
        if(!run.empty() && fs::is_directory(run))
            write_json(run/"error.json",{{"status","failed"},{"error",e.what()}});
        std::cerr<<"LLM_ERROR "<<e.what()<<'\n';return 4;
    }
}
