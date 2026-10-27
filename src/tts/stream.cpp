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
    CancellableGenerate generate;
    std::shared_ptr<TtsCancellation> cancellation;
    TtsConfig text;
    QueuesConfig limits;
    int timeout_ms;
    const std::size_t frame_limit;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker, publisher;
    bool producer_busy=false, publisher_busy=false, publish_pending=false;
    bool native_generating=false, native_publishing=false, synthesis_done=false;
    struct NativeSegment { TtsNativeAudio audio; std::size_t timing_index; };
    std::deque<NativeSegment> native;
    std::vector<TtsSegmentTiming> timings;
    std::uint64_t next_segment_seq=0;
    static constexpr std::size_t no_timing=kTtsSegmentTimingCapacity;
    TtsSegmentTiming* timing(std::size_t index) {
        return index<timings.size() ? &timings[index] : nullptr;
    }
    std::size_t native_samples=0;
    bool stopping=false, busy=false, current=false, start_pending=false, input_end=false;
    std::size_t input_bytes=0;
    std::deque<std::string> chunks;
    std::deque<TtsStreamFrame> pcm;
    TtsStreamStats report;
    Impl(CancellableGenerate f,TtsConfig t,QueuesConfig q,int timeout)
        :generate(std::move(f)),text(t),limits(q),timeout_ms(timeout),
         frame_limit(static_cast<std::size_t>(q.playback_ms/20)) {
        if(!generate || q.llm_text_bytes<1 || q.llm_text_bytes>8192 ||
           q.tts_segments<1 || q.tts_segments>64 || q.playback_ms<20 ||
           q.playback_ms>2000 || q.max_tts_audio_ms<1 || q.max_tts_audio_ms>20000 || timeout<1 ||
           t.prebuffer_ms<0 || t.prebuffer_ms>2000)
            throw std::invalid_argument("TTS stream: invalid bounds");
        TtsSegmenter check(text);
        timings.reserve(kTtsSegmentTimingCapacity);
        worker=std::thread([this]{loop();});
        try {
            if(text.prebuffer_ms)publisher=std::thread([this]{publish_loop();});
        } catch(...) {
            { std::lock_guard<std::mutex> lock(mutex);stopping=true;cv.notify_all(); }
            worker.join();throw;
        }
    }
    bool valid(TtsStreamId id)const{return current && equal(report.id,id) && !stopping;}
    void fail_locked(const std::string& why) {
        if(cancellation)cancellation->cancel();
        current=false; chunks.clear(); input_bytes=0; pcm.clear(); native.clear(); native_samples=0;
        report.current_segments=0; report.segmenter_pending_bytes=0;
        report.status="failed"; report.error=why; cv.notify_all();
    }
    bool put_frames(TtsStreamId id,std::vector<PaddedAudioFrame> frames,
                    std::size_t timing_index=no_timing) {
        for(auto &frame:frames) {
            std::unique_lock<std::mutex> lock(mutex);
            const auto wait_begin=now_ns();
            cv.wait(lock,[&]{return !valid(id) || pcm.size()<frame_limit;});
            if(!valid(id)) return false;
            const auto enqueued=now_ns();
            if(!report.first_pcm_ns)report.first_pcm_ns=enqueued;
            if(auto* t=timing(timing_index)) {
                t->pcm_queue_wait_ns+=enqueued-wait_begin;
                if(!t->pcm_frames) {
                    t->first_pcm_seq=report.frames;t->first_pcm_enqueued_ns=enqueued;
                }
                t->last_pcm_seq=report.frames;t->last_pcm_enqueued_ns=enqueued;
                ++t->pcm_frames;
            }
            pcm.push_back({id,report.frames++,std::move(frame)});
            report.peak_pcm_frames=std::max(report.peak_pcm_frames,pcm.size());
            cv.notify_all();
        }
        return true;
    }
    bool publish_audio(TtsStreamId id,TtsPcmConverter& converter,
                       const TtsNativeAudio& audio,std::size_t timing_index) {
        const auto begin=now_ns();
        auto frames=converter.push(audio.samples.data(),audio.samples.size());
        const auto converted=now_ns();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(!valid(id))return false;
            if(auto* t=timing(timing_index)) {
                t->publish_begin_ns=begin;t->resample_end_ns=converted;
            }
        }
        if(!put_frames(id,std::move(frames),timing_index))return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(!valid(id))return false;
            if(auto* t=timing(timing_index))t->publish_end_ns=now_ns();
        }
        return true;
    }
    bool reserve_native(TtsStreamId id) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock,[&]{return !valid(id) || native.size()+native_publishing+native_generating<2;});
        if(!valid(id))return false;
        native_generating=true;
        report.peak_native_slots=std::max(report.peak_native_slots,
            native.size()+static_cast<std::size_t>(native_publishing)+1);
        return true;
    }
    void publish_turn(TtsStreamId id) {
        TtsPcmConverter converter(44100);
        bool released=false;
        while(true) {
            TtsNativeAudio audio;
            std::size_t timing_index=no_timing;
            bool end=false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock,[&] {
                    if(!valid(id))return true;
                    if(!released) {
                        // Two short segments must also release at capacity,
                        // otherwise the producer could never reserve slot three.
                        return synthesis_done || native.size()>=2 ||
                            native_samples>=static_cast<std::size_t>(44100)*text.prebuffer_ms/1000;
                    }
                    return synthesis_done || !native.empty();
                });
                if(!valid(id))return;
                if(!released && !native.empty()) {
                    released=true;report.playback_release_ns=now_ns();
                }
                if(!native.empty()) {
                    timing_index=native.front().timing_index;
                    audio=std::move(native.front().audio);native.pop_front();native_publishing=true;
                } else end=synthesis_done;
            }
            if(end) {
                if(!put_frames(id,converter.finish()))return;
                std::lock_guard<std::mutex> lock(mutex);
                if(valid(id))report.status="completed";
                return;
            }
            if(audio.samples.empty())continue;
            if(!publish_audio(id,converter,audio,timing_index))return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(!valid(id))return;
                native_samples-=audio.samples.size();
                // Free the previous waveform before opening another native slot.
                std::vector<float>().swap(audio.samples);
                native_publishing=false;cv.notify_all();
            }
        }
    }
    void publish_loop() {
        while(true) {
            TtsStreamId id;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock,[&]{return stopping||publish_pending;});
                if(stopping)return;
                publish_pending=false;id=report.id;
            }
            try {publish_turn(id);}
            catch(const std::exception& e) {
                std::lock_guard<std::mutex> lock(mutex);if(valid(id))fail_locked(e.what());
            } catch(...) {
                std::lock_guard<std::mutex> lock(mutex);if(valid(id))fail_locked("unknown PCM publisher error");
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                native_publishing=false;publisher_busy=false;busy=producer_busy;
                if(!busy)report.producer_done_ns=now_ns();
                cv.notify_all();
            }
        }
    }
    void turn(TtsStreamId id, const std::shared_ptr<TtsCancellation>& token) {
        TtsSegmenter segmenter(text);
        TtsPcmConverter converter(44100);
        struct SelectedSegment { std::string text; std::size_t timing_index; };
        std::deque<SelectedSegment> segments;
        auto pending_since=Clock::now();
        bool finished=false;
        const auto add=[&](std::vector<std::string> parts) {
            std::lock_guard<std::mutex> lock(mutex);
            if(!valid(id))return false;
            if(segments.size()+parts.size()>static_cast<std::size_t>(limits.tts_segments)) {
                fail_locked("pending TTS segment queue overflow");
                return false;
            }
            for(auto &part:parts) {
                const auto seq=next_segment_seq++;
                std::size_t index=no_timing;
                if(timings.size()<kTtsSegmentTimingCapacity) {
                    index=timings.size();
                    TtsSegmentTiming t;t.segment_seq=seq;t.text_bytes=part.size();
                    t.text_chars=std::count_if(part.begin(),part.end(),
                        [](unsigned char c){return (c&0xc0)!=0x80;});
                    const bool first=seq==0 && text.first_segment.has_value();
                    t.min_chars=first ? text.first_segment->min_chars : text.min_chars;
                    t.target_chars=first ? text.first_segment->target_chars : text.target_chars;
                    t.boundary_lookahead_chars=first ?
                        text.first_segment->boundary_lookahead_chars : text.boundary_lookahead_chars;
                    t.text_selected_ns=now_ns();timings.push_back(t);
                } else ++report.segment_timings_dropped;
                segments.push_back({std::move(part),index});
            }
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
                bool end=false,more_input=false;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait_for(lock,std::chrono::milliseconds(std::min(10,text.flush_ms)),
                        [&]{return !valid(id)||!chunks.empty()||input_end;});
                    if(!valid(id))return;
                    if(!chunks.empty()){
                        chunk=std::move(chunks.front());chunks.pop_front();
                        input_bytes-=chunk.size();
                    } else end=input_end;
                    more_input=!chunks.empty();
                    if(text.sentence_mode && input_end && !more_input)end=true;
                }
                if(!chunk.empty()) {
                    if(segmenter.pending_bytes()==0)pending_since=Clock::now();
                    if(!add(segmenter.push(chunk)))return;
                }
                if(end){
                    if(!add(segmenter.finish()))return;
                    finished=true;
                } else if(!((text.sentence_mode || text.first_segment.has_value()) && more_input) &&
                          !add(segmenter.poll(static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-pending_since).count()))))return;
            }
            if(!segments.empty()) {
                std::string part;
                std::size_t timing_index=no_timing;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(!valid(id))return;
                    timing_index=segments.front().timing_index;
                    part=std::move(segments.front().text);segments.pop_front();
                    report.current_segments=segments.size();
                }
                const auto wait_begin=now_ns();
                if(text.prebuffer_ms && !reserve_native(id))return;
                const auto acquired=now_ns();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(!valid(id))return;
                    if(auto* t=timing(timing_index)) {
                        t->native_wait_begin_ns=wait_begin;t->native_slot_acquired_ns=acquired;
                        t->input_queued_bytes=input_bytes;t->pcm_frames_before_generate=pcm.size();
                        t->native_samples_before_generate=native_samples;
                    }
                }
                const auto generate_begin=now_ns();
                auto audio=generate(part,token);
                const auto generate_end=now_ns();
                const auto elapsed=(generate_end-generate_begin)/1000000;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    native_generating=false;
                    if(!valid(id)){++report.discarded_generation_results;return;}
                    if(elapsed>timeout_ms){fail_locked("blocking TTS segment timed out; result discarded");return;}
                    if(audio.sample_rate!=44100 || audio.samples.empty() ||
                       audio.samples.size()>static_cast<std::size_t>(44100)*limits.max_tts_audio_ms/1000){
                        fail_locked("invalid or oversized generated TTS segment");return;
                    }
                    ++report.segments;report.generate_seconds+=audio.generate_seconds;
                    if(auto* t=timing(timing_index)) {
                        t->generate_begin_ns=generate_begin;t->generate_end_ns=generate_end;
                        t->sdk_generate_seconds=audio.generate_seconds;
                        t->native_samples=audio.samples.size();t->native_sample_rate=audio.sample_rate;
                        t->native_enqueued_ns=now_ns();
                    }
                    if(!report.first_generated_audio_ns)report.first_generated_audio_ns=now_ns();
                    if(text.prebuffer_ms) {
                        native_samples+=audio.samples.size();
                        report.peak_native_samples=std::max(report.peak_native_samples,native_samples);
                        native.push_back({std::move(audio),timing_index});cv.notify_all();
                    }
                }
                if(!text.prebuffer_ms && !publish_audio(id,converter,audio,timing_index))return;
            } else if(finished) {
                if(text.prebuffer_ms) {
                    std::lock_guard<std::mutex> lock(mutex);
                    synthesis_done=true;cv.notify_all();return;
                }
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
            std::shared_ptr<TtsCancellation> token;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock,[&]{return stopping||start_pending;});
                if(stopping)return;
                start_pending=false;id=report.id;
                token=cancellation;
            }
            try {turn(id,token);}
            catch(const std::exception &e) {
                std::lock_guard<std::mutex> lock(mutex);
                if(valid(id))fail_locked(e.what());
            } catch(...) {
                std::lock_guard<std::mutex> lock(mutex);
                if(valid(id))fail_locked("unknown TTS worker error");
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                native_generating=false;producer_busy=false;busy=publisher_busy;
                report.current_segments=0;report.segmenter_pending_bytes=0;
                if(!busy)report.producer_done_ns=now_ns();
                cv.notify_all();
            }
        }
    }
};
TtsStreamWorker::TtsStreamWorker(Generate f,TtsConfig t,QueuesConfig q,int timeout)
    :TtsStreamWorker(f ? CancellableGenerate([f=std::move(f)](
        const std::string& text, const std::shared_ptr<TtsCancellation>&){return f(text);})
        : CancellableGenerate{},t,q,timeout){}
