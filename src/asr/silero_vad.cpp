#include "voice_engine/vad.hpp"
#include "voice_engine/probe_support.hpp"
#include "sherpa-onnx/c-api/c-api.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include "onnxruntime_c_api.h"
#include <string>

namespace voice_engine {
struct SileroVad::Impl {
    VadConfig config;
    VadWindowAssembler windows;
    VadStats stat;
    std::vector<VadSegment> spans;
    const SherpaOnnxVoiceActivityDetector* detector=nullptr;
    bool finished=false,active=false;
    std::uint64_t segment_base=0,active_start=0,pre_roll_samples=0,detector_windows=0;

    Impl(const std::filesystem::path& model,const VadConfig& c,int threads):config(c){
        probe::require_model_validation_lock();
        if(c.min_speech_ms<=0||c.max_speech_ms<c.min_speech_ms||c.end_silence_ms<=0||c.start_timeout_ms<=0||c.pre_roll_ms<0||threads<1)throw std::invalid_argument("invalid VAD configuration");
        if(std::string(SherpaOnnxGetVersionStr())!="1.12.14"||std::string(OrtGetApiBase()->GetVersionString())!="1.17.1")throw std::runtime_error("unexpected sherpa/ORT runtime version");
        // The fixed release uses old libstdc++ string ABI. Use its stable C API
        // while keeping the project's normal C++17 ABI and owning RAII wrapper.
        SherpaOnnxVadModelConfig v{};
        const auto model_string=model.string();
        v.silero_vad.model=model_string.c_str();
        v.silero_vad.threshold=0.5f;
        v.silero_vad.min_speech_duration=c.min_speech_ms/1000.0f;
        v.silero_vad.min_silence_duration=c.end_silence_ms/1000.0f;
        v.silero_vad.max_speech_duration=c.max_speech_ms/1000.0f;
        v.silero_vad.window_size=512;
        v.sample_rate=16000;
        v.num_threads=threads;
        v.provider="cpu";
        detector=SherpaOnnxCreateVoiceActivityDetector(&v,(c.max_speech_ms+c.pre_roll_ms+c.end_silence_ms)/1000.0f+2);
        if(!detector)throw std::runtime_error("Silero VAD creation failed");
    }
    ~Impl(){if(detector)SherpaOnnxDestroyVoiceActivityDetector(detector);}

