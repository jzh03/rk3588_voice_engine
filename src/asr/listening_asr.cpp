#include "voice_engine/asr.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace voice_engine {
namespace {
void validate_pcm(const float* pcm, std::size_t count) {
    if(count&&!pcm)throw std::invalid_argument("null PCM");
    for(std::size_t i=0;i<count;++i)
        if(!std::isfinite(pcm[i])||std::abs(pcm[i])>1.001f)
            throw std::invalid_argument("PCM must be finite normalized float32");
}
}
const char* listening_end_name(ListeningEnd reason) {
    switch(reason) {
        case ListeningEnd::none: return "none";
        case ListeningEnd::vad_silence: return "vad_silence";
        case ListeningEnd::start_timeout: return "start_timeout";
        case ListeningEnd::max_speech: return "max_speech";
        case ListeningEnd::input_finished: return "input_finished";
        case ListeningEnd::cancelled: return "cancelled";
        case ListeningEnd::error: return "error";
    }
    return "error";
}
ListeningAsr::ListeningAsr(ListeningBackends backends,const VadConfig& config)
    :backends_(std::move(backends)),config_(config) {
    if(!backends_.reset||!backends_.accept_asr||!backends_.accept_vad||
       !backends_.finish_vad||!backends_.finish_asr||!backends_.partial_asr||
       !backends_.asr_stats||!backends_.vad_stats||!backends_.vad_segments)
        throw std::invalid_argument("incomplete LISTENING backends");
    if(config.pre_roll_ms<0||config.pre_roll_ms>120000)
        throw std::invalid_argument("invalid LISTENING pre-roll bound");
}
void ListeningAsr::reset_backends(std::size_t prefix) {
    backends_.reset(prefix);
    if(!backends_.partial_asr().empty()||backends_.asr_stats().input_samples||
       backends_.asr_stats().encoder_chunks||backends_.vad_stats().input_samples||
       backends_.vad_stats().speech_detected||!backends_.vad_segments().empty())
        throw std::runtime_error("LISTENING backend reset did not clear state");
    used_=0;
    pending_.fill(0);
}
void ListeningAsr::fail() noexcept {
    state_=ListeningState::error;
    result_.reason=ListeningEnd::error;
    result_.hypothesis.clear();
    result_.unfiltered_hypothesis.clear();
    used_=0;
    pending_.fill(0);
    try { backends_.reset(0); } catch(...) {}
}
void ListeningAsr::begin(const float* prefix,std::size_t count,std::int64_t start_deadline_ns) {
    if(start_deadline_ns<0)throw std::invalid_argument("negative LISTENING start deadline");
    if(count>static_cast<std::uint64_t>(config_.pre_roll_ms)*16)
        throw std::length_error("LISTENING pre-roll exceeds configured bound");
    validate_pcm(prefix,count);
    try {
        reset_backends(count);
        result_={};
        start_deadline_ns_=start_deadline_ns;
        result_.pre_roll_samples=count;
        state_=ListeningState::listening;
        // Prefix is before the trigger; endpoint dispatch starts with new input.
        while(count) {
            const auto take=std::min(count,pending_.size()-used_);
            std::copy_n(prefix,take,pending_.begin()+used_);
            prefix+=take;count-=take;used_+=take;
            if(used_==pending_.size()) {
                consume(pending_.data(),used_,false);
                used_=0;
            }
        }
    } catch(...) { fail();throw; }
}
bool ListeningAsr::poll_start_timeout(std::int64_t now_ns) {
    if(state_!=ListeningState::listening||!start_deadline_ns_||now_ns<start_deadline_ns_)
        return false;
    // A dialogue's opening deadline must never truncate an established utterance.
    if(backends_.vad_stats().speech_detected) { start_deadline_ns_=0;return false; }
    try {
        // Keep the last valid samples; do not synthesize missing capture audio.
        if(used_) {
            const auto valid=used_;used_=0;
            consume(pending_.data(),valid,true);
            if(state_==ListeningState::final)return true;
        }
        if(backends_.vad_stats().speech_detected) { start_deadline_ns_=0;return false; }
        endpoint_time_=std::chrono::steady_clock::now();
        complete(ListeningEnd::start_timeout);
        return true;
    } catch(...) { fail();throw; }
}
void ListeningAsr::consume(const float* pcm,std::size_t count,bool check_endpoint) {
    // No zero padded VAD sample is submitted as original ASR audio.
    backends_.accept_vad(pcm,count);
    endpoint_time_=std::chrono::steady_clock::now();
    backends_.accept_asr(pcm,count);
    if(check_endpoint)endpoint();
}
bool ListeningAsr::endpoint() {
    const auto stats=backends_.vad_stats();
    if(stats.start_timed_out)complete(ListeningEnd::start_timeout);
    else if(stats.max_duration_endpoints)complete(ListeningEnd::max_speech);
    else if(!backends_.vad_segments().empty())complete(ListeningEnd::vad_silence);
    return state_==ListeningState::final;
}
std::size_t ListeningAsr::accept(const float* pcm,std::size_t count) {
    if(state_!=ListeningState::listening)throw std::logic_error("LISTENING begin required before accept");
    if(count>16000)throw std::length_error("LISTENING accept exceeds 1 second");
    validate_pcm(pcm,count);
    if(count>120*16000-result_.pre_roll_samples-result_.input_samples)
        throw std::length_error("LISTENING stream exceeds 120 seconds");
    const auto supplied=count;
    try {
        while(count&&state_==ListeningState::listening) {
            const auto take=std::min(count,pending_.size()-used_);
            std::copy_n(pcm,take,pending_.begin()+used_);
            pcm+=take;count-=take;used_+=take;result_.input_samples+=take;
            if(used_==pending_.size()) {
                const auto valid=used_;
                used_=0;
                consume(pending_.data(),valid,true);
            }
        }
        return supplied-count;
    } catch(...) { fail();throw; }
}
void ListeningAsr::complete(ListeningEnd reason) {
    state_=ListeningState::finalizing;
    const auto endpoint_time=endpoint_time_;
    backends_.finish_vad();
    result_.reason=reason;
    result_.chunks_before_finish=backends_.asr_stats().encoder_chunks;
    result_.vad=backends_.vad_stats();
    result_.segments=backends_.vad_segments();
    result_.asr_finish_called=result_.vad.speech_detected&&reason!=ListeningEnd::start_timeout;
    result_.unfiltered_hypothesis=result_.asr_finish_called?
        backends_.finish_asr():backends_.partial_asr();
    if(result_.asr_finish_called)result_.hypothesis=result_.unfiltered_hypothesis;
    result_.asr=backends_.asr_stats();
    result_.endpoint_sample=result_.asr.input_samples;
    reset_backends(0);
    const auto asr=backends_.asr_stats();
    const auto vad=backends_.vad_stats();
    result_.backends_reset=backends_.partial_asr().empty()&&asr.input_samples==0&&
        asr.encoder_chunks==0&&vad.input_samples==0&&!vad.speech_detected&&
        !vad.start_timed_out&&backends_.vad_segments().empty();
    if(!result_.backends_reset)throw std::runtime_error("LISTENING backend reset failed");
    result_.endpoint_to_final_sec=std::chrono::duration<double>(
        std::chrono::steady_clock::now()-endpoint_time).count();
    state_=ListeningState::final;
}
const ListeningResult& ListeningAsr::finish() {
    if(state_==ListeningState::final)return result_;
    if(state_!=ListeningState::listening)throw std::logic_error("LISTENING begin required before finish");
    try {
        const auto manual_stop=std::chrono::steady_clock::now();
        if(used_) {
            const auto valid=used_;
            used_=0;
            consume(pending_.data(),valid,false);
        }
        // Finish exposes the final partial VAD window using its valid length.
        // EOF itself is not a VAD silence endpoint.
        const auto before=backends_.vad_segments().size();
        backends_.finish_vad();
        const auto stats=backends_.vad_stats();
        auto reason=ListeningEnd::input_finished;
        if(stats.start_timed_out)reason=ListeningEnd::start_timeout;
        else if(stats.max_duration_endpoints)reason=ListeningEnd::max_speech;
        else if(before)reason=ListeningEnd::vad_silence;
        endpoint_time_=reason==ListeningEnd::input_finished?manual_stop:std::chrono::steady_clock::now();
        complete(reason);
        return result_;
    } catch(...) { fail();throw; }
}
void ListeningAsr::cancel() {
    try {
        reset_backends(0);
        result_={};
        result_.reason=ListeningEnd::cancelled;
        result_.backends_reset=true;
        state_=ListeningState::cancelled;
    } catch(...) { fail();throw; }
}
std::string ListeningAsr::partial() const {
    return state_==ListeningState::listening?backends_.partial_asr():std::string();
}
} // namespace voice_engine
