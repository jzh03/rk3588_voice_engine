#pragma once
#include <algorithm>
#include <codecvt>
#include <cwctype>
#include <locale>
#include <numeric>
#include <string>
#include <vector>
namespace voice_engine::asr_metrics {
inline std::u32string codepoints(const std::string& s){return std::wstring_convert<std::codecvt_utf8<char32_t>,char32_t>{}.from_bytes(s);
}
// Current fixture alphabet: CJK/ASCII/full-width ASCII. References already carry
// fixture NFKC/casefold normalization. This is not a general Unicode NFKC engine.
inline std::string normalize(const std::string& s){
    std::u32string out;
    for(auto c:codepoints(s)){
        if(c>=0xff01&&c<=0xff5e)c-=0xfee0;
        if(c>='A'&&c<='Z')c+='a'-'A';
        if(c==' '||c=='\t'||c=='\r'||c=='\n'||c==0x3000||(c>=0x2000&&c<=0x200b))continue;
        if((c>=33&&c<=47)||(c>=58&&c<=64)||(c>=91&&c<=96)||(c>=123&&c<=126)||(c>=0x3001&&c<=0x303f)||(c>=0x2010&&c<=0x2027)||c==0xffe5)continue;
        out.push_back(c);
    }
    return std::wstring_convert<std::codecvt_utf8<char32_t>,char32_t>{}.to_bytes(out);
}
inline std::size_t edit_distance(const std::string& a,const std::string& b){
    auto x=codepoints(a),y=codepoints(b);
    std::vector<std::size_t> previous(y.size()+1),current(y.size()+1);
    std::iota(previous.begin(),previous.end(),0);
    for(std::size_t i=0;i<x.size();++i){current[0]=i+1;
    for(std::size_t j=0;j<y.size();++j)current[j+1]=std::min({previous[j+1]+1,current[j]+1,previous[j]+(x[i]!=y[j])});
    std::swap(previous,current);
    }return previous.back();
}
} // namespace voice_engine::asr_metrics