    void drain(){
        while(!SherpaOnnxVoiceActivityDetectorEmpty(detector)){
            std::unique_ptr<const SherpaOnnxSpeechSegment,decltype(&SherpaOnnxDestroySpeechSegment)> segment(SherpaOnnxVoiceActivityDetectorFront(detector),SherpaOnnxDestroySpeechSegment);
            if(!segment)throw std::runtime_error("VAD returned null segment");
            SherpaOnnxVoiceActivityDetectorPop(detector);
            auto start=segment_base+static_cast<std::uint64_t>(std::max(0,segment->start));
            auto end=std::min(stat.input_samples,start+segment->n);
            if(end>start){
                if(spans.size()>=8192)throw std::length_error("VAD segment bound exceeded");
                auto pre=static_cast<std::uint64_t>(config.pre_roll_ms)*16;
                spans.push_back({start,end,start>pre?start-pre:0});
                stat.speech_detected=true;
            }
        }
    }
    void window(const float* p,std::size_t valid){
        if(valid>120*16000-stat.input_samples)throw std::length_error("VAD stream exceeds 120 s diagnostic bound");
        stat.input_samples+=valid;
        stat.tail_valid_samples=valid;
        ++stat.windows;
        if(stat.start_timed_out)return;
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(detector,p,512);
        ++detector_windows;
        stat.speech_detected=stat.speech_detected||SherpaOnnxVoiceActivityDetectorDetected(detector);
        drain();
        const bool detected=SherpaOnnxVoiceActivityDetectorDetected(detector);
        if(detected&&!active){
            const auto minimum=static_cast<std::uint64_t>(config.min_speech_ms)*16;
            // Match fixed sherpa 1.12.14's v5 segment timeline: WindowSize
            // is 512+64, WindowShift is 512, and the first 512 samples are
            // buffered before its first model window. On first detection the
            // upstream segment starts at buffer.Tail()-2*WindowSize-minimum.
            // Merely subtracting minimum from received PCM starts the hard
            // limit 1664 samples late (104 ms), independent of speech content.
            const auto model_tail=(detector_windows>1?detector_windows-1:0)*512;
            const auto lookback=2*(512+64)+minimum;
            active_start=segment_base+(model_tail>lookback?model_tail-lookback:0);
            if(!spans.empty())active_start=std::max(active_start,spans.back().end_sample);
            active=true;
        }
        if(!detected)active=false;
        // sherpa's max duration raises its threshold rather than imposing a
        // strict deadline. Enforce the configured endpoint at a window boundary.
        if(active&&stat.input_samples-active_start>=static_cast<std::uint64_t>(config.max_speech_ms)*16){
            SherpaOnnxVoiceActivityDetectorFlush(detector);
            drain();
            SherpaOnnxVoiceActivityDetectorReset(detector);
            SherpaOnnxVoiceActivityDetectorClear(detector);
            stat.max_duration_start_sample=active_start;
            segment_base=stat.input_samples;
            detector_windows=0;
            active=false;
            ++stat.max_duration_endpoints;
        }
        if(!stat.speech_detected&&stat.input_samples>=pre_roll_samples+static_cast<std::uint64_t>(config.start_timeout_ms)*16)stat.start_timed_out=true;
    }
};

SileroVad::SileroVad(const std::filesystem::path& p,const VadConfig& c,int t):impl_(std::make_unique<Impl>(p,c,t)){}
SileroVad::~SileroVad()=default;
void SileroVad::accept(const float* p,std::size_t n){impl_->windows.accept(p,n,[this](auto data,auto valid){impl_->window(data,valid);});}
void SileroVad::finish(){
    if(impl_->finished)return;
    impl_->windows.finish([this](auto p,auto n){impl_->window(p,n);});
    SherpaOnnxVoiceActivityDetectorFlush(impl_->detector);
    impl_->drain();
    impl_->finished=true;
}
void SileroVad::reset(std::size_t pre_roll_samples){
    if(pre_roll_samples>static_cast<std::uint64_t>(impl_->config.pre_roll_ms)*16)throw std::invalid_argument("VAD pre-roll exceeds configured bound");
    SherpaOnnxVoiceActivityDetectorReset(impl_->detector);
    SherpaOnnxVoiceActivityDetectorClear(impl_->detector);
    impl_->windows.reset();
    impl_->stat={};
    impl_->spans.clear();
    impl_->finished=false;
    impl_->active=false;
    impl_->segment_base=0;
    impl_->active_start=0;
    impl_->pre_roll_samples=pre_roll_samples;
    impl_->detector_windows=0;
}
VadStats SileroVad::stats()const{return impl_->stat;}
std::string SileroVad::runtime_info_json()const{return nlohmann::json({{"backend","CPU"},{"sherpa",SherpaOnnxGetVersionStr()},{"onnxruntime",OrtGetApiBase()->GetVersionString()}}).dump();}
const std::vector<VadSegment>& SileroVad::segments()const{return impl_->spans;}
struct BargeInDetector::Impl {
    VadWindowAssembler windows;
    const SherpaOnnxVoiceActivityDetector* detector = nullptr;
    bool hit = false;
    Impl(const std::filesystem::path& model, int minimum_ms, int threads) {
        probe::require_model_validation_lock();
        if (minimum_ms < 20 || minimum_ms > 5000 || threads < 1)
            throw std::invalid_argument("invalid barge-in VAD configuration");
        if (std::string(SherpaOnnxGetVersionStr()) != "1.12.14" ||
            std::string(OrtGetApiBase()->GetVersionString()) != "1.17.1")
            throw std::runtime_error("unexpected sherpa/ORT runtime version");
        SherpaOnnxVadModelConfig v{};
        const auto name = model.string();
        v.silero_vad.model = name.c_str();
        v.silero_vad.threshold = 0.5f;
        v.silero_vad.min_speech_duration = minimum_ms / 1000.0f;
        v.silero_vad.min_silence_duration = 0.6f;
        v.silero_vad.max_speech_duration = 15.0f;
        v.silero_vad.window_size = 512;
        v.sample_rate = 16000; v.num_threads = threads; v.provider = "cpu";
        detector = SherpaOnnxCreateVoiceActivityDetector(&v, minimum_ms / 1000.0f + 2.0f);
        if (!detector) throw std::runtime_error("barge-in VAD creation failed");
    }
    ~Impl() { if (detector) SherpaOnnxDestroyVoiceActivityDetector(detector); }
};
BargeInDetector::BargeInDetector(const std::filesystem::path& p, int m, int t)
    : impl_(std::make_unique<Impl>(p, m, t)) {}
BargeInDetector::~BargeInDetector() = default;
void BargeInDetector::reset() {
    SherpaOnnxVoiceActivityDetectorReset(impl_->detector);
    SherpaOnnxVoiceActivityDetectorClear(impl_->detector);
    impl_->windows.reset(); impl_->hit = false;
}
bool BargeInDetector::accept(const float* pcm, std::size_t count) {
    impl_->windows.accept(pcm, count, [this](const float* p, std::size_t) {
        if (impl_->hit) return;
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(impl_->detector, p, 512);
        impl_->hit = SherpaOnnxVoiceActivityDetectorDetected(impl_->detector);
        // Never retain completed segments or a full playback-length recording.
        SherpaOnnxVoiceActivityDetectorClear(impl_->detector);
    });
    return impl_->hit;
}
} // namespace voice_engine