TtsStreamWorker::TtsStreamWorker(CancellableGenerate f,TtsConfig t,QueuesConfig q,int timeout)
    :impl_(std::make_unique<Impl>(std::move(f),t,q,timeout)){}
TtsStreamWorker::~TtsStreamWorker(){
    if(!impl_)return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(impl_->cancellation)impl_->cancellation->cancel();
        impl_->stopping=true;impl_->current=false;impl_->chunks.clear();impl_->pcm.clear();
        impl_->native.clear();impl_->native_samples=0;impl_->cv.notify_all();
    }
    if(impl_->worker.joinable())impl_->worker.join();
    if(impl_->publisher.joinable())impl_->publisher.join();
}
void TtsStreamWorker::begin(TtsStreamId id){
    auto &i=*impl_;std::lock_guard<std::mutex> lock(i.mutex);
    if(!id.session || !id.generation || i.busy || i.stopping ||
       id.session<i.report.id.session ||
       (id.session==i.report.id.session && id.generation<=i.report.id.generation))
        throw std::logic_error("TTS stream: begin requires a newer nonzero identity and idle worker");
    i.cancellation=std::make_shared<TtsCancellation>();
    i.report={};i.report.id=id;i.report.status="streaming";i.report.begin_ns=now_ns();
    i.timings.clear();i.next_segment_seq=0;
    i.chunks.clear();i.pcm.clear();i.input_bytes=0;i.input_end=false;
    i.native.clear();i.native_samples=0;i.native_generating=false;i.native_publishing=false;i.synthesis_done=false;
    i.current=true;i.busy=true;i.producer_busy=true;i.start_pending=true;
    i.publisher_busy=i.publish_pending=i.text.prebuffer_ms>0;i.cv.notify_all();
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
    if(i.cancellation)i.cancellation->cancel();
    i.current=false;i.input_end=true;i.chunks.clear();i.input_bytes=0;i.pcm.clear();
    i.native.clear();i.native_samples=0;
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
    if(impl_->cancellation) {
        report.native_run_started_ns=impl_->cancellation->native_run_started_ns.load();
        report.native_runs=impl_->cancellation->native_runs.load();
    }
    report.current_input_bytes=impl_->input_bytes;
    report.current_pcm_frames=impl_->pcm.size();
    report.native_buffered_segments=impl_->current ?
        impl_->native.size()+static_cast<std::size_t>(impl_->native_publishing) : 0;
    report.native_buffered_samples=impl_->native_samples;
    return report;
}
std::vector<TtsSegmentTiming> TtsStreamWorker::segment_timings(TtsStreamId id)const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(!impl_->valid(id))return {};
    return impl_->timings;
}
} // namespace voice_engine
