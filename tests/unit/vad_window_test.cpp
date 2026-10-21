#include "voice_engine/vad.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using voice_engine::VadWindowAssembler;
void require(bool b){if(!b)throw std::runtime_error("VAD aggregation assertion failed");
}
int main(){try{
    std::vector<float> pcm(16017);
    for(std::size_t i=0;i<pcm.size();++i)pcm[i]=(i%100)/100.0f;
    for(auto chunk:{160u,320u,592u}){
        VadWindowAssembler a;
        std::vector<float> result;
        std::size_t calls=0,last=0;
        auto sink=[&](const float* p,std::size_t n){++calls;
        last=n;
        result.insert(result.end(),p,p+n);
        for(auto i=n;i<512;++i)require(p[i]==0);
        };
        for(std::size_t i=0;i<pcm.size();i+=chunk){a.accept(pcm.data()+i,std::min<std::size_t>(chunk,pcm.size()-i),sink);
        require(a.buffered()<512);
        }
        a.finish(sink);
        a.finish(sink);
        require(result==pcm&&calls==32&&last==145);
        bool threw=false;
        try{a.accept(pcm.data(),1,sink);
        }catch(const std::logic_error&){threw=true;
        }require(threw);
        a.reset();
        result.clear();
        calls=0;
        a.finish(sink);
        require(calls==0);
        a.reset();
        float nan=std::numeric_limits<float>::quiet_NaN();
        threw=false;
        try{a.accept(&nan,1,sink);
        }catch(const std::invalid_argument&){threw=true;
        }require(threw);
        a.accept(pcm.data(),320,sink);
        a.reset();
        result.clear();
        a.accept(pcm.data()+1000,20,sink);
        a.finish(sink);
        require(result==std::vector<float>(pcm.begin()+1000,pcm.begin()+1020));
    }
    std::cout<<"VAD_WINDOW_OK\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';
return 1;
}}
