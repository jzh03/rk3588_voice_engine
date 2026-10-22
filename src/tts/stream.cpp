#include "voice_engine/tts_stream.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace voice_engine {
namespace {
using Clock = std::chrono::steady_clock;
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
bool equal(TtsStreamId a,TtsStreamId b) {
    return a.session==b.session && a.generation==b.generation;
}
}
struct TtsStreamWorker::Impl {
    Generate generate;
    TtsConfig text;
    QueuesConfig limits;
    int timeout_ms;
    const std::size_t frame_limit;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool stopping=false, busy=false, current=false, start_pending=false, input_end=false;
    std::size_t input_bytes=0;
    std::deque<std::string> chunks;
    std::deque<TtsStreamFrame> pcm;
    TtsStreamStats report;
    Impl(Generate f,TtsConfig t,QueuesConfig q,int timeout)
        :generate(std::move(f)),text(t),limits(q),timeout_ms(timeout),
         frame_limit(static_cast<std::size_t>(q.playback_ms/20)) {
        if(!generate || q.llm_text_bytes<1 || q.llm_text_bytes>8192 ||
           q.tts_segments<1 || q.tts_segments>64 || q.playback_ms<20 ||
           q.playback_ms>2000 || q.max_tts_audio_ms<1 || q.max_tts_audio_ms>20000 || timeout<1)
            throw std::invalid_argument("TTS stream: invalid bounds");
        TtsSegmenter check(text);
        worker=std::thread([this]{loop();});
    }
    bool valid(TtsStreamId id)const{return current && equal(report.id,id) && !stopping;}
    void fail_locked(const std::string& why) {
        current=false; chunks.clear(); input_bytes=0; pcm.clear();
        report.current_segments=0; report.segmenter_pending_bytes=0;
        report.status="failed"; report.error=why; cv.notify_all();
    }
    bool put_frames(TtsStreamId id,std::vector<PaddedAudioFrame> frames) {
        for(auto &frame:frames) {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock,[&]{return !valid(id) || pcm.size()<frame_limit;});
            if(!valid(id)) return false;
            if(!report.first_pcm_ns)report.first_pcm_ns=now_ns();
            pcm.push_back({id,report.frames++,std::move(frame)});
            report.peak_pcm_frames=std::max(report.peak_pcm_frames,pcm.size());
            cv.notify_all();
        }
        return true;
    }
    void turn(TtsStreamId id) {
        TtsSegmenter segmenter(text);
        TtsPcmConverter converter(44100);
        std::deque<std::string> segments;
        auto pending_since=Clock::now();
        bool finished=false;
        const auto add=[&](std::vector<std::string> parts) {
            std::lock_guard<std::mutex> lock(mutex);
            if(!valid(id))return false;
            if(segments.size()+parts.size()>static_cast<std::size_t>(limits.tts_segments)) {
                fail_locked("pending TTS segment queue overflow");
                return false;
            }
            for(auto &part:parts)segments.push_back(std::move(part));
            report.current_segments=segments.size();
            report.segmenter_pending_bytes=segmenter.pending_bytes();
            report.peak_segments=std::max(report.peak_segments,segments.size());
            if(!parts.empty()){
                if(!report.first_segment_ns)report.first_segment_ns=now_ns();
                pending_since=Clock::now();
            }
            return true;
        };
        while(true) {
            if(segments.empty() && !finished) {
                std::string chunk;
                bool end=false;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait_for(lock,std::chrono::milliseconds(std::min(10,text.flush_ms)),
                        [&]{return !valid(id)||!chunks.empty()||input_end;});
                    if(!valid(id))return;
                    if(!chunks.empty()){
                        chunk=std::move(chunks.front());chunks.pop_front();
                        input_bytes-=chunk.size();
                    } else end=input_end;
                }
                if(!chunk.empty()) {
                    if(segmenter.pending_bytes()==0)pending_since=Clock::now();
                    if(!add(segmenter.push(chunk)))return;
                }
                if(end){
                    if(!add(segmenter.finish()))return;
                    finished=true;
                } else if(!add(segmenter.poll(static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-pending_since).count()))))return;
            }
            if(!segments.empty()) {
                std::string part;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(!valid(id))return;
                    part=std::move(segments.front());segments.pop_front();
                    report.current_segments=segments.size();
                }
                const auto before=Clock::now();
                auto audio=generate(part);
                const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-before).count();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(!valid(id)){++report.discarded_generation_results;return;}
                    if(elapsed>timeout_ms){fail_locked("blocking TTS segment timed out; result discarded");return;}
                    if(audio.sample_rate!=44100 || audio.samples.empty() ||
                       audio.samples.size()>static_cast<std::size_t>(44100)*limits.max_tts_audio_ms/1000){
                        fail_locked("invalid or oversized generated TTS segment");return;
                    }
                    ++report.segments;report.generate_seconds+=audio.generate_seconds;
                }
                if(!put_frames(id,converter.push(audio.samples.data(),audio.samples.size())))return;
            } else if(finished) {
                if(!put_frames(id,converter.finish()))return;
                std::lock_guard<std::mutex> lock(mutex);
                if(valid(id))report.status="completed";
                return;
            }
        }
    }
    void loop() {
        while(true) {
            TtsStreamId id;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock,[&]{return stopping||start_pending;});
                if(stopping)return;
                start_pending=false;id=report.id;
            }
            try {turn(id);}
            catch(const std::exception &e) {
                std::lock_guard<std::mutex> lock(mutex);
                if(valid(id))fail_locked(e.what());
            } catch(...) {
                std::lock_guard<std::mutex> lock(mutex);
                if(valid(id))fail_locked("unknown TTS worker error");
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                busy=false;report.current_segments=0;report.segmenter_pending_bytes=0;
                report.producer_done_ns=now_ns();cv.notify_all();
            }
        }
    }
};
TtsStreamWorker::TtsStreamWorker(Generate f,TtsConfig t,QueuesConfig q,int timeout)
    :impl_(std::make_unique<Impl>(std::move(f),t,q,timeout)){}
