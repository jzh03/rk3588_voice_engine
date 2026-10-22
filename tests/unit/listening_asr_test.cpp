#include "voice_engine/asr.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace voice_engine;
namespace {
void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
template<class E,class F> void rejects(F&& f,const char* why){
    bool caught=false;try{f();}catch(const E&){caught=true;}require(caught,why);
}
struct Fake {
    AsrStats asr;
    VadStats vad;
    std::vector<VadSegment> segments;
    std::vector<float> pcm,last_pcm;
    std::size_t prefix=0,reset_count=0,finish_count=0;
    std::uint64_t speech_at=512,end_at=4096,max_at=0,timeout=80000;
    bool throw_accept=false,throw_finish=false;
    void evaluate(){
        if(speech_at&&vad.input_samples>=speech_at)vad.speech_detected=true;
        if(!vad.speech_detected&&vad.input_samples>=prefix+timeout)vad.start_timed_out=true;
        if(max_at&&vad.input_samples>=max_at)vad.max_duration_endpoints=1;
        if(end_at&&vad.speech_detected&&vad.input_samples>=end_at&&segments.empty())
            segments.push_back({0,end_at,0});
    }
    ListeningBackends bindings(){
        return {
            [this](std::size_t n){last_pcm=pcm;pcm.clear();asr={};vad={};segments.clear();prefix=n;++reset_count;},
            [this](const float* p,std::size_t n){
                if(throw_accept)throw std::runtime_error("injected accept failure");
                pcm.insert(pcm.end(),p,p+n);asr.input_samples+=n;asr.encoder_chunks=asr.input_samples/15360;
            },
            [this](const float*,std::size_t n){vad.input_samples+=n;vad.tail_valid_samples=n;++vad.windows;if(n==512)evaluate();},
            [this]{evaluate();},
            [this]{++finish_count;if(throw_finish)throw std::runtime_error("injected finish failure");return std::string("fresh:")+std::to_string(asr.input_samples);},
            [this]{return pcm.empty()?std::string():std::string("partial");},
            [this]{return asr;},[this]{return vad;},[this]{return segments;}
        };
    }
};
}
int main(){try {
    VadConfig config;
    std::vector<float> prefix(641),audio(10000);
    for(std::size_t i=0;i<prefix.size();++i)prefix[i]=float(i%101)/101;
    for(std::size_t i=0;i<audio.size();++i)audio[i]=-float(i%97)/97;
    for(const auto chunk:{160u,320u,592u}) {
        Fake fake;
        ListeningAsr turn(fake.bindings(),config);
        turn.begin(prefix.data(),prefix.size());
        require(fake.prefix==641,"pre-roll timeout offset missing");
        std::size_t consumed=0;
        while(turn.state()==ListeningState::listening) {
            consumed+=turn.accept(audio.data()+consumed,chunk);
            require(turn.buffered_samples()<512,"pending bound");
        }
        const auto& result=turn.result();
        require(result.reason==ListeningEnd::vad_silence,"normal VAD endpoint");
        require(consumed==4096-641&&result.input_samples==consumed,"exact post-endpoint remainder");
        require(result.endpoint_sample==4096&&result.pre_roll_samples==641,"prefix coordinates");
        require(result.asr.input_samples==4096&&result.vad.input_samples==4096,"same valid input");
        auto expected=prefix;
        expected.insert(expected.end(),audio.begin(),audio.begin()+consumed);
        require(fake.last_pcm==expected,"first/tail PCM duplicated, padded or lost");
        require(result.backends_reset&&fake.pcm.empty()&&turn.partial().empty(),"reset after final");
        require(turn.finish().hypothesis=="fresh:4096"&&fake.finish_count==1,"finish idempotence");
        rejects<std::logic_error>([&]{turn.accept(audio.data(),1);},"post-endpoint input rejected");

        // A cancelled partial frame must never be fed/flushed into a new turn.
        fake.end_at=0;
        turn.begin();turn.accept(audio.data(),chunk);
        const auto finished=fake.finish_count;
        turn.cancel();turn.cancel();
        require(turn.state()==ListeningState::cancelled&&turn.buffered_samples()==0&&
            turn.result().hypothesis.empty()&&fake.finish_count==finished,"cancel decoded or retained input");
        rejects<std::logic_error>([&]{turn.finish();},"cancelled finish rejected");
        turn.begin();turn.accept(audio.data()+6000,731);
        require(turn.finish().hypothesis=="fresh:731","cancel/new turn isolation");
        require(fake.last_pcm==std::vector<float>(audio.begin()+6000,audio.begin()+6731),
                "new round must contain only new PCM");
        require(turn.result().vad.tail_valid_samples==219,"EOF valid VAD tail");
        require(turn.result().asr.input_samples==731,"EOF original length");
        require(turn.result().reason==ListeningEnd::input_finished,"manual finish reason");

        // Start replaces an unfinished round without decoding its tail.
        turn.begin();turn.accept(audio.data(),137);turn.begin();turn.finish();
        require(turn.result().hypothesis.empty()&&turn.result().asr.input_samples==0,
                "begin replacement leaked old prefix");

        // Long-speech termination consumes only the endpoint quantum.
        fake.max_at=1536;
        turn.begin();
        std::size_t n=0;
        while(turn.state()==ListeningState::listening)n+=turn.accept(audio.data()+n,chunk);
        require(n==1536&&turn.result().reason==ListeningEnd::max_speech&&
                turn.result().asr_finish_called,"long speech endpoint missing");
    }

    Fake silent;
    silent.speech_at=0;silent.end_at=0;silent.timeout=1600;
    ListeningAsr wait(silent.bindings(),config);
    wait.begin(prefix.data(),641);
    std::size_t fresh=0;
    while(wait.state()==ListeningState::listening)fresh+=wait.accept(audio.data()+fresh,592);
    require(fresh>=1600&&fresh<1600+512,"start timer includes pre-roll");
    require(wait.result().reason==ListeningEnd::start_timeout&&wait.result().hypothesis.empty()&&
            !wait.result().asr_finish_called&&silent.finish_count==0,"silent timeout must not final-decode");
    wait.begin();wait.accept(audio.data(),80);wait.finish();
    require(wait.result().hypothesis.empty()&&wait.result().asr.input_samples==80&&
            wait.result().vad.tail_valid_samples==80,"short input valid length");
    wait.begin();wait.finish();
    require(wait.result().endpoint_sample==0&&wait.result().hypothesis.empty(),"empty stop");

    // Speech confirmation in the short EOF window must still be finalized.
    Fake short_speech;short_speech.speech_at=100;short_speech.end_at=0;
    ListeningAsr short_turn(short_speech.bindings(),config);
    short_turn.begin();short_turn.accept(audio.data(),137);short_turn.finish();
    require(short_turn.result().hypothesis=="fresh:137"&&short_turn.result().asr_finish_called,
            "short speech confirmation at EOF lost");
    short_turn.begin();
    const float nan=std::numeric_limits<float>::quiet_NaN();
    rejects<std::invalid_argument>([&]{short_turn.accept(&nan,1);},"NaN rejection");
    require(short_turn.buffered_samples()==0,"invalid input changed pending");
    rejects<std::length_error>([&]{short_turn.begin(audio.data(),8001);},"pre-roll bound");
    rejects<std::length_error>([&]{short_turn.accept(audio.data(),16001);},"transport block bound");
    short_speech.throw_accept=true;
    rejects<std::runtime_error>([&]{short_turn.accept(audio.data(),512);},"accept error handling");
    require(short_turn.state()==ListeningState::error&&short_speech.pcm.empty()&&
            short_turn.result().hypothesis.empty(),"error cleanup");
    short_speech.throw_accept=false;short_speech.throw_finish=true;
    short_turn.begin();short_turn.accept(audio.data(),512);
    rejects<std::runtime_error>([&]{short_turn.finish();},"finish error handling");
    require(short_turn.state()==ListeningState::error&&short_speech.pcm.empty(),"finish error reset");
    short_speech.throw_finish=false;
    short_turn.begin();short_turn.accept(audio.data()+1000,777);short_turn.finish();
    require(short_turn.result().hypothesis=="fresh:777","recovery after backend failure");
    std::cout<<"LISTENING_ASR_OK\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
