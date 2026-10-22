#include "voice_engine/tts.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unistd.h>
using namespace voice_engine;
using J=nlohmann::json;
namespace fs=std::filesystem;
void require(bool x,const char*s){if(!x)throw std::runtime_error(s);}
std::string read(const fs::path&p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
void write(const fs::path&p,const std::string&s){std::ofstream f(p);f<<s;}
template<class F>void reject(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
int main(){
 char pattern[]="/tmp/tts-pronunciation-test-XXXXXX";char *name=::mkdtemp(pattern);
 if(!name)return 1;fs::path r=name;
 try{
    const std::string original="调 d iao 4 4\n低 d i 1 1\n调动 d iao d ong 4 4 4 4\n调整 t iao zh eng 2 2 3 3\n";
    write(r/"base",original);
    write(r/"tokens","_ 0\nd 1\nt 2\niao 3\ni 4\nong 5\nzh 6\neng 7\n");
    J data={{"schema_version",1},{"entries",J::array({{{"word","调低"},{"phones",{"t","iao","d","i"}},{"tones",{2,2,1,1}}}})}};
    write(r/"rules",data.dump());
    require(prepare_tts_pronunciation_lexicon(r/"base",r/"tokens",r/"rules",r/"out")==1,"wrong rule count");
    auto merged=read(r/"out");
    require(merged.rfind("调低 t iao d i 2 2 1 1\n",0)==0,"word pronunciation not added");
    require(merged.find(original)!=std::string::npos,"unrelated diao/tiao entries changed");
    require(read(r/"base")==original,"frozen lexicon modified");
    reject([&]{prepare_tts_pronunciation_lexicon(r/"base",r/"tokens",r/"rules",r/"out");});
    require(read(r/"out")==merged,"existing output overwritten");
    auto bad=data;bad["entries"][0]["phones"][0]="NOT_A_PHONE";
    write(r/"bad",bad.dump());
    reject([&]{prepare_tts_pronunciation_lexicon(r/"base",r/"tokens",r/"bad",r/"bad-out");});
    require(!fs::exists(r/"bad-out"),"invalid phones wrote output");
    bad=data;bad["entries"][0]["tones"][0]=2.5;write(r/"bad",bad.dump());
    reject([&]{prepare_tts_pronunciation_lexicon(r/"base",r/"tokens",r/"bad",r/"bad-out");});
    bad=data;bad["entries"][0]["tones"][0]=50;write(r/"bad",bad.dump());
    reject([&]{prepare_tts_pronunciation_lexicon(r/"base",r/"tokens",r/"bad",r/"bad-out");});
    bad=data;bad["entries"].push_back(bad["entries"][0]);write(r/"bad",bad.dump());
    reject([&]{prepare_tts_pronunciation_lexicon(r/"base",r/"tokens",r/"bad",r/"bad-out");});
    bad=data;bad["entries"][0]["word"]="<think>";write(r/"bad",bad.dump());
    reject([&]{prepare_tts_pronunciation_lexicon(r/"base",r/"tokens",r/"bad",r/"bad-out");});
    fs::create_symlink(r/"base",r/"link");
    reject([&]{prepare_tts_pronunciation_lexicon(r/"base",r/"tokens",r/"rules",r/"link");});
    require(read(r/"base")==original,"symlink followed");
    fs::remove_all(r);std::cout<<"TTS_PRONUNCIATION_OK\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';fs::remove_all(r);return 1;}
}
