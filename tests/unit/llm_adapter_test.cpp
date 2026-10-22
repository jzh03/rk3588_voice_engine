#include "voice_engine/llm.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace voice_engine;
using namespace std::chrono_literals;
using namespace std::string_literals;
namespace {
void require(bool ok,const char* msg) { if(!ok) throw std::runtime_error(msg); }
struct Control {
    std::atomic<int> mode{0}, runs{0}, clears{0}, destroys{0}, destructors{0}, aborts{0};
    std::atomic<bool> running{false}, aborted{false}, release{false};
    std::mutex mutex;
    LlmBackend::Callback old;
    LlmBackend::Callback* live_callback = nullptr;
    std::vector<std::string> prompts;
    std::vector<LlmBackendEvent> scripted;
};
class Stub final:public LlmBackend {
    std::shared_ptr<Control> c;
    Callback live_callback_;
public:
    explicit Stub(std::shared_ptr<Control> p):c(std::move(p)){}
    ~Stub() override {
        std::lock_guard<std::mutex> l(c->mutex);
        c->live_callback=nullptr;++c->destructors;
    }
    void initialize() override {}
    int clear() override { ++c->clears; return 0; }
    int running() override { return c->running?1:0; }
    int shutdown() override { require(!c->running,"destroyed active backend");++c->destroys;return 0; }
    int abort() override {
        ++c->aborts;c->aborted=true;
        if(c->mode==3) std::this_thread::sleep_for(40ms);
        return 0;
    }
    int run(const std::string& prompt,int max_tokens,Callback cb) override {
        ++c->runs;c->aborted=false;c->running=true;
        const int mode=c->mode;
        {
            std::lock_guard<std::mutex> l(c->mutex);
            c->old=cb;c->prompts.push_back(prompt);
        }
        if(mode==7) {
            for(const auto& event:c->scripted) cb(event);
            c->running=false;return 0;
        }
        if(mode==6) {
            // Model run returned, but SDK still owns this exact callback object.
            // The test uses its address, not a separately retained callback copy.
            std::lock_guard<std::mutex> l(c->mutex);
            live_callback_=std::move(cb);c->live_callback=&live_callback_;
            return 0;
        }
        if(mode==1 || mode==3 || mode==4) {
            while(!c->release && (mode==4 || !c->aborted)) std::this_thread::sleep_for(1ms);
            // Tail callback after abort is deliberate.
            cb({LlmBackendEvent::Kind::token,7,"旧",{}});
        } else if(mode==2) cb({LlmBackendEvent::Kind::token,8,std::string(100,'x'),{}});
        else if(mode==5) {
            for(int i=0;i<100 && !c->aborted;++i) {
                cb({LlmBackendEvent::Kind::token,i,"字",{}});
                std::this_thread::sleep_for(2ms);
            }
        } else {
            for(const auto& x:std::vector<std::string>{"<thi","nk>隐藏</thi","nk>","\xe4","\xb8\xad","文。","<|im_","end|>"})
                cb({LlmBackendEvent::Kind::token,9,x,{}});
        }
        cb({LlmBackendEvent::Kind::finish,-1,"",{40,std::min(8,max_tokens),10,20,1}});
        c->running=false;return 0;
    }
};
LlmOptions options() { LlmOptions o;o.first_token_timeout_ms=200;o.total_timeout_ms=500;o.cancel_timeout_ms=150;return o; }
LlmResult result(LlmAdapter& a,const LlmTurn& t) {
    const auto r=a.wait(t,2000);require(r.has_value(),"turn did not finish");return *r;
}
void wait_running(const std::shared_ptr<Control>& c) {
    for(int i=0;i<500 && !c->running;++i) std::this_thread::sleep_for(1ms);
    require(c->running,"stub did not enter run");
}
void standard() {
    auto c=std::make_shared<Control>();LlmAdapter a(std::make_unique<Stub>(c),options());
    auto t=a.start("你好");auto r=result(a,t);
    require(r.status=="completed" && r.text=="中文。" && !r.invalid_utf8,"UTF8/control filtering");
    require(r.raw_text.find("隐藏")!=std::string::npos && r.control_markers_seen,"raw evidence retained");
    require(c->prompts[0]==llm_render_prompt({},"你好"),"template not applied exactly once");
    require(a.history_pairs()==0,"implicit history is forbidden");
    require(a.commit_history(t) && !a.commit_history(t),"explicit one-time history commit");
    auto follow=a.start("继续");auto rr=result(a,follow);
    require(c->prompts[1].find("中文。")!=std::string::npos,"history missing");
    require(a.commit_history(follow),"followup history");
    a.new_session();require(a.history_pairs()==0 && !a.commit_history(follow),"new session leaked history");
    auto warm=a.warmup("预热");require(warm.status=="completed" && a.history_pairs()==0,"warmup polluted history");
    auto foreign_c=std::make_shared<Control>();
    LlmAdapter other(std::make_unique<Stub>(foreign_c),options());
    bool rejected=false;try{other.wait(t,0);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"foreign turn accepted");
    require(a.close() && c->destroys==1,"release failed");
    auto small_c=std::make_shared<Control>();auto small_options=options();small_options.max_output_tokens=16;
    LlmAdapter small(std::make_unique<Stub>(small_c),small_options);
    require(small.warmup("预热").output_budget==16,"warmup exceeded configured output budget");
}
void queue_snapshots() {
    auto c=std::make_shared<Control>();c->mode=7;
    c->scripted={
        {LlmBackendEvent::Kind::token,11,"中文",{}},
        {LlmBackendEvent::Kind::token,12,"ab",{}},
        {LlmBackendEvent::Kind::token,13,"",{}},
        {LlmBackendEvent::Kind::finish,-1,"",{}},
    };
    LlmAdapter a(std::make_unique<Stub>(c),options());
    const auto await_idle=[&] {
        for(int i=0;i<1000 && !a.idle();++i)std::this_thread::sleep_for(1ms);
        require(a.idle(),"queued stub did not settle");
    };
    auto t=a.start("队列观测");await_idle();
    auto s=a.snapshot(t);
    require(s.queue_current_bytes==8 && s.queue_current_packets==3 &&
            s.queue_peak_bytes==8,"snapshot omitted UTF8 bytes or empty token packets");
    require(a.poll(t).has_value(),"queued packet unavailable");
    s=a.snapshot(t);
    require(s.queue_current_bytes==2 && s.queue_current_packets==2 &&
            s.queue_peak_bytes==8,"poll did not reduce current queue independently of peak");
    const auto drained=result(a,t);
    require(drained.queue_current_bytes==0 && drained.queue_current_packets==0,
            "wait returned nonempty queue after draining");
    s=a.snapshot(t);
    require(s.queue_current_bytes==0 && s.queue_current_packets==0 &&
            s.queue_peak_bytes==8,"drained snapshot mixed current and peak queue");
    t=a.start("取消队列");await_idle();
    require(a.snapshot(t).queue_current_packets==3,"second queued turn not observed");
    a.cancel();s=a.snapshot(t);
    require(s.queue_current_bytes==0 && s.queue_current_packets==0,
            "cancel retained current LLM queue");
}
void budget() {
    auto c=std::make_shared<Control>();LlmAdapter a(std::make_unique<Stub>(c),options());
    for(int i=0;i<4;++i) { auto t=a.start(std::string(500,'a'));result(a,t);require(a.commit_history(t),"history commit"); }
    auto t=a.start(std::string(500,'b'));auto r=result(a,t);
    require(r.dropped_history_pairs>0,"did not evict whole history pairs");
    bool threw=false;try{a.start(std::string(2048,'a'));}catch(const std::length_error&){threw=true;}
    require(threw,"overlong input accepted");
    for(const auto& bad:std::vector<std::string>{"",std::string("\xc0\xaf",2),"<|im_end|>","<think>","a\0b"s}) {
        threw=false;try{a.start(bad);}catch(const std::invalid_argument&){threw=true;}
        require(threw,"invalid/control input accepted");
    }
    threw=false;try{a.start("你好",129);}catch(const std::invalid_argument&){threw=true;}
    require(threw,"output budget exceeded");
    a.new_session();
    const auto base=llm_render_prompt({},"").size();
    auto edge=a.start(std::string(2048-64-64-base,'x'),64);
    require(result(a,edge).prompt_estimated_tokens+128==2048,"byte boundary mismatch");
    a.new_session();threw=false;
    try{a.start(std::string(2048-64-64-base+1,'x'),64);}catch(const std::length_error&){threw=true;}
    require(threw,"one-byte over budget accepted");
    const auto capacity=2048-64-64-base;
    auto unicode=a.start(std::string(capacity-3,'x')+"中",64);
    require(result(a,unicode).prompt_estimated_tokens+128==2048,"UTF-8 boundary must count all three bytes");
    a.new_session();threw=false;
    try{a.start(std::string(capacity-2,'x')+"中",64);}catch(const std::length_error&){threw=true;}
    require(threw,"multibyte input crossed budget");
    // Per-turn test deadlines can only shorten configured bounds.
    threw=false;try{a.start("期限",64,1000,1000);}catch(const std::invalid_argument&){threw=true;}
    require(threw,"request deadline widened configured bound");
}
void races() {
    auto c=std::make_shared<Control>();c->mode=3;
    LlmAdapter a(std::make_unique<Stub>(c),options());
    auto t=a.start("取消");wait_running(c);
    LlmBackend::Callback tail;
    {std::lock_guard<std::mutex> l(c->mutex);tail=c->old;}
    a.cancel();a.cancel();a.new_session();
    auto r=result(a,t);
    require(r.status!="completed" && r.idle_ns>=r.cancel_requested_ns,"abort/idle not confirmed");
    require(!a.commit_history(t) && a.history_pairs()==0,"cancelled history committed");
    c->mode=0;
    auto fresh=a.start("下一轮");
    tail({LlmBackendEvent::Kind::token,1,"不应出现",{}});
    tail({LlmBackendEvent::Kind::error,-1,"",{}});
    tail({LlmBackendEvent::Kind::finish,-1,"",{}});
    bool submitted=false;
    require(!a.commit_if_current(t,[&]{submitted=true;}) && !submitted,"stale commit accepted");
    auto good=result(a,fresh);
    require(good.text=="中文。" && good.status=="completed","old abort/callback hit fresh turn");
    require(a.snapshot(t).stale_callbacks>=1,"stale callback not recorded");
    // Caller has checked and prepared a submission; cancel happens before the
    // atomic commit API. The API must recheck under the cancellation mutex.
    a.cancel();
    require(!a.commit_if_current(fresh,[&]{submitted=true;}),"check/commit cancellation race");
    // Continuous start cancels the previous worker request before replacement.
    c->mode=1;auto x=a.start("先发");wait_running(c);c->mode=0;
    auto y=a.start("后发");
    require(result(a,x).status=="superseded","consecutive start did not cancel old turn");
    require(result(a,y).status=="completed","consecutive start recovery failed");
}
void boundaries() {
    auto c=std::make_shared<Control>();c->mode=1;auto o=options();o.first_token_timeout_ms=10;
    LlmAdapter a(std::make_unique<Stub>(c),o);
    auto t=a.start("首token超时");auto r=result(a,t);
    require(r.status=="first_token_timeout" && r.abort_rc==0 && !r.restart_required,"first-token timeout recovery");
    c->mode=0;require(result(a,a.start("恢复")).status=="completed","same handle recovery");
    auto d=std::make_shared<Control>();d->mode=5;o=options();o.total_timeout_ms=20;o.first_token_timeout_ms=15;
    LlmAdapter total(std::make_unique<Stub>(d),o);
    require(result(total,total.start("总超时")).status=="total_timeout","total deadline not enforced");
    auto e=std::make_shared<Control>();e->mode=2;o=options();o.queue_bytes=8;
    LlmAdapter overflow(std::make_unique<Stub>(e),o);
    auto q=result(overflow,overflow.start("队列"));
    require(q.status=="queue_overflow" && q.overflow && !overflow.commit_history({}),"queue bound");
    auto f=std::make_shared<Control>();f->mode=4;o=options();o.cancel_timeout_ms=30;o.first_token_timeout_ms=10;
    {
        LlmAdapter stuck(std::make_unique<Stub>(f),o);
        auto z=stuck.start("无法中止");auto rz=result(stuck,z);
        require(rz.status=="cancel_timeout" && rz.restart_required,"stuck handle not poisoned");
        require(!stuck.close() && f->destroys==0,"active handle was destroyed");
        f->release=true;
    }
    for(int i=0;i<200 && f->destroys==0;++i) std::this_thread::sleep_for(1ms);
    require(f->destroys==1,"released worker did not clean up");
}
struct Observation {
    LlmResult result;
    LlmTermination termination;
    int tokens=0,packets=0,last=-1;
    std::string delivered;
};
Observation observe_script(std::vector<LlmBackendEvent> events,int budget,int sdk_tokens) {
    auto c=std::make_shared<Control>();c->mode=7;
    c->scripted=std::move(events);
    // Even a positive token ID on FINISH is not a generated-token callback.
    c->scripted.push_back({LlmBackendEvent::Kind::finish,248046,"",{40,sdk_tokens,10,20,1}});
    LlmAdapter a(std::make_unique<Stub>(c),options());
    const auto turn=a.start("观测输出上限",budget);
    Observation out;
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(std::chrono::steady_clock::now()<deadline) {
        const bool observed_idle=a.idle();
        const auto packet=a.poll(turn,1);
        if(packet) {
            ++out.packets;out.delivered+=packet->text;
            if(packet->token_id>=0) {++out.tokens;out.last=packet->token_id;}
        }
        if(observed_idle && !packet) {
            const auto r=a.wait(turn,0);
            require(r.has_value(),"finished scripted observation missing result");
            out.result=*r;
            if(r->status=="completed")
                require(out.delivered==r->text && out.tokens==r->token_callbacks,
                        "scripted trace omitted text or valid token IDs");
            out.termination=llm_observed_termination(*r,out.tokens,out.last);
            return out;
        }
    }
    throw std::runtime_error("scripted token observation did not finish");
}
void token_limit_observations() {
    using Kind=LlmBackendEvent::Kind;
    auto one=observe_script({{Kind::token,11,"字",{}}},1,1);
    require(one.termination.reason=="token_limit" && !one.termination.observed_budget_exceeded,
            "one observed token did not reach one-token budget");
    auto four=observe_script({
        {Kind::token,101,"a",{}},{Kind::token,-1,"",{}},
        {Kind::token,102,"",{}},{Kind::token,-1,"\xe4",{}},
        {Kind::token,103,"\xb8",{}},{Kind::token,104,"\xad",{}}},4,3);
    require(four.tokens==4 && four.packets==6 && four.last==104 &&
            four.result.token_callbacks==4 && four.result.perf.generate_tokens==3 &&
            four.result.text=="a中" && !four.result.invalid_utf8,
            "empty token ID, no-ID UTF-8 flush, or FINISH counted incorrectly");
    require(four.termination.reason=="token_limit" &&
            four.termination.basis=="observed_valid_token_ids_equal_requested_output_limit_inference_not_sdk_stop_reason",
            "four output tokens with SDK generate count three misclassified");
    auto short_run=observe_script({
        {Kind::token,101,"a",{}},{Kind::token,102,"b",{}},{Kind::token,103,"c",{}},
        {Kind::token,-1,"d",{}},{Kind::token,-1,"",{}}},4,4);
    require(short_run.packets==5 && short_run.tokens==3 &&
            short_run.termination.reason=="sdk_finish_without_visible_eos",
            "non-token packets or SDK counter invented an output-limit stop");
    auto no_ids=observe_script({{Kind::token,-1,"字",{}}},1,1);
    require(no_ids.tokens==0 && no_ids.termination.reason=="sdk_finish_without_visible_eos",
            "text without a token ID was counted as an output token");
    auto eos=observe_script({
        {Kind::token,101,"a",{}},{Kind::token,102,"b",{}},{Kind::token,103,"c",{}},
        {Kind::token,248046,"",{}}},4,3);
    require(eos.tokens==4 && eos.termination.reason=="observed_eos",
            "observed EOS did not take precedence at the token budget");
    auto over=observe_script({
        {Kind::token,101,"a",{}},{Kind::token,102,"b",{}},{Kind::token,103,"c",{}},
        {Kind::token,104,"d",{}},{Kind::token,105,"e",{}}},4,3);
    require(over.tokens==5 && over.termination.reason=="observed_token_budget_exceeded" &&
            over.termination.observed_budget_exceeded,
            "observed output-budget breach was hidden by SDK undercount");
    auto over_eos=observe_script({
        {Kind::token,101,"a",{}},{Kind::token,102,"b",{}},{Kind::token,103,"c",{}},
        {Kind::token,104,"d",{}},{Kind::token,248046,"",{}}},4,3);
    require(over_eos.termination.reason=="observed_eos" && over_eos.termination.observed_budget_exceeded,
            "EOS hid an output-budget breach");
    auto error=observe_script({{Kind::token,101,"a",{}},{Kind::error,248046,"",{}}},4,3);
    require(error.result.token_callbacks==1 && error.result.callback_errors==1 &&
            error.result.finish_callbacks==1 && error.termination.reason=="model_error",
            "ERROR/FINISH token IDs were counted or obscured failure");
    auto cancelled=one.result;cancelled.status="cancelled";
    require(llm_observed_termination(cancelled,1,248046).reason=="cancelled",
            "EOS or output limit obscured cancellation");
    std::cout<<"LLM_TOKEN_OBSERVATION_REGRESSIONS_OK cases=9\n";
}
void close_completed() {
    for(const bool consumed:{false,true}) {
        auto c=std::make_shared<Control>();
        LlmAdapter a(std::make_unique<Stub>(c),options());
        auto t=a.start("完成后关闭");
        if(consumed) {
            require(result(a,t).status=="completed","completed close setup");
            require(a.commit_history(t),"completed close history setup");
        } else {
            for(int n=0;n<500 && !a.idle();++n) std::this_thread::sleep_for(1ms);
            require(a.idle() && a.snapshot(t).status=="completed","unconsumed close setup");
        }
        require(a.close(),"completed close failed");
        require(!a.poll(t) && !a.commit_history(t) && a.history_pairs()==0,
                "closed turn still exposed packets or history");
        bool submitted=false;
        require(!a.commit_if_current(t,[&]{submitted=true;}) && !submitted,
                "closed completed turn accepted downstream submission");
        require(a.snapshot(t).status=="closed","closed turn did not invalidate");
        require(c->destroys==1,"close did not release idle handle exactly once");
    }
}
void returned_but_active() {
    auto c=std::make_shared<Control>();c->mode=6;
    auto o=options();o.first_token_timeout_ms=10;o.cancel_timeout_ms=30;
    {
        auto a=std::make_unique<LlmAdapter>(std::make_unique<Stub>(c),o);
        auto t=a->start("返回后仍活跃");
        auto r=result(*a,t);
        require(r.run_rc==0 && r.run_return_ns>0 && !a->idle(),
                "returned-active setup did not exercise SDK activity after run return");
        require(r.status=="cancel_timeout" && r.restart_required && r.idle_ns==0,
                "returned-active handle did not fail closed");
        require(!a->close() && c->destroys==0 && c->destructors==0,
                "returned-active handle shutdown/destruction before idle");
        a.reset();
    }
    {
        std::lock_guard<std::mutex> l(c->mutex);
        require(c->destructors==0 && c->live_callback,
                "worker failed to retain backend callback storage after adapter destruction");
        (*c->live_callback)({LlmBackendEvent::Kind::token,17,"迟到",{}});
        (*c->live_callback)({LlmBackendEvent::Kind::finish,-1,"",{}});
    }
    c->running=false;
    for(int n=0;n<1000 && c->destructors==0;++n) std::this_thread::sleep_for(1ms);
    require(c->destroys==1 && c->destructors==1,
            "retained backend did not release after delayed SDK idle");
}
}
int main() {
    try { standard();queue_snapshots();budget();races();boundaries();close_completed();returned_but_active();token_limit_observations();std::cout<<"LLM_ADAPTER_TEST_OK\n";return 0; }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
