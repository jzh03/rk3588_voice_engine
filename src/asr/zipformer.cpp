// Copyright (c) 2024 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tensor feedback/layout and greedy decoding derived from Rockchip RKNN Model
// Zoo bad6c7334531becaf90a561988519b7bec34d0ab, Apache-2.0. This adapter adds
// incremental feature consumption, RAII, explicit contracts and reset handling.
#include "voice_engine/asr.hpp"
#include "voice_engine/probe_support.hpp"
#include "rknn_api.h"
#include "kaldi-native-fbank/csrc/online-feature.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace voice_engine {
namespace {
using json=nlohmann::json;
void check(int rc,const char* op) { if(rc!=RKNN_SUCC) throw std::runtime_error(std::string(op)+": "+std::to_string(rc));
 }
struct Net {
    rknn_context ctx=0;
    std::vector<rknn_tensor_attr> ia,oa;
    std::vector<rknn_input> inputs;
    std::vector<rknn_output> outputs;
    std::vector<std::vector<std::uint64_t>> ib,ob;
     // aligned storage
    std::string sdk,driver;
    ~Net(){ if(ctx) rknn_destroy(ctx);
     }
    void load(const std::filesystem::path& p,unsigned ni,unsigned no) {
        check(rknn_init(&ctx,const_cast<char*>(p.c_str()),0,0,nullptr),"rknn_init");
        rknn_sdk_version v{};
         check(rknn_query(ctx,RKNN_QUERY_SDK_VERSION,&v,sizeof(v)),"runtime version");
         sdk=v.api_version;
         driver=v.drv_version;
        if(sdk.rfind("2.3.2",0)!=0 || driver!="0.9.8") throw std::runtime_error("unvalidated RKNN SDK/driver: "+sdk+" / "+driver);
        rknn_input_output_num io{};
         check(rknn_query(ctx,RKNN_QUERY_IN_OUT_NUM,&io,sizeof(io)),"io count");
        if(io.n_input!=ni||io.n_output!=no) throw std::runtime_error("unexpected RKNN input/output counts");
        ia.resize(ni);
         oa.resize(no);
         inputs.resize(ni);
         outputs.resize(no);
         ib.resize(ni);
         ob.resize(no);
        for(unsigned i=0;i<ni;++i){
            ia[i].index=i;
             check(rknn_query(ctx,RKNN_QUERY_INPUT_ATTR,&ia[i],sizeof(ia[i])),"input attr");
            if(ia[i].type!=RKNN_TENSOR_FLOAT16 && ia[i].type!=RKNN_TENSOR_INT64) throw std::runtime_error("unsupported input tensor type");
            auto bytes=ia[i].n_elems*(ia[i].type==RKNN_TENSOR_INT64?8:4);
            ib[i].resize((bytes+7)/8);
             inputs[i].index=i;
             inputs[i].buf=ib[i].data();
             inputs[i].size=bytes;
            inputs[i].type=ia[i].type==RKNN_TENSOR_INT64?RKNN_TENSOR_INT64:RKNN_TENSOR_FLOAT32;
             inputs[i].fmt=ia[i].fmt;
        }
        for(unsigned i=0;i<no;++i){
            oa[i].index=i;
             check(rknn_query(ctx,RKNN_QUERY_OUTPUT_ATTR,&oa[i],sizeof(oa[i])),"output attr");
            if(oa[i].type!=RKNN_TENSOR_FLOAT16 && oa[i].type!=RKNN_TENSOR_INT64) throw std::runtime_error("unsupported output tensor type");
            auto bytes=oa[i].n_elems*(oa[i].type==RKNN_TENSOR_INT64?8:4);
            ob[i].resize((bytes+7)/8);
             outputs[i].index=i;
             outputs[i].buf=ob[i].data();
             outputs[i].size=bytes;
            outputs[i].is_prealloc=1;
             outputs[i].want_float=oa[i].type!=RKNN_TENSOR_INT64;
        }
    }
    void zero(){ for(auto& v:ib)std::fill(v.begin(),v.end(),0);
     for(auto& v:ob)std::fill(v.begin(),v.end(),0);
     }
    void run(){
        check(rknn_inputs_set(ctx,inputs.size(),inputs.data()),"rknn_inputs_set");
        check(rknn_run(ctx,nullptr),"rknn_run");
        check(rknn_outputs_get(ctx,outputs.size(),outputs.data(),nullptr),"rknn_outputs_get");
        // Preallocated outputs remain caller-owned after release.
        check(rknn_outputs_release(ctx,outputs.size(),outputs.data()),"rknn_outputs_release");
    }
    float* in(unsigned i){return static_cast<float*>(inputs[i].buf);
    }
    float* out(unsigned i){return static_cast<float*>(outputs[i].buf);
    }
    json info()const{
        json j={{"sdk",sdk},{"driver",driver},{"inputs",json::array()},{"outputs",json::array()}};
        for(auto a:ia)j["inputs"].push_back({{"name",a.name},{"elements",a.n_elems},{"format",get_format_string(a.fmt)},{"type",get_type_string(a.type)},{"shape",std::vector<unsigned>(a.dims,a.dims+a.n_dims)}});
        for(auto a:oa)j["outputs"].push_back({{"name",a.name},{"elements",a.n_elems},{"format",get_format_string(a.fmt)},{"type",get_type_string(a.type)},{"shape",std::vector<unsigned>(a.dims,a.dims+a.n_dims)}});
        return j;
    }
};
void require_feature_contract(const std::filesystem::path& p){
    std::ifstream in(p);
     json j;
     in>>j;
    const json expected={{"sample_rate_hz",16000},{"num_mel_bins",80},{"high_freq_hz",-400},{"dither",0},{"snip_edges",false},{"frame_shift_ms",10},{"frame_length_ms",25},{"window_type","povey"},{"low_freq_hz",20},{"preemphasis_coefficient",0.97},{"remove_dc_offset",true},{"round_to_power_of_two",true},{"use_log_fbank",true},{"use_power",true},{"batch",1},{"feature_segment_frames",103},{"feature_offset_frames",96},{"encoder_output_steps_per_chunk",24},{"decoder_context_size",2},{"decoder_dim",512},{"joiner_vocab_size",6254},{"blank_id",0},{"unk_id",2}};
    for(auto it=expected.begin();it!=expected.end();++it)if(!j.contains(it.key())||j[it.key()]!=it.value())throw std::runtime_error("feature contract mismatch: "+it.key());
}
}
struct ZipformerAsr::Impl {
    Net encoder,decoder,joiner;
    std::unique_ptr<knf::OnlineFbank> fbank;
    std::vector<std::string> vocabulary;
    std::string text;
    std::size_t max_text;
    int processed=0;
    bool finished=false;
    AsrStats stat;
    explicit Impl(const ZipformerFiles& f,std::size_t limit):max_text(limit){
        probe::require_model_validation_lock();
         require_feature_contract(f.feature_config);
        if(limit==0||limit>1048576)throw std::invalid_argument("invalid text limit");
        std::ifstream in(f.vocabulary);
         if(!in)throw std::runtime_error("cannot open vocabulary");
        vocabulary.resize(6257);
         std::string line;
        while(std::getline(in,line)){
            const auto pos=line.find_last_of(' ');
             if(pos==std::string::npos)throw std::runtime_error("bad vocabulary row");
            auto id=std::stoul(line.substr(pos+1));
             if(id>=vocabulary.size()||!vocabulary[id].empty())throw std::runtime_error("bad vocabulary id");
            auto token=line.substr(0,pos);
             std::size_t marker;
            while((marker=token.find("▁"))!=std::string::npos) token.replace(marker,3," ");
            vocabulary[id]=token;
        }
        if(std::any_of(vocabulary.begin(),vocabulary.end(),[](const auto& s){return s.empty();}))throw std::runtime_error("incomplete vocabulary");
        encoder.load(f.encoder,36,36);
         decoder.load(f.decoder,1,1);
         joiner.load(f.joiner,2,1);
        if(encoder.ia[0].n_elems!=103*80||encoder.oa[0].n_elems!=24*512||decoder.ia[0].n_elems!=2||decoder.ia[0].type!=RKNN_TENSOR_INT64||decoder.oa[0].n_elems!=512||joiner.ia[0].n_elems!=512||joiner.ia[1].n_elems!=512||joiner.oa[0].n_elems!=6254)throw std::runtime_error("Zipformer tensor contract mismatch");
        for(unsigned i=1;i<36;++i)if(encoder.ia[i].n_elems!=encoder.oa[i].n_elems||encoder.ia[i].type!=encoder.oa[i].type)throw std::runtime_error("cache contract mismatch");
        reset();
    }
    void reset(){
        knf::FbankOptions opts;
         opts.frame_opts.samp_freq=16000;
         opts.mel_opts.num_bins=80;
         opts.mel_opts.high_freq=-400;
         opts.frame_opts.dither=0;
         opts.frame_opts.snip_edges=false;
        fbank=std::make_unique<knf::OnlineFbank>(opts);
         encoder.zero();
         decoder.zero();
         joiner.zero();
         text.clear();
         processed=0;
         finished=false;
         stat={};
    }
    void chunk(){
        for(int i=0;i<103;++i)std::memcpy(encoder.in(0)+i*80,fbank->GetFrame(processed+i),80*sizeof(float));
        encoder.run();
        for(unsigned i=1;i<36;++i){
            const auto& a=encoder.ia[i];
            if(a.fmt==RKNN_TENSOR_NHWC){
                if(a.n_dims!=4||a.type!=RKNN_TENSOR_FLOAT16)throw std::runtime_error("unsupported NHWC cache");
                auto N=a.dims[0],H=a.dims[1],W=a.dims[2],C=a.dims[3];
                for(unsigned n=0;n<N;++n)for(unsigned c=0;c<C;++c)for(unsigned h=0;h<H;++h)for(unsigned w=0;w<W;++w)
                    encoder.in(i)[((n*H+h)*W+w)*C+c]=encoder.out(i)[((n*C+c)*H+h)*W+w];
            }else std::memcpy(encoder.inputs[i].buf,encoder.outputs[i].buf,encoder.inputs[i].size);
        }
        if(processed==0)decoder.run();
        for(int t=0;t<24;++t){
            std::memcpy(joiner.in(0),encoder.out(0)+t*512,512*sizeof(float));
             std::memcpy(joiner.in(1),decoder.out(0),512*sizeof(float));
             joiner.run();
            const auto token=std::max_element(joiner.out(0),joiner.out(0)+6254)-joiner.out(0);
            if(token!=0&&token!=2){
                if(text.size()+vocabulary[token].size()>max_text)throw std::runtime_error("ASR text capacity exceeded");
                text+=vocabulary[token];
                 auto* hyp=static_cast<std::int64_t*>(decoder.inputs[0].buf);
                 hyp[0]=hyp[1];
                 hyp[1]=token;
                 decoder.run();
            }
        }
        processed+=96;
         fbank->Pop(96);
         ++stat.encoder_chunks;
    }
    void available(){stat.peak_feature_frames=std::max(stat.peak_feature_frames,static_cast<std::size_t>(fbank->NumFramesReady()-processed));
     while(fbank->NumFramesReady()-processed>=103)chunk();
    }
    void accept(const float* p,std::size_t n){
        if(finished)throw std::logic_error("ASR accept after finish; reset required");
        if(n&&!p)throw std::invalid_argument("null PCM");
        if(n>120*16000-stat.input_samples)throw std::length_error("ASR stream exceeds 120 s diagnostic bound");
        for(std::size_t i=0;i<n;++i)if(!std::isfinite(p[i])||std::abs(p[i])>1.001f)throw std::invalid_argument("PCM must be finite normalized float32");
        stat.input_samples+=n;
        while(n){auto take=std::min<std::size_t>(n,320);
         fbank->AcceptWaveform(16000,p,take);
         available();
         p+=take;
        n-=take;
        }
    }
    std::string finish(){
        if(finished)return text;
        // Match the fixed official model's end padding while consuming PCM online.
        // Snapshot unpadded ready-frame count before padding; never infer extra
        // chunks solely because padding created more features.
        const int original=fbank->NumFramesReady();
        if(original>processed){
            const int needed=std::max(0,103-(original-processed));
            std::vector<float> zeros(needed*160,0);
             stat.tail_padding_samples=zeros.size();
            if(!zeros.empty())fbank->AcceptWaveform(16000,zeros.data(),zeros.size());
            fbank->InputFinished();
             available();
        }else fbank->InputFinished();
        finished=true;
         return text;
    }
};
ZipformerAsr::ZipformerAsr(const ZipformerFiles& f,std::size_t n):impl_(std::make_unique<Impl>(f,n)){}
ZipformerAsr::~ZipformerAsr()=default;
void ZipformerAsr::accept(const float* p,std::size_t n){impl_->accept(p,n);
}
std::string ZipformerAsr::finish(){return impl_->finish();
}
void ZipformerAsr::reset(){impl_->reset();
}
const std::string& ZipformerAsr::partial()const{return impl_->text;
}
AsrStats ZipformerAsr::stats()const{return impl_->stat;
}
std::string ZipformerAsr::runtime_info_json()const{return json({{"backend","RKNN NPU"},{"encoder",impl_->encoder.info()},{"decoder",impl_->decoder.info()},{"joiner",impl_->joiner.info()}}).dump();
}
} // namespace voice_engine
