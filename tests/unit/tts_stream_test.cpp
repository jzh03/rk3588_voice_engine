#include "voice_engine/tts_stream.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace voice_engine;
using namespace std::chrono_literals;
void require(bool v,const char *s){if(!v)throw std::runtime_error(s);}
TtsNativeAudio sound(float value=0.1f){return {std::vector<float>(4410,value),44100,0.001};}
void drain(TtsStreamWorker &w,TtsStreamId id){
    auto deadline=std::chrono::steady_clock::now()+2s;
    while(std::chrono::steady_clock::now()<deadline){
        while(w.poll(id,1)){}
        if(w.wait_idle(1)){while(w.poll(id)){};return;}
    }
    throw std::runtime_error("drain timed out");
}
template<class F> bool await(F predicate) {
    const auto end=std::chrono::steady_clock::now()+2s;
    while(std::chrono::steady_clock::now()<end) {
        if(predicate())return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}
std::vector<float> collect(TtsStreamWorker& w,TtsStreamId id) {
    std::vector<float> samples;
    const auto end=std::chrono::steady_clock::now()+2s;
    while(std::chrono::steady_clock::now()<end) {
        while(auto f=w.poll(id,1))
            samples.insert(samples.end(),f->audio.samples.begin(),
                           f->audio.samples.begin()+f->audio.valid_samples);
        if(w.wait_idle(0) && w.stats().current_pcm_frames==0)return samples;
    }
    throw std::runtime_error("prefetch collection timed out");
}
void prefetch_checks() {
    const TtsStreamId id{1,1};
    QueuesConfig tiny;tiny.playback_ms=20;
    TtsConfig reserve{1,40,10,2000};
    // Hold generation two: the short first waveform must remain real reserve.
    std::atomic<int> calls{0};
    std::atomic<bool> release{false};
    TtsStreamWorker staged([&](const std::string&) {
        const int call=++calls;
        if(call==2) {
            const auto end=std::chrono::steady_clock::now()+2s;
            while(!release && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);
        }
        return sound(call==1?0.1f:0.2f);
    },reserve,tiny);
    staged.begin(id);staged.submit(id,"一。二。");staged.finish(id);
    const bool reached=await([&]{return calls.load()==2;});
    const auto blocked=staged.stats();
    const auto premature=staged.poll(id);
    release=true;
    require(reached && !premature && blocked.native_buffered_segments==1 &&
            blocked.native_buffered_samples==4410 && blocked.peak_native_slots==2,
            "short first segment escaped reserve or native slots exceeded");
    const auto actual=collect(staged,id);
    TtsPcmConverter converter;
    std::vector<float> expected;
    for(float value:{0.1f,0.2f}) {
        auto audio=sound(value);
        for(auto& frame:converter.push(audio.samples.data(),audio.samples.size()))
            expected.insert(expected.end(),frame.samples.begin(),frame.samples.begin()+frame.valid_samples);
    }
    for(auto& frame:converter.finish())
        expected.insert(expected.end(),frame.samples.begin(),frame.samples.begin()+frame.valid_samples);
    require(actual==expected,"prefetch reordered/truncated/modified PCM or added silence");
    auto stats=staged.stats();
    require(stats.status=="completed" && stats.peak_pcm_frames==1 &&
            stats.peak_native_slots==2 && stats.peak_native_samples==8820 &&
            stats.native_buffered_segments==0 && stats.native_buffered_samples==0 &&
            stats.playback_release_ns>=stats.first_generated_audio_ns &&
            stats.first_pcm_ns>=stats.playback_release_ns,
            "prefetch bounds, release timing or drain incorrect");

    // An independent producer must synthesize the next segment even when the
    // publisher is blocked on a single-frame queue; a third slot is forbidden.
    std::atomic<int> ahead_calls{0};
    TtsStreamWorker ahead([&](const std::string&) {
        ++ahead_calls;return TtsNativeAudio{std::vector<float>(22050,0.3f),44100,0.001};
    },{1,40,10,200},tiny);
    ahead.begin(id);ahead.submit(id,"一。二。三。");ahead.finish(id);
    const bool advanced=await([&]{return ahead_calls.load()>=2 && ahead.stats().current_pcm_frames==1;});
    std::this_thread::sleep_for(15ms);
    const auto full=ahead.stats();const auto call_count=ahead_calls.load();
    ahead.cancel(id);
    require(ahead.wait_idle(2000),"prefetch cancellation failed to wake native/PCM backpressure");
    require(advanced && call_count==2 && full.peak_native_slots==2 &&
            full.peak_pcm_frames==1 && full.native_buffered_segments==2,
            "synthesis still serialized behind PCM delivery or native pool unbounded");
    require(!ahead.poll(id) && ahead.stats().native_buffered_samples==0,
            "cancel retained prefetch PCM");

    // Cancellation before playback release must discard both the ready first
    // waveform and the late result of the still-running second Generate.
    calls=0;release=false;
    TtsStreamWorker cancelled([&](const std::string&) {
        const int call=++calls;
        if(call==2) {
            const auto end=std::chrono::steady_clock::now()+2s;
            while(!release && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);
        }
        return sound(call<3?0.4f:0.05f);
    },reserve,tiny);
    cancelled.begin(id);cancelled.submit(id,"旧。一。");cancelled.finish(id);
    const bool entered=await([&]{return calls.load()==2;});
    cancelled.cancel(id);
    const bool falsely_idle=cancelled.wait_idle(0);
    release=true;
    require(entered && !falsely_idle && cancelled.wait_idle(2000),
            "cancel claimed synchronous inference idle or did not settle");
    require(!cancelled.poll(id) && cancelled.stats().discarded_generation_results==1 &&
            cancelled.stats().native_buffered_samples==0,
            "old ready/late native waveform survived cancellation");
    cancelled.begin({1,2});cancelled.submit({1,2},"新。");cancelled.finish({1,2});
    const auto fresh=collect(cancelled,{1,2});
    require(!fresh.empty() && cancelled.stats().segments==1 && cancelled.stats().status=="completed",
            "single short EOS did not release below reserve");
    require(!cancelled.commit_if_current(id,[]{throw std::runtime_error("stale prefetch commit");}),
            "old prefetch identity committed into new turn");

    TtsStreamWorker empty([](const std::string&){return sound();},reserve,tiny);
    empty.begin(id);empty.finish(id);
    require(empty.wait_idle(2000) && empty.stats().status=="completed" && !empty.poll(id),
            "empty EOS deadlocked reserve");

    std::atomic<int> errors{0};
    TtsStreamWorker failed([&](const std::string&) {
        if(++errors==2)throw std::runtime_error("injected generation failure");
        return sound();
    },reserve,tiny);
    failed.begin(id);failed.submit(id,"一。二。");failed.finish(id);
    require(failed.wait_idle(2000) && failed.stats().status=="failed" &&
            !failed.poll(id) && failed.stats().native_buffered_samples==0,
            "inference failure did not invalidate staged native audio");

    tiny.max_tts_audio_ms=50;
    TtsStreamWorker oversized([](const std::string&){return sound();},reserve,tiny);
    oversized.begin(id);oversized.submit(id,"超长。");oversized.finish(id);
    require(oversized.wait_idle(2000) && oversized.stats().status=="failed" && !oversized.poll(id),
            "prefetch bypassed per-segment audio bound");
}

void sentence_worker_checks() {
    TtsConfig text;text.min_chars=4;text.flush_ms=10;text.sentence_mode=true;
    std::mutex guard;std::vector<std::string> selected;
    std::atomic<bool> entered{false},release{false};
    TtsStreamWorker worker([&](const std::string& part) {
        std::size_t count;
        {std::lock_guard<std::mutex> lock(guard);selected.push_back(part);count=selected.size();}
        if(count==1) {
            entered=true;
            const auto until=std::chrono::steady_clock::now()+2s;
            while(!release && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);
        }
        return sound();
    },text);
    const TtsStreamId id{40,1};
    worker.begin(id);worker.submit(id,"现在开始，请");
    const bool started=await([&]{return entered.load();});
    worker.submit(id,"告诉我你的城市或区域，");
    worker.submit(id,"我可以尝试帮你查找。");
    worker.finish(id);
    // The synthesis delay expires the old pending timer while complete later
    // text is already queued; it must not force a cut at the queued comma.
    std::this_thread::sleep_for(20ms);
    release=true;
    require(started,"first clause did not synthesize before EOS");
    drain(worker,id);
    require(worker.stats().status=="completed" && selected==std::vector<std::string>{
                "现在开始，","请告诉我你的城市或区域，我可以尝试帮你查找。"},
            "queued complete sentence was fragmented by an expired timer");
    worker.begin({40,2});worker.submit({40,2},"新一轮开始，下一句。");worker.finish({40,2});
    drain(worker,{40,2});
    require(worker.stats().status=="completed" && selected.at(2)=="新一轮开始，",
            "new generation did not restore first-clause behavior");
}



void dynamic_worker_checks() {
    TtsConfig c;c.min_chars=10;c.target_chars=24;c.boundary_lookahead_chars=4;c.flush_ms=10;
    c.first_segment=TtsSegmentPolicy{8,15,2};
    const auto repeated=[](int n) {std::string s;while(n--)s+="中";return s;};
    std::atomic<bool> entered{false},release{false};
    std::mutex guard;std::vector<std::string> seen;
    TtsStreamWorker w([&](const std::string& text) {
        std::size_t n;
        {std::lock_guard<std::mutex> lock(guard);seen.push_back(text);n=seen.size();}
        if(n==1) {
            entered=true;
            const auto until=std::chrono::steady_clock::now()+2s;
            while(!release && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);
        }
        return sound();
    },c);
    const TtsStreamId id{60,1};w.begin(id);w.submit(id,repeated(17));
    const bool started=await([&]{return entered.load();});
    w.submit(id,repeated(20));w.submit(id,repeated(10));w.finish(id);
    std::this_thread::sleep_for(20ms);release=true;
    require(started,"dynamic first segment never generated");
    collect(w,id);
    require(seen==std::vector<std::string>{repeated(15),repeated(24),repeated(8)},
            "expired first timer fragmented already queued later text");
    const auto times=w.segment_timings(id);
    require(times.size()==3 && times[0].text_chars==15 && times[0].min_chars==8 &&
            times[0].target_chars==15 && times[0].boundary_lookahead_chars==2 &&
            times[1].text_chars==24 && times[1].min_chars==10 &&
            times[1].target_chars==24 && times[1].boundary_lookahead_chars==4,
            "dynamic timing metadata does not identify active stage");
    w.begin({60,2});w.submit({60,2},"你好这是开头啊，");w.finish({60,2});collect(w,{60,2});
    require(w.segment_timings({60,2}).at(0).min_chars==8 &&
            w.segment_timings({60,2}).at(0).text_chars==8,
            "new worker turn did not reset first-stage override");
}

void segment_timing_checks() {
    const auto timestamp=[] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    for(int prebuffer:{0,1000}) {
        std::vector<std::pair<std::int64_t,std::int64_t>> observed;
        TtsConfig text;text.min_chars=1;text.prebuffer_ms=prebuffer;
        QueuesConfig queues;queues.playback_ms=20;
        TtsStreamWorker w([&](const std::string&) {
            const auto begin=timestamp();
            std::this_thread::sleep_for(3ms);
            const auto end=timestamp();
            observed.push_back({begin,end});
            return TtsNativeAudio{std::vector<float>(4641,0.1f),44100,(end-begin)/1e9};
        },text,queues);
        const TtsStreamId id{50,1};
        w.begin(id);w.submit(id,"一。二。三。");w.finish(id);
        // Backpressure must be visible in publication, not inflated into Generate.
        std::this_thread::sleep_for(20ms);
        const auto audio=collect(w,id);
        const auto times=w.segment_timings(id);
        require(times.size()==3 && observed.size()==3 && !audio.empty(),
                "segment timing count or PCM missing");
        std::size_t frames=0;
        for(std::size_t i=0;i<times.size();++i) {
            const auto& t=times[i];
            require(t.segment_seq==i && t.text_bytes==6 && t.native_samples==4641 &&
                    t.native_sample_rate==44100 && t.sdk_generate_seconds>0,
                    "segment timing identity/length/SDK duration incorrect");
            require(t.text_selected_ns>0 &&
                    t.text_selected_ns<=t.native_wait_begin_ns &&
                    t.native_wait_begin_ns<=t.native_slot_acquired_ns &&
                    t.native_slot_acquired_ns<=t.generate_begin_ns &&
                    t.generate_begin_ns<=observed[i].first &&
                    observed[i].second<=t.generate_end_ns &&
                    t.generate_end_ns<=t.native_enqueued_ns &&
                    t.native_enqueued_ns<=t.publish_begin_ns &&
                    t.publish_begin_ns<=t.resample_end_ns &&
                    t.resample_end_ns<=t.first_pcm_enqueued_ns &&
                    t.first_pcm_enqueued_ns<=t.last_pcm_enqueued_ns &&
                    t.last_pcm_enqueued_ns<=t.publish_end_ns,
                    "segment timestamps lost exact callback boundaries or stage order");
            require(t.pcm_frames>0 && t.first_pcm_seq==frames &&
                    t.last_pcm_seq+1==frames+t.pcm_frames && t.pcm_queue_wait_ns>=0,
                    "segment PCM range is not joinable to playback sequence");
            frames+=t.pcm_frames;
        }
        require(w.stats().frames>=frames && w.stats().frames-frames<=1 &&
                w.stats().segment_timings_dropped==0,
                "EOS trace attribution or timing drop counter incorrect");
        require(w.segment_timings({50,2}).empty(),"stale identity exposed timing records");
        w.cancel(id);
        require(w.segment_timings(id).empty(),"cancelled identity exposed timing records");
        w.begin({50,2});w.submit({50,2},"新。");w.finish({50,2});collect(w,{50,2});
        const auto fresh=w.segment_timings({50,2});
        require(fresh.size()==1 && fresh[0].segment_seq==0 &&
                w.segment_timings(id).empty(),"timings leaked into new generation");
    }
    TtsConfig text;text.min_chars=1;
    TtsStreamWorker capped([](const std::string&){return sound();},text);
    const TtsStreamId id{51,1};capped.begin(id);
    for(std::size_t i=0;i<kTtsSegmentTimingCapacity+2;++i)capped.submit(id,"一。");
    capped.finish(id);collect(capped,id);
    require(capped.stats().segments==kTtsSegmentTimingCapacity+2 &&
            capped.segment_timings(id).size()==kTtsSegmentTimingCapacity &&
            capped.stats().segment_timings_dropped==2,
            "diagnostic capacity changed synthesis or failed to bound storage");
}

void cancellable_worker_checks() {
    // Cancellation can precede native registration. It must not disappear on
    // entry to Generate, and registering later must interrupt immediately.
    auto early = std::make_shared<TtsCancellation>();
    early->cancel();
    int interrupted = 0;
    {
        TtsCancellation::Binding binding(*early, [&]{++interrupted;});
        require(interrupted==1, "pre-entry cancellation was lost");
        early->cancel();
        require(interrupted==1, "cancellation was delivered twice");
    }
    // Binding destruction must serialize with a concurrent cancellation before
    // a native options object could be destroyed.
    TtsCancellation race;
    std::mutex m; std::condition_variable cv;
    bool entered=false, release=false;
    auto binding=std::make_unique<TtsCancellation::Binding>(race,[&]{
        std::unique_lock<std::mutex> l(m);entered=true;cv.notify_all();
        cv.wait(l,[&]{return release;});
    });
    std::thread cancelling([&]{race.cancel();});
    {std::unique_lock<std::mutex> l(m);cv.wait(l,[&]{return entered;});}
    std::atomic<bool> destroyed{false};
    std::thread destroying([&]{binding.reset();destroyed=true;});
    std::this_thread::sleep_for(5ms);
    const bool protected_lifetime=!destroyed;
    {std::lock_guard<std::mutex> l(m);release=true;cv.notify_all();}
    cancelling.join();destroying.join();
    require(protected_lifetime, "native cancel binding lifetime was not protected");

    std::shared_ptr<TtsCancellation> old;
    std::atomic<bool> running{false}, stopped{false};
    int calls=0;
    TtsStreamWorker worker([&](const std::string&,const std::shared_ptr<TtsCancellation>& token){
        if(++calls==1) {
            old=token;
            TtsCancellation::Binding binding(*token,[&]{stopped=true;});
            running=true;
            const bool cancelled=await([&]{return stopped.load();});
            require(cancelled,"native interrupt was not signalled");
            throw std::runtime_error("expected native cancellation");
        }
        require(!token->requested(),"old cancellation poisoned new generation");
        old->cancel();
        require(!token->requested(),"old token interrupted newer generation");
        return sound();
    },TtsConfig{1,40,10});
    worker.begin({1,1});worker.submit({1,1},"取消。");worker.finish({1,1});
    const bool began=await([&]{return running.load();});
    worker.cancel({1,1});
    require(began && worker.wait_idle(2000),"cancellable worker failed to settle");
    require(worker.stats().status=="cancelled" && !worker.poll({1,1}),
            "cancelled native result became a failure or leaked PCM");
    worker.begin({1,2});worker.submit({1,2},"恢复。");worker.finish({1,2});
    drain(worker,{1,2});
    require(worker.stats().status=="completed","same worker did not recover after native cancel");
}
int main(){
 try{
    segment_timing_checks();
    dynamic_worker_checks();
    std::mutex mutex;std::vector<std::string> seen;
    TtsStreamWorker normal([&](const std::string&s){
        std::lock_guard<std::mutex> l(mutex);seen.push_back(s);return sound();});
    TtsStreamId a{1,1};
    normal.begin(a);
    const std::string phrase="今天是2026年10月20日。";
    for(char c:phrase)require(normal.submit(a,std::string(1,c)),"split byte rejected");
    require(normal.finish(a),"finish failed");drain(normal,a);
    require(normal.stats().status=="completed","normal not completed");
    require(seen.size()==1 && seen[0]==phrase,"UTF8/date not preserved");
    require(normal.stats().consumed_frames>0,"missing PCM");
    const auto completed=normal.stats();
    require(completed.current_input_bytes==0 && completed.current_segments==0 &&
            completed.current_pcm_frames==0 && completed.segmenter_pending_bytes==0 &&
            completed.peak_input_bytes>0 && completed.peak_pcm_frames>0,
            "drained current queues mixed with retained peaks");
    normal.cancel(a);require(!normal.commit_if_current(a,[]{throw std::runtime_error("stale commit");}),"stale commit accepted");

    bool reused=false;try{normal.begin(a);}catch(const std::logic_error&){reused=true;}
    require(reused,"generation identity reused");
    normal.begin({1,2});normal.submit({1,2},"下一句。");normal.finish({1,2});
    require(!normal.commit_if_current(a,[]{throw std::runtime_error("old frame committed");}),
            "old frame committed after new generation");
    drain(normal,{1,2});
    bool entered=false,release=false;
    std::condition_variable cv;
    TtsStreamWorker slow([&](const std::string&){
        std::unique_lock<std::mutex> l(mutex);entered=true;cv.notify_all();
        cv.wait(l,[&]{return release;});return sound(0.4f);
    },{1,40,10});
    slow.begin(a);slow.submit(a,"旧。一。二。");
    {std::unique_lock<std::mutex> l(mutex);require(cv.wait_for(l,2s,[&]{return entered;}),"synth did not start");}
    const bool extra_accepted=slow.submit(a,"额外");
    const auto blocked=slow.stats();
    slow.finish(a);
    slow.cancel(a);
    const auto cancelled=slow.stats();
    require(!slow.poll(a),"cancel retained PCM");
    require(!slow.wait_idle(1),"cancel falsely claimed SDK idle");
    bool rejected=false;try{slow.begin({1,2});}catch(const std::logic_error&){rejected=true;}
    require(rejected,"new turn overlapped old Generate");
    {std::lock_guard<std::mutex> l(mutex);release=true;cv.notify_all();}
    require(slow.wait_idle(2000),"cancel did not settle");
    require(extra_accepted,"could not queue text behind blocked Generate");
    require(blocked.current_input_bytes==std::string("额外").size() &&
            blocked.current_segments==2 && blocked.current_pcm_frames==0 &&
            blocked.segmenter_pending_bytes==0,
            "blocked Generate snapshot omitted queued text or pending segments");
    require(cancelled.current_input_bytes==0 && cancelled.current_segments==0 &&
            cancelled.current_pcm_frames==0 && cancelled.segmenter_pending_bytes==0,
            "cancel retained current TTS queues");
    require(slow.stats().discarded_generation_results==1,"late result not discarded");
    slow.begin({1,2});slow.submit({1,2},"新。");slow.finish({1,2});drain(slow,{1,2});
    require(slow.stats().status=="completed","new turn failed after cancel");

    QueuesConfig q;q.playback_ms=20;
    TtsStreamWorker pressure([](const std::string&){return sound();},{1,40,10},q);
    pressure.begin(a);pressure.submit(a,"满。");pressure.finish(a);
    auto deadline=std::chrono::steady_clock::now()+2s;
    while(pressure.stats().peak_pcm_frames==0 && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(pressure.stats().peak_pcm_frames==1 &&
            pressure.stats().current_pcm_frames==1,"PCM queue bound/current depth wrong");
    pressure.cancel(a);require(pressure.wait_idle(2000),"cancel failed to wake PCM backpressure");
    require(!pressure.poll(a),"old queued PCM survived cancel");
    require(pressure.stats().current_pcm_frames==0 &&
            pressure.stats().peak_pcm_frames==1,"cancel did not clear depth and retain peak");

    TtsStreamWorker partial([](const std::string&){
        throw std::runtime_error("partial text unexpectedly synthesized");
        return sound();
    },{8,40,10000});
    partial.begin(a);require(partial.submit(a,"短"),"partial text rejected");
    deadline=std::chrono::steady_clock::now()+2s;
    while(partial.stats().segmenter_pending_bytes==0 &&
          std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    const auto pending=partial.stats();
    require(pending.current_input_bytes==0 && pending.segmenter_pending_bytes==3 &&
            pending.current_segments==0,"segmenter buffer not distinguished from chunk queue");
    partial.cancel(a);require(partial.wait_idle(2000),"partial cancellation stuck");
    require(partial.stats().segmenter_pending_bytes==0,"cancel retained partial text");

    TtsStreamWorker overflow([](const std::string&){return sound();},{1,40,10});
    overflow.begin(a);overflow.submit(a,"一。二。三。四。五。");overflow.finish(a);
    require(overflow.wait_idle(2000),"segment overflow stuck");
    require(overflow.stats().status=="failed" && overflow.stats().segments==0,"segment overflow silently accepted");
    overflow.begin({1,2});require(!overflow.submit({1,2},std::string(8193,'x')),"byte bound not enforced");
    require(overflow.wait_idle(2000),"byte overflow stuck");

    std::atomic<int> calls{0};
    TtsStreamWorker timer([&](const std::string&){++calls;return sound();},{2,40,10});
    timer.begin(a);timer.submit(a,"你好");
    deadline=std::chrono::steady_clock::now()+2s;
    while(calls==0 && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(calls>0,"timer flush did not run without more tokens");
    timer.finish(a);drain(timer,a);
    timer.begin({1,2});timer.submit({1,2},"<think>隐藏");timer.finish({1,2});
    require(timer.wait_idle(2000) && timer.stats().status=="failed","control input not rejected");
    require(!timer.poll({1,2}),"failed input emitted PCM");
    cancellable_worker_checks();
    prefetch_checks();
    sentence_worker_checks();
    std::cout<<"TTS_STREAM_OK\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
