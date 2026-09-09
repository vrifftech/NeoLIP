#include "audio.hpp"
#include "playback.hpp"
#include "fingerprint.hpp"
#include "review.hpp"
#include "raw_frontend.hpp"
#include "engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <chrono>
namespace fs=std::filesystem;
namespace {
void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected explicit rejection");}
struct Scratch{fs::path root=fs::temp_directory_path()/fs::u8path("NeoLIP polish spaces e-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));Scratch(){fs::create_directories(root);}~Scratch(){std::error_code ec;fs::remove_all(root,ec);}};
void put(const fs::path&p,const std::string&b){std::ofstream f(p,std::ios::binary);f.write(b.data(),static_cast<std::streamsize>(b.size()));check(bool(f),"Fixture write failed");}
void u16(std::string&b,unsigned x){b+=char(x&255);b+=char((x>>8)&255);}
void u32(std::string&b,std::uint32_t x){u16(b,x&65535);u16(b,x>>16);}
std::string wav(unsigned tag,unsigned bits,unsigned channels,unsigned rate,const std::string& samples){std::string b="RIFF";u32(b,36+static_cast<std::uint32_t>(samples.size()));b+="WAVEfmt ";u32(b,16);u16(b,tag);u16(b,channels);u32(b,rate);u32(b,rate*channels*(bits/8));u16(b,channels*(bits/8));u16(b,bits);b+="data";u32(b,static_cast<std::uint32_t>(samples.size()));b+=samples;return b;}
void sha(const fs::path&root){
 check(neolip::sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA empty vector");
 check(neolip::sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA abc vector");
 check(neolip::sha256(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","SHA million vector");
 const auto file=root/"same size same time.txt";put(file,"abc");const auto time=fs::last_write_time(file);const auto first=neolip::fileFingerprint(file);put(file,"abd");fs::last_write_time(file,time);check(neolip::fileFingerprint(file)!=first,"Fingerprint reused changed bytes with same size/mtime");
}
void decoding(const fs::path&root){
 for(unsigned bits:{8u,16u,24u,32u}){
  std::string data;for(unsigned i=0;i<3000;++i){const std::int32_t q=(int(i)%101-50)*100; if(bits==8)data+=char(128+q/256);else if(bits==16)u16(data,static_cast<unsigned>(q));else if(bits==24){const auto v=static_cast<std::uint32_t>(q*256);data+=char(v);data+=char(v>>8);data+=char(v>>16);}else u32(data,static_cast<std::uint32_t>(q*65536));}
  const auto file=root/("pcm"+std::to_string(bits)+".wav");put(file,wav(1,bits,1,16000,data));const auto decoded=neolip::decodeAudio(file);const auto preview=cslu::readMonoWaveform(file);const auto analysis=cslu::readPcm16Mono(file);
  check(decoded->waveform.mono==preview.mono,"Preview and shared decoder diverged");check(analysis.samples.size()==preview.mono.size(),"Frontend duration changed");
  for(std::size_t i=0;i<preview.mono.size();++i)check(std::abs(analysis.samples[i]-preview.mono[i]*32768.0)<1e-6,"Analysis used different samples from preview");
 }
 std::string data;for(float f:{-.5f,0.f,.25f,.5f}){std::uint32_t q=0;std::memcpy(&q,&f,4);u32(data,q);}const auto file=root/"float.wav";put(file,wav(3,32,1,16000,data));check(neolip::decodeAudio(file)->waveform.mono==std::vector<float>({-.5f,0,.25f,.5f}),"Float decoding changed finite samples");
 const float bad=std::numeric_limits<float>::quiet_NaN();std::uint32_t q=0;std::memcpy(&q,&bad,4);data.clear();u32(data,q);put(file,wav(3,32,1,16000,data));rejects([&]{neolip::decodeAudio(file);});
 put(file,wav(1,16,1,16000,"x"));rejects([&]{neolip::decodeAudio(file);});
 neolip::InterleavedAudio stereo{16000,2,{}};for(int i=0;i<16000;++i){const float x=.2f*std::sin(float(i)*.07f);stereo.samples.push_back(x);stereo.samples.push_back(-x);}
 const auto mix=neolip::analyseAudio(stereo);check(mix->channelChoiceRequired,"Stereo cancellation not caught");const auto left=neolip::analyseAudio(stereo,0);check(!left->channelChoiceRequired&&left->rms>.1,"Explicit channel not respected");check(left->waveform.mono.size()==16000,"Channel selection moved timing");rejects([&]{neolip::analyseAudio(stereo,2);});
 for(std::size_t i=1;i<stereo.samples.size();i+=2)stereo.samples[i]=0;check(neolip::analyseAudio(stereo)->channelChoiceRequired,"One silent channel not caught");
 auto quiet=neolip::analyseAudio({16000,1,std::vector<float>(16000,0.00001f)});check(!quiet->warnings.empty()&&quiet->waveform.mono.size()==16000,"Quiet audio warning or duration incorrect");
 auto clipped=neolip::analyseAudio({16000,1,std::vector<float>(16000,1.2f)});check(clipped->clippedFraction==1&&clipped->waveform.mono[0]<1,"Clipping handling failed");
}
std::shared_ptr<const neolip::AudioBuffer> longAudio(){neolip::InterleavedAudio audio{16000,1,{}};audio.samples.resize(160000);for(std::size_t i=0;i<audio.samples.size();++i)audio.samples[i]=float(int(i%511)-255)/512;return neolip::analyseAudio(std::move(audio));}
void transport(){
 const auto a=longAudio();neolip::PcmTransport player;player.load(a);check(player.position()==0&&!player.playing(),"Playback should start paused");player.play();check(player.position()==0,"Position advanced without a callback");
 std::vector<float> out(144000);check(player.render(out.data(),out.size())==out.size(),"Playback still stops at eight seconds");check(std::equal(out.begin(),out.end(),a->waveform.mono.begin()),"Output samples differ");check(player.position()==9,"Callback clock does not follow rendered frames");player.pause();player.render(out.data(),10);check(player.position()==9&&out[0]==0,"Pause moved cursor or emitted audio");
 player.seek(1);player.setLoop(true,1,1.25);player.play();out.resize(10000);player.render(out.data(),out.size());for(std::size_t i=0;i<out.size();++i)check(out[i]==a->waveform.mono[16000+i%4000],"Loop skipped or duplicated samples");check(std::abs(player.position()-1.125)<1e-9,"Loop cursor wrong");player.setLoop(false,0,0);player.seek(9.9);out.assign(4000,1);check(player.render(out.data(),4000)==1600,"End handling wrong");check(!player.playing()&&player.position()==10&&out[3999]==0,"End-of-audio failed");
 player.stop();check(player.position()==0,"Stop must reset playback");rejects([&]{player.seek(std::numeric_limits<double>::infinity());});rejects([&]{player.setLoop(true,4,2);});
 neolip::WaveformPeaks peaks(a);std::mt19937 rng(42);for(int i=0;i<1500;++i){auto begin=std::size_t(rng())%a->waveform.mono.size();auto end=std::min(a->waveform.mono.size(),begin+std::size_t(rng()%7000));auto p=peaks.range(begin,end);float lo=0,hi=0;for(auto j=begin;j<end;++j){lo=std::min(lo,a->waveform.mono[j]);hi=std::max(hi,a->waveform.mono[j]);}check(p.first==lo&&p.second==hi,"Cached waveform peaks differ from samples");}
}
void review(){
 const auto audio=neolip::analyseAudio({16000,1,std::vector<float>(32000,0)});cslu::ViterbiAlignment a;a.phn.intervals.push_back({0,1500,"A"});const auto warnings=neolip::reviewAlignment(*audio,a,{});std::set<std::string>codes;for(const auto&w:warnings){codes.insert(w.code);check(w.startSeconds>=0&&w.endSeconds<=2,"Warning outside audio range");}check(codes.count("stretched-sound")&&codes.count("speech-in-silence")&&codes.count("near-silence"),"Missing review flags");
}
void codecs(const fs::path&source){
#ifndef NEOLIP_OFFLINE_NO_CODECS
 for(const auto&name:{"tone.mp3","tone-ima.wav","tone-msadpcm.wav","tone-kotor.wav"}){
  const auto path=source/"tests/fixtures/audio"/name;const auto a=neolip::decodeAudio(path);const auto p=cslu::readMonoWaveform(path);const auto f=cslu::readPcm16Mono(path);check(a->waveform.mono==p.mono&&f.samples.size()==p.mono.size(),"Compressed generation/preview differ");check(a->rms>.01&&p.durationSeconds()>.4&&p.durationSeconds()<.8,"Compressed fixture failed to decode");for(std::size_t i=0;i<f.samples.size();++i)check(std::abs(f.samples[i]-p.mono[i]*32768.0)<1e-6,"Compressed frontend samples differ");
 }
#else
 (void)source;std::cout<<"NOT RUN: compressed codecs (offline validation harness only)\n";
#endif
}
}
int main(int argc,char**argv){try{check(argc==2,"Pass source tree");Scratch s;sha(s.root);decoding(s.root);transport();review();codecs(fs::u8path(argv[1]));std::cout<<"PASS: SHA/content invalidation, shared PCM/float decoding, input checks, callback transport, loop/seek/end, waveform peaks and review flags\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
