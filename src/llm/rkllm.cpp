#include "voice_engine/llm.hpp"
#include "voice_engine/assets.hpp"
#include "voice_engine/config.hpp"
#include "voice_engine/probe_support.hpp"
#include "rkllm.h"
#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace voice_engine {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
bool under(const fs::path& p,const fs::path& root) {
    auto a=p.begin();
    for (auto b=root.begin();b!=root.end();++a,++b)
        if(a==p.end() || *a!=*b) return false;
    return true;
}
fs::path scoped(const fs::path& root,const std::string& p) {
    if(fs::path(p).is_absolute()) throw ConfigError("LLM asset path must be relative");
    const auto q=fs::canonical(root/p);
    if(!under(q,root) || !fs::is_regular_file(q))
        throw ConfigError("LLM asset path escapes manifest directory");
    return q;
}
Json read_json(const fs::path& p) {
    if(fs::file_size(p)>4*1024*1024) throw ConfigError("LLM manifest exceeds 4 MiB");
    std::ifstream f(p); return Json::parse(f);
}
void runtime_mapping(const fs::path& expected) {
    std::ifstream f("/proc/self/maps");
    std::string line; bool seen=false;
    while(std::getline(f,line)) {
        const auto p=line.find('/');
        if(p==std::string::npos || line.find("librkllmrt.so",p)==std::string::npos) continue;
        if(fs::canonical(line.substr(p))!=expected)
            throw ConfigError("LLM loaded runtime is outside the validated RKLLM 1.3.0 asset");
        seen=true;
    }
    if(!seen) throw ConfigError("LLM RKLLM runtime mapping not found");
}
class RkllmBackend final : public LlmBackend {
    RkllmAssets assets_;
    LlmOptions options_;
    LLMHandle handle_=nullptr;
    // This callback remains alive until synchronous run has returned and SDK
    // running is zero; the adapter enforces that boundary before another run.
    struct RunCallback { Callback sink; std::size_t queue_bytes; };
    std::unique_ptr<RunCallback> callback_;
    static int callback(RKLLMResult* result,void* userdata,LLMCallState state) noexcept {
        auto* context=static_cast<RunCallback*>(userdata);
        if(!context || !context->sink) return 0;
        try {
            LlmBackendEvent e;
            if(state==RKLLM_RUN_FINISH) {
                e.kind=LlmBackendEvent::Kind::finish;
                if(result) e.perf={result->perf.prefill_tokens,result->perf.generate_tokens,
                    result->perf.prefill_time_ms,result->perf.generate_time_ms,
                    result->perf.memory_usage_mb};
            } else if(state==RKLLM_RUN_ERROR) e.kind=LlmBackendEvent::Kind::error;
            else if(state==RKLLM_RUN_NORMAL || state==RKLLM_RUN_WAITING) {
                e.kind=LlmBackendEvent::Kind::token;
                if(result) {
                    e.token_id=result->token_id;
                    // Bound copying before constructing a string in the SDK callback.
                    if(state==RKLLM_RUN_NORMAL && result->text) {
                        const auto n=strnlen(result->text,context->queue_bytes+1);
                        e.text.assign(result->text,n);
                    }
                }
            } else return 0;
            context->sink(e);
        } catch (...) {
            // Exceptions must never cross the C ABI.
            try { context->sink({LlmBackendEvent::Kind::error,-1,"",{}}); } catch(...) {}
        }
        return 0;
    }
public:
    RkllmBackend(RkllmAssets a,LlmOptions o):assets_(std::move(a)),options_(o){}
    void initialize() override {
        probe::require_model_validation_lock();
        runtime_mapping(assets_.runtime);
        RKLLMParam p=rkllm_createDefaultParam();
        p.model_path=assets_.model.c_str(); p.max_context_len=2048;
        p.max_new_tokens=options_.max_output_tokens; p.top_k=1; p.top_p=0.95F;
        p.temperature=0.8F; p.repeat_penalty=1.0F;
        p.presence_penalty=0; p.frequency_penalty=0; p.n_keep=-1;
        p.mirostat=0; p.mirostat_tau=5; p.mirostat_eta=0.1F;
        p.skip_special_token=true; p.ignore_eos_token=false; p.is_async=false;
        p.extend_param.base_domain_id=0; p.extend_param.embed_flash=1;
        p.extend_param.enabled_cpus_num=4; p.extend_param.enabled_cpus_mask=240;
        p.extend_param.n_batch=1; p.extend_param.use_cross_attn=0;
        RKLLMCallback cb{}; cb.result_callback=callback; cb.result_userdata=nullptr;
        if(rkllm_init(&handle_,&p,&cb)!=0 || !handle_)
            throw std::runtime_error("RKLLM 1.3.0 initialization failed");
        // The application renders the complete selected-R template exactly once.
        // Empty SDK wrappers prevent implicit template/history accumulation.
        if(rkllm_set_chat_template(handle_,"","","")!=0) {
            rkllm_destroy(handle_);handle_=nullptr;
            throw std::runtime_error("RKLLM could not disable implicit chat template");
        }
    }
    int run(const std::string& prompt,int max_tokens,Callback cb) override {
        callback_=std::make_unique<RunCallback>(RunCallback{std::move(cb),options_.queue_bytes});
        RKLLMInput in{}; in.input_type=RKLLM_INPUT_PROMPT;
        in.role="user"; in.enable_thinking=false; in.prompt_input=prompt.c_str();
        RKLLMInferParam p{}; p.mode=RKLLM_INFER_GENERATE;
        p.keep_history=0; p.max_new_tokens=max_tokens;
        return rkllm_run(handle_,&in,&p,callback_.get());
    }
    int abort() override { return handle_?rkllm_abort(handle_):-1; }
    int running() override { return handle_?rkllm_is_running(handle_):0; }
    int clear() override { return handle_?rkllm_clear_kv_cache(handle_,0,nullptr,nullptr):-1; }
    int shutdown() override {
        if(!handle_) return 0;
        if(running()!=0) return -1;
        const int rc=rkllm_destroy(handle_);
        if(rc==0) { handle_=nullptr; callback_={}; }
        return rc;
    }
};
} // namespace
LlmOptions llm_options_from_config(const Config& c) {
    LlmOptions o; o.context_tokens=c.llm.context_tokens;
    o.max_output_tokens=c.llm.max_output_tokens; o.margin_tokens=c.llm.margin_tokens;
    o.queue_bytes=static_cast<std::size_t>(c.queues.llm_text_bytes);
    o.first_token_timeout_ms=c.timeouts.llm_first_token_ms;
    o.total_timeout_ms=c.timeouts.llm_total_ms; o.cancel_timeout_ms=c.timeouts.cancel_ms;
    return o;
}
RkllmAssets resolve_rkllm_assets(const Config& original) {
    probe::require_model_validation_lock();
    Config c=original;
    if(!c.models.llm.enabled || c.models.llm.id!="qwen3.5-2b-rk3588-w8a8")
        throw ConfigError("LLM requires enabled qwen3.5-2b-rk3588-w8a8");
    c.models.vad.enabled=c.models.asr.enabled=c.models.tts.enabled=c.models.kws.enabled=false;
    validate_assets(c);
    const auto mp=fs::canonical(c.source_path.parent_path()/c.assets.models_manifest);
    const auto dp=fs::canonical(c.source_path.parent_path()/c.assets.deps_manifest);
    RkllmAssets a; a.model_id=c.models.llm.id;
    const auto models=read_json(mp), deps=read_json(dp);
    for(const auto& m:models.at("models"))
        if(m.at("id")==c.models.llm.id) {
            if(m.at("runtime_version")!="1.3.0" || m.at("toolkit_version")!="1.3.0" ||
               m.at("quantization")!="W8A8")
                throw ConfigError("LLM requires fixed Qwen3.5-2B W8A8 / RKLLM 1.3.0");
            for(const auto& f:m.at("files")) {
                const auto p=f.at("path").get<std::string>();
                if(fs::path(p).extension()==".rkllm") {
                    if(!a.model.empty()) throw ConfigError("multiple LLM weights in selected asset");
                    a.model=scoped(mp.parent_path(),p);
                }
            }
        }
    for(const auto& d:deps.at("dependencies"))
        if(d.at("id")=="rkllm-runtime") {
            if(d.at("version")!="1.3.0") throw ConfigError("RKLLM runtime version mismatch");
            for(const auto& f:d.at("files")) {
                const auto p=f.at("path").get<std::string>();
                if(fs::path(p).filename()=="librkllmrt.so")
                    a.runtime=scoped(dp.parent_path(),p);
            }
        }
    if(a.model.empty() || a.runtime.empty()) throw ConfigError("missing LLM weight/runtime role");
    runtime_mapping(a.runtime); return a;
}
std::unique_ptr<LlmBackend> make_rkllm_backend(const RkllmAssets& a,const LlmOptions& o) {
    return std::make_unique<RkllmBackend>(a,o);
}
} // namespace voice_engine