TtsStreamWorker::~TtsStreamWorker(){
    if(!impl_)return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping=true;impl_->current=false;impl_->chunks.clear();impl_->pcm.clear();impl_->cv.notify_all();
    }
    if(impl_->worker.joinable())impl_->worker.join();
}
void TtsStreamWorker::begin(TtsStreamId id){
    auto &i=*impl_;std::lock_guard<std::mutex> lock(i.mutex);
    if(!id.session || !id.generation || i.busy || i.stopping ||
       id.session<i.report.id.session ||
       (id.session==i.report.id.session && id.generation<=i.report.id.generation))
        throw std::logic_error("TTS stream: begin requires a newer nonzero identity and idle worker");
    i.report={};i.report.id=id;i.report.status="streaming";i.report.begin_ns=now_ns();
    i.chunks.clear();i.pcm.clear();i.input_bytes=0;i.input_end=false;
    i.current=true;i.busy=true;i.start_pending=true;i.cv.notify_all();
}
bool TtsStreamWorker::submit(TtsStreamId id,const std::string &chunk){
    auto &i=*impl_;std::lock_guard<std::mutex> lock(i.mutex);
    if(!i.valid(id)||i.input_end){++i.report.rejected_stale_inputs;return false;}
    if(chunk.size()>static_cast<std::size_t>(i.limits.llm_text_bytes)-i.input_bytes){
        i.fail_locked("incoming TTS text queue overflow");return false;
    }
    if(!chunk.empty()){
        i.input_bytes+=chunk.size();i.report.input_bytes+=chunk.size();
        i.report.peak_input_bytes=std::max(i.report.peak_input_bytes,i.input_bytes);
        i.chunks.push_back(chunk);i.cv.notify_all();
    }
    return true;
}
bool TtsStreamWorker::finish(TtsStreamId id){
    auto &i=*impl_;std::lock_guard<std::mutex> lock(i.mutex);
    if(!i.valid(id))return false;
    i.input_end=true;i.cv.notify_all();return true;
}
void TtsStreamWorker::cancel(TtsStreamId id){
    auto &i=*impl_;std::lock_guard<std::mutex> lock(i.mutex);
    if(!equal(i.report.id,id))return;
    i.current=false;i.input_end=true;i.chunks.clear();i.input_bytes=0;i.pcm.clear();
    i.report.current_segments=0;i.report.segmenter_pending_bytes=0;
    i.report.status="cancelled";i.report.cancel_ns=now_ns();i.cv.notify_all();
}
std::optional<TtsStreamFrame> TtsStreamWorker::poll(TtsStreamId id,int wait_ms){
    auto &i=*impl_;std::unique_lock<std::mutex> lock(i.mutex);
    if(wait_ms<0)throw std::invalid_argument("TTS stream: negative wait");
    if(wait_ms)i.cv.wait_for(lock,std::chrono::milliseconds(wait_ms),
        [&]{return !i.valid(id)||!i.pcm.empty()||!i.busy;});
    if(!i.valid(id)||i.pcm.empty())return {};
    auto frame=std::move(i.pcm.front());i.pcm.pop_front();++i.report.consumed_frames;i.cv.notify_all();return frame;
}
bool TtsStreamWorker::commit_if_current(TtsStreamId id,const std::function<void()> &sink){
    auto &i=*impl_;std::lock_guard<std::mutex> lock(i.mutex);
    if(!i.valid(id))return false;
    sink();return true;
}
bool TtsStreamWorker::wait_idle(int timeout_ms){
    auto &i=*impl_;std::unique_lock<std::mutex> lock(i.mutex);
    if(timeout_ms<0)throw std::invalid_argument("TTS stream: negative idle wait");
    return i.cv.wait_for(lock,std::chrono::milliseconds(timeout_ms),[&]{return !i.busy;});
}
TtsStreamStats TtsStreamWorker::stats()const{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto report=impl_->report;
    report.current_input_bytes=impl_->input_bytes;
    report.current_pcm_frames=impl_->pcm.size();
    return report;
}
} // namespace voice_engine
