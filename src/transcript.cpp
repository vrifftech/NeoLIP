#include "transcript.hpp"
#include "work_control.hpp"
#include <cstdint>
#include <regex>
#include <stdexcept>

namespace cslu {
namespace {
struct Scalar { std::uint32_t value; std::size_t begin, end; };
std::vector<Scalar> scalars(const std::string& text) {
    std::vector<Scalar> out;
    for (std::size_t i=0; i<text.size();) {
        if ((i & 255)==0) checkWork();
        const auto begin=i;
        const auto first=static_cast<unsigned char>(text[i++]);
        std::uint32_t cp=first, minimum=0; unsigned more=0;
        if (first>=0xc2 && first<=0xdf) { cp=first&31;more=1;minimum=0x80; }
        else if (first>=0xe0 && first<=0xef) { cp=first&15;more=2;minimum=0x800; }
        else if (first>=0xf0 && first<=0xf4) { cp=first&7;more=3;minimum=0x10000; }
        else if (first>=0x80) throw std::runtime_error("Transcript is not valid UTF-8. Use UTF-8 or BOM-marked UTF-16 text.");
        for (unsigned j=0;j<more;++j) {
            if(i>=text.size() || (static_cast<unsigned char>(text[i])&0xc0)!=0x80)
                throw std::runtime_error("Transcript contains a truncated or invalid UTF-8 character.");
            cp=(cp<<6)|(static_cast<unsigned char>(text[i++])&63);
        }
        if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) || cp==0)
            throw std::runtime_error("Transcript contains an invalid text character.");
        if(cp<32 && cp!=9 && cp!=10 && cp!=13)
            throw std::runtime_error("Transcript contains binary control characters.");
        out.push_back({cp,begin,i});
    }
    return out;
}
void appendUtf8(std::string& text,std::uint32_t cp) {
    if(cp<0x80) text+=static_cast<char>(cp);
    else if(cp<0x800) { text+=static_cast<char>(0xc0|(cp>>6));text+=static_cast<char>(0x80|(cp&63)); }
    else if(cp<0x10000) { text+=static_cast<char>(0xe0|(cp>>12));text+=static_cast<char>(0x80|((cp>>6)&63));text+=static_cast<char>(0x80|(cp&63)); }
    else { text+=static_cast<char>(0xf0|(cp>>18));text+=static_cast<char>(0x80|((cp>>12)&63));text+=static_cast<char>(0x80|((cp>>6)&63));text+=static_cast<char>(0x80|(cp&63)); }
}
bool separator(std::uint32_t c) {
    if(c<=0x20 || c==0x7f || c==0x85 || c==0xa0 || c==0x1680 ||
       (c>=0x2000 && c<=0x200b) || c==0x2028 || c==0x2029 || c==0x202f ||
       c==0x205f || c==0x3000 || c==0xfeff) return true;
    if(c==0x2012 || c==0x2013 || c==0x2014 || c==0x2015 || c==0x2026 ||
       c==0x201c || c==0x201d || c==0x201e || c==0x201f || c==0xab || c==0xbb ||
       c==0x3001 || c==0x3002) return true;
    if(c<128) return std::string("\",?!;:()/[]{}\\<>|*&").find(static_cast<char>(c))!=std::string::npos;
    return false;
}
std::uint32_t mapped(std::uint32_t c) {
    if(c>= 'A' && c<='Z') return c+('a'-'A');
    if(c==0x2018 || c==0x2019 || c==0x02bc) return '\'';
    if(c==0x2010 || c==0x2011) return '-';
    return c;
}
bool base(std::uint32_t cp) { const auto c=mapped(cp);return !separator(c) && c!='\'' && c!='-' && c!='.'; }
}
std::string decodeTranscriptBytes(const std::string& bytes) {
    std::string result=bytes;
    const auto is=[&](std::size_t i,unsigned char c){return bytes.size()>i && static_cast<unsigned char>(bytes[i])==c;};
    if ((is(0,0xff)&&is(1,0xfe)&&is(2,0)&&is(3,0)) || (is(0,0)&&is(1,0)&&is(2,0xfe)&&is(3,0xff)))
        throw std::runtime_error("UTF-32 transcripts are not supported. Save as UTF-8 or UTF-16.");
    if((is(0,0xff)&&is(1,0xfe)) || (is(0,0xfe)&&is(1,0xff))) {
        if(bytes.size()%2) throw std::runtime_error("Transcript ends in an incomplete UTF-16 character.");
        const bool le=is(0,0xff);result.clear();
        const auto unit=[&](std::size_t i)->std::uint32_t {auto a=static_cast<unsigned char>(bytes[i]),b=static_cast<unsigned char>(bytes[i+1]);return le?(a|(b<<8)):((a<<8)|b);};
        for(std::size_t i=2;i<bytes.size();i+=2) {
            auto cp=unit(i);
            if(cp>=0xd800 && cp<=0xdbff) {
                if(i+3>=bytes.size() || unit(i+2)<0xdc00 || unit(i+2)>0xdfff) throw std::runtime_error("Transcript contains an unpaired UTF-16 surrogate.");
                cp=0x10000+((cp-0xd800)<<10)+(unit(i+2)-0xdc00);i+=2;
            } else if(cp>=0xdc00 && cp<=0xdfff) throw std::runtime_error("Transcript contains an unpaired UTF-16 surrogate.");
            appendUtf8(result,cp);
        }
    } else if(is(0,0xef)&&is(1,0xbb)&&is(2,0xbf)) result.erase(0,3);
    (void)scalars(result);return result;
}
std::vector<TranscriptToken> tokenizeTranscript(const std::string& text,bool cleanup,bool stripBraces) {
    const auto input=scalars(text); // Validate before masking, so bad encoding is never hidden.
    std::vector<bool> hidden(text.size(),false);
    const auto mask=[&](const char* pattern) {
        const std::regex re(pattern);
        for(auto i=std::sregex_iterator(text.begin(),text.end(),re);i!=std::sregex_iterator();++i)
            for(std::size_t p=static_cast<std::size_t>(i->position()),end=p+i->length();p<end;++p)hidden[p]=true;
    };
    mask("<[^>]+>");
    if(cleanup) { mask(R"(\[[A-Za-z][A-Za-z0-9 _/+.-]{0,63}\])");if(stripBraces)mask(R"(\{[^}]*\})"); }
    std::vector<TranscriptToken> out;TranscriptToken token;
    const auto flush=[&]() {if(!token.word.empty()){out.push_back(token);token={};}};
    for(std::size_t i=0;i<input.size();++i) {
        const auto& s=input[i];auto c=mapped(s.value);
        if(hidden[s.begin] || separator(c)) {flush();continue;}
        if(c=='\'' || c=='-' || c=='.') {
            const bool next=i+1<input.size() && !hidden[input[i+1].begin] && base(input[i+1].value);
            const bool previous=!token.word.empty() && i>0 && base(input[i-1].value);
            // Decimal forms stay visibly unresolved instead of becoming separate digits.
            const bool decimal=c=='.' && previous && next && input[i-1].value>='0' && input[i-1].value<='9' && input[i+1].value>='0' && input[i+1].value<='9';
            if(c=='.' && token.word.empty()) {
                const auto rest=text.substr(s.begin);
                const auto special=rest.rfind(".garbage",0)==0?std::string(".garbage"):(rest.rfind(".pau",0)==0?std::string(".pau"):std::string{});
                if(!special.empty() && (i+special.size()==input.size() ||
                    (i+special.size()<input.size() && separator(input[i+special.size()].value)))) {
                    token.beginByte=s.begin;token.word=".";token.endByte=s.end;continue;
                }
            }
            if(!decimal && (c=='.' || !previous || !next)) {flush();continue;}
        }
        if(token.word.empty())token.beginByte=s.begin;
        appendUtf8(token.word,c);token.endByte=s.end;
    }
    flush();return out;
}
}
