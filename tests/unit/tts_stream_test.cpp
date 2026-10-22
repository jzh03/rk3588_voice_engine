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
int main(){
 try{
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
    std::cout<<"TTS_STREAM_OK\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
