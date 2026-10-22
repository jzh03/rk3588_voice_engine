#include "voice_engine/asr.hpp"
namespace voice_engine {
ListeningAsr::ListeningAsr(ZipformerAsr& asr,SileroVad& vad,const VadConfig& config)
    :ListeningAsr(ListeningBackends{
        [&asr,&vad](std::size_t prefix){asr.reset();vad.reset(prefix);},
        [&asr](const float* pcm,std::size_t n){asr.accept(pcm,n);},
        [&vad](const float* pcm,std::size_t n){vad.accept(pcm,n);},
        [&vad]{vad.finish();},
        [&asr]{return asr.finish();},
        [&asr]{return asr.partial();},
        [&asr]{return asr.stats();},
        [&vad]{return vad.stats();},
        [&vad]{return vad.segments();}
    },config) {}
} // namespace voice_engine
