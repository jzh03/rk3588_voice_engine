#include "voice_engine/tts.hpp"
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
namespace voice_engine {
namespace {
std::string lower_ascii(std::string s) {
    for(char &c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
    return s;
}
}
std::size_t prepare_tts_pronunciation_lexicon(const std::filesystem::path &base,
    const std::filesystem::path &tokens,const std::filesystem::path &rules,
    const std::filesystem::path &destination) {
    using J=nlohmann::json;
    if(std::filesystem::file_size(rules)>65536 || std::filesystem::file_size(base)>32*1024*1024 ||
       std::filesystem::file_size(tokens)>1024*1024)
        throw std::invalid_argument("TTS pronunciation: input exceeds bound");
    std::ifstream input(rules);J doc=J::parse(input);
    if(!doc.is_object() || !doc.contains("schema_version") ||
       !doc["schema_version"].is_number_integer() || doc["schema_version"]!=1 ||
       !doc.contains("entries") || !doc["entries"].is_array() ||
       doc["entries"].empty() || doc["entries"].size()>256)
        throw std::invalid_argument("TTS pronunciation: invalid schema");
    std::set<std::string> known;
    std::ifstream ts(tokens);std::string line;
    while(std::getline(ts,line)){
        std::istringstream row(line);std::string token;int id;
        if(row>>token>>id)known.insert(token);
    }
    if(known.empty())throw std::invalid_argument("TTS pronunciation: empty token inventory");
    // The upstream parser accepts 0..50, but that is not the model's embedding
    // bound. Admit only tones already used by the frozen, validated base package.
    std::set<int> known_tones;
    std::ifstream inventory(base);
    if(!inventory)throw std::runtime_error("TTS pronunciation: cannot read base lexicon");
    while(std::getline(inventory,line)){
        std::istringstream row(line);std::string word,item;row>>word;
        std::vector<std::string> columns;
        while(row>>item)columns.push_back(item);
        if(columns.empty() || columns.size()%2)continue;
        for(std::size_t n=columns.size()/2;n<columns.size();++n){
            std::size_t end=0;int tone=std::stoi(columns[n],&end);
            if(end!=columns[n].size() || tone<0 || tone>50)
                throw std::invalid_argument("TTS pronunciation: invalid base tone");
            known_tones.insert(tone);
        }
    }
    std::map<std::string,std::string> replacements;
    for(const auto &entry:doc["entries"]){
        auto word=lower_ascii(entry.at("word").get<std::string>());
        const auto parts=segment_tts_text(word);
        if(word.empty() || word.size()>96 || word.find_first_of(" \r\n\t")!=std::string::npos ||
           (parts.size()!=1 || parts.front()!=word) || replacements.count(word))
            throw std::invalid_argument("TTS pronunciation: invalid/duplicate word");
        const auto &phones=entry.at("phones");const auto &tones=entry.at("tones");
        if(!phones.is_array() || !tones.is_array() || phones.empty() ||
           phones.size()>32 || phones.size()!=tones.size())
            throw std::invalid_argument("TTS pronunciation: phone/tone shape mismatch");
        std::string row=word;
        for(const auto &p:phones){
            auto phone=p.get<std::string>();
            if(!known.count(phone))throw std::invalid_argument("TTS pronunciation: unknown phone "+phone);
            row+=' ';row+=phone;
        }
        for(const auto &t:tones){
            if(!t.is_number_integer())throw std::invalid_argument("TTS pronunciation: tone must be integer");
            auto tone=t.get<int>();
            if(tone<0 || tone>50 || !known_tones.count(tone))throw std::invalid_argument("TTS pronunciation: tone out of range");
            row+=' ';row+=std::to_string(tone);
        }
        replacements.emplace(word,row+'\n');
    }
    std::string merged;
    for(const auto &p:replacements)merged+=p.second;
    std::ifstream source(base);
    if(!source)throw std::runtime_error("TTS pronunciation: cannot read base lexicon");
    while(std::getline(source,line)){
        std::istringstream row(line);std::string word;row>>word;
        if(!replacements.count(lower_ascii(word)))merged+=line+'\n';
    }
    const int fd=::open(destination.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0)throw std::runtime_error("TTS pronunciation: derived lexicon exists or cannot be created");
    std::size_t offset=0;
    while(offset<merged.size()){
        auto n=::write(fd,merged.data()+offset,merged.size()-offset);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){::close(fd);throw std::runtime_error("TTS pronunciation: derived lexicon write failed");}
        offset+=static_cast<std::size_t>(n);
    }
    if(::close(fd)!=0)throw std::runtime_error("TTS pronunciation: derived lexicon close failed");
    return replacements.size();
}
} // namespace voice_engine
