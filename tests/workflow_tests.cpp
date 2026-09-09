#include "engine.hpp"
#include "graph_build.hpp"
#include "playback.hpp"
#include "transcript.hpp"
#include "wav.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F&& action, const std::string& message) {
    bool caught=false; try { action(); } catch(const std::exception&) { caught=true; }
    require(caught,message);
}
void put(const fs::path& path,const std::string& bytes) {
    fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);
    out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));out.close();require(!out.fail(),"Write fixture");
}
std::string get(const fs::path& p) {std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
struct Scratch {
    fs::path root=fs::temp_directory_path()/fs::u8path("NeoLIP workflow spaces-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){fs::create_directories(root);}~Scratch(){std::error_code ec;fs::remove_all(root,ec);}
};
std::string utf16(const std::u16string& value,bool le) {
    std::string bytes=le?std::string("\xff\xfe",2):std::string("\xfe\xff",2);
    for(auto c:value){bytes+=static_cast<char>(le?(c&255):(c>>8));bytes+=static_cast<char>(le?(c>>8):(c&255));}return bytes;
}
void transcriptTests(const fs::path& root) {
    const std::vector<std::pair<std::string,std::vector<std::string>>> cases={
        {"...Hello there.",{"hello","there"}},
        {u8"…Hello there.",{"hello","there"}},
        {u8"Well—hello there.",{"well","hello","there"}},
        {u8"Hello\u00a0there.",{"hello","there"}},
        {u8"“Hello there.”",{"hello","there"}},
        {u8"I’m ready; I’d go, but I can’t.",{"i'm","ready","i'd","go","but","i","can't"}},
        {u8"HK‑47 says 100, not 1.5.",{"hk-47","says","100","not","1.5"}},
        {u8"[Persuade] <b>Hello</b> {whispers} hello\u202fthere.",{"hello","hello","there"}},
        {"I am a Jedi.",{"i","am","a","jedi"}},
        {".pau hello .pau",{".pau","hello",".pau"}},
        {".pau\r\nhello .pau\t",{".pau","hello",".pau"}}
    };
    for(const auto& item:cases) {
        const auto tokens=cslu::tokenizeTranscript(item.first);std::vector<std::string> words;
        std::size_t end=0;
        for(const auto& token:tokens){words.push_back(token.word);require(token.beginByte>=end&&token.endByte<=item.first.size()&&token.endByte>token.beginByte,"Invalid original text span");end=token.endByte;
            const auto again=cslu::tokenizeTranscript(item.first.substr(token.beginByte,token.endByte-token.beginByte));
            require(again.size()==1&&again[0].word==token.word,"Visible selection differs from pronunciation token");}
        require(words==item.second,"Token preservation failed: "+item.first);
    }
    const std::string expected=u8"“Hello”—I’m ready.\nName: \U0001f642";
    for(bool le:{true,false}) {
        const auto bytes=utf16(u"“Hello”—I’m ready.\nName: \U0001f642",le);
        const auto path=root/(le?"transcript LE.txt":"transcript BE.lab");put(path,bytes);
        require(neolip::readTranscript(path)==expected,"BOM UTF-16 import differs");require(get(path)==bytes,"Import modified source");
    }
    require(cslu::decodeTranscriptBytes(std::string("\xef\xbb\xbf")+expected)==expected,"UTF-8 BOM import");
    for(const auto& bad:std::vector<std::string>{std::string("\xff\xfe\x00",3),std::string("\xff\xfe\x00\xd8",4),std::string("\xfe\xff\xdc\x00",4),std::string("\xed\xa0\x80",3),std::string("\xc0\xaf",2),std::string("a\0b",3),std::string("\xff\xfe\0\0",4)})
        rejects([&]{(void)cslu::decodeTranscriptBytes(bad);},"Malformed transcript accepted");
}
void graphTests(neolip::Engine& engine,const fs::path& models) {
    for(const std::string line : {"hello hello there", "Why hello there welcome to the domain of Bogga the Hutt how may one serve you today", "I'm ready. I can't go. I'd already left.", ".pau hello .pau there .pau", "read the record again", "one two three"}) {
        const auto text=engine.pronounce(line);require(text.unknownWords.empty(),"Dictionary fixture unknown: "+line);
        const auto old=cslu::buildGraphBundleFromNtpText(text.ntp,models);
        const auto tagged=cslu::buildGraphBundleFromWords(text,models);
        require(old.graph.statesInitial==tagged.graph.statesInitial && old.graph.uniqueStates==tagged.graph.uniqueStates &&
            old.graph.stateToFromIdx==tagged.graph.stateToFromIdx && old.graph.stateToFrom==tagged.graph.stateToFrom && old.graph.phnToFrom==tagged.graph.phnToFrom,"Word metadata changed pronunciation graph: "+line);
        require(old.graph.stateDurations.size()==tagged.graph.stateDurations.size(),"Duration size changed");
        for(std::size_t i=0;i<old.graph.stateDurations.size();++i)
            require(old.graph.stateDurations[i].minimumFrames==tagged.graph.stateDurations[i].minimumFrames&&old.graph.stateDurations[i].maximumFrames==tagged.graph.stateDurations[i].maximumFrames,"Duration score changed");
        const auto& a=old.mappings;const auto& b=tagged.mappings;
        require(a.featStatesM==b.featStatesM&&a.featStatesP==b.featStatesP&&a.featStatesH==b.featStatesH&&a.transStatesM==b.transStatesM&&a.transStatesP==b.transStatesP&&a.transStatesH==b.transStatesH&&a.depList==b.depList&&a.vcList==b.vcList,"Word tags changed acoustic mapping");
        std::set<int> owners;
        for(auto owner:tagged.graph.stateWords)if(owner>=0)owners.insert(owner);
        for(std::size_t i=0;i<text.tokens.size();++i)if(text.tokens[i].word!=".pau"&&text.tokens[i].word!=".garbage")require(owners.count(static_cast<int>(i))==1,"Word lost in graph");
    }
    // Deliberately identical state labels: ownership must use node IDs, not label matching.
    const auto text=engine.pronounce(u8"…hello hello");
    cslu::TraceGraph graph;graph.statesInitial={"same","same",".pau"};graph.stateWords={0,1,-1};
    cslu::ViterbiAlignment path;path.segmentNodes={2,0,2,1};
    path.stateSegments={{0,100,".pau"},{100,255,"same"},{255,520,".pau"},{520,900,"same"}};
    const auto words=neolip::alignedWords(text,graph,path);
    require(words.size()==2&&words[0].startSeconds==.1&&words[0].endSeconds==.255&&words[1].startSeconds==.52&&words[1].endSeconds==.9,"Word times inferred instead of taken from path");
    require(words[0].beginByte==3&&words[1].beginByte>words[0].endByte,"Repeated word spans merged");
    path.segmentNodes.clear();rejects([&]{(void)neolip::alignedWords(text,graph,path);},"Missing word metadata silently estimated");
}
void wave(const fs::path& path) {
    cslu::DecodedWaveform data;data.sampleRate=16000;data.mono.resize(32000);
    for(std::size_t i=0;i<data.mono.size();++i)data.mono[i]=static_cast<float>(.18*std::sin(i*2.*3.141592653589793*150./16000.));
    fs::create_directories(path.parent_path());cslu::writePcm16WaveformSegment(path,data,0,2);
}
void batchAndWords(neolip::Engine& engine,const fs::path& root) {
    const auto input=root/"source voice",output=root/"separate lips";
    wave(input/"line.wav");put(input/"line.txt",u8"…Hello hello there.");
    wave(input/"conflict.wav");put(input/"conflict.mp3","not decoded because it conflicts");put(input/"conflict.txt","hello");
    auto plan=neolip::planBatch(input,output,false);
    require(plan.pairs.size()==1&&plan.conflicts.size()==2&&plan.problems.size()==1,"Every collision contender must be quarantined");
    plan.pairs[0].audioChannel=0;
    const auto result=engine.batch(plan,neolip::MouthMovement::Natural,false);
    require(result.created==1&&result.failed==0&&result.items.size()==1,"Independent valid batch line blocked");
    const auto& item=result.items[0];require(item.input==input/"line.wav"&&item.output==output/"line.lip"&&item.audioChannel==0,"Batch lost audio/channel association");
    require(neolip::matchingAudio(item.output).empty(),"Separate-output fixture must require recorded input path");
    require(item.words.size()==3&&item.transcript==u8"…Hello hello there.","Batch lost aligned word metadata");
    require(!item.audio&&!item.prepared&&!item.previewLip,"Batch retains large decoded buffers");
    std::size_t count=0;for(const auto& entry:fs::recursive_directory_iterator(output))if(entry.is_regular_file()){++count;require(entry.path().extension()==".lip","Unrequested output");}require(count==1,"Unexpected output count");
    auto duplicate=plan;duplicate.pairs.push_back(plan.pairs.front());rejects([&]{engine.batch(duplicate,neolip::MouthMovement::Natural,true);},"Hand-built collision accepted");
    neolip::CreateRequest request{input/"line.wav",u8"…Hello hello there.",output/"preview.lip"};request.audioChannel=0;
    const auto created=engine.create(request);require(created.status==neolip::Status::Created,"Word alignment fixture failed: "+created.message);
    require(created.words.size()==3,"Some words disappeared from actual alignment");double end=0;
    for(const auto& word:created.words){require(word.startSeconds>=end&&word.endSeconds>word.startSeconds&&word.endSeconds<=created.durationSeconds+.005,"Actual word interval invalid");require(request.spokenText.substr(word.beginByte,word.endByte-word.beginByte)==word.text,"Original spelling lost");end=word.endSeconds;}
    request.movement=neolip::MouthMovement::MinimalSmoothing;const auto preview=engine.preview(request);
    require(preview.reusedAlignment&&preview.words.size()==created.words.size(),"Preset discarded cached word alignment");
    for(std::size_t i=0;i<preview.words.size();++i)require(preview.words[i].startSeconds==created.words[i].startSeconds&&preview.words[i].endSeconds==created.words[i].endSeconds,"Preset changed word times");
    const auto skipped=engine.batch(plan,neolip::MouthMovement::Natural,false);
    require(skipped.skipped==1&&skipped.items[0].audioChannel==0&&skipped.items[0].input==item.input,"Skipped row lost source/channel");
}
void gainTest() {
    auto audio=std::make_shared<neolip::AudioBuffer>();audio->waveform.sampleRate=100;audio->waveform.mono.assign(100,.8f);
    neolip::PcmTransport transport;transport.load(audio);transport.setVolume(.25f);transport.play();float samples[10];
    require(transport.render(samples,10)==10,"Playback failed");for(float value:samples)require(std::abs(value-.2f)<1e-6f,"Preview volume failed");
    transport.setVolume(0);transport.render(samples,10);for(float value:samples)require(value==0,"Mute failed");
    require(transport.position()==.2&&audio->waveform.mono[0]==.8f,"Mute changed timing or source");
    rejects([&]{transport.setVolume(NAN);},"Invalid preview gain accepted");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Pass source tree");const fs::path source=fs::u8path(argv[1]);Scratch scratch;
        transcriptTests(scratch.root);gainTest();
        const neolip::Assets assets{source/"third_party/cslu_runtime/Toolkit/2.0/apps/fa",source/"dictionaries",scratch.root/"user.dict"};
        neolip::Engine engine(assets);graphTests(engine,assets.models);batchAndWords(engine,scratch.root);
        std::cout<<"PASS: transcript/import spans, graph equivalence, actual word times, batch conflicts/source/channel, preview gain\n";return 0;
    } catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
