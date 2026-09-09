#include "fingerprint.hpp"
#include "work_control.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <vector>
namespace neolip {
namespace {
class Sha256 {
    std::array<std::uint32_t,8> h_{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    std::array<unsigned char,64> block_{};std::uint64_t bytes_=0;std::size_t used_=0;
    static std::uint32_t r(std::uint32_t n,unsigned b){return(n>>b)|(n<<(32-b));}
    void block(){
        static constexpr std::uint32_t k[]{0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];for(unsigned i=0;i<16;++i)w[i]=(std::uint32_t(block_[4*i])<<24)|(std::uint32_t(block_[4*i+1])<<16)|(std::uint32_t(block_[4*i+2])<<8)|block_[4*i+3];
        for(unsigned i=16;i<64;++i){const auto x=w[i-15],y=w[i-2];w[i]=w[i-16]+(r(x,7)^r(x,18)^(x>>3))+w[i-7]+(r(y,17)^r(y,19)^(y>>10));}
        auto a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
        for(unsigned i=0;i<64;++i){const auto t=h+(r(e,6)^r(e,11)^r(e,25))+((e&f)^(~e&g))+k[i]+w[i];const auto u=(r(a,2)^r(a,13)^r(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
        h_[0]+=a;h_[1]+=b;h_[2]+=c;h_[3]+=d;h_[4]+=e;h_[5]+=f;h_[6]+=g;h_[7]+=h;
    }
public:
    void add(const char* data,std::size_t size){bytes_+=size;for(std::size_t i=0;i<size;++i){block_[used_++]=static_cast<unsigned char>(data[i]);if(used_==64){block();used_=0;}}}
    std::string finish(){const auto bits=bytes_*8;block_[used_++]=0x80;if(used_>56){while(used_<64)block_[used_++]=0;block();used_=0;}while(used_<56)block_[used_++]=0;for(unsigned i=0;i<8;++i)block_[63-i]=static_cast<unsigned char>(bits>>(8*i));block();std::ostringstream o;o<<std::hex<<std::setfill('0');for(auto v:h_)o<<std::setw(8)<<v;return o.str();}
};
}
std::string sha256(const std::string& data){Sha256 h;h.add(data.data(),data.size());return h.finish();}
std::string fileFingerprint(const std::filesystem::path& file){
    const auto size=std::filesystem::file_size(file);const auto time=std::filesystem::last_write_time(file);
    std::ifstream f(file,std::ios::binary);if(!f)throw std::runtime_error("Cannot read input: "+file.u8string());
    Sha256 h;std::array<char,65536>b{};std::uintmax_t total=0;
    while(f){cslu::checkWork();f.read(b.data(),b.size());const auto n=f.gcount();h.add(b.data(),static_cast<std::size_t>(n));total+=static_cast<std::uintmax_t>(n);}
    if(f.bad()||total!=size||size!=std::filesystem::file_size(file)||time!=std::filesystem::last_write_time(file))throw std::runtime_error("An input file changed while it was being read. Retry the job.");
    return h.finish();
}
std::string treeFingerprint(const std::filesystem::path& root){
    if(root.empty()||!std::filesystem::exists(root))return sha256("<absent>");
    std::vector<std::filesystem::path> files;for(const auto& f:std::filesystem::recursive_directory_iterator(root))if(f.is_regular_file())files.push_back(f.path());
    std::sort(files.begin(),files.end());std::string list;
    for(const auto& f:files){const auto name=f.lexically_relative(root).generic_u8string();list+=std::to_string(name.size())+":"+name+fileFingerprint(f);}
    return sha256(list);
}
}
